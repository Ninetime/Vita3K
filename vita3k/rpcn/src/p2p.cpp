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

#include <rpcn/p2p.h>

#include <util/log.h>

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <array>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#define CLOSE_SOCKET closesocket
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSE_SOCKET close
#endif

namespace rpcn {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {

constexpr uint16_t RPCN_UDP_PORT = 3657;
constexpr int PORT_ATTEMPTS = 16;

// vport 0 header: u16 vport + u8 subset
constexpr size_t VPORT_0_HEADER_SIZE = 3;
constexpr uint8_t SUBSET_RPCN = 0;
constexpr uint8_t SUBSET_SIGNALING = 1;

// Same values as RPCS3's signaling_handler
constexpr uint32_t SIGNALING_VERSION = 3;
constexpr size_t SIGNALING_PACKET_SIZE = 72; // sizeof(signaling_packet) in RPCS3
constexpr size_t NPID_SIZE = 16;
constexpr size_t SCE_NP_ID_SIZE = 0x24;
constexpr auto REPEAT_CONNECT_DELAY = 200ms;
constexpr auto REPEAT_PING_DELAY = 500ms;
constexpr auto REPEAT_FINISHED_DELAY = 500ms;
constexpr auto REPEAT_INFO_DELAY = 200ms;
constexpr auto CONNECTION_TIMEOUT = 60s;

uint64_t micro_timestamp(Clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
}

template <typename T>
void put_le(uint8_t *p, T v) {
    for (size_t i = 0; i < sizeof(T); i++)
        p[i] = static_cast<uint8_t>(static_cast<uint64_t>(v) >> (8 * i));
}

template <typename T>
T get_le(const uint8_t *p) {
    uint64_t v = 0;
    for (size_t i = 0; i < sizeof(T); i++)
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return static_cast<T>(v);
}

std::string ip_string(uint32_t addr) {
    const auto *b = reinterpret_cast<const uint8_t *>(&addr);
    return fmt::format("{}.{}.{}.{}", b[0], b[1], b[2], b[3]);
}

bool valid_npid(const uint8_t *npid) {
    // SceNpOnlineId: data[16], term, dummy[3]
    for (size_t i = 0; i < NPID_SIZE; i++) {
        const char c = static_cast<char>(npid[i]);
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == 0))
            return false;
    }
    return npid[16] == 0 && npid[17] == 0 && npid[18] == 0 && npid[19] == 0;
}

} // namespace

enum P2P::Command : uint32_t {
    signal_ping,
    signal_pong,
    signal_connect,
    signal_connect_ack,
    signal_confirm,
    signal_finished,
    signal_finished_ack,
    signal_info,
};

struct P2P::Packet {
    uint64_t timestamp_sender = 0;
    uint64_t timestamp_receiver = 0;
    Command command = signal_ping;
    std::string npid;
};

struct P2P::Peer {
    std::string npid;
    SignalingStatus status = SignalingStatus::Inactive;
    uint32_t addr = 0;
    uint16_t port = 0;
    uint32_t mapped_addr = 0;
    uint16_t mapped_port = 0;
    Clock::time_point last_recv = Clock::now();
    bool op_activated = false;
    // The connection is established once our CONNECT was acknowledged and the peer sent its own CONNECT.
    // A peer acknowledges a CONNECT even before it knows the room (RPCS3 processes its JoinRoom reply up to
    // 200 ms late), and games send their first message as soon as the connection is established: waiting for
    // the peer's CONNECT makes sure it can already handle that message.
    bool connect_acked = false;
    bool peer_connected = false;
    uint32_t info_counter = 10;
    uint64_t room_id = 0;
    uint16_t member_id = 0;
    std::array<uint64_t, 6> rtts{};
    size_t rtt_count = 0;
    uint32_t rtt = 0;
};

struct P2P::Queued {
    Packet packet;
    std::shared_ptr<Peer> peer;
};

P2P::P2P(uint16_t port) {
    const auto s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    if (s == INVALID_SOCKET) {
#else
    if (s < 0) {
#endif
        LOG_ERROR("RPCN P2P: failed to create the UDP socket");
        return;
    }

    int optval = 131072; // receive buffer size RPCS3 uses for its P2P socket
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&optval), sizeof(optval));
    optval = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&optval), sizeof(optval));
#ifdef _WIN32
    // Don't let ICMP port unreachable replies fail the following recvfrom calls
    BOOL report_reset = FALSE;
    DWORD returned = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &report_reset, sizeof(report_reset), nullptr, 0, &returned, nullptr, nullptr);
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif

    for (int i = 0; i < PORT_ATTEMPTS; i++) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port + i));
        if (::bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
            bound_port = static_cast<uint16_t>(port + i);
            break;
        }
    }
    if (!bound_port) {
        LOG_ERROR("RPCN P2P: could not bind UDP ports {} to {}", port, port + PORT_ATTEMPTS - 1);
        CLOSE_SOCKET(s);
        return;
    }
    if (bound_port != port)
        LOG_WARN("RPCN P2P: UDP port {} is in use (RPCS3 on this PC?), using {} instead", port, bound_port);
    LOG_INFO("RPCN P2P: bound UDP port {}", bound_port);

    sock = static_cast<intptr_t>(s);
    thread = std::thread([this] { run(); });
}

P2P::~P2P() {
    quit = true;
    if (thread.joinable())
        thread.join();
    if (sock != -1)
        CLOSE_SOCKET(static_cast<decltype(::socket(0, 0, 0))>(sock));
}

void P2P::set_rpcn(uint32_t server_addr, int64_t user_id, uint32_t local_addr) {
    std::lock_guard lock(mutex);
    rpcn_addr = server_addr;
    rpcn_user_id = user_id;
    rpcn_local_addr = local_addr;
    last_ping = {};
    last_pong = {};
    LOG_INFO("RPCN P2P: announcing to {}:{} (user {}, local {})", ip_string(server_addr), RPCN_UDP_PORT, user_id, ip_string(local_addr));
}

void P2P::set_self_npid(const std::string &npid) {
    std::lock_guard lock(mutex);
    self_npid = npid;
}

void P2P::set_data_handler(DataHandler handler) {
    std::lock_guard lock(handler_mutex);
    data_handler = std::move(handler);
}

bool P2P::send_to(const uint8_t *data, size_t size, uint32_t addr, uint16_t port) {
    if (sock == -1)
        return false;
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = addr;
    dest.sin_port = htons(port);
    const auto s = static_cast<decltype(::socket(0, 0, 0))>(sock);
    return ::sendto(s, reinterpret_cast<const char *>(data), static_cast<int>(size), 0, reinterpret_cast<sockaddr *>(&dest), sizeof(dest)) >= 0;
}

void P2P::run() {
    const auto s = static_cast<decltype(::socket(0, 0, 0))>(sock);
    std::vector<uint8_t> buf(65536);
    while (!quit) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(s, &read_set);
        timeval tv{ 0, 20000 };
        ::select(static_cast<int>(s) + 1, &read_set, nullptr, nullptr, &tv);

        while (true) {
            sockaddr_in from{};
            socklen_t from_len = sizeof(from);
            const auto received = ::recvfrom(s, reinterpret_cast<char *>(buf.data()), static_cast<int>(buf.size()), 0, reinterpret_cast<sockaddr *>(&from), &from_len);
            if (received < 0)
                break;
            handle_packet(buf.data(), static_cast<size_t>(received), from.sin_addr.s_addr, ntohs(from.sin_port));
        }

        const auto now = Clock::now();
        std::lock_guard lock(mutex);
        ping_rpcn(now);
        resend(now);
    }
}

void P2P::handle_packet(const uint8_t *data, size_t size, uint32_t addr, uint16_t port) {
    if (size < 2)
        return;
    const uint16_t vport = get_le<uint16_t>(data);
    if (vport != 0) {
        std::lock_guard lock(handler_mutex);
        if (data_handler)
            data_handler(addr, port, data, size);
        else
            LOG_TRACE("RPCN P2P: dropping {} byte packet for vport {} from {}:{}", size, vport, ip_string(addr), port);
        return;
    }
    if (size < VPORT_0_HEADER_SIZE)
        return;

    switch (data[2]) {
    case SUBSET_RPCN: {
        // Our address as seen by RPCN: IPv4 (network order) + big-endian port
        if (size != VPORT_0_HEADER_SIZE + 6)
            return;
        uint32_t seen_addr;
        memcpy(&seen_addr, data + 3, 4);
        const uint16_t seen_port = static_cast<uint16_t>((data[7] << 8) | data[8]);
        {
            std::lock_guard lock(mutex);
            last_pong = Clock::now();
        }
        if (pub_addr.exchange(seen_addr) != seen_addr || pub_port.exchange(seen_port) != seen_port)
            LOG_INFO("RPCN P2P: RPCN sees us at {}:{}", ip_string(seen_addr), seen_port);
        break;
    }
    case SUBSET_SIGNALING:
        handle_signaling(data + VPORT_0_HEADER_SIZE, size - VPORT_0_HEADER_SIZE, addr, port);
        break;
    default:
        LOG_WARN("RPCN P2P: invalid vport 0 subset {}", data[2]);
        break;
    }
}

void P2P::ping_rpcn(Clock::time_point now) {
    if (!rpcn_addr)
        return;
    // Every 5 seconds, then every 500ms until RPCN answers (same as RPCS3)
    if (now - last_pong < 5s || now - last_ping <= 500ms)
        return;
    std::array<uint8_t, 13> ping{};
    ping[0] = 1;
    put_le<int64_t>(ping.data() + 1, rpcn_user_id);
    memcpy(ping.data() + 9, &rpcn_local_addr, 4);
    if (!send_to(ping.data(), ping.size(), rpcn_addr, RPCN_UDP_PORT))
        LOG_ERROR("RPCN P2P: failed to ping RPCN");
    last_ping = now;
}

P2P::Packet P2P::make_packet(Command cmd) const {
    Packet p;
    p.command = cmd;
    p.npid = self_npid;
    return p;
}

void P2P::send_signaling(const Packet &packet, uint32_t addr, uint16_t port) {
    std::array<uint8_t, VPORT_0_HEADER_SIZE + SIGNALING_PACKET_SIZE> raw{};
    raw[2] = SUBSET_SIGNALING;
    uint8_t *p = raw.data() + VPORT_0_HEADER_SIZE;
    memcpy(p, "SIGN", 4); // big-endian signature
    put_le<uint32_t>(p + 4, SIGNALING_VERSION);
    put_le<uint64_t>(p + 8, packet.timestamp_sender);
    put_le<uint64_t>(p + 16, packet.timestamp_receiver);
    put_le<uint32_t>(p + 24, packet.command);
    memcpy(p + 28, &addr, 4); // where we sent it, as the peer's mapped address
    put_le<uint16_t>(p + 32, port);
    memcpy(p + 34, packet.npid.data(), std::min(packet.npid.size(), NPID_SIZE));
    // SceNpId.reserved[0]: NP IDs issued by the NP server are valid. Peers keep this ID for the connection
    // (SCE_NP_SIGNALING_CONN_INFO_PEER_NPID) and games compare it with the room member's ID.
    p[34 + 28] = 1;
    if (!send_to(raw.data(), raw.size(), addr, port))
        LOG_ERROR("RPCN P2P: failed to send signaling packet to {}:{}", ip_string(addr), port);
}

void P2P::queue(const Packet &packet, const std::shared_ptr<Peer> &peer, Clock::time_point when) {
    queued.emplace(when, std::make_unique<Queued>(Queued{ packet, peer }));
}

void P2P::reschedule(const std::shared_ptr<Peer> &peer, Command cmd, Clock::time_point when) {
    for (auto it = queued.begin(); it != queued.end(); ++it) {
        if (it->second->packet.command == cmd && it->second->peer == peer) {
            auto node = queued.extract(it);
            node.key() = when;
            queued.insert(std::move(node));
            return;
        }
    }
}

void P2P::retire(const std::shared_ptr<Peer> &peer, Command cmd) {
    for (auto it = queued.begin(); it != queued.end(); ++it) {
        if (it->second->packet.command == cmd && it->second->peer == peer) {
            queued.erase(it);
            return;
        }
    }
}

void P2P::retire_all(const std::shared_ptr<Peer> &peer) {
    for (auto it = queued.begin(); it != queued.end();) {
        if (it->second->peer == peer)
            it = queued.erase(it);
        else
            ++it;
    }
}

std::shared_ptr<P2P::Peer> P2P::get_peer(const std::string &npid, bool create) {
    const auto it = peers.find(npid);
    if (it != peers.end())
        return it->second;
    if (!create)
        return nullptr;
    auto peer = std::make_shared<Peer>();
    peer->npid = npid;
    peers.emplace(npid, peer);
    return peer;
}

void P2P::set_status(const std::shared_ptr<Peer> &peer, SignalingStatus status, SignalingDeadReason reason) {
    if (peer->status == SignalingStatus::Pending && status == SignalingStatus::Active) {
        peer->status = SignalingStatus::Active;
        LOG_INFO("RPCN P2P: connection with {} ({}:{}) established", peer->npid, ip_string(peer->addr), peer->port);
        if (peer->room_id)
            events.push_back({ peer->room_id, peer->member_id, true, SignalingDeadReason::None });
    } else if (peer->status != SignalingStatus::Inactive && status == SignalingStatus::Inactive) {
        peer->status = SignalingStatus::Inactive;
        peer->connect_acked = false;
        peer->peer_connected = false;
        LOG_INFO("RPCN P2P: connection with {} is dead ({})", peer->npid, static_cast<int>(reason));
        if (peer->room_id)
            events.push_back({ peer->room_id, peer->member_id, false, reason });
        retire_all(peer);
    }
}

void P2P::handle_signaling(const uint8_t *data, size_t size, uint32_t op_addr, uint16_t op_port) {
    if (size != SIGNALING_PACKET_SIZE) {
        LOG_ERROR("RPCN P2P: invalid signaling packet size {}", size);
        return;
    }
    if (memcmp(data, "SIGN", 4) != 0) {
        LOG_ERROR("RPCN P2P: signaling packet with an invalid signature");
        return;
    }
    if (get_le<uint32_t>(data + 4) != SIGNALING_VERSION) {
        LOG_ERROR("RPCN P2P: signaling version {} from {}:{}, expected {}", get_le<uint32_t>(data + 4), ip_string(op_addr), op_port, SIGNALING_VERSION);
        return;
    }
    const uint8_t *raw_npid = data + 34;
    if (!valid_npid(raw_npid)) {
        LOG_ERROR("RPCN P2P: signaling packet with an invalid NP ID");
        return;
    }
    const std::string npid(reinterpret_cast<const char *>(raw_npid), strnlen(reinterpret_cast<const char *>(raw_npid), NPID_SIZE));
    const uint64_t ts_sender = get_le<uint64_t>(data + 8);
    const uint64_t ts_receiver = get_le<uint64_t>(data + 16);
    const auto command = static_cast<Command>(get_le<uint32_t>(data + 24));
    uint32_t sent_addr;
    memcpy(&sent_addr, data + 28, 4);
    const uint16_t sent_port = get_le<uint16_t>(data + 32);

    std::lock_guard lock(mutex);
    LOG_TRACE("RPCN P2P: signaling command {} from {} ({}:{})", static_cast<uint32_t>(command), npid, ip_string(op_addr), op_port);

    // A connection can be established remotely without us asking for it
    auto peer = get_peer(npid, command == signal_connect || command == signal_info);
    if (!peer)
        return;

    const auto now = Clock::now();
    const auto update_addr = [&] {
        if (peer->addr != op_addr || peer->port != op_port) {
            LOG_INFO("RPCN P2P: {} is at {}:{}", npid, ip_string(op_addr), op_port);
            peer->addr = op_addr;
            peer->port = op_port;
        }
    };
    const auto update_rtt = [&](uint64_t timestamp) {
        const uint64_t now_us = micro_timestamp(now);
        if (timestamp > now_us || now_us - timestamp > UINT32_MAX) {
            LOG_ERROR("RPCN P2P: signaling packet with an invalid RTT timestamp");
            return false;
        }
        peer->rtts[peer->rtt_count % peer->rtts.size()] = now_us - timestamp;
        peer->rtt_count++;
        const size_t count = std::min(peer->rtts.size(), peer->rtt_count);
        uint64_t sum = 0;
        for (size_t i = 0; i < count; i++)
            sum += peer->rtts[i];
        peer->rtt = static_cast<uint32_t>(sum / count);
        return true;
    };
    const auto setup_ping = [&] {
        for (const auto &[_, q] : queued)
            if (q->packet.command == signal_ping && q->peer == peer)
                return;
        Packet ping = make_packet(signal_ping);
        ping.timestamp_sender = micro_timestamp(now);
        send_signaling(ping, peer->addr, peer->port);
        queue(ping, peer, now + REPEAT_PING_DELAY);
    };

    Packet reply;
    bool send_reply = false, repeat = false;
    switch (command) {
    case signal_ping:
        send_reply = true;
        reply = make_packet(signal_pong);
        reply.timestamp_sender = ts_sender;
        break;
    case signal_pong:
        if (!update_rtt(ts_sender))
            return;
        reschedule(peer, signal_ping, now + 10s);
        break;
    case signal_info:
        update_addr();
        break;
    case signal_connect:
        send_reply = true;
        repeat = true;
        reply = make_packet(signal_connect_ack);
        reply.timestamp_sender = ts_sender;
        reply.timestamp_receiver = micro_timestamp(now);
        update_addr();
        peer->peer_connected = true;
        if (peer->connect_acked)
            set_status(peer, SignalingStatus::Active, SignalingDeadReason::None);
        break;
    case signal_connect_ack:
        if (!update_rtt(ts_sender))
            return;
        send_reply = true;
        update_addr();
        setup_ping();
        reply = make_packet(signal_confirm);
        reply.timestamp_receiver = ts_receiver;
        retire(peer, signal_connect);
        peer->mapped_addr = sent_addr;
        peer->mapped_port = sent_port;
        peer->connect_acked = true;
        if (peer->peer_connected)
            set_status(peer, SignalingStatus::Active, SignalingDeadReason::None);
        else
            LOG_INFO("RPCN P2P: {} acknowledged our connection, waiting for its own", peer->npid);
        break;
    case signal_confirm:
        if (!update_rtt(ts_receiver))
            return;
        update_addr();
        setup_ping();
        retire(peer, signal_connect_ack);
        peer->mapped_addr = sent_addr;
        peer->mapped_port = sent_port;
        peer->op_activated = true;
        break;
    case signal_finished:
        send_reply = true;
        reply = make_packet(signal_finished_ack);
        peer->op_activated = false;
        set_status(peer, SignalingStatus::Inactive, SignalingDeadReason::TerminatedByPeer);
        break;
    case signal_finished_ack:
        set_status(peer, SignalingStatus::Inactive, SignalingDeadReason::TerminatedByMyself);
        retire(peer, signal_finished);
        break;
    default:
        LOG_ERROR("RPCN P2P: invalid signaling command {}", static_cast<uint32_t>(command));
        return;
    }

    peer->last_recv = now;
    if (send_reply) {
        send_signaling(reply, op_addr, op_port);
        if (repeat)
            queue(reply, peer, now + REPEAT_CONNECT_DELAY);
    }
}

void P2P::resend(Clock::time_point now) {
    for (auto it = queued.begin(); it != queued.end();) {
        if (it->first > now)
            break;
        Queued &q = *it->second;
        const auto peer = q.peer;
        const Command cmd = q.packet.command;
        if (peer->last_recv < now - CONNECTION_TIMEOUT && cmd != signal_info) {
            // No sign of the peer for a minute
            set_status(peer, SignalingStatus::Inactive, SignalingDeadReason::Timeout);
            retire_all(peer);
            it = queued.begin();
            continue;
        }

        switch (cmd) {
        case signal_connect:
        case signal_ping:
            q.packet.timestamp_sender = micro_timestamp(now);
            break;
        case signal_connect_ack:
            q.packet.timestamp_receiver = micro_timestamp(now);
            break;
        default:
            break;
        }
        send_signaling(q.packet, peer->addr, peer->port);

        auto delay = std::chrono::duration_cast<Clock::duration>(500ms);
        switch (cmd) {
        case signal_ping:
        case signal_pong:
            delay = REPEAT_PING_DELAY;
            break;
        case signal_connect:
        case signal_connect_ack:
        case signal_confirm:
            delay = REPEAT_CONNECT_DELAY;
            break;
        case signal_finished:
        case signal_finished_ack:
            delay = REPEAT_FINISHED_DELAY;
            break;
        case signal_info:
            if (peer->info_counter == 0) {
                it = queued.erase(it);
                continue;
            }
            delay = REPEAT_INFO_DELAY;
            peer->info_counter--;
            break;
        }
        auto node = queued.extract(it++);
        node.key() = now + delay;
        queued.insert(std::move(node));
    }
}

void P2P::connect_member(const std::string &npid, uint64_t room_id, uint16_t member_id, uint32_t addr, uint16_t port) {
    std::lock_guard lock(mutex);
    LOG_INFO("RPCN P2P: connecting to member {} ({}) of room {} at {}:{}", member_id, npid, room_id, ip_string(addr), port);
    auto peer = get_peer(npid, true);
    peer->room_id = room_id;
    peer->member_id = member_id;
    if (peer->status == SignalingStatus::Active)
        events.push_back({ room_id, member_id, true, SignalingDeadReason::None });
    else
        peer->status = SignalingStatus::Pending;

    const auto now = Clock::now();
    peer->last_recv = now;
    // Keep an address learned from a packet the peer sent first
    if (peer->addr == 0 || peer->port == 0) {
        peer->addr = addr;
        peer->port = port;
    }
    Packet connect = make_packet(signal_connect);
    connect.timestamp_sender = micro_timestamp(now);
    send_signaling(connect, peer->addr, peer->port);
    queue(connect, peer, now + REPEAT_CONNECT_DELAY);
}

void P2P::send_info(const std::string &npid, uint32_t addr, uint16_t port) {
    std::lock_guard lock(mutex);
    auto peer = get_peer(npid, true);
    peer->addr = addr;
    peer->port = port;
    peer->info_counter = 10;
    retire(peer, signal_info);
    const Packet info = make_packet(signal_info);
    send_signaling(info, addr, port);
    queue(info, peer, Clock::now() + REPEAT_INFO_DELAY);
}

void P2P::stop_peer(const std::shared_ptr<Peer> &peer) {
    retire_all(peer);
    if (peer->status == SignalingStatus::Inactive)
        return;
    const Packet finished = make_packet(signal_finished);
    send_signaling(finished, peer->addr, peer->port);
    queue(finished, peer, Clock::now() + REPEAT_FINISHED_DELAY);
}

void P2P::disconnect_room(uint64_t room_id) {
    std::lock_guard lock(mutex);
    for (auto &[_, peer] : peers)
        if (peer->room_id == room_id)
            stop_peer(peer);
}

std::optional<PeerInfo> P2P::peer(const std::string &npid) const {
    std::lock_guard lock(mutex);
    const auto it = peers.find(npid);
    if (it == peers.end())
        return std::nullopt;
    const Peer &p = *it->second;
    return PeerInfo{ p.status, p.npid, p.addr, p.port, p.mapped_addr, p.mapped_port, p.rtt };
}

std::vector<SignalingEvent> P2P::take_events() {
    std::lock_guard lock(mutex);
    std::vector<SignalingEvent> out;
    out.swap(events);
    return out;
}

} // namespace rpcn
