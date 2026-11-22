// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <net/epoll.h>

#include <algorithm>
#include <chrono>
#include <optional>

int Epoll::add(int id, std::weak_ptr<Socket> sock, SceNetEpollEvent *ev) {
    if (!eventEntries.try_emplace(id, EpollSocket{ ev->events, ev->data, sock }).second) {
        return SCE_NET_ERROR_EEXIST;
    }

    return 0;
}

int Epoll::del(int id) {
    if (eventEntries.erase(id) == 0) {
        return SCE_NET_ERROR_ENOENT;
    }

    return 0;
}

int Epoll::mod(int id, SceNetEpollEvent *ev) {
    auto it = eventEntries.find(id);
    if (it == eventEntries.end()) {
        return SCE_NET_ERROR_ENOENT;
    }

    it->second.events = ev->events;
    it->second.data = ev->data;
    return 0;
}

static void add_event_fd_set(fd_set *set, int *maxFd, abs_socket sock) {
    FD_SET(sock, set);
    if (sock > *maxFd)
        *maxFd = sock;
}

// Checks the sockets that have no host descriptor (RPCN P2P sockets)
static int poll_virtual(const std::map<int, EpollSocket> &entries, std::map<int, unsigned int> &ready) {
    int count = 0;
    for (const auto &[id, entry] : entries) {
        const auto sock = entry.sock.lock();
        if (!sock)
            continue;
        const int events = sock->poll_events();
        if (events < 0)
            continue;
        count++;
        const unsigned int pending = static_cast<unsigned int>(events) & (entry.events | SCE_NET_EPOLLERR);
        if (pending)
            ready[id] |= pending;
    }
    return count;
}

int Epoll::wait(SceNetEpollEvent *events, int maxevents, int timeout_microseconds) {
    std::map<int, unsigned int> ready;
    const bool has_virtual = poll_virtual(eventEntries, ready) > 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(std::max(timeout_microseconds, 0));

    while (true) {
        int slice = timeout_microseconds;
        if (has_virtual) {
            // Poll the P2P sockets every millisecond while waiting on the host ones
            const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now()).count();
            slice = ready.empty() ? static_cast<int>(std::clamp<long long>(timeout_microseconds < 0 ? 1000 : left, 0, 1000)) : 0;
        }
        const int ret = wait_host(ready, slice);
        if (ret < 0)
            return ret;
        if (has_virtual)
            poll_virtual(eventEntries, ready);
        if (!ready.empty() || !has_virtual)
            break;
        if (timeout_microseconds >= 0 && std::chrono::steady_clock::now() >= deadline)
            break;
    }

    int eventCount = 0;
    for (const auto &[id, eventTypes] : ready) {
        if (eventCount >= maxevents)
            break;
        events[eventCount].events = eventTypes;
        events[eventCount].data = eventEntries.at(id).data;
        eventCount++;
    }
    return eventCount;
}

int Epoll::wait_host(std::map<int, unsigned int> &ready, int timeout_microseconds) {
    fd_set readFds, writeFds, exceptFds;
    FD_ZERO(&readFds);
    FD_ZERO(&writeFds);
    FD_ZERO(&exceptFds);
    int maxFd = 0;

    const auto get_valid_posix_socket = [](const std::weak_ptr<Socket> &weak_sock, int id) -> std::optional<abs_socket> {
        const auto sock = weak_sock.lock();
        if (!sock)
            return std::nullopt;

        const auto posixSocket = std::dynamic_pointer_cast<PosixSocket>(sock);
        if (!posixSocket)
            return std::nullopt;

        return posixSocket->sock;
    };

    for (const auto &[id, entry] : eventEntries) {
        const auto sock = get_valid_posix_socket(entry.sock, id);
        if (!sock)
            continue;

        if (entry.events & SCE_NET_EPOLLIN)
            add_event_fd_set(&readFds, &maxFd, *sock);
        if (entry.events & SCE_NET_EPOLLOUT)
            add_event_fd_set(&writeFds, &maxFd, *sock);
        if (entry.events & SCE_NET_EPOLLERR)
            add_event_fd_set(&exceptFds, &maxFd, *sock);
    }

    if (maxFd == 0)
        return 0;

    timeval timeout;
    timeout.tv_sec = timeout_microseconds / 1000000;
    timeout.tv_usec = timeout_microseconds % 1000000;
    auto ret = select(maxFd + 1, &readFds, &writeFds, &exceptFds, &timeout);
    if (ret < 0)
        return PosixSocket::translate_return_value(ret);

    for (const auto &[id, entry] : eventEntries) {
        unsigned int eventTypes = 0;
        const auto sock = get_valid_posix_socket(entry.sock, id);
        if (!sock)
            continue;

        if (FD_ISSET(*sock, &readFds))
            eventTypes |= SCE_NET_EPOLLIN;
        if (FD_ISSET(*sock, &writeFds))
            eventTypes |= SCE_NET_EPOLLOUT;
        if (FD_ISSET(*sock, &exceptFds))
            eventTypes |= SCE_NET_EPOLLERR;

        if (eventTypes != 0)
            ready[id] |= eventTypes;
    }

    return 0;
}
