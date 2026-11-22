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

// P2P sockets (SCE_NET_SOCK_DGRAM_P2P / SCE_NET_SOCK_STREAM_P2P) carried over the RPCN P2P port.
//
// Same wire format as RPCS3 so the two emulators can exchange game traffic:
// every packet starts with { u16 dst_vport, u16 src_vport, u16 flags } (little endian),
// flags 1 = datagram, 2 = stream. Stream packets then carry RPCS3's "U2S0" encapsulated TCP
// header (big endian) and are acknowledged and retransmitted like TCP.
// Addresses are IPv4 in network byte order, ports and vports in host byte order.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace rpcn {

class P2P;

enum VportError : int {
    VPORT_EWOULDBLOCK = -1,
    VPORT_ENOTCONN = -2,
    VPORT_EADDRINUSE = -3,
    VPORT_EALREADY = -4,
    VPORT_EINVAL = -5,
    VPORT_ECONNRESET = -6,
    VPORT_ENETDOWN = -7,
    VPORT_EADDRNOTAVAIL = -8,
    VPORT_EMSGSIZE = -9,
    VPORT_EISCONN = -10,
};

enum VportPoll : unsigned {
    VPORT_POLL_IN = 1,
    VPORT_POLL_OUT = 2,
    VPORT_POLL_HUP = 4,
};

struct VportAddr {
    uint32_t addr = 0;
    uint16_t port = 0; // UDP port of the peer's P2P socket
    uint16_t vport = 0;
};

struct VportSocket;
using VportSocketPtr = std::shared_ptr<VportSocket>;

class VportHub {
public:
    static VportHub &get();
    ~VportHub();

    // Packets are sent and received through this RPCN P2P port
    void attach(const std::shared_ptr<P2P> &p2p);
    uint16_t udp_port() const;

    VportSocketPtr create(bool stream);
    void close(const VportSocketPtr &s);

    // All of these return 0 / a byte count, or a negative VportError
    int bind(const VportSocketPtr &s, uint16_t vport);
    int listen(const VportSocketPtr &s, int backlog);
    int connect(const VportSocketPtr &s, const VportAddr &to);
    VportSocketPtr accept(const VportSocketPtr &s, VportAddr *from, int &err);
    int send(const VportSocketPtr &s, const uint8_t *data, size_t size, const VportAddr *to);
    int recv(const VportSocketPtr &s, uint8_t *buf, size_t size, VportAddr *from, bool peek);
    void local_addr(const VportSocketPtr &s, VportAddr &out);
    int peer_addr(const VportSocketPtr &s, VportAddr &out);
    void set_rcvbuf(const VportSocketPtr &s, int size);

    unsigned poll(const VportSocketPtr &s);
    // Waits until one of the events is pending; false on timeout or when aborted
    bool wait(const VportSocketPtr &s, unsigned events, std::chrono::microseconds timeout, const std::atomic<bool> *abort);
    void wake_all();

private:
    VportHub() = default;

    using Clock = std::chrono::steady_clock;

    void on_packet(uint32_t addr, uint16_t port, const uint8_t *data, size_t size);
    void timer_loop();

    // Called with mutex held
    unsigned poll_locked(const VportSocket &s) const;
    uint16_t alloc_vport(bool stream);
    void send_raw(const std::vector<uint8_t> &packet, uint32_t addr, uint16_t port);
    void send_stream(VportSocket &s, uint8_t flags, uint64_t seq, uint64_t ack, const uint8_t *data, uint32_t size, bool reliable);
    void send_rst(uint32_t addr, uint16_t port, uint16_t src_vport, uint16_t dst_vport);
    void send_ack(VportSocket &s);
    void close_stream(VportSocket &s);
    void unregister(const VportSocketPtr &s);
    void handle_stream(uint32_t addr, uint16_t port, uint16_t dst_vport, const uint8_t *data, size_t size);
    void handle_connected(VportSocket &s, uint8_t flags, uint64_t seq, uint64_t ack, const uint8_t *payload, uint32_t length);

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::shared_ptr<P2P> transport;

    std::map<uint16_t, std::vector<std::weak_ptr<VportSocket>>> dgram_vports;
    std::map<uint16_t, std::vector<std::weak_ptr<VportSocket>>> stream_vports;
    // (peer addr, peer vport, our vport) -> connected stream
    std::map<uint64_t, std::weak_ptr<VportSocket>> streams;
    std::vector<std::weak_ptr<VportSocket>> pending_resend;
    uint16_t next_vport = 30000;

    std::atomic<bool> quit{ false };
    std::thread timer;
};

} // namespace rpcn
