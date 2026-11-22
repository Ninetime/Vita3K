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

#include <module/module.h>

#include "../SceProcessmgr/SceProcessmgr.h"

#include <kernel/state.h>
#include <mem/functions.h>
#include <np/state.h>
#include <rpcn/client.h>
#include <rpcn/pb.h>
#include <util/log.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>

// Error codes as returned by the firmware's np_basic library
enum SceNpBasicError : uint32_t {
    SCE_NP_BASIC_ERROR_INVALID_ARGUMENT = 0x80551d02,
    SCE_NP_BASIC_ERROR_NOT_INITIALIZED = 0x80551d04,
    SCE_NP_BASIC_ERROR_ALREADY_INITIALIZED = 0x80551d05,
    SCE_NP_BASIC_ERROR_BUSY = 0x80551d09, // more than 20 in-game messages in 60 seconds
    SCE_NP_BASIC_ERROR_NOT_REGISTERED = 0x80551d0a, // no handler registered
};

namespace {

// sceNpBasicRegisterHandler copies these into the library's per-event handler table.
// Offset -> event: +4 -> 2, +8 -> 3, +0xC -> 8, +0x10 -> 4, +0x14 -> 6, +0x18 -> 10, +0x1C -> 13.
struct SceNpBasicEventHandlers {
    SceUInt32 sdkVersion;
    Ptr<void> friendListEventHandler;
    Ptr<void> friendOnlineStatusEventHandler;
    Ptr<void> blockListEventHandler;
    Ptr<void> friendGamePresenceEventHandler;
    Ptr<void> inGameDataMessageEventHandler; // (const SceNpId *from, const SceNpBasicInGameDataMessage *msg, void *userdata)
    Ptr<void> friendContextEventHandler;
    Ptr<void> extendedHandler; // only read when sdkVersion >= 0x01600000
};

// The in-game data message struct grew with SDK 2.00: {data[128], dataSize} before,
// {data[512], dataSize} after (np_basic picks the size from the main module SDK version).
constexpr uint32_t IN_GAME_MESSAGE_OLD_MAX = 0x80;
constexpr uint32_t IN_GAME_MESSAGE_NEW_MAX = 0x200;
constexpr uint32_t IN_GAME_MESSAGE_NEW_SDK = 0x02000000;

// np_basic event 6 payload: {SceNpId from; SceNpCommunicationId commId; pad; message}
constexpr uint32_t EVENT_MESSAGE_FROM = 0;
constexpr uint32_t EVENT_MESSAGE_COMM_ID = 0x24;
constexpr uint32_t EVENT_MESSAGE_DATA = 0x30;
constexpr uint32_t EVENT_MESSAGE_SIZE = 0x238;

// PS3 sceNpBasicSendMessage uses these too, so both platforms see the same message kind
constexpr uint32_t MESSAGE_MAIN_TYPE_GENERAL = 1;
constexpr uint32_t MESSAGE_GENERAL_SUBTYPE_NONE = 0;

constexpr size_t SEND_LIMIT_COUNT = 20;
constexpr auto SEND_LIMIT_WINDOW = std::chrono::seconds(60);

struct BasicState {
    std::mutex mutex;
    bool inited = false;
    bool registered = false;
    SceNpBasicEventHandlers handlers{};
    Address userdata = 0;
    std::string com_id; // "NPWR01781_00"
    std::deque<std::chrono::steady_clock::time_point> sent;
};

BasicState &basic_state() {
    static auto *state = new BasicState();
    return *state;
}

std::string com_id_string(const np::CommunicationID &id) {
    return fmt::format("{}_{:02}", std::string(id.data, strnlen(id.data, sizeof(id.data))), id.num);
}

bool new_message_layout(EmuEnvState &emuenv, SceUID thread_id, const char *export_name) {
    return static_cast<uint32_t>(CALL_EXPORT(sceKernelGetMainModuleSdkVersion)) >= IN_GAME_MESSAGE_NEW_SDK;
}

struct ReceivedMessage {
    std::string from;
    std::string com_id;
    std::vector<uint8_t> data;
};

// MessageReceived notification: sender npid (null terminated), u32 size, MessageDetails protobuf
std::optional<ReceivedMessage> parse_message(const std::vector<uint8_t> &raw) {
    const auto end = std::find(raw.begin(), raw.end(), 0);
    if (end == raw.end() || raw.end() - end < 5)
        return std::nullopt;
    ReceivedMessage msg;
    msg.from.assign(raw.begin(), end);
    uint32_t len;
    memcpy(&len, &*(end + 1), 4);
    const size_t start = (end - raw.begin()) + 5;
    if (start + len > raw.size())
        return std::nullopt;
    const rpcn::pb::Reader details(raw.data() + start, len);
    if (!details.ok())
        return std::nullopt;
    if (details.wrapped(3) != MESSAGE_MAIN_TYPE_GENERAL) {
        LOG_INFO("NP Basic: ignoring message of type {}/{} from {}", details.wrapped(3), details.wrapped(4), msg.from);
        return std::nullopt;
    }
    msg.com_id = details.string(1);
    msg.data = details.bytes(8);
    return msg;
}

void deliver_messages(EmuEnvState &emuenv, SceUID thread_id, const char *export_name) {
    const auto client = emuenv.np.rpcn;
    if (!client)
        return;
    const auto messages = client->take_messages();
    if (messages.empty())
        return;

    auto &state = basic_state();
    Address handler, userdata;
    std::string com_id;
    {
        std::lock_guard lock(state.mutex);
        handler = state.registered ? state.handlers.inGameDataMessageEventHandler.address() : 0;
        userdata = state.userdata;
        com_id = state.com_id;
    }
    const uint32_t max_size = new_message_layout(emuenv, thread_id, export_name) ? IN_GAME_MESSAGE_NEW_MAX : IN_GAME_MESSAGE_OLD_MAX;
    const ThreadStateRef thread = emuenv.kernel.get_thread(thread_id);

    for (const auto &notification : messages) {
        const auto msg = parse_message(notification.data);
        if (!msg)
            continue;
        // np_basic only hands over messages sent under the registered communication ID
        if (msg->com_id != com_id) {
            LOG_INFO("NP Basic: dropping message from {} for {} (registered {})", msg->from, msg->com_id, com_id);
            continue;
        }
        if (!handler) {
            LOG_INFO("NP Basic: no in-game data message handler, dropping message from {}", msg->from);
            continue;
        }
        const uint32_t size = static_cast<uint32_t>(std::min<size_t>(msg->data.size(), max_size));
        if (size < msg->data.size())
            LOG_WARN("NP Basic: message from {} truncated from {} to {} bytes", msg->from, msg->data.size(), size);
        LOG_TRACE("NP Basic: in-game data message from {} ({} bytes)", msg->from, size);

        const Address event = alloc(emuenv.mem, EVENT_MESSAGE_SIZE, "NpBasicInGameDataMessage");
        auto *base = Ptr<uint8_t>(event).get(emuenv.mem);
        memset(base, 0, EVENT_MESSAGE_SIZE);
        auto *from = reinterpret_cast<np::SceNpId *>(base + EVENT_MESSAGE_FROM);
        np::fill_server_npid(*from, msg->from);
        memcpy(base + EVENT_MESSAGE_COMM_ID, msg->com_id.data(), std::min<size_t>(msg->com_id.size(), 9));
        memcpy(base + EVENT_MESSAGE_DATA, msg->data.data(), size);
        memcpy(base + EVENT_MESSAGE_DATA + max_size, &size, 4);
        thread->call_guest(handler, { event + EVENT_MESSAGE_FROM, event + EVENT_MESSAGE_DATA, userdata });
        free(emuenv.mem, event);
    }
}

} // namespace

EXPORT(int, sceNpBasicCheckCallback) {
    if (!basic_state().inited)
        return RET_ERROR(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    deliver_messages(emuenv, thread_id, export_name);
    return 0;
}

EXPORT(int, sceNpBasicCheckIfPlayerIsBlocked) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicGetBlockListEntries) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicGetBlockListEntryCount) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicGetFriendContextState) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicGetFriendListEntries) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicGetFriendListEntryCount, int *nb_friends) {
    if (!nb_friends)
        return RET_ERROR(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);

    STUBBED("No friends");
    *nb_friends = 0;
    return 0;
}

EXPORT(int, sceNpBasicGetFriendOnlineStatus) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicGetFriendRequestEntries) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicGetFriendRequestEntryCount) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicGetGameJoiningPresence) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicGetGamePresenceOfFriend) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicGetPlaySessionLog) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicGetPlaySessionLogSize) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicGetRequestedFriendRequestEntries) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicGetRequestedFriendRequestEntryCount) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicInit, const void *option) {
    auto &state = basic_state();
    std::lock_guard lock(state.mutex);
    if (state.inited)
        return RET_ERROR(SCE_NP_BASIC_ERROR_ALREADY_INITIALIZED);
    state.inited = true;
    emuenv.np.app_stop_hooks.push_back([] {
        auto &state = basic_state();
        std::lock_guard lock(state.mutex);
        state.inited = false;
        state.registered = false;
        state.handlers = {};
        state.userdata = 0;
        state.com_id.clear();
        state.sent.clear();
    });
    return 0;
}

EXPORT(int, sceNpBasicJoinGameAckResponseSend) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicRecordPlaySessionLog) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicRegisterHandler, const SceNpBasicEventHandlers *handlers, const np::CommunicationID *commId, Ptr<void> userdata) {
    auto &state = basic_state();
    std::lock_guard lock(state.mutex);
    if (!state.inited)
        return RET_ERROR(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    if (!handlers)
        return RET_ERROR(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
    state.handlers = *handlers;
    if (handlers->sdkVersion < 0x01600000)
        state.handlers.extendedHandler = Ptr<void>();
    state.userdata = userdata.address();
    // Without an explicit ID the library uses the one given to sceNpInit
    state.com_id = com_id_string(commId ? *commId : emuenv.np.comm_id);
    state.registered = true;
    LOG_INFO("NP Basic: handlers registered for {} (sdk {}, in-game message handler {})", state.com_id,
        log_hex(handlers->sdkVersion), log_hex(handlers->inGameDataMessageEventHandler.address()));
    return 0;
}

EXPORT(int, sceNpBasicRegisterInGameDataMessageHandler) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicRegisterJoinGameAckHandler) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicSendInGameDataMessage, const np::SceNpId *to, const uint8_t *message) {
    auto &state = basic_state();
    std::string com_id;
    {
        std::lock_guard lock(state.mutex);
        if (!state.inited)
            return RET_ERROR(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
        if (!state.registered)
            return RET_ERROR(SCE_NP_BASIC_ERROR_NOT_REGISTERED);
        if (!to || !message)
            return RET_ERROR(SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);
        const auto now = std::chrono::steady_clock::now();
        while (!state.sent.empty() && now - state.sent.front() > SEND_LIMIT_WINDOW)
            state.sent.pop_front();
        if (state.sent.size() >= SEND_LIMIT_COUNT)
            return RET_ERROR(SCE_NP_BASIC_ERROR_BUSY);
        state.sent.push_back(now);
        com_id = state.com_id;
    }

    const uint32_t max_size = new_message_layout(emuenv, thread_id, export_name) ? IN_GAME_MESSAGE_NEW_MAX : IN_GAME_MESSAGE_OLD_MAX;
    uint32_t size;
    memcpy(&size, message + max_size, 4);
    if (size > max_size) {
        LOG_WARN("NP Basic: in-game data message of {} bytes clamped to {}", size, max_size);
        size = max_size;
    }
    const std::string target(to->handle.data, strnlen(to->handle.data, sizeof(to->handle.data)));

    const auto client = emuenv.np.rpcn;
    if (!client || !client->is_authenticated()) {
        LOG_WARN("NP Basic: not connected to RPCN, in-game data message to {} not sent", target);
        return 0;
    }
    LOG_TRACE("NP Basic: sending {} byte in-game data message to {} ({})", size, target, com_id);

    rpcn::pb::Writer details;
    details.string(1, com_id);
    details.wrapped(3, MESSAGE_MAIN_TYPE_GENERAL);
    details.wrapped(4, MESSAGE_GENERAL_SUBTYPE_NONE);
    details.bytes(8, message, size);
    rpcn::pb::Writer request;
    request.bytes(1, details.data().data(), details.data().size());
    request.string(2, target);

    std::vector<uint8_t> payload(4);
    const uint32_t len = static_cast<uint32_t>(request.data().size());
    memcpy(payload.data(), &len, 4);
    payload.insert(payload.end(), request.data().begin(), request.data().end());
    client->request_async(rpcn::CommandType::SendMessage, payload, [target](rpcn::CommandType, std::vector<uint8_t> reply) {
        if (reply.empty() || reply[0] != 0)
            LOG_WARN("NP Basic: in-game data message to {} failed ({})", target, reply.empty() ? -1 : static_cast<int>(reply[0]));
    });
    return 0;
}

EXPORT(int, sceNpBasicSetInGamePresence) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicTerm) {
    auto &state = basic_state();
    std::lock_guard lock(state.mutex);
    if (!state.inited)
        return RET_ERROR(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    state.inited = false;
    state.registered = false;
    state.handlers = {};
    state.sent.clear();
    return 0;
}

EXPORT(int, sceNpBasicUnregisterHandler) {
    auto &state = basic_state();
    std::lock_guard lock(state.mutex);
    if (!state.inited)
        return RET_ERROR(SCE_NP_BASIC_ERROR_NOT_INITIALIZED);
    state.registered = false;
    state.handlers = {};
    return 0;
}

EXPORT(int, sceNpBasicUnregisterInGameDataMessageHandler) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicUnregisterJoinGameAckHandler) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBasicUnsetInGamePresence) {
    return UNIMPLEMENTED();
}
