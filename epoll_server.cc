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

#include <iostream>
#include <string>
#include <thread>
#include <memory>
#include <functional>


namespace mux {

namespace transport {

static const uint32_t kEpollWaitTime = 10; // epoll wait timeout 10 ms
static const uint32_t kMaxEvents = 100;    // epoll wait return max size


// packet of send/recv binary content
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

// the implementation of Epoll Tcp Server
class EpollTcpServer : public ETBase {
public:
    EpollTcpServer()                                       = default;
    EpollTcpServer(const EpollTcpServer& other)            = delete;
    EpollTcpServer& operator=(const EpollTcpServer& other) = delete;
    EpollTcpServer(EpollTcpServer&& other)                 = delete;
    EpollTcpServer& operator=(EpollTcpServer&& other)      = delete;
    ~EpollTcpServer() override;

    // the local ip and port of tcp server
    EpollTcpServer(const std::string& local_ip, uint16_t local_port);

public:
    // start tcp server
    bool Start() override;
    // stop tcp server
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
    // set socket noblock
    int32_t MakeSocketNonBlock(int32_t fd);
    // listen() 
    int32_t Listen(int32_t listenfd);
    // add/modify/remove a item(socket/fd) in epoll instance(rbtree), for this example, just add a socket to epoll rbtree
    int32_t UpdateEpollEvents(int efd, int op, int fd, int events);

    // handle tcp accept event
    void OnSocketAccept();
    // handle tcp socket readable event(read())
    void OnSocketRead(int32_t fd);
    // handle tcp socket writeable event(write())
    void OnSocketWrite(int32_t fd);
    // one loop per thread, call epoll_wait and return ready socket(accept,readable,writeable,error...)
    void EpollLoop();


private:
    std::string local_ip_; // tcp local ip
    uint16_t local_port_ { 0 }; // tcp bind local port
    int32_t handle_ { -1 }; // listenfd
    int32_t efd_ { -1 }; // epoll fd
    std::shared_ptr<std::thread> th_loop_ { nullptr }; // one loop per thread(call epoll_wait in loop)
    bool loop_flag_ { true }; // if loop_flag_ is false, then exit the epoll loop
    callback_recv_t recv_callback_ { nullptr }; // callback when received
};

using ETServer = EpollTcpServer;

typedef std::shared_ptr<ETServer> ETServerPtr;


EpollTcpServer::EpollTcpServer(const std::string& local_ip, uint16_t local_port)
    : local_ip_ { local_ip },
      local_port_ { local_port } {
}

EpollTcpServer::~EpollTcpServer() {
    Stop();
}

bool EpollTcpServer::Start() {
    // create epoll instance
    if (CreateEpoll() < 0) {
        return false;
    }
    // create socket and bind
    int listenfd = CreateSocket();
    if (listenfd < 0) {
        return false;
    }
    // set listen socket noblock
    int mr = MakeSocketNonBlock(listenfd);
    if (mr < 0) {
        return false;
    }

    // call listen()
    int lr = Listen(listenfd);
    if (lr < 0) {
        return false;
    }
    std::cout << "EpollTcpServer Init success!" << std::endl;
    handle_ = listenfd;

    // add listen socket to epoll instance, and focus on event EPOLLIN and EPOLLOUT, actually EPOLLIN is enough
    int er = UpdateEpollEvents(efd_, EPOLL_CTL_ADD, handle_, EPOLLIN | EPOLLET);
    if (er < 0) {
        // if something goes wrong, close listen socket and return false
        ::close(handle_);
        return false;
    }

    assert(!th_loop_);

    // the implementation of one loop per thread: create a thread to loop epoll
    th_loop_ = std::make_shared<std::thread>(&EpollTcpServer::EpollLoop, this);
    if (!th_loop_) {
        return false;
    }
    // detach the thread(using loop_flag_ to control the start/stop of loop)
    th_loop_->detach();

    return true;
}


// stop epoll tcp server and release epoll
bool EpollTcpServer::Stop() {
    // set loop_flag_ false to stop epoll loop
    loop_flag_ = false;
    ::close(handle_);
    ::close(efd_);
    std::cout << "stop epoll!" << std::endl;
    UnRegisterOnRecvCallback();
    return true;
}

int32_t EpollTcpServer::CreateEpoll() {
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

int32_t EpollTcpServer::CreateSocket() {
    // create tcp socket
    int listenfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        std::cout << "create socket " << local_ip_ << ":" << local_port_ << " failed!" << std::endl;
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(local_port_);
    addr.sin_addr.s_addr  = inet_addr(local_ip_.c_str());

    // bind to local ip and local port
    int r = ::bind(listenfd, (struct sockaddr*)&addr, sizeof(struct sockaddr));
    if (r != 0) {
        std::cout << "bind socket " << local_ip_ << ":" << local_port_ << " failed!" << std::endl;
        ::close(listenfd);
        return -1;
    }
    std::cout << "create and bind socket " << local_ip_ << ":" << local_port_ << " success!" << std::endl;
    return listenfd;
}

// set noblock fd
int32_t EpollTcpServer::MakeSocketNonBlock(int32_t fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        std::cout << "fcntl failed!" << std::endl;
        return -1;
    }
    int r = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (r < 0) {
        std::cout << "fcntl failed!" << std::endl;
        return -1;
    }
    return 0;
}

// call listen() api and set listen queue size using SOMAXCONN
int32_t EpollTcpServer::Listen(int32_t listenfd) {
    int r = ::listen(listenfd, SOMAXCONN);
    if ( r < 0) {
        std::cout << "listen failed!" << std::endl;
        return -1;
    }
    return 0;
}

// add/modify/remove a item(socket/fd) in epoll instance(rbtree), for this example, just add a socket to epoll rbtree
int32_t EpollTcpServer::UpdateEpollEvents(int efd, int op, int fd, int events) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd; // ev.data is a enum
    fprintf(stdout,"%s fd %d events read %d write %d\n", op == EPOLL_CTL_MOD ? "mod" : "add", fd, ev.events & EPOLLIN, ev.events & EPOLLOUT);
    int r = epoll_ctl(efd, op, fd, &ev);
    if (r < 0) {
        std::cout << "epoll_ctl failed!" << std::endl;
        return -1;
    }
    return 0;
}

// handle accept event
void EpollTcpServer::OnSocketAccept() {
    // epoll working on et mode, must read all coming data, so use a while loop here
    while (true) {
        struct sockaddr_in in_addr;
        socklen_t in_len = sizeof(in_addr);

        // accept a new connection and get a new socket
        int cli_fd = accept(handle_, (struct sockaddr*)&in_addr, &in_len);
        if (cli_fd == -1) {
            if ( (errno == EAGAIN) || (errno == EWOULDBLOCK) ) {
                // read all accept finished(epoll et mode only trigger one time,so must read all data in listen socket)
                std::cout << "accept all coming connections!" << std::endl;
                break;
            } else {
                std::cout << "accept error!" << std::endl;
                continue;
            }
        }

        sockaddr_in peer;
        socklen_t p_len = sizeof(peer);
        // get client ip and port
        int r = getpeername(cli_fd, (struct sockaddr*)&peer, &p_len);
        if (r < 0) {
            std::cout << "getpeername error!" << std::endl;
            continue;
        }
        std::cout << "accpet connection from " << inet_ntoa(in_addr.sin_addr) << std::endl;
        int mr = MakeSocketNonBlock(cli_fd);
        if (mr < 0) {
            ::close(cli_fd);
            continue;
        }

        //  add this new socket to epoll instance, and focus on EPOLLIN and EPOLLOUT and EPOLLRDHUP event
        int er = UpdateEpollEvents(efd_, EPOLL_CTL_ADD, cli_fd, EPOLLIN | EPOLLRDHUP | EPOLLET);
        if (er < 0 ) {
            // if something goes wrong, close this new socket
            ::close(cli_fd);
            continue;
        }
    }
}

// register a callback when packet received
void EpollTcpServer::RegisterOnRecvCallback(callback_recv_t callback) {
    assert(!recv_callback_);
    recv_callback_ = callback;
}

void EpollTcpServer::UnRegisterOnRecvCallback() {
    assert(recv_callback_);
    recv_callback_ = nullptr;
}

// handle read events on fd
void EpollTcpServer::OnSocketRead(int32_t fd) {
    char read_buf[4096];
    bzero(read_buf, sizeof(read_buf));
    int n = -1;
    // epoll working on et mode, must read all data
    while ( (n = ::read(fd, read_buf, sizeof(read_buf))) > 0) {
        // callback for recv
        std::cout << "fd: " << fd <<  " recv: " << read_buf << std::endl;
        std::string msg(read_buf, n);
        // create a recv packet
        PacketPtr data = std::make_shared<Packet>(fd, msg);
        if (recv_callback_) {
            // handle recv packet
            recv_callback_(data);
        }
    }
    if (n == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // read all data finished
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
void EpollTcpServer::OnSocketWrite(int32_t fd) {
    // TODO(smaugx) not care for now
    std::cout << "fd: " << fd << " writeable!" << std::endl;
}

// send packet
int32_t EpollTcpServer::SendData(const PacketPtr& data) {
    if (data->fd == -1) {
        return -1;
    }
    // send packet on fd
    int r = ::write(data->fd, data->msg.data(), data->msg.size());
    if (r == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1;
        }
        // error happend
        ::close(data->fd);
        std::cout << "fd: " << data->fd << " write error, close it!" << std::endl;
        return -1;
    }
    std::cout << "fd: " << data->fd << " write size: " << r << " ok!" << std::endl;
    return r;
}

// one loop per thread, call epoll_wait and handle all coming events
void EpollTcpServer::EpollLoop() {
    // request some memory, if events ready, socket events will copy to this memory from kernel
    struct epoll_event* alive_events =  static_cast<epoll_event*>(calloc(kMaxEvents, sizeof(epoll_event)));
    if (!alive_events) {
        std::cout << "calloc memory failed for epoll_events!" << std::endl;
        return;
    }
    // if loop_flag_ is false, will exit this loop
    while (loop_flag_) {
        // call epoll_wait and return ready socket
        int num = epoll_wait(efd_, alive_events, kMaxEvents, kEpollWaitTime);

        for (int i = 0; i < num; ++i) {
            // get fd
            int fd = alive_events[i].data.fd;
            // get events(readable/writeable/error)
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
                std::cout << "epollin" << std::endl;
                if (fd == handle_) {
                    // listen fd coming connections
                    OnSocketAccept();
                } else {
                    // other fd read event coming, meaning data coming
                    OnSocketRead(fd);
                }
            } else if ( events & EPOLLOUT ) {
                std::cout << "epollout" << std::endl;
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
using namespace transport;

int main(int argc, char* argv[]) {
    std::string local_ip {"127.0.0.1"};
    uint16_t local_port { 6666 };
    if (argc >= 2) {
        local_ip = std::string(argv[1]);
    }
    if (argc >= 3) {
        local_port = std::atoi(argv[2]);
    }
    // create a epoll tcp server
    auto epoll_server = std::make_shared<EpollTcpServer>(local_ip, local_port);
    if (!epoll_server) {
        std::cout << "tcp_server create faield!" << std::endl;
        exit(-1);
    }

    // recv callback in lambda mode, you can set your own callback here
    auto recv_call = [&](const PacketPtr& data) -> void {
        // just echo packet
        epoll_server->SendData(data);
        return;
    };

    // register recv callback to epoll tcp server
    epoll_server->RegisterOnRecvCallback(recv_call);

    // start the epoll tcp server
    if (!epoll_server->Start()) {
        std::cout << "tcp_server start failed!" << std::endl;
        exit(1);
    }
    std::cout << "############tcp_server started!################" << std::endl;

    // block here 
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    epoll_server->Stop();

    return 0;
}


