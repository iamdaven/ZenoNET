# TODO

stuff i should probably do but probably wont

## high priority (not really)

- [ ] fix the server read loop so it doesnt lose the server reference
- [ ] actually implement packet reading in the server instead of just accepting connections
- [ ] add a real json parser or at least make the hand-written one not crash on malformed input
- [ ] proper buffer handling in the read loops (right now it assumes one recv = one packet)
- [ ] memory leaks. definitely memory leaks.

## medium priority (maybe)

- [ ] client reconnect logic
- [ ] room list packet type
- [ ] server stats endpoint
- [ ] rate limiting (so one client cant spam 10000 packets per second)
- [ ] connection timeout / heartbeat

## low priority (never gonna happen)

- [ ] ssl/tls support
- [ ] udp support for games that need it
- [ ] webrtc or something idk
- [ ] actually good documentation
- [ ] unit tests that test more than just constructors
- [ ] benchmarks
- [ ] zig bindings because why not
- [ ] javascript/wasm bindings
- [ ] a GUI admin panel
- [ ] docker image
- [ ] kubernetes operator lmao

## known bugs

- server.start() is a blocking call (whoops)
- the C++ Server::read_loop deletes the connection on disconnect but the erase might invalidate iterators somewhere
- packet deserializer will crash if you send it random bytes (ask me how i know)
- winsock ref counting is probably wrong
- the Makefile doesn't actually work on Windows (just use cmake)
- C++ Client passes *this to connect callbacks which is sketchy
- if you join a room thats full the error message gets lost somewhere