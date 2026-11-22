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

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

#include <mem/ptr.h>
#include <rtc/rtc.h>
#include <util/types.h>

#include <cstdint>
#include <cstring>

namespace np {

#define SCE_NP_ONLINEID_MIN_LENGTH 3
#define SCE_NP_ONLINEID_MAX_LENGTH 16

struct SceNpOnlineId {
    char data[SCE_NP_ONLINEID_MAX_LENGTH];
    char term;
    char dummy[3];
};
static_assert(sizeof(SceNpOnlineId) == 0x14, "SceNpOnlineId is an invalid size");

struct SceNpIdOptParam {
    SceUChar8 unknown[4];
    char platformType[4];
};
static_assert(sizeof(SceNpIdOptParam) == 0x8, "SceNpIdOptParam is an invalid size");

struct SceNpId {
    SceNpOnlineId handle;
    SceNpIdOptParam opt;
    SceInt8 isIdValid;
    SceUChar8 reserved[7];
};
static_assert(sizeof(SceNpId) == 0x24, "SceNpId is an invalid size");

struct CommunicationID {
    char data[9];
    char term;
    std::uint8_t num;
    char dummy;
};

// Online ID of the signed-in user, set at sign-in
inline std::string self_online_id;

// SceNpPlatformType values, as in the PS3 sceNpUtilGetPlatformType
enum SceNpPlatformType : uint32_t {
    SCE_NP_PLATFORM_TYPE_NONE = 0,
    SCE_NP_PLATFORM_TYPE_PS3 = 1,
    SCE_NP_PLATFORM_TYPE_PSP2 = 2,
    SCE_NP_PLATFORM_TYPE_PS4 = 3,
};

// Platforms of other users, from the NP server's user infos (RPCN UserInfo.platformType). Messages only
// carry the sender's name, so the platform learned from rooms is reused for them.
struct KnownPlatforms {
    std::mutex mutex;
    std::map<std::string, uint32_t, std::less<>> platforms;
};
inline KnownPlatforms known_platforms;

inline void remember_platform(std::string_view online_id, uint32_t platform) {
    if (online_id.empty() || platform == SCE_NP_PLATFORM_TYPE_NONE)
        return;
    std::lock_guard lock(known_platforms.mutex);
    known_platforms.platforms[std::string(online_id)] = platform;
}

inline uint32_t get_known_platform(std::string_view online_id) {
    if (!online_id.empty() && online_id == self_online_id)
        return SCE_NP_PLATFORM_TYPE_PSP2;
    std::lock_guard lock(known_platforms.mutex);
    const auto it = known_platforms.platforms.find(online_id);
    return it == known_platforms.platforms.end() ? SCE_NP_PLATFORM_TYPE_NONE : it->second;
}

// Platform part of SceNpId.opt, the same 4 characters as the PS3 sceNpUtilSetPlatformType writes
inline void set_npid_platform(SceNpId &id, uint32_t platform) {
    switch (platform) {
    case SCE_NP_PLATFORM_TYPE_PS3: memcpy(id.opt.platformType, "ps3\0", 4); break;
    case SCE_NP_PLATFORM_TYPE_PSP2: memcpy(id.opt.platformType, "psp2", 4); break;
    case SCE_NP_PLATFORM_TYPE_PS4: memcpy(id.opt.platformType, "ps4\0", 4); break;
    default: memset(id.opt.platformType, 0, 4); break;
    }
}

// NP IDs given out by the NP servers (room members, peers, message senders) are marked valid and carry
// the platform of the user's console, like on a real Vita: sceNpCmpNpId rejects invalid IDs, and games
// check the platform (SFxT measures a room host's connection differently for PS3 and PS Vita hosts).
inline void fill_server_npid(SceNpId &id, std::string_view online_id) {
    memset(&id, 0, sizeof(id));
    memcpy(id.handle.data, online_id.data(), std::min(online_id.size(), sizeof(id.handle.data)));
    id.isIdValid = 1;
    set_npid_platform(id, get_known_platform(online_id));
}

inline bool operator==(const CommunicationID &lhs, const CommunicationID &rhs) {
    return (strncmp(lhs.data, rhs.data, 9) == 0) && (lhs.num == rhs.num);
}

struct CommunicationConfig {
    Ptr<CommunicationID> comm_id;
    Ptr<void> unk0;
    Ptr<void> unk1;
};

#define SCE_NP_COMMUNICATION_PASSPHRASE_SIZE (128)
struct SceNpCommunicationPassphrase {
    SceUChar8 data[SCE_NP_COMMUNICATION_PASSPHRASE_SIZE];
};

#define SCE_NP_TUS_MAX_CTX_NUM (32)

enum SceNpTssStatusCodeType : SceInt32 {
    SCE_NP_TSS_STATUS_TYPE_OK = 0,
    SCE_NP_TSS_STATUS_TYPE_PARTIAL = 1,
    SCE_NP_TSS_STATUS_TYPE_NOT_MODIFIED = 2
};

struct SceNpTssDataStatus {
    SceRtcTick lastModified;
    SceInt32 statusCodeType;
    SceSize contentLength;
};
static_assert(sizeof(SceNpTssDataStatus) == 0x10);

constexpr auto SCE_NP_TROPHY_INVALID_TROPHY_ID = -1;

enum class NpTrophyError {
    TROPHY_ERROR_NONE = 0,
    TROPHY_CONTEXT_EXIST = 1,
    TROPHY_CONTEXT_FILE_NON_EXIST = 2,
    TROPHY_ID_INVALID = 3,
    TROPHY_ALREADY_UNLOCKED = 4,
    TROPHY_PLATINUM_IS_UNBREAKABLE = 5, // Platinum is unbreakable
};

} // namespace np
