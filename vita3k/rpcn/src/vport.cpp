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
#include <rpcn/vport.h>

#include <util/log.h>

#include <fmt/format.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <string>

namespace rpcn {

using namespace std::chrono_literals;

namespace {

constexpr uint16_t DEFAULT_P2P_PORT = 3658; // SCE_NP_PORT
constexpr size_t VPORT_HEADER_SIZE = 6;
constexpr uint16_t FLAG_P2P = 1;
constexpr uint16_t FLAG_P2PS = 2;

// RPCS3 p2ps_encapsulated_tcp (big endian, 32 bytes with padding)
constexpr uint32_t U2S_SIGNATURE = 0x55325330; // "U2S0"
constexpr size_t U2S_HEADER_SIZE = 32;
constexpr size_t U2S_CHECKSUM_OFFSET = 28;
constexpr uint32_t STREAM_SEGMENT_SIZE = 1300;

enum TcpFlags : uint8_t {
    TCP_FIN = 1 << 0,
    TCP_SYN = 1 << 1,
    TCP_RST = 1 << 2,
    TCP_PSH = 1 << 3,
    TCP_ACK = 1 << 4,
};

constexpr size_t DEFAULT_RCVBUF = 128 * 1024;
constexpr size_t DGRAM_PACKET_OVERHEAD = 16;
constexpr size_t STREAM_SEGMENT_OVERHEAD = 4;
constexpr int MAX_RETRIES = 10;

void put_le16(uint8_t *p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

uint16_t get_le16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

template <typename T>
void put_be(uint8_t *p, T v) {
    for (size_t i = 0; i < sizeof(T); i++)
        p[i] = static_cast<uint8_t>(v >> (8 * (sizeof(T) - 1 - i)));
}

template <typename T>
T get_be(const uint8_t *p) {
    T v = 0;
    for (size_t i = 0; i < sizeof(T); i++)
        v = static_cast<T>((v << 8) | p[i]);
    return v;
}

// Sum of little-endian 16-bit words, as RPCS3's u2s_tcp_checksum
uint16_t u2s_checksum(const uint8_t *data, size_t size) {
    uint32_t sum = 0;
    while (size > 1) {
        sum += get_le16(data);
        data += 2;
        size -= 2;
    }
    if (size)
        sum += *data;
    sum = (sum >> 16) + (sum & 0xffff);
    sum += sum >> 16;
    return static_cast<uint16_t>(~sum);
}

uint64_t stream_key(uint32_t addr, uint16_t peer_vport, uint16_t our_vport) {
    return addr | (static_cast<uint64_t>(peer_vport) << 48) | (static_cast<uint64_t>(our_vport) << 32);
}

uint64_t random_seq() {
    static std::mt19937 rng{ std::random_device{}() };
    return rng() & 0x7fffffff;
}

std::string ip_str(uint32_t addr) {
    const auto *b = reinterpret_cast<const uint8_t *>(&addr);
    return fmt::format("{}.{}.{}.{}", b[0], b[1], b[2], b[3]);
}

} // namespace

enum class StreamState {
    Closed,
    Listening,
    Handshaking,
    Connected,
    Disconnected,
};

struct VportSocket {
    explicit VportSocket(bool stream)
        : stream(stream) {}

    const bool stream;
    uint16_t vport = 0;
    uint32_t bound_addr = 0;
    size_t rcvbuf = DEFAULT_RCVBUF;
    bool registered = false;

    // DGRAM_P2P
    std::deque<std::pair<VportAddr, std::vector<uint8_t>>> dgrams;
    size_t dgram_bytes = 0;

    // STREAM_P2P
    StreamState state = StreamState::Closed;
    size_t max_backlog = 0;
    std::deque<VportSocketPtr> backlog;
    VportAddr peer;
    uint64_t cur_seq = 0; // next sequence number to send
    uint64_t data_beg_seq = 0; // sequence number of the first unread byte
    uint64_t data_available = 0;
    std::map<uint64_t, std::vector<uint8_t>> received;
    size_t received_size = 0;

    struct Unacked {
        std::vector<uint8_t> packet;
        std::chrono::steady_clock::time_point first_sent;
        std::chrono::steady_clock::time_point due;
    };
    std::map<uint64_t, Unacked> unacked;
    std::chrono::milliseconds rtt{ 50 };
    int retries = 0;
};

VportHub &VportHub::get() {
    static VportHub hub;
    return hub;
}

VportHub::~VportHub() {
    quit = true;
    cv.notify_all();
    if (timer.joinable())
        timer.join();
}

void VportHub::attach(const std::shared_ptr<P2P> &p2p) {
    {
        std::lock_guard lock(mutex);
        transport = p2p;
        if (!timer.joinable())
            timer = std::thread([this] { timer_loop(); });
    }
    if (p2p)
        p2p->set_data_handler([this](uint32_t addr, uint16_t port, const uint8_t *data, size_t size) {
            on_packet(addr, port, data, size);
        });
}

uint16_t VportHub::udp_port() const {
    std::lock_guard lock(mutex);
    return transport ? transport->port() : DEFAULT_P2P_PORT;
}

VportSocketPtr VportHub::create(bool stream) {
    return std::make_shared<VportSocket>(stream);
}

void VportHub::unregister(const VportSocketPtr &s) {
    const auto drop = [&](std::map<uint16_t, std::vector<std::weak_ptr<VportSocket>>> &bound) {
        const auto it = bound.find(s->vport);
        if (it == bound.end())
            return;
        auto &list = it->second;
        list.erase(std::remove_if(list.begin(), list.end(), [&](const std::weak_ptr<VportSocket> &w) {
            const auto p = w.lock();
            return !p || p == s;
        }),
            list.end());
        if (list.empty())
            bound.erase(it);
    };
    drop(s->stream ? stream_vports : dgram_vports);
    for (auto it = streams.begin(); it != streams.end();) {
        const auto p = it->second.lock();
        if (!p || p == s)
            it = streams.erase(it);
        else
            ++it;
    }
    s->unacked.clear();
    s->registered = false;
}

void VportHub::close(const VportSocketPtr &s) {
    std::lock_guard lock(mutex);
    // Connections waiting in the backlog were never accepted
    for (const auto &pending : s->backlog)
        unregister(pending);
    s->backlog.clear();
    // Tell the peer, like closing a TCP socket does: it sees the stream end at once instead of when its retries
    // run out (games treat it as the opponent leaving). RPCS3 closes the stream on FIN too.
    if (s->stream && s->state == StreamState::Connected)
        send_stream(*s, TCP_FIN, s->cur_seq, 0, nullptr, 0, false);
    unregister(s);
    s->state = StreamState::Closed;
    cv.notify_all();
}

uint16_t VportHub::alloc_vport(bool stream) {
    auto &bound = stream ? stream_vports : dgram_vports;
    for (int i = 0; i < 0x10000; i++) {
        const uint16_t vport = next_vport++;
        if (next_vport == 0)
            next_vport = 30000;
        if (vport != 0 && !bound.contains(vport))
            return vport;
    }
    return 0;
}

int VportHub::bind(const VportSocketPtr &s, uint16_t vport) {
    std::lock_guard lock(mutex);
    if (s->registered)
        return VPORT_EINVAL;
    auto &bound = s->stream ? stream_vports : dgram_vports;
    if (vport == 0)
        vport = alloc_vport(s->stream);
    if (vport == 0)
        return VPORT_EADDRINUSE;
    // SFxT sets SO_REUSEADDR on its sockets, so sharing a vport is allowed as in RPCS3 with reusable sockets
    bound[vport].push_back(s);
    s->vport = vport;
    s->registered = true;
    LOG_INFO("RPCN P2P: {} socket bound to vport {}", s->stream ? "stream" : "datagram", vport);
    return 0;
}

int VportHub::listen(const VportSocketPtr &s, int backlog) {
    std::lock_guard lock(mutex);
    if (!s->stream)
        return VPORT_EINVAL;
    if (!s->registered) {
        const uint16_t vport = alloc_vport(true);
        stream_vports[vport].push_back(s);
        s->vport = vport;
        s->registered = true;
    }
    s->state = StreamState::Listening;
    s->max_backlog = static_cast<size_t>(std::max(backlog, 1));
    return 0;
}

void VportHub::send_raw(const std::vector<uint8_t> &packet, uint32_t addr, uint16_t port) {
    if (!transport || !transport->send_to(packet.data(), packet.size(), addr, port))
        LOG_TRACE("RPCN P2P: could not send {} bytes to {}:{}", packet.size(), ip_str(addr), port);
}

static std::vector<uint8_t> make_stream_packet(uint16_t src_vport, uint16_t dst_vport, uint8_t flags, uint64_t seq, uint64_t ack, const uint8_t *data, uint32_t size) {
    std::vector<uint8_t> packet(VPORT_HEADER_SIZE + U2S_HEADER_SIZE + size, 0);
    put_le16(packet.data(), dst_vport);
    put_le16(packet.data() + 2, src_vport);
    put_le16(packet.data() + 4, FLAG_P2PS);
    uint8_t *h = packet.data() + VPORT_HEADER_SIZE;
    put_be<uint32_t>(h, U2S_SIGNATURE);
    put_be<uint32_t>(h + 4, size);
    put_be<uint64_t>(h + 8, seq);
    put_be<uint64_t>(h + 16, ack);
    put_be<uint16_t>(h + 24, src_vport);
    put_be<uint16_t>(h + 26, dst_vport);
    h[30] = flags;
    if (size)
        memcpy(h + U2S_HEADER_SIZE, data, size);
    put_be<uint16_t>(h + U2S_CHECKSUM_OFFSET, u2s_checksum(h, U2S_HEADER_SIZE + size));
    return packet;
}

void VportHub::send_stream(VportSocket &s, uint8_t flags, uint64_t seq, uint64_t ack, const uint8_t *data, uint32_t size, bool reliable) {
    auto packet = make_stream_packet(s.vport, s.peer.vport, flags, seq, ack, data, size);
    send_raw(packet, s.peer.addr, s.peer.port);
    if (reliable) {
        const auto now = Clock::now();
        s.unacked[seq] = { std::move(packet), now, now + s.rtt };
    }
}

void VportHub::send_rst(uint32_t addr, uint16_t port, uint16_t src_vport, uint16_t dst_vport) {
    send_raw(make_stream_packet(src_vport, dst_vport, TCP_RST, 0, 0, nullptr, 0), addr, port);
}

int VportHub::connect(const VportSocketPtr &s, const VportAddr &to) {
    std::lock_guard lock(mutex);
    if (!s->stream)
        return VPORT_EINVAL; // DGRAM_P2P sockets are not connected
    if (to.vport == 0)
        return VPORT_EADDRNOTAVAIL;
    if (s->state != StreamState::Closed && s->state != StreamState::Disconnected)
        return s->state == StreamState::Connected ? VPORT_EISCONN : VPORT_EALREADY;
    if (!s->registered) {
        const uint16_t vport = alloc_vport(true);
        stream_vports[vport].push_back(s);
        s->vport = vport;
        s->registered = true;
    }
    s->peer = to;
    if (s->peer.port == 0)
        s->peer.port = DEFAULT_P2P_PORT;
    streams[stream_key(to.addr, to.vport, s->vport)] = s;

    const uint64_t seq = random_seq();
    s->cur_seq = seq + 1;
    s->data_beg_seq = 0;
    s->data_available = 0;
    s->received.clear();
    s->received_size = 0;
    s->retries = 0;
    s->state = StreamState::Handshaking;
    LOG_INFO("RPCN P2P: stream vport {} connecting to {}:{} vport {}", s->vport, ip_str(to.addr), s->peer.port, to.vport);
    send_stream(*s, TCP_SYN, seq, 0, nullptr, 0, true);
    // Like RPCS3, the handshake completes in the background
    return 0;
}

VportSocketPtr VportHub::accept(const VportSocketPtr &s, VportAddr *from, int &err) {
    std::lock_guard lock(mutex);
    if (!s->stream || s->state != StreamState::Listening) {
        err = VPORT_EINVAL;
        return nullptr;
    }
    if (s->backlog.empty()) {
        err = VPORT_EWOULDBLOCK;
        return nullptr;
    }
    auto client = s->backlog.front();
    s->backlog.pop_front();
    if (from)
        *from = client->peer;
    err = 0;
    return client;
}

int VportHub::send(const VportSocketPtr &s, const uint8_t *data, size_t size, const VportAddr *to) {
    std::lock_guard lock(mutex);
    if (!transport)
        return VPORT_ENETDOWN;
    if (!s->stream) {
        if (!to)
            return VPORT_ENOTCONN;
        if (size > 65535 - VPORT_HEADER_SIZE)
            return VPORT_EMSGSIZE;
        if (!s->registered) {
            const uint16_t vport = alloc_vport(false);
            dgram_vports[vport].push_back(s);
            s->vport = vport;
            s->registered = true;
        }
        std::vector<uint8_t> packet(VPORT_HEADER_SIZE + size);
        put_le16(packet.data(), to->vport);
        put_le16(packet.data() + 2, s->vport);
        put_le16(packet.data() + 4, FLAG_P2P);
        if (size)
            memcpy(packet.data() + VPORT_HEADER_SIZE, data, size);
        send_raw(packet, to->addr, to->port ? to->port : DEFAULT_P2P_PORT);
        return static_cast<int>(size);
    }

    switch (s->state) {
    case StreamState::Disconnected:
        return VPORT_ECONNRESET;
    case StreamState::Closed:
    case StreamState::Listening:
        return VPORT_ENOTCONN;
    default:
        break;
    }
    size_t left = size;
    while (left) {
        const uint32_t chunk = static_cast<uint32_t>(std::min<size_t>(left, STREAM_SEGMENT_SIZE));
        send_stream(*s, 0, s->cur_seq, 0, data + (size - left), chunk, true);
        s->cur_seq += chunk;
        left -= chunk;
    }
    return static_cast<int>(size);
}

int VportHub::recv(const VportSocketPtr &s, uint8_t *buf, size_t size, VportAddr *from, bool peek) {
    std::lock_guard lock(mutex);
    if (!s->stream) {
        if (s->dgrams.empty())
            return VPORT_EWOULDBLOCK;
        auto &[addr, data] = s->dgrams.front();
        const size_t n = std::min(size, data.size());
        if (n)
            memcpy(buf, data.data(), n);
        if (from)
            *from = addr;
        if (!peek) {
            s->dgram_bytes -= data.size() + DGRAM_PACKET_OVERHEAD;
            s->dgrams.pop_front();
        }
        return static_cast<int>(n);
    }

    if (!s->data_available) {
        switch (s->state) {
        case StreamState::Disconnected:
            return 0;
        case StreamState::Closed:
        case StreamState::Listening:
            return VPORT_ENOTCONN;
        default:
            return VPORT_EWOULDBLOCK;
        }
    }
    const size_t to_give = static_cast<size_t>(std::min<uint64_t>(s->data_available, size));
    size_t given = 0;
    uint64_t pos = s->data_beg_seq;
    while (given < to_give) {
        auto it = s->received.find(pos);
        if (it == s->received.end())
            break; // cannot happen, data_available only counts contiguous segments
        const size_t n = std::min(it->second.size(), to_give - given);
        memcpy(buf + given, it->second.data(), n);
        given += n;
        pos += n;
        if (peek)
            continue;
        if (n == it->second.size()) {
            s->received_size -= n + STREAM_SEGMENT_OVERHEAD;
            s->received.erase(it);
        } else {
            std::vector<uint8_t> rest(it->second.begin() + n, it->second.end());
            s->received_size -= n;
            s->received.erase(it);
            const auto [slot, inserted] = s->received.try_emplace(pos);
            if (!inserted)
                s->received_size -= slot->second.size() + STREAM_SEGMENT_OVERHEAD;
            slot->second = std::move(rest);
        }
    }
    if (!peek) {
        s->data_available -= given;
        s->data_beg_seq += given;
        // Drop retransmitted segments that overlap data already read
        for (auto it = s->received.begin(); it != s->received.end() && it->first < s->data_beg_seq;) {
            s->received_size -= it->second.size() + STREAM_SEGMENT_OVERHEAD;
            it = s->received.erase(it);
        }
    }
    if (from)
        *from = s->peer;
    return static_cast<int>(given);
}

void VportHub::local_addr(const VportSocketPtr &s, VportAddr &out) {
    const uint16_t port = udp_port();
    std::lock_guard lock(mutex);
    out.addr = s->bound_addr;
    out.port = s->registered ? port : 0;
    out.vport = s->vport;
}

int VportHub::peer_addr(const VportSocketPtr &s, VportAddr &out) {
    std::lock_guard lock(mutex);
    if (!s->stream || !s->peer.addr || !s->peer.vport)
        return VPORT_ENOTCONN;
    out = s->peer;
    return 0;
}

void VportHub::set_rcvbuf(const VportSocketPtr &s, int size) {
    std::lock_guard lock(mutex);
    if (size > 0)
        s->rcvbuf = static_cast<size_t>(size);
}

unsigned VportHub::poll_locked(const VportSocket &s) const {
    if (!s.stream)
        return (s.dgrams.empty() ? 0 : VPORT_POLL_IN) | VPORT_POLL_OUT;
    switch (s.state) {
    case StreamState::Connected:
        return (s.data_available ? VPORT_POLL_IN : 0) | VPORT_POLL_OUT;
    case StreamState::Listening:
        return s.backlog.empty() ? 0 : VPORT_POLL_IN;
    case StreamState::Disconnected:
        return VPORT_POLL_IN | VPORT_POLL_OUT | VPORT_POLL_HUP;
    default:
        return 0;
    }
}

unsigned VportHub::poll(const VportSocketPtr &s) {
    std::lock_guard lock(mutex);
    return poll_locked(*s);
}

bool VportHub::wait(const VportSocketPtr &s, unsigned events, std::chrono::microseconds timeout, const std::atomic<bool> *abort) {
    std::unique_lock lock(mutex);
    const auto deadline = Clock::now() + timeout;
    while (!(poll_locked(*s) & events)) {
        if (quit || (abort && *abort))
            return false;
        const auto now = Clock::now();
        if (now >= deadline)
            return false;
        // Short slices so that aborts are noticed
        cv.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(20)));
    }
    return true;
}

void VportHub::wake_all() {
    cv.notify_all();
}

void VportHub::send_ack(VportSocket &s) {
    uint64_t final_ack = s.data_beg_seq;
    for (auto it = s.received.find(final_ack); it != s.received.end(); it = s.received.find(final_ack))
        final_ack += it->second.size();
    s.data_available = final_ack - s.data_beg_seq;
    send_stream(s, TCP_ACK, 0, final_ack, nullptr, 0, false);
    cv.notify_all();
}

void VportHub::close_stream(VportSocket &s) {
    s.state = StreamState::Disconnected;
    for (auto it = streams.begin(); it != streams.end();) {
        const auto p = it->second.lock();
        if (!p || p.get() == &s)
            it = streams.erase(it);
        else
            ++it;
    }
    s.unacked.clear();
    cv.notify_all();
}

void VportHub::handle_connected(VportSocket &s, uint8_t flags, uint64_t seq, uint64_t ack, const uint8_t *payload, uint32_t length) {
    if (flags & TCP_ACK) {
        const auto now = Clock::now();
        s.retries = 0;
        for (auto it = s.unacked.begin(); it != s.unacked.end() && it->first < ack;) {
            if (now < it->second.due) {
                const auto actual = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.first_sent);
                if (s.rtt > actual)
                    s.rtt = std::max((actual + s.rtt) / 2, std::chrono::milliseconds(1));
            }
            it = s.unacked.erase(it);
        }
    }

    if (s.state == StreamState::Handshaking) {
        if (flags == (TCP_SYN | TCP_ACK)) {
            s.data_beg_seq = seq + 1;
            s.state = StreamState::Connected;
            LOG_INFO("RPCN P2P: stream vport {} connected to {} vport {}", s.vport, ip_str(s.peer.addr), s.peer.vport);
            send_ack(s);
        } else {
            LOG_WARN("RPCN P2P: unexpected stream flags 0x{:02X} while connecting", flags);
        }
        return;
    }
    if (s.state != StreamState::Connected)
        return;

    switch (flags) {
    case 0:
    case TCP_PSH:
    case TCP_ACK:
    case TCP_SYN:
    case TCP_SYN | TCP_ACK: {
        if (seq < s.data_beg_seq) {
            // Already received
            if (flags != TCP_ACK)
                send_ack(s);
            return;
        }
        if (length && !s.received.contains(seq)) {
            const uint64_t offset = seq - s.data_beg_seq;
            const size_t cost = length + STREAM_SEGMENT_OVERHEAD;
            const bool fits_window = offset <= s.rcvbuf && length <= s.rcvbuf - offset;
            const bool fits_queue = s.received_size < s.rcvbuf && cost <= s.rcvbuf - s.received_size;
            const bool next_expected = seq == s.data_beg_seq + s.data_available;
            if ((next_expected && (fits_window || !s.data_available)) || (fits_window && fits_queue)) {
                s.received.emplace(seq, std::vector<uint8_t>(payload, payload + length));
                s.received_size += cost;
            }
        }
        send_ack(s);
        return;
    }
    case TCP_RST:
    case TCP_FIN:
        LOG_INFO("RPCN P2P: stream vport {} closed by peer", s.vport);
        close_stream(s);
        return;
    default:
        LOG_WARN("RPCN P2P: unexpected stream flags 0x{:02X}", flags);
        return;
    }
}

void VportHub::handle_stream(uint32_t addr, uint16_t port, uint16_t dst_vport, const uint8_t *data, size_t size) {
    if (size < U2S_HEADER_SIZE || get_be<uint32_t>(data) != U2S_SIGNATURE)
        return;
    const uint32_t length = get_be<uint32_t>(data + 4);
    if (length != size - U2S_HEADER_SIZE)
        return;
    const uint64_t seq = get_be<uint64_t>(data + 8);
    const uint64_t ack = get_be<uint64_t>(data + 16);
    const uint16_t src_port = get_be<uint16_t>(data + 24);
    const uint16_t dst_port = get_be<uint16_t>(data + 26);
    const uint16_t checksum = get_be<uint16_t>(data + U2S_CHECKSUM_OFFSET);
    const uint8_t flags = data[30];
    if (dst_port != dst_vport) {
        LOG_WARN("RPCN P2P: stream packet with mismatched vports {} / {}", dst_port, dst_vport);
        return;
    }
    std::vector<uint8_t> copy(data, data + size);
    copy[U2S_CHECKSUM_OFFSET] = copy[U2S_CHECKSUM_OFFSET + 1] = 0;
    if (u2s_checksum(copy.data(), copy.size()) != checksum) {
        LOG_WARN("RPCN P2P: dropping stream packet with a bad checksum");
        return;
    }
    const uint8_t *payload = data + U2S_HEADER_SIZE;

    if (const auto it = streams.find(stream_key(addr, src_port, dst_port)); it != streams.end()) {
        if (const auto s = it->second.lock()) {
            s->peer.port = port; // follow the peer if its NAT mapping changed
            handle_connected(*s, flags, seq, ack, payload, length);
            return;
        }
        streams.erase(it);
    }

    if (const auto it = stream_vports.find(dst_port); it != stream_vports.end()) {
        for (const auto &weak : it->second) {
            const auto listener = weak.lock();
            if (!listener || listener->state != StreamState::Listening)
                continue;
            if (flags != TCP_SYN) {
                LOG_WARN("RPCN P2P: unexpected stream flags 0x{:02X} on listening vport {}", flags, dst_port);
                return;
            }
            if (listener->backlog.size() >= listener->max_backlog) {
                send_rst(addr, port, dst_port, src_port);
                return;
            }
            auto client = std::make_shared<VportSocket>(true);
            client->vport = listener->vport;
            client->rcvbuf = listener->rcvbuf;
            client->registered = true;
            client->peer = { addr, port, src_port };
            client->state = StreamState::Connected;
            client->data_beg_seq = seq + 1;
            const uint64_t our_seq = random_seq();
            client->cur_seq = our_seq + 1;
            streams[stream_key(addr, src_port, dst_port)] = client;
            send_stream(*client, TCP_SYN | TCP_ACK, our_seq, seq + 1, nullptr, 0, true);
            listener->backlog.push_back(client);
            LOG_INFO("RPCN P2P: incoming stream connection on vport {} from {}:{} vport {}", dst_port, ip_str(addr), port, src_port);
            cv.notify_all();
            return;
        }
    }

    if (flags != TCP_RST)
        send_rst(addr, port, dst_port, src_port);
}

void VportHub::on_packet(uint32_t addr, uint16_t port, const uint8_t *data, size_t size) {
    if (size < VPORT_HEADER_SIZE)
        return;
    const uint16_t dst_vport = get_le16(data);
    const uint16_t src_vport = get_le16(data + 2);
    const uint16_t flags = get_le16(data + 4);
    data += VPORT_HEADER_SIZE;
    size -= VPORT_HEADER_SIZE;

    std::lock_guard lock(mutex);
    if (flags & FLAG_P2P) {
        const auto it = dgram_vports.find(dst_vport);
        if (it == dgram_vports.end()) {
            LOG_WARN("RPCN P2P: datagram for unbound vport {}", dst_vport);
            return;
        }
        for (const auto &weak : it->second) {
            const auto s = weak.lock();
            if (!s)
                continue;
            const size_t cost = size + DGRAM_PACKET_OVERHEAD;
            if (s->dgram_bytes >= s->rcvbuf || cost > s->rcvbuf - s->dgram_bytes)
                continue;
            s->dgram_bytes += cost;
            s->dgrams.emplace_back(VportAddr{ addr, port, src_vport }, std::vector<uint8_t>(data, data + size));
        }
        cv.notify_all();
    } else if (flags & FLAG_P2PS) {
        handle_stream(addr, port, dst_vport, data, size);
    }
}

void VportHub::timer_loop() {
    std::vector<VportSocketPtr> to_close;
    while (!quit) {
        {
            std::unique_lock lock(mutex);
            cv.wait_for(lock, 10ms);
            if (quit)
                return;
            const auto now = Clock::now();
            std::vector<VportSocketPtr> sockets;
            for (const auto &[key, weak] : streams)
                if (auto s = weak.lock())
                    sockets.push_back(std::move(s));
            std::sort(sockets.begin(), sockets.end());
            sockets.erase(std::unique(sockets.begin(), sockets.end()), sockets.end());

            for (const auto &s : sockets) {
                bool increased = false;
                for (auto &[seq, msg] : s->unacked) {
                    if (msg.due > now)
                        continue;
                    if (!increased) {
                        // Late reply: back off once per round
                        s->retries++;
                        s->rtt += s->rtt / 10;
                        increased = true;
                    }
                    if (s->retries >= MAX_RETRIES)
                        break;
                    send_raw(msg.packet, s->peer.addr, s->peer.port);
                    msg.due = now + s->rtt;
                }
                if (s->retries >= MAX_RETRIES) {
                    LOG_WARN("RPCN P2P: stream vport {} to {} vport {}: too many retries, closing", s->vport, ip_str(s->peer.addr), s->peer.vport);
                    close_stream(*s);
                }
            }
        }
    }
}

} // namespace rpcn
