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

// ICMP echo emulation for SCE_NET_SOCK_RAW / SCE_NET_IPPROTO_ICMP sockets.
//
// A real raw socket needs administrator rights on the host. Games only use these sockets to ping
// (e.g. SFxT measures its connection quality by pinging the default route), so echo requests are
// sent with the host's ICMP API instead, and the replies are returned the way a BSD raw socket
// returns them: IPv4 header followed by the ICMP echo reply, with the game's identifier,
// sequence number and data.

#include <net/socket.h>

#include <util/log.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>

#ifdef _WIN32
#include <icmpapi.h>
#endif

namespace {

constexpr uint8_t ICMP_TYPE_ECHO_REPLY = 0;
constexpr uint8_t ICMP_TYPE_ECHO_REQUEST = 8;
constexpr uint32_t ECHO_TIMEOUT_MS = 1000;

uint16_t internet_checksum(const uint8_t *data, size_t size) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < size; i += 2)
        sum += (data[i] << 8) | data[i + 1];
    if (size & 1)
        sum += data[size - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return static_cast<uint16_t>(~sum);
}

// IPv4 header + echo reply for the request the game sent to addr (network order)
std::vector<uint8_t> make_reply(uint32_t addr, const std::vector<uint8_t> &request) {
    std::vector<uint8_t> packet(20 + request.size());
    packet[0] = 0x45; // Version 4, 20 byte header
    packet[2] = static_cast<uint8_t>(packet.size() >> 8);
    packet[3] = static_cast<uint8_t>(packet.size());
    packet[8] = 64; // TTL
    packet[9] = SCE_NET_IPPROTO_ICMP;
    memcpy(&packet[12], &addr, 4); // Source: the pinged host
    const uint16_t ip_checksum = internet_checksum(packet.data(), 20);
    packet[10] = static_cast<uint8_t>(ip_checksum >> 8);
    packet[11] = static_cast<uint8_t>(ip_checksum);

    uint8_t *icmp = packet.data() + 20;
    memcpy(icmp, request.data(), request.size());
    icmp[0] = ICMP_TYPE_ECHO_REPLY;
    icmp[1] = 0;
    icmp[2] = icmp[3] = 0;
    const uint16_t icmp_checksum = internet_checksum(icmp, request.size());
    icmp[2] = static_cast<uint8_t>(icmp_checksum >> 8);
    icmp[3] = static_cast<uint8_t>(icmp_checksum);
    return packet;
}

} // namespace

struct IcmpSocket::Shared {
    struct Echo {
        Shared *owner = nullptr;
        uint32_t addr = 0;
        std::vector<uint8_t> request;
        std::atomic<bool> finished{ false }; // The completion callback no longer uses this request
#ifdef _WIN32
        HANDLE event = nullptr;
        HANDLE wait = nullptr;
        std::vector<uint8_t> reply_buf;

        ~Echo() {
            if (wait)
                UnregisterWaitEx(wait, INVALID_HANDLE_VALUE); // Waits for a running callback
            if (event) {
                WaitForSingleObject(event, ECHO_TIMEOUT_MS + 500); // The reply buffer must outlive the request
                CloseHandle(event);
            }
        }

        static void CALLBACK on_complete(void *ctx, BOOLEAN) {
            auto *echo = static_cast<Echo *>(ctx);
            Shared &shared = *echo->owner;
            {
                std::lock_guard lock(shared.mutex);
                if (IcmpParseReplies(echo->reply_buf.data(), static_cast<DWORD>(echo->reply_buf.size())) != 0) {
                    const auto *reply = reinterpret_cast<const ICMP_ECHO_REPLY *>(echo->reply_buf.data());
                    if (reply->Status == IP_SUCCESS) {
                        LOG_TRACE("ICMP: echo reply in {} ms", reply->RoundTripTime);
                        shared.replies.emplace_back(make_reply(echo->addr, echo->request), echo->addr);
                    }
                }
            }
            shared.cv.notify_all();
            echo->finished = true;
        }
#endif
    };

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::pair<std::vector<uint8_t>, uint32_t>> replies; // Packet, source address
    std::vector<std::unique_ptr<Echo>> pending;
    bool aborted = false;
#ifdef _WIN32
    HANDLE icmp = INVALID_HANDLE_VALUE;
#endif

    ~Shared() {
        pending.clear();
#ifdef _WIN32
        if (icmp != INVALID_HANDLE_VALUE)
            IcmpCloseHandle(icmp);
#endif
    }

    // Called with mutex held, never from a completion callback
    void release_finished() {
        std::erase_if(pending, [](const std::unique_ptr<Echo> &echo) { return echo->finished.load(); });
    }
};

IcmpSocket::IcmpSocket(int domain, int type, int protocol)
    : Socket(domain, type, protocol)
    , shared(std::make_shared<Shared>()) {}

IcmpSocket::~IcmpSocket() = default;

int IcmpSocket::abort(int flags) {
    {
        std::lock_guard lock(shared->mutex);
        shared->aborted = true;
    }
    shared->cv.notify_all();
    return 0;
}

int IcmpSocket::close() {
    return abort(0);
}

int IcmpSocket::shutdown_socket(int how) {
    return 0;
}

int IcmpSocket::bind(const SceNetSockaddr *addr, unsigned int addrlen) {
    return 0;
}

int IcmpSocket::send_packet(const void *msg, unsigned int len, int flags, const SceNetSockaddr *to, unsigned int tolen) {
    if (!msg)
        return SCE_NET_ERROR_EFAULT;
    if (!to || tolen < sizeof(SceNetSockaddrIn))
        return SCE_NET_ERROR_EDESTADDRREQ;

    const auto *data = static_cast<const uint8_t *>(msg);
    if (len < 8 || data[0] != ICMP_TYPE_ECHO_REQUEST) {
        LOG_WARN_ONCE("ICMP: only echo requests are emulated (type {})", len ? data[0] : -1);
        return static_cast<int>(len);
    }

    SceNetSockaddrIn sin;
    memcpy(&sin, to, sizeof(sin));
    const uint32_t addr = sin.sin_addr.s_addr;
    std::vector<uint8_t> request(data, data + len);

    std::lock_guard lock(shared->mutex);
    shared->release_finished();

#ifdef _WIN32
    if (shared->icmp == INVALID_HANDLE_VALUE) {
        shared->icmp = IcmpCreateFile();
        if (shared->icmp == INVALID_HANDLE_VALUE) {
            LOG_ERROR("ICMP: IcmpCreateFile failed: {}", GetLastError());
            return static_cast<int>(len);
        }
    }

    auto echo = std::make_unique<Shared::Echo>();
    echo->owner = shared.get();
    echo->addr = addr;
    echo->request = std::move(request);
    echo->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    echo->reply_buf.resize(sizeof(ICMP_ECHO_REPLY) + len + 8 + 64);

    // The host builds its own ICMP header: the game's identifier and sequence number are restored in the reply
    const DWORD res = IcmpSendEcho2(shared->icmp, echo->event, nullptr, nullptr, addr, const_cast<uint8_t *>(echo->request.data()) + 8,
        static_cast<WORD>(len - 8), nullptr, echo->reply_buf.data(), static_cast<DWORD>(echo->reply_buf.size()), ECHO_TIMEOUT_MS);
    if (res == 0 && GetLastError() != ERROR_IO_PENDING) {
        LOG_TRACE("ICMP: IcmpSendEcho2 failed: {}", GetLastError());
        return static_cast<int>(len);
    }

    // The callback needs the mutex held here, so it runs after this returns
    if (!RegisterWaitForSingleObject(&echo->wait, echo->event, &Shared::Echo::on_complete, echo.get(), INFINITE, WT_EXECUTEONLYONCE)) {
        LOG_ERROR("ICMP: RegisterWaitForSingleObject failed: {}", GetLastError());
        echo->wait = nullptr;
        echo->finished = true;
    }
    shared->pending.push_back(std::move(echo));
#else
    // No unprivileged ICMP API is used here: answer immediately so the game still gets a latency sample
    LOG_WARN_ONCE("ICMP: echo replies are simulated on this platform");
    shared->replies.emplace_back(make_reply(addr, request), addr);
    shared->cv.notify_all();
#endif
    return static_cast<int>(len);
}

int IcmpSocket::recv_packet(void *buf, unsigned int len, int flags, SceNetSockaddr *from, unsigned int *fromlen) {
    if (!buf)
        return SCE_NET_ERROR_EFAULT;
    const bool dont_wait = nbio || (flags & SCE_NET_MSG_DONTWAIT);
    const auto deadline = std::chrono::steady_clock::now() + (rcvtimeo ? std::chrono::microseconds(rcvtimeo) : std::chrono::hours(24 * 365));

    std::unique_lock lock(shared->mutex);
    shared->release_finished();
    while (shared->replies.empty()) {
        if (dont_wait)
            return SCE_NET_ERROR_EWOULDBLOCK;
        if (shared->aborted) {
            shared->aborted = false;
            return SCE_NET_ERROR_EINTR;
        }
        if (shared->cv.wait_until(lock, deadline) == std::cv_status::timeout && shared->replies.empty())
            return SCE_NET_ERROR_EWOULDBLOCK;
    }

    const auto [packet, addr] = std::move(shared->replies.front());
    shared->replies.pop_front();

    const unsigned int size = std::min<unsigned int>(len, static_cast<unsigned int>(packet.size()));
    memcpy(buf, packet.data(), size);
    if (from) {
        SceNetSockaddrIn sin{};
        sin.sin_len = sizeof(sin);
        sin.sin_family = SCE_NET_AF_INET;
        sin.sin_addr.s_addr = addr;
        memcpy(from, &sin, sizeof(sin));
        if (fromlen)
            *fromlen = sizeof(sin);
    }
    return static_cast<int>(size);
}

int IcmpSocket::set_socket_options(int level, int optname, const void *optval, unsigned int optlen) {
    if (!optval)
        return SCE_NET_ERROR_EFAULT;
    int value = 0;
    if (optlen >= sizeof(int))
        memcpy(&value, optval, sizeof(int));
    if (level == SCE_NET_SOL_SOCKET) {
        switch (optname) {
        case SCE_NET_SO_NBIO:
            nbio = value;
            return 0;
        case SCE_NET_SO_RCVTIMEO:
            rcvtimeo = value;
            return 0;
        default: break;
        }
    }
    const auto *bytes = static_cast<const uint8_t *>(optval);
    other_options[(static_cast<uint64_t>(level) << 32) | static_cast<uint32_t>(optname)] = std::vector<uint8_t>(bytes, bytes + optlen);
    return 0;
}

int IcmpSocket::get_socket_options(int level, int optname, void *optval, unsigned int *optlen) {
    if (!optval || !optlen)
        return SCE_NET_ERROR_EFAULT;
    const auto give_int = [&](int value) -> int {
        if (*optlen < sizeof(int))
            return SCE_NET_ERROR_EINVAL;
        memcpy(optval, &value, sizeof(int));
        *optlen = sizeof(int);
        return 0;
    };
    if (level == SCE_NET_SOL_SOCKET) {
        switch (optname) {
        case SCE_NET_SO_NBIO: return give_int(nbio);
        case SCE_NET_SO_RCVTIMEO: return give_int(rcvtimeo);
        case SCE_NET_SO_TYPE: return give_int(sce_type);
        case SCE_NET_SO_ERROR: return give_int(0);
        default: break;
        }
    }
    const auto it = other_options.find((static_cast<uint64_t>(level) << 32) | static_cast<uint32_t>(optname));
    if (it == other_options.end())
        return SCE_NET_ERROR_ENOPROTOOPT;
    const unsigned int n = std::min<unsigned int>(*optlen, static_cast<unsigned int>(it->second.size()));
    memcpy(optval, it->second.data(), n);
    *optlen = n;
    return 0;
}

int IcmpSocket::connect(const SceNetSockaddr *addr, unsigned int addrlen) {
    return SCE_NET_ERROR_EOPNOTSUPP;
}

SocketPtr IcmpSocket::accept(SceNetSockaddr *addr, unsigned int *addrlen, int &err) {
    err = SCE_NET_ERROR_EOPNOTSUPP;
    return nullptr;
}

int IcmpSocket::listen(int backlog) {
    return SCE_NET_ERROR_EOPNOTSUPP;
}

int IcmpSocket::get_peer_address(SceNetSockaddr *addr, unsigned int *addrlen) {
    return SCE_NET_ERROR_EOPNOTSUPP;
}

int IcmpSocket::get_socket_address(SceNetSockaddr *addr, unsigned int *addrlen) {
    if (!addr)
        return SCE_NET_ERROR_EFAULT;
    SceNetSockaddrIn sin{};
    sin.sin_len = sizeof(sin);
    sin.sin_family = SCE_NET_AF_INET;
    memcpy(addr, &sin, sizeof(sin));
    if (addrlen)
        *addrlen = sizeof(sin);
    return 0;
}

int IcmpSocket::poll_events() {
    std::lock_guard lock(shared->mutex);
    shared->release_finished();
    return (shared->replies.empty() ? 0 : SCE_NET_EPOLLIN) | SCE_NET_EPOLLOUT;
}
