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

// Platform-neutral client for the RPCN server. It only deals in host types and raw
// payloads; converting to and from Vita guest structures is done by the HLE modules.

#include <rpcn/types.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace rpcn {

struct Config {
    std::string host; // "hostname" or "hostname:port"
    std::string npid;
    std::string password; // already derived, see derive_password()
    std::string token;
};

// Same derivation RPCS3 applies to the password the user types:
// PBKDF2-HMAC-SHA3-256, 200000 iterations, uppercase hex.
std::string derive_password(std::string_view user_password);

struct Notification {
    NotificationType type;
    std::vector<uint8_t> data;
};

struct FriendInfo {
    std::string npid;
    bool online = false;
};

// Reply payload, starting with the ErrorType byte. Empty if the connection dropped.
using ReplyCallback = std::function<void(CommandType command, std::vector<uint8_t> reply)>;

class Client {
public:
    explicit Client(Config config);
    ~Client();

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    // Blocking. Connects (TLS + protocol check) and logs in if not already done.
    State connect_and_login();
    void disconnect();

    State state() const { return current_state.load(); }
    bool is_authenticated() const { return current_state.load() == State::Authenticated; }

    // Sends a request and waits for its reply. Returns nullopt on timeout/disconnect.
    std::optional<std::vector<uint8_t>> request(CommandType command, const std::vector<uint8_t> &payload,
        std::chrono::milliseconds timeout = std::chrono::seconds(10));

    // Sends a request; the callback runs on the network thread when the reply arrives.
    bool request_async(CommandType command, const std::vector<uint8_t> &payload, ReplyCallback callback);

    // Drains server notifications (room events, messages, ...) received so far.
    std::vector<Notification> take_notifications();

    // Simple commands
    std::optional<std::vector<uint16_t>> get_server_list(const std::string &com_id);
    std::optional<uint64_t> get_network_time();

    // Account information, valid once authenticated
    const std::string &npid() const { return cfg.npid; }
    const std::string &online_name() const { return user_online_name; }
    const std::string &avatar_url() const { return user_avatar_url; }
    int64_t user_id() const { return user_uid; }
    std::vector<FriendInfo> friends() const;
    uint32_t local_ipv4() const { return local_addr.load(); } // network byte order

private:
    struct Pending {
        CommandType command;
        ReplyCallback callback;
    };

    bool open_connection();
    bool login();
    void close_connection();
    void network_thread_main();
    bool pump_io();
    bool handle_packet(PacketType type, uint16_t command, uint64_t id, std::vector<uint8_t> data);
    uint64_t queue_packet(CommandType command, const std::vector<uint8_t> &payload, ReplyCallback callback);
    void fail_pending();

    Config cfg;
    std::atomic<State> current_state{ State::Disconnected };

    // Connection objects, owned by the network thread once it runs
    std::mutex connect_mutex;
    intptr_t sock = -1;
    void *ssl_ctx = nullptr; // SSL_CTX*
    void *ssl = nullptr; // SSL*
    std::thread net_thread;
    std::atomic<bool> stop_thread{ false };
    std::atomic<uint32_t> local_addr{ 0 };

    // Protocol handshake
    std::atomic<bool> server_info_received{ false };
    std::atomic<uint32_t> server_version{ 0 };

    // Outgoing
    std::mutex send_mutex;
    std::deque<std::vector<uint8_t>> send_queue;
    std::vector<uint8_t> send_partial; // packet being written
    size_t send_offset = 0;

    // Incoming
    std::vector<uint8_t> recv_buf;
    std::mutex pending_mutex;
    std::map<uint64_t, Pending> pending;
    std::atomic<uint64_t> next_packet_id{ 1 };

    std::mutex notif_mutex;
    std::vector<Notification> notifications;

    // Account
    std::string user_online_name;
    std::string user_avatar_url;
    int64_t user_uid = 0;
    mutable std::mutex friends_mutex;
    std::vector<FriendInfo> friend_list;
};

} // namespace rpcn
