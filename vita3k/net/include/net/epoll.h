#pragma once

#include <net/socket.h>

struct EpollSocket {
    unsigned int events;
    SceNetEpollData data;
    std::weak_ptr<Socket> sock;
};

struct Epoll {
    std::map<int, EpollSocket> eventEntries;

    int add(int id, std::weak_ptr<Socket> sock, SceNetEpollEvent *ev);
    int del(int id);
    int mod(int id, SceNetEpollEvent *ev);
    int wait(SceNetEpollEvent *events, int maxevents, int timeout);

private:
    int wait_host(std::map<int, unsigned int> &ready, int timeout_microseconds);
};

using EpollPtr = std::shared_ptr<Epoll>;
