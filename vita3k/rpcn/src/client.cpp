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

#include <rpcn/client.h>
#include <rpcn/stream.h>

#include <util/log.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>

#include <future>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#define CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSE_SOCKET close
#endif

namespace rpcn {

using namespace std::chrono_literals;

static SSL_CTX *as_ctx(void *p) { return static_cast<SSL_CTX *>(p); }
static SSL *as_ssl(void *p) { return static_cast<SSL *>(p); }

static std::string openssl_error() {
    char buf[256]{};
    ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
    return buf;
}

std::string derive_password(std::string_view user_password) {
    static constexpr std::string_view salt = "No matter where you go, everybody's connected.";
    uint8_t digest[32]{};
    if (PKCS5_PBKDF2_HMAC(user_password.data(), static_cast<int>(user_password.size()),
            reinterpret_cast<const unsigned char *>(salt.data()), static_cast<int>(salt.size()),
            200'000, EVP_sha3_256(), sizeof(digest), digest)
        != 1) {
        LOG_ERROR("RPCN: password derivation failed: {}", openssl_error());
        return {};
    }

    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out(sizeof(digest) * 2, '0');
    for (size_t i = 0; i < sizeof(digest); i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    return out;
}

Client::Client(Config config)
    : cfg(std::move(config)) {
#ifdef _WIN32
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

Client::~Client() {
    disconnect();
#ifdef _WIN32
    WSACleanup();
#endif
}

State Client::connect_and_login() {
    std::lock_guard lock(connect_mutex);

    if (current_state == State::Authenticated)
        return State::Authenticated;

    close_connection();

    if (!open_connection()) {
        close_connection();
        return current_state;
    }

    if (!login()) {
        close_connection();
        return current_state;
    }

    return current_state;
}

State Client::connect_only() {
    std::lock_guard lock(connect_mutex);
    close_connection();
    if (!open_connection())
        close_connection();
    return current_state;
}

void Client::disconnect() {
    std::lock_guard lock(connect_mutex);
    if (current_state == State::Authenticated) {
        // Polite goodbye, the server cleans up rooms/presence on its own otherwise
        request(CommandType::Terminate, {}, 1s);
    }
    close_connection();
    current_state = State::Disconnected;
}

bool Client::open_connection() {
    LOG_INFO("RPCN: connecting to {}", cfg.host);

    std::string hostname = cfg.host;
    uint16_t port = DEFAULT_PORT;
    if (const auto colon = cfg.host.rfind(':'); colon != std::string::npos) {
        hostname = cfg.host.substr(0, colon);
        const unsigned long parsed = std::strtoul(cfg.host.c_str() + colon + 1, nullptr, 10);
        if (parsed == 0 || parsed > 0xFFFF) {
            LOG_ERROR("RPCN: invalid port in host '{}'", cfg.host);
            current_state = State::FailureInput;
            return false;
        }
        port = static_cast<uint16_t>(parsed);
    }

    if (hostname.empty()) {
        LOG_ERROR("RPCN: no host configured");
        current_state = State::FailureInput;
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *result = nullptr;
    if (getaddrinfo(hostname.c_str(), nullptr, &hints, &result) != 0 || !result) {
        LOG_ERROR("RPCN: failed to resolve {}", hostname);
        current_state = State::FailureResolve;
        return false;
    }

    sockaddr_in addr = *reinterpret_cast<sockaddr_in *>(result->ai_addr);
    addr.sin_port = htons(port);
    server_addr = addr.sin_addr.s_addr;
    freeaddrinfo(result);

    const auto s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
    if (s == INVALID_SOCKET) {
#else
    if (s < 0) {
#endif
        current_state = State::FailureConnect;
        return false;
    }
    sock = static_cast<intptr_t>(s);

    if (::connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        LOG_ERROR("RPCN: failed to connect to {}:{}", hostname, port);
        current_state = State::FailureConnect;
        return false;
    }

    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&nodelay), sizeof(nodelay));

    sockaddr_in local{};
    socklen_t local_len = sizeof(local);
    if (getsockname(s, reinterpret_cast<sockaddr *>(&local), &local_len) == 0)
        local_addr = local.sin_addr.s_addr;

#ifdef _WIN32
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif

    // RPCN uses a self-signed certificate, like RPCS3 we don't verify it
    ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx) {
        LOG_ERROR("RPCN: SSL_CTX_new failed: {}", openssl_error());
        current_state = State::FailureSsl;
        return false;
    }
    SSL_CTX_set_min_proto_version(as_ctx(ssl_ctx), TLS1_2_VERSION);
    SSL_CTX_set_verify(as_ctx(ssl_ctx), SSL_VERIFY_NONE, nullptr);

    ssl = SSL_new(as_ctx(ssl_ctx));
    if (!ssl || SSL_set_fd(as_ssl(ssl), static_cast<int>(s)) != 1) {
        LOG_ERROR("RPCN: SSL setup failed: {}", openssl_error());
        current_state = State::FailureSsl;
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (true) {
        const int ret = SSL_connect(as_ssl(ssl));
        if (ret == 1)
            break;

        const int err = SSL_get_error(as_ssl(ssl), ret);
        if ((err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) || std::chrono::steady_clock::now() > deadline) {
            LOG_ERROR("RPCN: TLS handshake failed: {}", openssl_error());
            current_state = State::FailureSsl;
            return false;
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        timeval tv{ 0, 50'000 };
        select(static_cast<int>(s) + 1, err == SSL_ERROR_WANT_READ ? &fds : nullptr, err == SSL_ERROR_WANT_WRITE ? &fds : nullptr, nullptr, &tv);
    }

    LOG_INFO("RPCN: TLS handshake successful, waiting for protocol version");

    server_info_received = false;
    stop_thread = false;
    current_state = State::Connected;
    net_thread = std::thread(&Client::network_thread_main, this);

    const auto version_deadline = std::chrono::steady_clock::now() + 10s;
    while (!server_info_received && current_state == State::Connected) {
        if (std::chrono::steady_clock::now() > version_deadline) {
            LOG_ERROR("RPCN: timed out waiting for the server version");
            current_state = State::FailureOther;
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }

    if (!server_info_received) {
        current_state = State::FailureOther;
        return false;
    }

    if (server_version != PROTOCOL_VERSION) {
        LOG_ERROR("RPCN: server protocol version is {}, expected {}", server_version.load(), PROTOCOL_VERSION);
        current_state = State::FailureProtocol;
        return false;
    }

    LOG_INFO("RPCN: protocol version {} matches", PROTOCOL_VERSION);
    return true;
}

bool Client::login() {
    if (cfg.npid.empty()) {
        current_state = State::FailureLoginUsername;
        return false;
    }
    if (cfg.password.empty()) {
        current_state = State::FailureLoginPassword;
        return false;
    }

    StreamWriter req;
    // Console we emulate, so that the server can give it in our NP ID to other users
    req.put_string(cfg.npid).put_string(cfg.password).put_string(cfg.token).put_string("psp2");

    const auto reply = request(CommandType::Login, req.data);
    if (!reply) {
        current_state = State::FailureOther;
        return false;
    }

    StreamReader in(*reply);
    const auto error = static_cast<ErrorType>(in.get<uint8_t>());
    if (error != ErrorType::NoError) {
        LOG_ERROR("RPCN: login failed: {}", to_string(error));
        switch (error) {
        case ErrorType::LoginAlreadyLoggedIn: current_state = State::FailureLoginAlreadyLoggedIn; break;
        case ErrorType::LoginInvalidUsername: current_state = State::FailureLoginUsername; break;
        case ErrorType::LoginInvalidPassword: current_state = State::FailureLoginPassword; break;
        case ErrorType::LoginInvalidToken: current_state = State::FailureLoginToken; break;
        default: current_state = State::FailureLogin; break;
        }
        return false;
    }

    user_online_name = in.get_string(false);
    user_avatar_url = in.get_string(false);
    user_uid = in.get<int64_t>();

    std::vector<FriendInfo> friends_read;
    const uint32_t num_friends = in.get<uint32_t>();
    for (uint32_t i = 0; i < num_friends && !in.is_error(); i++) {
        FriendInfo info;
        info.npid = in.get_string(false);
        info.online = in.get<uint8_t>() != 0;
        // Presence: com id, title, status, comment, data (unused for now)
        in.get_com_id();
        in.get_string(true);
        in.get_string(true);
        in.get_string(true);
        in.get_rawdata();
        friends_read.push_back(std::move(info));
    }
    // Friend requests sent/received and blocked users follow; not needed yet

    if (in.is_error()) {
        LOG_ERROR("RPCN: malformed login reply");
        current_state = State::FailureOther;
        return false;
    }

    {
        std::lock_guard lock(friends_mutex);
        friend_list = std::move(friends_read);
    }

    current_state = State::Authenticated;
    LOG_INFO("RPCN: you are now logged in RPCN ({} | {})", cfg.npid, user_online_name);
    return true;
}

void Client::close_connection() {
    stop_thread = true;
    if (net_thread.joinable())
        net_thread.join();

    if (ssl) {
        SSL_free(as_ssl(ssl));
        ssl = nullptr;
    }
    if (ssl_ctx) {
        SSL_CTX_free(as_ctx(ssl_ctx));
        ssl_ctx = nullptr;
    }
    if (sock != -1) {
        CLOSE_SOCKET(static_cast<decltype(::socket(0, 0, 0))>(sock));
        sock = -1;
    }

    {
        std::lock_guard lock(send_mutex);
        send_queue.clear();
        send_partial.clear();
        send_offset = 0;
    }
    recv_buf.clear();
    fail_pending();
}

void Client::fail_pending() {
    std::map<uint64_t, Pending> to_fail;
    {
        std::lock_guard lock(pending_mutex);
        to_fail.swap(pending);
    }
    for (auto &[id, p] : to_fail) {
        if (p.callback)
            p.callback(p.command, {});
    }
}

void Client::network_thread_main() {
    while (!stop_thread) {
        if (!pump_io()) {
            const State st = current_state;
            if (st == State::Connected || st == State::Authenticated) {
                LOG_WARN("RPCN: connection lost");
                current_state = State::Disconnected;
            }
            fail_pending();
            return;
        }
    }
}

bool Client::pump_io() {
    SSL *s = as_ssl(ssl);
    bool progress = false;
    bool want_write = false;

    // Write side
    while (true) {
        if (send_partial.empty()) {
            std::lock_guard lock(send_mutex);
            if (send_queue.empty())
                break;
            send_partial = std::move(send_queue.front());
            send_queue.pop_front();
            send_offset = 0;
        }

        const int ret = SSL_write(s, send_partial.data() + send_offset, static_cast<int>(send_partial.size() - send_offset));
        if (ret <= 0) {
            const int err = SSL_get_error(s, ret);
            if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                want_write = true;
                break;
            }
            LOG_ERROR("RPCN: write failed: {}", openssl_error());
            return false;
        }

        progress = true;
        send_offset += ret;
        if (send_offset == send_partial.size()) {
            send_partial.clear();
            send_offset = 0;
        }
    }

    // Read side. The server may close the connection right after a reply (it does after account creation),
    // so packets read before the close are still handled.
    bool closed = false;
    uint8_t tmp[16384];
    while (true) {
        const int ret = SSL_read(s, tmp, sizeof(tmp));
        if (ret <= 0) {
            const int err = SSL_get_error(s, ret);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                break;
            if (err == SSL_ERROR_ZERO_RETURN)
                LOG_INFO("RPCN: server closed the connection");
            else
                LOG_ERROR("RPCN: read failed: {}", openssl_error());
            closed = true;
            break;
        }
        progress = true;
        recv_buf.insert(recv_buf.end(), tmp, tmp + ret);
    }

    // Parse complete packets
    size_t offset = 0;
    while (recv_buf.size() - offset >= HEADER_SIZE) {
        const uint8_t *h = recv_buf.data() + offset;
        uint16_t command;
        uint32_t size;
        uint64_t id;
        std::memcpy(&command, h + 1, sizeof(command));
        std::memcpy(&size, h + 3, sizeof(size));
        std::memcpy(&id, h + 7, sizeof(id));

        if (size < HEADER_SIZE) {
            LOG_ERROR("RPCN: invalid packet size {}", size);
            return false;
        }
        if (recv_buf.size() - offset < size)
            break;

        std::vector<uint8_t> data(h + HEADER_SIZE, h + size);
        if (!handle_packet(static_cast<PacketType>(h[0]), command, id, std::move(data)))
            return false;
        offset += size;
    }
    if (offset)
        recv_buf.erase(recv_buf.begin(), recv_buf.begin() + offset);

    if (closed)
        return false;

    if (progress || SSL_pending(s) > 0)
        return true;

    // Nothing to do: wait a bit for socket activity. New outgoing packets are picked up within 10ms.
    const auto fd = static_cast<decltype(::socket(0, 0, 0))>(sock);
    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_SET(fd, &rfds);
    if (want_write)
        FD_SET(fd, &wfds);
    timeval tv{ 0, 10'000 };
    select(static_cast<int>(fd) + 1, &rfds, want_write ? &wfds : nullptr, nullptr, &tv);
    return true;
}

bool Client::handle_packet(PacketType type, uint16_t command, uint64_t id, std::vector<uint8_t> data) {
    switch (type) {
    case PacketType::ServerInfo: {
        if (data.size() != 4) {
            LOG_ERROR("RPCN: invalid ServerInfo packet");
            return false;
        }
        uint32_t version;
        std::memcpy(&version, data.data(), sizeof(version));
        server_version = version;
        server_info_received = true;
        return true;
    }
    case PacketType::Reply: {
        if (data.empty()) {
            LOG_ERROR("RPCN: reply without result");
            return false;
        }
        Pending p;
        {
            std::lock_guard lock(pending_mutex);
            const auto it = pending.find(id);
            if (it == pending.end()) {
                // Fire-and-forget command, or a request whose waiter timed out
                if (data[0] != static_cast<uint8_t>(ErrorType::NoError))
                    LOG_WARN("RPCN: {} failed: {}", to_string(static_cast<CommandType>(command)), to_string(static_cast<ErrorType>(data[0])));
                return true;
            }
            p = std::move(it->second);
            pending.erase(it);
        }
        if (p.callback)
            p.callback(static_cast<CommandType>(command), std::move(data));
        return true;
    }
    case PacketType::Notification: {
        std::lock_guard lock(notif_mutex);
        if (static_cast<NotificationType>(command) == NotificationType::MessageReceived) {
            // Nobody may be reading them (NP Basic not in use), keep the newest only
            constexpr size_t MAX_QUEUED_MESSAGES = 64;
            if (messages.size() >= MAX_QUEUED_MESSAGES)
                messages.erase(messages.begin());
            messages.push_back({ NotificationType::MessageReceived, std::move(data) });
        } else {
            notifications.push_back({ static_cast<NotificationType>(command), std::move(data) });
        }
        return true;
    }
    case PacketType::Request:
    default:
        LOG_ERROR("RPCN: unexpected packet type {}", static_cast<int>(type));
        return false;
    }
}

uint64_t Client::queue_packet(CommandType command, const std::vector<uint8_t> &payload, ReplyCallback callback) {
    const uint64_t id = next_packet_id.fetch_add(1);
    const uint32_t size = static_cast<uint32_t>(HEADER_SIZE + payload.size());

    std::vector<uint8_t> packet(size);
    packet[0] = static_cast<uint8_t>(PacketType::Request);
    const auto cmd = static_cast<uint16_t>(command);
    std::memcpy(packet.data() + 1, &cmd, sizeof(cmd));
    std::memcpy(packet.data() + 3, &size, sizeof(size));
    std::memcpy(packet.data() + 7, &id, sizeof(id));
    if (!payload.empty())
        std::memcpy(packet.data() + HEADER_SIZE, payload.data(), payload.size());

    if (callback) {
        std::lock_guard lock(pending_mutex);
        pending.emplace(id, Pending{ command, std::move(callback) });
    }

    std::lock_guard lock(send_mutex);
    send_queue.push_back(std::move(packet));
    return id;
}

std::optional<std::vector<uint8_t>> Client::request(CommandType command, const std::vector<uint8_t> &payload, std::chrono::milliseconds timeout) {
    const State st = current_state;
    if (st != State::Connected && st != State::Authenticated)
        return std::nullopt;

    auto promise = std::make_shared<std::promise<std::vector<uint8_t>>>();
    auto future = promise->get_future();
    const uint64_t id = queue_packet(command, payload, [promise](CommandType, std::vector<uint8_t> reply) {
        promise->set_value(std::move(reply));
    });

    if (future.wait_for(timeout) != std::future_status::ready) {
        std::lock_guard lock(pending_mutex);
        pending.erase(id);
        LOG_ERROR("RPCN: {} timed out", to_string(command));
        return std::nullopt;
    }

    auto reply = future.get();
    if (reply.empty())
        return std::nullopt; // connection dropped
    return reply;
}

bool Client::request_async(CommandType command, const std::vector<uint8_t> &payload, ReplyCallback callback) {
    if (!is_authenticated())
        return false;
    queue_packet(command, payload, std::move(callback));
    return true;
}

std::vector<Notification> Client::take_notifications() {
    std::lock_guard lock(notif_mutex);
    std::vector<Notification> out;
    out.swap(notifications);
    return out;
}

std::vector<Notification> Client::take_messages() {
    std::lock_guard lock(notif_mutex);
    std::vector<Notification> out;
    out.swap(messages);
    return out;
}

std::vector<FriendInfo> Client::friends() const {
    std::lock_guard lock(friends_mutex);
    return friend_list;
}

std::optional<std::vector<uint16_t>> Client::get_server_list(const std::string &com_id) {
    StreamWriter req;
    req.put_com_id(com_id);
    const auto reply = request(CommandType::GetServerList, req.data);
    if (!reply)
        return std::nullopt;

    StreamReader in(*reply);
    const auto error = static_cast<ErrorType>(in.get<uint8_t>());
    if (error != ErrorType::NoError) {
        LOG_ERROR("RPCN: GetServerList failed: {}", to_string(error));
        return std::nullopt;
    }

    std::vector<uint16_t> servers(in.get<uint16_t>());
    for (auto &server : servers)
        server = in.get<uint16_t>();

    if (in.is_error())
        return std::nullopt;
    return servers;
}

std::optional<uint64_t> Client::get_network_time() {
    const auto reply = request(CommandType::GetNetworkTime, {});
    if (!reply)
        return std::nullopt;

    StreamReader in(*reply);
    if (static_cast<ErrorType>(in.get<uint8_t>()) != ErrorType::NoError)
        return std::nullopt;
    const uint64_t time = in.get<uint64_t>();
    if (in.is_error())
        return std::nullopt;
    return time;
}

const char *to_string(ErrorType error) {
    switch (error) {
    case ErrorType::NoError: return "NoError";
    case ErrorType::Malformed: return "Malformed";
    case ErrorType::Invalid: return "Invalid";
    case ErrorType::InvalidInput: return "InvalidInput";
    case ErrorType::TooSoon: return "TooSoon";
    case ErrorType::LoginError: return "LoginError";
    case ErrorType::LoginAlreadyLoggedIn: return "LoginAlreadyLoggedIn";
    case ErrorType::LoginInvalidUsername: return "LoginInvalidUsername";
    case ErrorType::LoginInvalidPassword: return "LoginInvalidPassword";
    case ErrorType::LoginInvalidToken: return "LoginInvalidToken";
    case ErrorType::CreationError: return "CreationError";
    case ErrorType::CreationExistingUsername: return "CreationExistingUsername";
    case ErrorType::CreationBannedEmailProvider: return "CreationBannedEmailProvider";
    case ErrorType::CreationExistingEmail: return "CreationExistingEmail";
    case ErrorType::RoomMissing: return "RoomMissing";
    case ErrorType::RoomAlreadyJoined: return "RoomAlreadyJoined";
    case ErrorType::RoomFull: return "RoomFull";
    case ErrorType::RoomPasswordMismatch: return "RoomPasswordMismatch";
    case ErrorType::RoomPasswordMissing: return "RoomPasswordMissing";
    case ErrorType::RoomGroupNoJoinLabel: return "RoomGroupNoJoinLabel";
    case ErrorType::RoomGroupFull: return "RoomGroupFull";
    case ErrorType::RoomGroupJoinLabelNotFound: return "RoomGroupJoinLabelNotFound";
    case ErrorType::RoomGroupMaxSlotMismatch: return "RoomGroupMaxSlotMismatch";
    case ErrorType::Unauthorized: return "Unauthorized";
    case ErrorType::DbFail: return "DbFail";
    case ErrorType::EmailFail: return "EmailFail";
    case ErrorType::NotFound: return "NotFound";
    case ErrorType::Blocked: return "Blocked";
    case ErrorType::AlreadyFriend: return "AlreadyFriend";
    case ErrorType::ScoreNotBest: return "ScoreNotBest";
    case ErrorType::ScoreInvalid: return "ScoreInvalid";
    case ErrorType::ScoreHasData: return "ScoreHasData";
    case ErrorType::CondFail: return "CondFail";
    case ErrorType::Unsupported: return "Unsupported";
    }
    return "Unknown";
}

const char *to_string(State state) {
    switch (state) {
    case State::Disconnected: return "Disconnected";
    case State::Connected: return "Connected";
    case State::Authenticated: return "Authenticated";
    case State::FailureInput: return "Invalid RPCN host";
    case State::FailureSsl: return "TLS error";
    case State::FailureResolve: return "Could not resolve the RPCN host";
    case State::FailureConnect: return "Could not connect to the RPCN server";
    case State::FailureProtocol: return "RPCN protocol version mismatch";
    case State::FailureLogin: return "Login failed";
    case State::FailureLoginAlreadyLoggedIn: return "Account already logged in elsewhere";
    case State::FailureLoginUsername: return "Invalid RPCN username";
    case State::FailureLoginPassword: return "Invalid RPCN password";
    case State::FailureLoginToken: return "Invalid RPCN token";
    case State::FailureOther: return "RPCN connection error";
    }
    return "Unknown";
}

const char *to_string(CommandType command) {
    static constexpr const char *names[] = {
        "Login", "Terminate", "Create", "Delete", "SendToken", "SendResetToken", "ResetPassword", "ResetState",
        "AddFriend", "RemoveFriend", "AddBlock", "RemoveBlock", "GetServerList", "GetWorldList", "CreateRoom",
        "JoinRoom", "LeaveRoom", "SearchRoom", "GetRoomDataExternalList", "SetRoomDataExternal", "GetRoomDataInternal",
        "SetRoomDataInternal", "GetRoomMemberDataInternal", "SetRoomMemberDataInternal", "SetUserInfo", "PingRoomOwner",
        "SendRoomMessage", "RequestSignalingInfos", "RequestTicket", "SendMessage", "GetBoardInfos", "RecordScore",
        "RecordScoreData", "GetScoreData", "GetScoreRange", "GetScoreFriends", "GetScoreNpid", "GetNetworkTime",
        "TusSetMultiSlotVariable", "TusGetMultiSlotVariable", "TusGetMultiUserVariable", "TusGetFriendsVariable",
        "TusAddAndGetVariable", "TusTryAndSetVariable", "TusDeleteMultiSlotVariable", "TusSetData", "TusGetData",
        "TusGetMultiSlotDataStatus", "TusGetMultiUserDataStatus", "TusGetFriendsDataStatus", "TusDeleteMultiSlotData",
        "SetPresence", "CreateRoomGUI", "JoinRoomGUI", "LeaveRoomGUI", "GetRoomListGUI", "SetRoomSearchFlagGUI",
        "GetRoomSearchFlagGUI", "SetRoomInfoGUI", "GetRoomInfoGUI", "QuickMatchGUI", "SearchJoinRoomGUI",
        "GetRoomMemberDataExternalList", "UnlockTrophy", "SyncTrophies", "DeleteTrophies", "GetTss",
        "GetLobbyInfoList", "JoinLobby", "LeaveLobby", "SetLobbyMemberDataInternal", "GetLobbyMemberDataInternalList",
        "SendLobbyChatMessage", "SendLobbyInvitation"
    };
    const auto index = static_cast<size_t>(command);
    return index < std::size(names) ? names[index] : "Unknown";
}

} // namespace rpcn
