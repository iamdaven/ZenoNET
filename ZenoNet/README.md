# ZenoNet

a lightweight networking framework in C/C++ for making multiplayer stuff.

kinda works? most of the time? depends on your definition of "works"

## what is this

a simple TCP networking library. has both C and C++ APIs because i couldnt decide which one to use. server, client, rooms, packets, the whole deal.

built this in like a weekend so the code quality varies wildly between files. some parts are ok, others are held together by string and prayers.

## building

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

## C API

```c
#include "zenonet.h"

void on_connect(Connection *conn, void *userdata) {
    zn_conn_send_data(conn, "{\"msg\":\"hello\"}");
}

int main() {
    Server *srv = zn_server_create(7777);
    zn_server_on_connect(srv, on_connect, NULL);
    zn_server_start(srv);
}
```

## C++ API

```cpp
#include "zenonet.h"

zenonet::Server srv(7777);
srv.on_connect([](zenonet::Connection &conn) {
    conn.send("{\"msg\":\"hello\"}");
});
srv.start();
```

## files

```
ZenoNet/
├── include/zenonet.h
├── src/
│   ├── packet.c
│   ├── connection.c
│   ├── room.c
│   ├── server.c
│   ├── client.c
│   └── zenonet_cpp.cpp
├── examples/
│   ├── chat_example.c
│   └── game_example.cpp
└── CMakeLists.txt
```

## stuff that needs fixing

- the server read loop loses the server reference (oops)
- json parsing is hand-written garbage
- no SSL/TLS
- packet buffer handling is sketchy
- probably has memory leaks somewhere
- threading is questionable
- the C++ API is incomplete
- no tests (lol)

## license

MIT