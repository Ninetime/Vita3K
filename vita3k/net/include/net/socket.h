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

#pragma once

#include <net/types.h>

#include <atomic>
#include <map>
#include <memory>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <Ws2tcpip.h>
#include <iphlpapi.h>
#include <winsock2.h>
#undef s_addr
using abs_socket = SOCKET;
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
using abs_socket = int;
#endif

struct Socket;

using SocketPtr = std::shared_ptr<Socket>;

struct Socket {
    int sockopt_so_onesbcast = 0;
    int sce_type;

    explicit Socket(int domain, int type, int protocol)
        : sce_type(type) {}

    virtual ~Socket() = default;

    virtual int abort(int flags) = 0;
    virtual int close() = 0;
    virtual int shutdown_socket(int how) = 0;
    virtual int bind(const SceNetSockaddr *addr, unsigned int addrlen) = 0;
    virtual int send_packet(const void *msg, unsigned int len, int flags, const SceNetSockaddr *to, unsigned int tolen) = 0;
    virtual int recv_packet(void *buf, unsigned int len, int flags, SceNetSockaddr *from, unsigned int *fromlen) = 0;
    virtual int set_socket_options(int level, int optname, const void *optval, unsigned int optlen) = 0;
    virtual int get_socket_options(int level, int optname, void *optval, unsigned int *optlen) = 0;
    virtual int connect(const SceNetSockaddr *addr, unsigned int addrlen) = 0;
    virtual SocketPtr accept(SceNetSockaddr *addr, unsigned int *addrlen, int &err) = 0;
    virtual int listen(int backlog) = 0;
    virtual int get_peer_address(SceNetSockaddr *addr, unsigned int *addrlen) = 0;
    virtual int get_socket_address(SceNetSockaddr *addr, unsigned int *addrlen) = 0;
    // SCE_NET_EPOLL* events pending on sockets that have no host descriptor, -1 otherwise
    virtual int poll_events() { return -1; }
};

// udp, tcp
struct PosixSocket : public Socket {
    abs_socket sock;

    int sockopt_so_reuseport = 0;
    int sockopt_so_usecrypto = 0;
    int sockopt_so_usesignature = 0;
    int sockopt_so_tppolicy = 0;
    int sockopt_so_nbio = 0;
    int sockopt_ip_ttlchk = 0;
    int sockopt_ip_maxttl = 0;
    int sockopt_tcp_mss_to_advertise = 0;

    int abort_flags = 0;
    bool is_aborted = false;

    explicit PosixSocket(int domain, int type, int protocol)
        : Socket(domain, type, protocol)
        , sock(socket(domain, type, protocol)) {}

    explicit PosixSocket(abs_socket sock, int type)
        : Socket(0, type, 0)
        , sock(sock) {}

    static int translate_return_value(int retval);

    int abort(int flags) override;
    int close() override;
    int shutdown_socket(int how) override;
    int bind(const SceNetSockaddr *addr, unsigned int addrlen) override;
    int send_packet(const void *msg, unsigned int len, int flags, const SceNetSockaddr *to, unsigned int tolen) override;
    int recv_packet(void *buf, unsigned int len, int flags, SceNetSockaddr *from, unsigned int *fromlen) override;
    int set_socket_options(int level, int optname, const void *optval, unsigned int optlen) override;
    int get_socket_options(int level, int optname, void *optval, unsigned int *optlen) override;
    int connect(const SceNetSockaddr *addr, unsigned int addrlen) override;
    SocketPtr accept(SceNetSockaddr *addr, unsigned int *addrlen, int &err) override;
    int listen(int backlog) override;
    int get_peer_address(SceNetSockaddr *addr, unsigned int *addrlen) override;
    int get_socket_address(SceNetSockaddr *addr, unsigned int *addrlen) override;
};

struct P2PSocket : public PosixSocket {
    explicit P2PSocket(int domain, int type, int protocol);
    explicit P2PSocket(abs_socket sock, int type)
        : PosixSocket(sock, type) {}

    int bind(const SceNetSockaddr *addr, unsigned int addrlen) override;
    int send_packet(const void *msg, unsigned int len, int flags, const SceNetSockaddr *to, unsigned int tolen) override;
    int recv_packet(void *buf, unsigned int len, int flags, SceNetSockaddr *from, unsigned int *fromlen) override;
    int connect(const SceNetSockaddr *addr, unsigned int addrlen) override;
    SocketPtr accept(SceNetSockaddr *addr, unsigned int *addrlen, int &err) override;
    int get_peer_address(SceNetSockaddr *addr, unsigned int *addrlen) override;
    int get_socket_address(SceNetSockaddr *addr, unsigned int *addrlen) override;
};

namespace rpcn {
struct VportSocket;
}

// P2P socket carried over the RPCN P2P port, compatible with RPCS3
// SCE_NET_SOCK_RAW / SCE_NET_IPPROTO_ICMP: ICMP echo (ping) emulation without raw socket privileges.
// Games use it to measure the connection quality (e.g. SFxT pings the default route).
// Echo requests are sent with the host's ICMP API; replies are returned with an IPv4 header, like a raw socket.
struct IcmpSocket : public Socket {
    struct Shared;
    std::shared_ptr<Shared> shared;
    int nbio = 0;
    int rcvtimeo = 0; // microseconds, 0 = forever
    std::map<uint64_t, std::vector<uint8_t>> other_options;

    IcmpSocket(int domain, int type, int protocol);
    ~IcmpSocket() override;

    int abort(int flags) override;
    int close() override;
    int shutdown_socket(int how) override;
    int bind(const SceNetSockaddr *addr, unsigned int addrlen) override;
    int send_packet(const void *msg, unsigned int len, int flags, const SceNetSockaddr *to, unsigned int tolen) override;
    int recv_packet(void *buf, unsigned int len, int flags, SceNetSockaddr *from, unsigned int *fromlen) override;
    int set_socket_options(int level, int optname, const void *optval, unsigned int optlen) override;
    int get_socket_options(int level, int optname, void *optval, unsigned int *optlen) override;
    int connect(const SceNetSockaddr *addr, unsigned int addrlen) override;
    SocketPtr accept(SceNetSockaddr *addr, unsigned int *addrlen, int &err) override;
    int listen(int backlog) override;
    int get_peer_address(SceNetSockaddr *addr, unsigned int *addrlen) override;
    int get_socket_address(SceNetSockaddr *addr, unsigned int *addrlen) override;
    int poll_events() override;
};

struct RpcnP2PSocket : public Socket {
    std::shared_ptr<rpcn::VportSocket> vsock;
    int nbio = 0;
    int rcvbuf = 128 * 1024;
    int sndbuf = 128 * 1024;
    int rcvtimeo = 0; // microseconds, 0 = forever
    int sndtimeo = 0;
    int reuseaddr = 0;
    int broadcast = 0;
    int last_error = 0;
    std::atomic<bool> aborted{ false };
    std::map<uint64_t, std::vector<uint8_t>> other_options;

    explicit RpcnP2PSocket(int type);
    explicit RpcnP2PSocket(std::shared_ptr<rpcn::VportSocket> accepted);
    ~RpcnP2PSocket() override;

    int abort(int flags) override;
    int close() override;
    int shutdown_socket(int how) override;
    int bind(const SceNetSockaddr *addr, unsigned int addrlen) override;
    int send_packet(const void *msg, unsigned int len, int flags, const SceNetSockaddr *to, unsigned int tolen) override;
    int recv_packet(void *buf, unsigned int len, int flags, SceNetSockaddr *from, unsigned int *fromlen) override;
    int set_socket_options(int level, int optname, const void *optval, unsigned int optlen) override;
    int get_socket_options(int level, int optname, void *optval, unsigned int *optlen) override;
    int connect(const SceNetSockaddr *addr, unsigned int addrlen) override;
    SocketPtr accept(SceNetSockaddr *addr, unsigned int *addrlen, int &err) override;
    int listen(int backlog) override;
    int get_peer_address(SceNetSockaddr *addr, unsigned int *addrlen) override;
    int get_socket_address(SceNetSockaddr *addr, unsigned int *addrlen) override;
    int poll_events() override;
};
