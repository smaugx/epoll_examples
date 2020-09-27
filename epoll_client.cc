#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <cstring>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>

#include <string>
#include <iostream>
#include <memory>
#include <functional>
#include <thread>

// actually no need to implement a tcp client using epoll


namespace mux {

namespace transport {

static const uint32_t kEpollWaitTime = 10; // epoll wait timeout 10 ms
static const uint32_t kMaxEvents = 100;    // epoll wait return max size

typedef struct Packet {
public:
    Packet()
        : msg { "" } {}
    Packet(const std::string& msg)
        : msg { msg } {}
    Packet(int fd, const std::string& msg)
        : fd(fd),
          msg(msg) {}

    int fd { -1 };     // meaning socket
    std::string msg;   // real binary content
} Packet;

typedef std::shared_ptr<Packet> PacketPtr;

// callback when packet received
using callback_recv_t = std::function<void(const PacketPtr& data)>;

// base class of EpollTcpServer, focus on Start(), Stop(), SendData(), RegisterOnRecvCallback()...
class EpollTcpBase {
public:
    EpollTcpBase()                                     = default;
    EpollTcpBase(const EpollTcpBase& other)            = delete;
    EpollTcpBase& operator=(const EpollTcpBase& other) = delete;
    EpollTcpBase(EpollTcpBase&& other)                 = delete;
    EpollTcpBase& operator=(EpollTcpBase&& other)      = delete;
    virtual ~EpollTcpBase()                            = default; 

public:
    virtual bool Start() = 0;
    virtual bool Stop()  = 0;
    virtual int32_t SendData(const PacketPtr& data) = 0;
    virtual void RegisterOnRecvCallback(callback_recv_t callback) = 0;
    virtual void UnRegisterOnRecvCallback() = 0;
};

using ETBase = EpollTcpBase;

typedef std::shared_ptr<ETBase> ETBasePtr;



// the implementation of Epoll Tcp client
class EpollTcpClient : public ETBase {
public:
    EpollTcpClient()                                       = default;
    EpollTcpClient(const EpollTcpClient& other)            = delete;
    EpollTcpClient& operator=(const EpollTcpClient& other) = delete;
    EpollTcpClient(EpollTcpClient&& other)                 = delete;
    EpollTcpClient& operator=(EpollTcpClient&& other)      = delete;
    ~EpollTcpClient() override;

    // the server ip and port
    EpollTcpClient(const std::string& server_ip, uint16_t server_port);

public:
    // start tcp client
    bool Start() override;
    // stop tcp client
    bool Stop() override;
    // send packet
    int32_t SendData(const PacketPtr& data) override;
    // register a callback when packet received
    void RegisterOnRecvCallback(callback_recv_t callback) override;
    void UnRegisterOnRecvCallback() override;

protected:
    // create epoll instance using epoll_create and return a fd of epoll
    int32_t CreateEpoll();
    // create a socket fd using api socket()
    int32_t CreateSocket();
    // connect to server
    int32_t Connect(int32_t listenfd);
    // add/modify/remove a item(socket/fd) in epoll instance(rbtree), for this example, just add a socket to epoll rbtree
    int32_t UpdateEpollEvents(int efd, int op, int fd, int events);
    // handle tcp socket readable event(read())
    void OnSocketRead(int32_t fd);
    // handle tcp socket writeable event(write())
    void OnSocketWrite(int32_t fd);
    // one loop per thread, call epoll_wait and return ready socket(readable,writeable,error...)
    void EpollLoop();


private:
    std::string server_ip_; // tcp server ip
    uint16_t server_port_ { 0 }; // tcp server port
    int32_t handle_ { -1 }; // client fd
    int32_t efd_ { -1 }; // epoll fd
    std::shared_ptr<std::thread> th_loop_ { nullptr }; // one loop per thread(call epoll_wait in loop)
    bool loop_flag_ { true }; // if loop_flag_ is false, then exit the epoll loop
    callback_recv_t recv_callback_ { nullptr }; // callback when received
};

using ETClient = EpollTcpClient;

typedef std::shared_ptr<ETClient> ETClientPtr;



EpollTcpClient::EpollTcpClient(const std::string& server_ip, uint16_t server_port)
    : server_ip_ { server_ip },
      server_port_ { server_port } {
}

EpollTcpClient::~EpollTcpClient() {
    Stop();
}

bool EpollTcpClient::Start() {
    // create epoll instance
    if (CreateEpoll() < 0) {
        return false;
    }
    // create socket and bind
    int cli_fd  = CreateSocket();
    if (cli_fd < 0) {
        return false;
    }

    // connect to server
    int lr = Connect(cli_fd);
    if (lr < 0) {
        return false;
    }
    std::cout << "EpollTcpClient Init success!" << std::endl;
    handle_ = cli_fd;

    // after connected successfully, add this socket to epoll instance, and focus on EPOLLIN and EPOLLOUT event
    int er = UpdateEpollEvents(efd_, EPOLL_CTL_ADD, handle_, EPOLLIN | EPOLLET);
    if (er < 0) {
        // if something goes wrong, close listen socket and return false
        ::close(handle_);
        return false;
    }

    assert(!th_loop_);

    // the implementation of one loop per thread: create a thread to loop epoll
    th_loop_ = std::make_shared<std::thread>(&EpollTcpClient::EpollLoop, this);
    if (!th_loop_) {
        return false;
    }
    // detach the thread(using loop_flag_ to control the start/stop of loop)
    th_loop_->detach();

    return true;
}


// stop epoll tcp client and release epoll
bool EpollTcpClient::Stop() {
    loop_flag_ = false;
    ::close(handle_);
    ::close(efd_);
    std::cout << "stop epoll!" << std::endl;
    UnRegisterOnRecvCallback();
    return true;
}

int32_t EpollTcpClient::CreateEpoll() {
    // the basic epoll api of create a epoll instance
    int epollfd = epoll_create(1);
    if (epollfd < 0) {
        // if something goes wrong, return -1
        std::cout << "epoll_create failed!" << std::endl;
        return -1;
    }
    efd_ = epollfd;
    return epollfd;
}

int32_t EpollTcpClient::CreateSocket() {
    // create tcp socket
    int cli_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (cli_fd < 0) {
        std::cout << "create socket failed!" << std::endl;
        return -1;
    }

    return cli_fd;
}

// connect to tcp server
int32_t EpollTcpClient::Connect(int32_t cli_fd) {
    struct sockaddr_in addr;  // server info
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server_port_);
    addr.sin_addr.s_addr  = inet_addr(server_ip_.c_str());

    int r = ::connect(cli_fd, (struct sockaddr*)&addr, sizeof(addr));
    if ( r < 0) {
        std::cout << "connect failed! r=" << r << " errno:" << errno << std::endl;
        return -1;
    }
    return 0;
}

// add/modify/remove a item(socket/fd) in epoll instance(rbtree), for this example, just add a socket to epoll rbtree
int32_t EpollTcpClient::UpdateEpollEvents(int efd, int op, int fd, int events) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    fprintf(stdout,"%s fd %d events read %d write %d\n", op == EPOLL_CTL_MOD ? "mod" : "add", fd, ev.events & EPOLLIN, ev.events & EPOLLOUT);
    int r = epoll_ctl(efd, op, fd, &ev);
    if (r < 0) {
        std::cout << "epoll_ctl failed!" << std::endl;
        return -1;
    }
    return 0;
}

// register a callback when packet received
void EpollTcpClient::RegisterOnRecvCallback(callback_recv_t callback) {
    assert(!recv_callback_);
    recv_callback_ = callback;
}

void EpollTcpClient::UnRegisterOnRecvCallback() {
    assert(recv_callback_);
    recv_callback_ = nullptr;
}

// handle read events on fd
void EpollTcpClient::OnSocketRead(int32_t fd) {
    char read_buf[4096];
    bzero(read_buf, sizeof(read_buf));
    int n = -1;
    while ( (n = ::read(fd, read_buf, sizeof(read_buf))) > 0) {
        // callback for recv
        std::string msg(read_buf, n);
        PacketPtr data = std::make_shared<Packet>(fd, msg);
        if (recv_callback_) {
            // handle recv packet
            recv_callback_(data);
        }
    }
    if (n == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // read finished
            return;
        }
        // something goes wrong for this fd, should close it
        ::close(fd);
        return;
    }
    if (n == 0) {
        // this may happen when client close socket. EPOLLRDHUP usually handle this, but just make sure; should close this fd
        ::close(fd);
        return;
    }
}

// handle write events on fd (usually happens when sending big files)
void EpollTcpClient::OnSocketWrite(int32_t fd) {
    std::cout << "fd: " << fd << " writeable!" << std::endl;
}

int32_t EpollTcpClient::SendData(const PacketPtr& data) {
    int r = ::write(handle_, data->msg.data(), data->msg.size());
    if (r == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1;
        }
        // error happend
        ::close(handle_);
        std::cout << "fd: " << handle_ << " write error, close it!" << std::endl;
        return -1;
    }
    return r;
}

// one loop per thread, call epoll_wait and handle all coming events
void EpollTcpClient::EpollLoop() {
    // request some memory, if events ready, socket events will copy to this memory from kernel
    struct epoll_event* alive_events =  static_cast<epoll_event*>(calloc(kMaxEvents, sizeof(epoll_event)));
    if (!alive_events) {
        std::cout << "calloc memory failed for epoll_events!" << std::endl;
        return;
    }
    while (loop_flag_) {
        int num = epoll_wait(efd_, alive_events, kMaxEvents, kEpollWaitTime);

        for (int i = 0; i < num; ++i) {
            int fd = alive_events[i].data.fd;
            int events = alive_events[i].events;

            if ( (events & EPOLLERR) || (events & EPOLLHUP) ) {
                std::cout << "epoll_wait error!" << std::endl;
                // An error has occured on this fd, or the socket is not ready for reading (why were we notified then?).
                ::close(fd);
            } else  if (events & EPOLLRDHUP) {
                // Stream socket peer closed connection, or shut down writing half of connection.
                // more inportant, We still to handle disconnection when read()/recv() return 0 or -1 just to be sure.
                std::cout << "fd:" << fd << " closed EPOLLRDHUP!" << std::endl;
                // close fd and epoll will remove it
                ::close(fd);
            } else if ( events & EPOLLIN ) {
                // other fd read event coming, meaning data coming
                OnSocketRead(fd);
            } else if ( events & EPOLLOUT ) {
                // write event for fd (not including listen-fd), meaning send buffer is available for big files
                OnSocketWrite(fd);
            } else {
                std::cout << "unknow epoll event!" << std::endl;
            }
        } // end for (int i = 0; ...

    } // end while (loop_flag_)
    free(alive_events);
}


} // end namespace transport
} // end namespace mux


using namespace mux;
using namespace mux::transport;

int main(int argc, char* argv[]) {
    std::string server_ip {"127.0.0.1"};
    uint16_t server_port { 6666 };
    if (argc >= 2) {
        server_ip = std::string(argv[1]);
    }
    if (argc >= 3) {
        server_port = std::atoi(argv[2]);
    }

    // create a tcp client
    auto tcp_client = std::make_shared<EpollTcpClient>(server_ip, server_port);
    if (!tcp_client) {
        std::cout << "tcp_client create faield!" << std::endl;
        exit(-1);
    }


    // recv callback in lambda mode, you can set your own callback here
    auto recv_call = [&](const transport::PacketPtr& data) -> void {
        // just print recv data to stdout
        std::cout << "recv: " << data->msg << std::endl;
        return;
    };

    // register recv callback to epoll tcp client
    tcp_client->RegisterOnRecvCallback(recv_call);

    // start the epoll tcp client
    if (!tcp_client->Start()) {
        std::cout << "tcp_client start failed!" << std::endl;
        exit(1);
    }
    std::cout << "############tcp_client started!################" << std::endl;

    std::string msg;
    while (true) {
        // read content from stdin
        std::cout << std::endl << "input:";
        std::getline(std::cin, msg);
        auto packet = std::make_shared<Packet>(msg);
        tcp_client->SendData(packet);
        //std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    tcp_client->Stop();

    return 0;
}
