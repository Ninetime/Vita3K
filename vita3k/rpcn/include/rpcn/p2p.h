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

// RPCN peer-to-peer port and signaling.
//
// Speaks the same UDP protocol as RPCS3 so both emulators can connect to each other:
// - every packet starts with a little-endian u16 destination vport;
// - vport 0 carries RPCN replies (subset 0) and signaling packets (subset 1);
// - pings to RPCN's UDP server (port 3657) let it learn our public address, which it hands
//   to the other room members.
// Addresses are IPv4 in network byte order (as in sockaddr_in), ports in host byte order.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace rpcn {

enum class SignalingStatus : int32_t {
    Inactive = 0,
    Pending = 1,
    Active = 2,
};

enum class SignalingDeadReason {
    None,
    TerminatedByPeer,
    TerminatedByMyself,
    Timeout,
};

// A Matching2 room member connection was established or died
struct SignalingEvent {
    uint64_t room_id = 0;
    uint16_t member_id = 0;
    bool established = false;
    SignalingDeadReason reason = SignalingDeadReason::None;
};

struct PeerInfo {
    SignalingStatus status = SignalingStatus::Inactive;
    std::string npid;
    uint32_t addr = 0;
    uint16_t port = 0;
    // Our address as seen by the peer
    uint32_t mapped_addr = 0;
    uint16_t mapped_port = 0;
    uint32_t rtt_us = 0;
};

class P2P {
public:
    // Binds the UDP port, trying the following ones when it is taken (e.g. by RPCS3 on the same PC)
    explicit P2P(uint16_t port);
    ~P2P();

    P2P(const P2P &) = delete;
    P2P &operator=(const P2P &) = delete;

    bool ok() const { return bound_port != 0; }
    uint16_t port() const { return bound_port; }

    // Starts announcing ourselves to RPCN's UDP server
    void set_rpcn(uint32_t server_addr, int64_t user_id, uint32_t local_addr);
    void set_self_npid(const std::string &npid);

    // Public address as reported by RPCN (0 until the first reply)
    uint32_t public_addr() const { return pub_addr.load(); }
    uint16_t public_port() const { return pub_port.load(); }

    // Matching2: connect to a room member (sig2)
    void connect_member(const std::string &npid, uint64_t room_id, uint16_t member_id, uint32_t addr, uint16_t port);
    // Matching2: RPCN asks us to send our address to a peer that wants to connect
    void send_info(const std::string &npid, uint32_t addr, uint16_t port);
    // Ends the connections with the members of a room we left
    void disconnect_room(uint64_t room_id);

    std::optional<PeerInfo> peer(const std::string &npid) const;
    std::vector<SignalingEvent> take_events();

    // Game traffic (packets for vports other than 0), called on the receive thread
    using DataHandler = std::function<void(uint32_t addr, uint16_t port, const uint8_t *data, size_t size)>;
    void set_data_handler(DataHandler handler);
    bool send_to(const uint8_t *data, size_t size, uint32_t addr, uint16_t port);

private:
    struct Peer;
    enum Command : uint32_t;
    struct Packet;
    struct Queued;

    void run();
    void handle_packet(const uint8_t *data, size_t size, uint32_t addr, uint16_t port);
    void handle_signaling(const uint8_t *data, size_t size, uint32_t addr, uint16_t port);
    void ping_rpcn(std::chrono::steady_clock::time_point now);
    void resend(std::chrono::steady_clock::time_point now);

    // Called with mutex held
    std::shared_ptr<Peer> get_peer(const std::string &npid, bool create);
    void send_signaling(const Packet &packet, uint32_t addr, uint16_t port);
    void queue(const Packet &packet, const std::shared_ptr<Peer> &peer, std::chrono::steady_clock::time_point when);
    void reschedule(const std::shared_ptr<Peer> &peer, Command cmd, std::chrono::steady_clock::time_point when);
    void retire(const std::shared_ptr<Peer> &peer, Command cmd);
    void retire_all(const std::shared_ptr<Peer> &peer);
    void set_status(const std::shared_ptr<Peer> &peer, SignalingStatus status, SignalingDeadReason reason);
    void stop_peer(const std::shared_ptr<Peer> &peer);
    Packet make_packet(Command cmd) const;

    intptr_t sock = -1;
    uint16_t bound_port = 0;
    std::atomic<bool> quit{ false };
    std::thread thread;

    mutable std::mutex mutex;
    uint32_t rpcn_addr = 0;
    int64_t rpcn_user_id = 0;
    uint32_t rpcn_local_addr = 0;
    std::chrono::steady_clock::time_point last_ping{};
    std::chrono::steady_clock::time_point last_pong{};
    std::atomic<uint32_t> pub_addr{ 0 };
    std::atomic<uint16_t> pub_port{ 0 };

    std::string self_npid;
    std::map<std::string, std::shared_ptr<Peer>> peers;
    std::multimap<std::chrono::steady_clock::time_point, std::unique_ptr<Queued>> queued;
    std::vector<SignalingEvent> events;

    std::mutex handler_mutex;
    DataHandler data_handler;
};

} // namespace rpcn
