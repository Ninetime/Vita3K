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

// SCE_NET_SOCK_DGRAM_P2P / SCE_NET_SOCK_STREAM_P2P sockets over the RPCN P2P port.
//
// P2P addresses use SceNetSockaddrIn with the same meaning as on PS3 (checked in SFxT, which
// builds them the same way on both consoles):
// - DGRAM_P2P: sin_port = UDP port of the peer's P2P socket, sin_vport = virtual port;
// - STREAM_P2P: inverted, sin_port = virtual port, sin_vport = UDP port.

#include <net/socket.h>

#include <rpcn/vport.h>
#include <util/log.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace {

int to_sce_error(int err) {
    switch (err) {
    case rpcn::VPORT_EWOULDBLOCK: return SCE_NET_ERROR_EWOULDBLOCK;
    case rpcn::VPORT_ENOTCONN: return SCE_NET_ERROR_ENOTCONN;
    case rpcn::VPORT_EADDRINUSE: return SCE_NET_ERROR_EADDRINUSE;
    case rpcn::VPORT_EALREADY: return SCE_NET_ERROR_EALREADY;
    case rpcn::VPORT_EINVAL: return SCE_NET_ERROR_EINVAL;
    case rpcn::VPORT_ECONNRESET: return SCE_NET_ERROR_ECONNRESET;
    case rpcn::VPORT_ENETDOWN: return SCE_NET_ERROR_ENETDOWN;
    case rpcn::VPORT_EADDRNOTAVAIL: return SCE_NET_ERROR_EADDRNOTAVAIL;
    case rpcn::VPORT_EMSGSIZE: return SCE_NET_ERROR_EMSGSIZE;
    case rpcn::VPORT_EISCONN: return SCE_NET_ERROR_EISCONN;
    default: return err;
    }
}

uint16_t swap16(uint16_t v) {
    return static_cast<uint16_t>((v >> 8) | (v << 8));
}

rpcn::VportAddr from_sce(const SceNetSockaddr *addr, bool stream) {
    SceNetSockaddrIn in;
    memcpy(&in, addr, sizeof(in));
    rpcn::VportAddr out;
    out.addr = in.sin_addr.s_addr;
    const uint16_t port = swap16(in.sin_port);
    const uint16_t vport = swap16(in.sin_vport);
    out.port = stream ? vport : port;
    out.vport = stream ? port : vport;
    return out;
}

void to_sce(const rpcn::VportAddr &in, bool stream, SceNetSockaddr *addr, unsigned int *addrlen) {
    if (!addr)
        return;
    SceNetSockaddrIn out{};
    out.sin_len = sizeof(out);
    out.sin_family = SCE_NET_AF_INET;
    out.sin_addr.s_addr = in.addr;
    out.sin_port = swap16(stream ? in.vport : in.port);
    out.sin_vport = swap16(stream ? in.port : in.vport);
    memcpy(addr, &out, sizeof(out));
    if (addrlen)
        *addrlen = sizeof(out);
}

rpcn::VportHub &hub() {
    return rpcn::VportHub::get();
}

} // namespace

RpcnP2PSocket::RpcnP2PSocket(int type)
    : Socket(SCE_NET_AF_INET, type, 0)
    , vsock(hub().create(type == SCE_NET_SOCK_STREAM_P2P)) {
}

RpcnP2PSocket::RpcnP2PSocket(std::shared_ptr<rpcn::VportSocket> accepted)
    : Socket(SCE_NET_AF_INET, SCE_NET_SOCK_STREAM_P2P, 0)
    , vsock(std::move(accepted)) {
}

RpcnP2PSocket::~RpcnP2PSocket() {
    hub().close(vsock);
}

int RpcnP2PSocket::abort(int flags) {
    aborted = true;
    hub().wake_all();
    return 0;
}

int RpcnP2PSocket::close() {
    aborted = true;
    hub().close(vsock);
    return 0;
}

int RpcnP2PSocket::shutdown_socket(int how) {
    return 0;
}

int RpcnP2PSocket::bind(const SceNetSockaddr *addr, unsigned int addrlen) {
    if (!addr || addrlen < sizeof(SceNetSockaddrIn))
        return SCE_NET_ERROR_EINVAL;
    const bool stream = sce_type == SCE_NET_SOCK_STREAM_P2P;
    const auto a = from_sce(addr, stream);
    return to_sce_error(hub().bind(vsock, a.vport));
}

int RpcnP2PSocket::listen(int backlog) {
    if (sce_type != SCE_NET_SOCK_STREAM_P2P)
        return SCE_NET_ERROR_EOPNOTSUPP;
    return to_sce_error(hub().listen(vsock, backlog));
}

int RpcnP2PSocket::connect(const SceNetSockaddr *addr, unsigned int addrlen) {
    if (sce_type != SCE_NET_SOCK_STREAM_P2P)
        return SCE_NET_ERROR_EOPNOTSUPP;
    if (!addr || addrlen < sizeof(SceNetSockaddrIn))
        return SCE_NET_ERROR_EINVAL;
    return to_sce_error(hub().connect(vsock, from_sce(addr, true)));
}

SocketPtr RpcnP2PSocket::accept(SceNetSockaddr *addr, unsigned int *addrlen, int &err) {
    const auto timeout = rcvtimeo ? std::chrono::microseconds(rcvtimeo) : std::chrono::hours(24 * 365);
    while (true) {
        rpcn::VportAddr from;
        int res = 0;
        auto client = hub().accept(vsock, &from, res);
        if (client) {
            to_sce(from, true, addr, addrlen);
            auto sock = std::make_shared<RpcnP2PSocket>(std::move(client));
            sock->nbio = nbio;
            return sock;
        }
        if (res != rpcn::VPORT_EWOULDBLOCK || nbio) {
            err = to_sce_error(res);
            return nullptr;
        }
        if (!hub().wait(vsock, rpcn::VPORT_POLL_IN | rpcn::VPORT_POLL_HUP, timeout, &aborted)) {
            err = aborted.exchange(false) ? SCE_NET_ERROR_EINTR : SCE_NET_ERROR_EWOULDBLOCK;
            return nullptr;
        }
    }
}

int RpcnP2PSocket::send_packet(const void *msg, unsigned int len, int flags, const SceNetSockaddr *to, unsigned int tolen) {
    const bool stream = sce_type == SCE_NET_SOCK_STREAM_P2P;
    if (!msg && len)
        return SCE_NET_ERROR_EFAULT;
    if (!stream) {
        if (!to || tolen < sizeof(SceNetSockaddrIn))
            return SCE_NET_ERROR_EDESTADDRREQ;
        const auto dst = from_sce(to, false);
        return to_sce_error(hub().send(vsock, static_cast<const uint8_t *>(msg), len, &dst));
    }
    // A stream still connecting accepts data, as in RPCS3
    return to_sce_error(hub().send(vsock, static_cast<const uint8_t *>(msg), len, nullptr));
}

int RpcnP2PSocket::recv_packet(void *buf, unsigned int len, int flags, SceNetSockaddr *from, unsigned int *fromlen) {
    const bool stream = sce_type == SCE_NET_SOCK_STREAM_P2P;
    const bool dont_wait = nbio || (flags & SCE_NET_MSG_DONTWAIT);
    const bool peek = flags & SCE_NET_MSG_PEEK;
    const auto timeout = rcvtimeo ? std::chrono::microseconds(rcvtimeo) : std::chrono::hours(24 * 365);
    while (true) {
        rpcn::VportAddr addr;
        const int res = hub().recv(vsock, static_cast<uint8_t *>(buf), len, &addr, peek);
        if (res >= 0) {
            to_sce(addr, stream, from, fromlen);
            return res;
        }
        if (res != rpcn::VPORT_EWOULDBLOCK || dont_wait)
            return to_sce_error(res);
        if (!hub().wait(vsock, rpcn::VPORT_POLL_IN | rpcn::VPORT_POLL_HUP, timeout, &aborted))
            return aborted.exchange(false) ? SCE_NET_ERROR_EINTR : SCE_NET_ERROR_EWOULDBLOCK;
    }
}

int RpcnP2PSocket::set_socket_options(int level, int optname, const void *optval, unsigned int optlen) {
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
        case SCE_NET_SO_RCVBUF:
            rcvbuf = value;
            hub().set_rcvbuf(vsock, value);
            return 0;
        case SCE_NET_SO_SNDBUF:
            sndbuf = value;
            return 0;
        case SCE_NET_SO_RCVTIMEO:
            rcvtimeo = value;
            return 0;
        case SCE_NET_SO_SNDTIMEO:
            sndtimeo = value;
            return 0;
        case SCE_NET_SO_REUSEADDR:
        case SCE_NET_SO_REUSEPORT:
            reuseaddr = value;
            return 0;
        case SCE_NET_SO_BROADCAST:
            broadcast = value;
            return 0;
        case SCE_NET_SO_ONESBCAST:
            sockopt_so_onesbcast = value;
            return 0;
        case SCE_NET_SO_TYPE:
        case SCE_NET_SO_ERROR:
            return SCE_NET_ERROR_ENOPROTOOPT;
        default:
            break;
        }
    }
    // Other options (TCP_NODELAY on streams...) have no effect on emulated P2P sockets
    const auto *bytes = static_cast<const uint8_t *>(optval);
    other_options[(static_cast<uint64_t>(level) << 32) | static_cast<uint32_t>(optname)] = std::vector<uint8_t>(bytes, bytes + optlen);
    return 0;
}

int RpcnP2PSocket::get_socket_options(int level, int optname, void *optval, unsigned int *optlen) {
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
        case SCE_NET_SO_RCVBUF: return give_int(rcvbuf);
        case SCE_NET_SO_SNDBUF: return give_int(sndbuf);
        case SCE_NET_SO_RCVTIMEO: return give_int(rcvtimeo);
        case SCE_NET_SO_SNDTIMEO: return give_int(sndtimeo);
        case SCE_NET_SO_REUSEADDR:
        case SCE_NET_SO_REUSEPORT: return give_int(reuseaddr);
        case SCE_NET_SO_BROADCAST: return give_int(broadcast);
        case SCE_NET_SO_TYPE: return give_int(sce_type);
        case SCE_NET_SO_ERROR: return give_int(std::exchange(last_error, 0));
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

int RpcnP2PSocket::get_peer_address(SceNetSockaddr *addr, unsigned int *addrlen) {
    rpcn::VportAddr peer;
    const int res = hub().peer_addr(vsock, peer);
    if (res < 0)
        return to_sce_error(res);
    to_sce(peer, sce_type == SCE_NET_SOCK_STREAM_P2P, addr, addrlen);
    return 0;
}

int RpcnP2PSocket::get_socket_address(SceNetSockaddr *addr, unsigned int *addrlen) {
    rpcn::VportAddr local;
    hub().local_addr(vsock, local);
    to_sce(local, sce_type == SCE_NET_SOCK_STREAM_P2P, addr, addrlen);
    return 0;
}

int RpcnP2PSocket::poll_events() {
    const unsigned ready = hub().poll(vsock);
    int events = 0;
    if (ready & rpcn::VPORT_POLL_IN)
        events |= SCE_NET_EPOLLIN;
    if (ready & rpcn::VPORT_POLL_OUT)
        events |= SCE_NET_EPOLLOUT;
    if (ready & rpcn::VPORT_POLL_HUP)
        events |= SCE_NET_EPOLLERR;
    return events;
}
