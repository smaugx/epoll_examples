<center>epoll 入门例子</center>

# 复习一下
上一篇博文 [epoll原理深入分析]() 详细分析了 epoll 底层的实现原理，如果对 epoll 原理有模糊的建议先看一下这篇文章。那么本文就开始用 epoll 实现一个简单的 tcp server/client。

本文基于我的 github: [https://github.com/smaugx/epoll_examples](https://github.com/smaugx/epoll_examples)。



# epoll 实现范式

```
# create listen socket
int listenfd = ::socket();

# bind to local port and ip
int r = ::bind();


# create epoll instance and get an epoll-fd
int epollfd = epoll_create(1);
 
# add listenfd to epoll instance
int r = epoll_ctl(..., listenfd, ...);


# begin epoll_wait, wait for ready socket

struct epoll_event* alive_events =  static_cast<epoll_event*>(calloc(kMaxEvents, sizeof(epoll_event)));

while (true) {
            int num = epoll_wait(epollfd, alive_events, kMaxEvents, kEpollWaitTime);
    
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
    
}
```


epoll 编程基本是按照上面的范式进行的，这里要注意的是上面的反应的只是单进程或者单线程的情况。


如果涉及到多线程或者多进程，那么通常来说会在 listen() 创建完成之后，创建多线程或者多进程，然后再操作 epoll.

```
int listenfd = ::socket();


...

int p = fork() # 多进程 或者多线程创建

int r = epoll_ctl(..., listenfd, ...);

...

while(true) {
int num = epoll_wait(epollfd, alive_events, kMaxEvents, kEpollWaitTime);
...
}
```

同理，多线程版本也是一样，把上面的 fork() 替换成 thread 创建即可。

也就是 listenfd 被添加到了多个进程或者多个线程中，提高吞吐量。这就是基本的 epoll 多进程或者多线程编程范式。

但本文就先讨论单进程（单线程）版本的 epoll 实现。

# epoll tcp server

先上代码：

```
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

static const uint32_t kEpollWaitTime = 10; // 10 ms
static const uint32_t kMaxEvents = 100;


typedef struct Packet {
public:
    Packet()
        : msg { "" } {}
    Packet(const std::string& msg)
        : msg { msg } {}
    Packet(int fd, const std::string& msg)
        : fd(fd),
          msg(msg) {}

    int fd { -1 };
    std::string msg;
} Packet;

typedef std::shared_ptr<Packet> PacketPtr;

using callback_recv_t = std::function<void(const PacketPtr& data)>;

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

class EpollTcpServer : public ETBase {
public:
    EpollTcpServer()                                       = default;
    EpollTcpServer(const EpollTcpServer& other)            = delete;
    EpollTcpServer& operator=(const EpollTcpServer& other) = delete;
    EpollTcpServer(EpollTcpServer&& other)                 = delete;
    EpollTcpServer& operator=(EpollTcpServer&& other)      = delete;
    ~EpollTcpServer() override;

    EpollTcpServer(const std::string& local_ip, uint16_t local_port);

public:
    bool Start() override;
    bool Stop() override;
    int32_t SendData(const PacketPtr& data) override;
    void RegisterOnRecvCallback(callback_recv_t callback) override;
    void UnRegisterOnRecvCallback() override;

protected:
    int32_t CreateEpoll();
    int32_t CreateSocket();
    int32_t MakeSocketNonBlock(int32_t fd);
    int32_t Listen(int32_t listenfd);
    int32_t UpdateEpollEvents(int efd, int op, int fd, int events);
    void OnSocketAccept();
    void OnSocketRead(int32_t fd);
    void OnSocketWrite(int32_t fd);
    void EpollLoop();


private:
    std::string local_ip_;
    uint16_t local_port_ { 0 };
    int32_t handle_ { -1 }; // listenfd
    int32_t efd_ { -1 }; // epoll fd
    std::shared_ptr<std::thread> th_loop_ { nullptr };
    bool loop_flag_ { true };
    callback_recv_t recv_callback_ { nullptr };
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
    if (CreateEpoll() < 0) {
        return false;
    }
    // create socket and bind
    int listenfd = CreateSocket();
    if (listenfd < 0) {
        return false;
    }
    int mr = MakeSocketNonBlock(listenfd);
    if (mr < 0) {
        return false;
    }

    int lr = Listen(listenfd);
    if (lr < 0) {
        return false;
    }
    std::cout << "EpollTcpServer Init success!" << std::endl;
    handle_ = listenfd;

    int er = UpdateEpollEvents(efd_, EPOLL_CTL_ADD, handle_, EPOLLIN | EPOLLET);
    if (er < 0) {
        ::close(handle_);
        return false;
    }

    assert(!th_loop_);

    th_loop_ = std::make_shared<std::thread>(&EpollTcpServer::EpollLoop, this);
    if (!th_loop_) {
        return false;
    }
    th_loop_->detach();

    return true;
}


bool EpollTcpServer::Stop() {
    loop_flag_ = false;
    ::close(handle_);
    ::close(efd_);
    std::cout << "stop epoll!" << std::endl;
    UnRegisterOnRecvCallback();
    return true;
}

int32_t EpollTcpServer::CreateEpoll() {
    int epollfd = epoll_create(1);
    if (epollfd < 0) {
        std::cout << "epoll_create failed!" << std::endl;
        return -1;
    }
    efd_ = epollfd;
    return epollfd;
}

int32_t EpollTcpServer::CreateSocket() {
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

    int r = ::bind(listenfd, (struct sockaddr*)&addr, sizeof(struct sockaddr));
    if (r != 0) {
        std::cout << "bind socket " << local_ip_ << ":" << local_port_ << " failed!" << std::endl;
        ::close(listenfd);
        return -1;
    }
    std::cout << "create and bind socket " << local_ip_ << ":" << local_port_ << " success!" << std::endl;
    return listenfd;
}

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

int32_t EpollTcpServer::Listen(int32_t listenfd) {
    int r = ::listen(listenfd, SOMAXCONN);
    if ( r < 0) {
        std::cout << "listen failed!" << std::endl;
        return -1;
    }
    return 0;
}

int32_t EpollTcpServer::UpdateEpollEvents(int efd, int op, int fd, int events) {
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

void EpollTcpServer::OnSocketAccept() {
    // epoll working on et mode, must read all coming data
    while (true) {
        struct sockaddr_in in_addr;
        socklen_t in_len = sizeof(in_addr);

        int cli_fd = accept(handle_, (struct sockaddr*)&in_addr, &in_len);
        if (cli_fd == -1) {
            if ( (errno == EAGAIN) || (errno == EWOULDBLOCK) ) {
                std::cout << "accept all coming connections!" << std::endl;
                break;
            } else {
                std::cout << "accept error!" << std::endl;
                continue;
            }
        }

        sockaddr_in peer;
        socklen_t p_len = sizeof(peer);
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

        int er = UpdateEpollEvents(efd_, EPOLL_CTL_ADD, cli_fd, EPOLLIN | EPOLLRDHUP | EPOLLET);
        if (er < 0 ) {
            ::close(cli_fd);
            continue;
        }
    }
}

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
    while ( (n = ::read(fd, read_buf, sizeof(read_buf))) > 0) {
        // callback for recv
        std::cout << "fd: " << fd <<  " recv: " << read_buf << std::endl;
        std::string msg(read_buf, n);
        PacketPtr data = std::make_shared<Packet>(fd, msg);
        if (recv_callback_) {
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
void EpollTcpServer::OnSocketWrite(int32_t fd) {
    std::cout << "fd: " << fd << " writeable!" << std::endl;
}

int32_t EpollTcpServer::SendData(const PacketPtr& data) {
    if (data->fd == -1) {
        return -1;
    }
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

void EpollTcpServer::EpollLoop() {
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
    auto epoll_server = std::make_shared<EpollTcpServer>(local_ip, local_port);
    if (!epoll_server) {
        std::cout << "tcp_server create faield!" << std::endl;
        exit(-1);
    }

    auto recv_call = [&](const PacketPtr& data) -> void {
        epoll_server->SendData(data);
        return;
    };

    epoll_server->RegisterOnRecvCallback(recv_call);

    if (!epoll_server->Start()) {
        std::cout << "tcp_server start failed!" << std::endl;
        exit(1);
    }
    std::cout << "############tcp_server started!################" << std::endl;

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    epoll_server->Stop();

    return 0;
}


```

代码看起来有点多，不过仔细分析下，其实也比较容易掌握。

核心的类是 **EpollTcpServer**，创建一个 **EpllTcpServer** 实例：

```
 auto epoll_server = std::make_shared<EpollTcpServer>(local_ip, local_port);
```

注册一个收包处理回调函数：

```

# 这里直接注册一个 echo 函数（可以替换成其他的处理函数）
auto recv_call = [&](const PacketPtr& data) -> void {
    epoll_server->SendData(data);
    return;
};

epoll_server->RegisterOnRecvCallback(recv_call);
```

启动 tcp server:

```
epoll_server->Start();
```

是不是很简单？至于 Start() 函数内部，其实实现的就是 epoll 编程范式的细节。

代码细节应该比较好理解的，可以参考 [https://github.com/smaugx/epoll_examples/blob/master/README.md](https://github.com/smaugx/epoll_examples/blob/master/README.md)


# epoll tcp client

```
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


namespace mux {

namespace transport {

static const uint32_t kEpollWaitTime = 10; // 10 ms
static const uint32_t kMaxEvents = 100;

typedef struct Packet {
public:
    Packet()
        : msg { "" } {}
    Packet(const std::string& msg)
        : msg { msg } {}
    Packet(int fd, const std::string& msg)
        : fd(fd),
          msg(msg) {}

    int fd { -1 };
    std::string msg;
} Packet;

typedef std::shared_ptr<Packet> PacketPtr;

using callback_recv_t = std::function<void(const PacketPtr& data)>;

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



class EpollTcpClient : public ETBase {
public:
    EpollTcpClient()                                       = default;
    EpollTcpClient(const EpollTcpClient& other)            = delete;
    EpollTcpClient& operator=(const EpollTcpClient& other) = delete;
    EpollTcpClient(EpollTcpClient&& other)                 = delete;
    EpollTcpClient& operator=(EpollTcpClient&& other)      = delete;
    ~EpollTcpClient() override;

    EpollTcpClient(const std::string& server_ip, uint16_t server_port);

public:
    bool Start() override;
    bool Stop() override;
    int32_t SendData(const PacketPtr& data) override;
    void RegisterOnRecvCallback(callback_recv_t callback) override;
    void UnRegisterOnRecvCallback() override;

protected:
    int32_t CreateEpoll();
    int32_t CreateSocket();
    int32_t Connect(int32_t listenfd);
    int32_t UpdateEpollEvents(int efd, int op, int fd, int events);
    void OnSocketRead(int32_t fd);
    void OnSocketWrite(int32_t fd);
    void EpollLoop();


private:
    std::string server_ip_;
    uint16_t server_port_ { 0 };
    int32_t handle_ { -1 }; // client fd
    int32_t efd_ { -1 }; // epoll fd
    std::shared_ptr<std::thread> th_loop_ { nullptr };
    bool loop_flag_ { true };
    callback_recv_t recv_callback_ { nullptr };
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
    if (CreateEpoll() < 0) {
        return false;
    }
    // create socket and bind
    int cli_fd  = CreateSocket();
    if (cli_fd < 0) {
        return false;
    }

    int lr = Connect(cli_fd);
    if (lr < 0) {
        return false;
    }
    std::cout << "EpollTcpClient Init success!" << std::endl;
    handle_ = cli_fd;

    int er = UpdateEpollEvents(efd_, EPOLL_CTL_ADD, handle_, EPOLLIN | EPOLLET);
    if (er < 0) {
        ::close(handle_);
        return false;
    }

    assert(!th_loop_);

    th_loop_ = std::make_shared<std::thread>(&EpollTcpClient::EpollLoop, this);
    if (!th_loop_) {
        return false;
    }
    th_loop_->detach();

    return true;
}


bool EpollTcpClient::Stop() {
    loop_flag_ = false;
    ::close(handle_);
    ::close(efd_);
    std::cout << "stop epoll!" << std::endl;
    UnRegisterOnRecvCallback();
    return true;
}

int32_t EpollTcpClient::CreateEpoll() {
    int epollfd = epoll_create(1);
    if (epollfd < 0) {
        std::cout << "epoll_create failed!" << std::endl;
        return -1;
    }
    efd_ = epollfd;
    return epollfd;
}

int32_t EpollTcpClient::CreateSocket() {
    int cli_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (cli_fd < 0) {
        std::cout << "create socket failed!" << std::endl;
        return -1;
    }

    return cli_fd;
}

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

void EpollTcpClient::EpollLoop() {
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

    auto tcp_client = std::make_shared<EpollTcpClient>(server_ip, server_port);
    if (!tcp_client) {
        std::cout << "tcp_client create faield!" << std::endl;
        exit(-1);
    }


    auto recv_call = [&](const transport::PacketPtr& data) -> void {
        std::cout << "recv: " << data->msg << std::endl;
        return;
    };

    tcp_client->RegisterOnRecvCallback(recv_call);

    if (!tcp_client->Start()) {
        std::cout << "tcp_client start failed!" << std::endl;
        exit(1);
    }
    std::cout << "############tcp_client started!################" << std::endl;

    std::string msg;
    while (true) {
        std::cout << std::endl << "input:";
        std::getline(std::cin, msg);
        auto packet = std::make_shared<Packet>(msg);
        tcp_client->SendData(packet);
        //std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    tcp_client->Stop();

    return 0;
}
```

代码和 server 端代码基本上很类似，除了没有 accept() 的处理，这里就不分析了。

# 注意
上面的代码是基于 ET模式（边缘触发模式）实现的。

源代码可以直接在我的 github: [https://github.com/smaugx/epoll_examples](https://github.com/smaugx/epoll_examples) 找到；

或者有兴趣的话也可以直接看我的另外一个项目 [https://github.com/smaugx/mux](https://github.com/smaugx/mux)，基于 epoll 实现的高并发网络库。