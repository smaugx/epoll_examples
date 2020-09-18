# epoll_examples
epoll examples of echo-server and client.

# build
```
g++ epoll_client.cc  -o epoll_client -std=c++11 -lpthread
```

```
g++ epoll_server.cc  -o epoll_server -std=c++11 -lpthread
```

# run

start echo server first:

```
./epoll_server
```

and then open a new terminal ,and start client:

```
./epoll_client
```

