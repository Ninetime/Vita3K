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

// PS Vita NP Matching2 guest structures.
// Layouts follow the PS3 library (same field order) in little-endian, except that Vita embeds a
// plain SceNpId where PS3 uses SceNpUserInfo2. Offsets marked "verified" were checked against
// the firmware's np_matching2.suprx (request validators and the member data builder).

#include <mem/ptr.h>
#include <np/common.h>

#include <cstdint>

namespace np2 {

using ServerId = uint16_t;
using WorldId = uint32_t;
using LobbyId = uint64_t;
using RoomId = uint64_t;
using RoomMemberId = uint16_t;
using ContextId = uint16_t;
using RequestId = uint32_t;
using AttributeId = uint16_t;
using FlagAttr = uint32_t;
using M2Event = uint16_t;

// Error codes: same low byte as PS3 (0x800223xx), Vita facility 0x80550Cxx
enum Error : uint32_t {
    ERROR_OUT_OF_MEMORY = 0x80550C01,
    ERROR_ALREADY_INITIALIZED = 0x80550C02,
    ERROR_NOT_INITIALIZED = 0x80550C03,
    ERROR_CONTEXT_MAX = 0x80550C04,
    ERROR_CONTEXT_ALREADY_EXISTS = 0x80550C05,
    ERROR_CONTEXT_NOT_FOUND = 0x80550C06,
    ERROR_CONTEXT_ALREADY_STARTED = 0x80550C07,
    ERROR_CONTEXT_NOT_STARTED = 0x80550C08,
    ERROR_SERVER_NOT_FOUND = 0x80550C09,
    ERROR_INVALID_ARGUMENT = 0x80550C0A,
    ERROR_INVALID_CONTEXT_ID = 0x80550C0B,
    ERROR_INVALID_SERVER_ID = 0x80550C0C,
    ERROR_INVALID_WORLD_ID = 0x80550C0D,
    ERROR_INVALID_ROOM_ID = 0x80550C0F,
    ERROR_INVALID_MEMBER_ID = 0x80550C10,
    ERROR_INVALID_ATTRIBUTE_ID = 0x80550C11,
    ERROR_INVALID_MAX_SLOT = 0x80550C14,
    ERROR_REQUEST_NOT_FOUND = 0x80550C30,
    ERROR_ROOM_NOT_FOUND = 0x80550C35,
    ERROR_ROOM_MEMBER_NOT_FOUND = 0x80550C36,

    // Server errors delivered through callbacks: PS3 0x80022Bxx -> Vita 0x80550Exx (same low byte)
    SERVER_ERROR_FORBIDDEN = 0x80550E07,
    SERVER_ERROR_NO_SUCH_ROOM = 0x80550E13,
    SERVER_ERROR_PASSWORD_MISMATCH = 0x80550E17,
    SERVER_ERROR_ROOM_FULL = 0x80550E19,
    SERVER_ERROR_ALREADY_JOINED = 0x80550E30,
};

enum : M2Event {
    REQUEST_EVENT_GetServerInfo = 0x0001,
    REQUEST_EVENT_GetWorldInfoList = 0x0002,
    REQUEST_EVENT_GetRoomMemberDataExternalList = 0x0003,
    REQUEST_EVENT_SetRoomDataExternal = 0x0004,
    REQUEST_EVENT_GetRoomDataExternalList = 0x0005,
    REQUEST_EVENT_SetUserInfo = 0x0007,
    REQUEST_EVENT_CreateJoinRoom = 0x0101,
    REQUEST_EVENT_JoinRoom = 0x0102,
    REQUEST_EVENT_LeaveRoom = 0x0103,
    REQUEST_EVENT_GrantRoomOwner = 0x0104,
    REQUEST_EVENT_KickoutRoomMember = 0x0105,
    REQUEST_EVENT_SearchRoom = 0x0106,
    REQUEST_EVENT_SendRoomChatMessage = 0x0107,
    REQUEST_EVENT_SendRoomMessage = 0x0108,
    REQUEST_EVENT_SetRoomDataInternal = 0x0109,
    REQUEST_EVENT_GetRoomDataInternal = 0x010A,
    REQUEST_EVENT_SetRoomMemberDataInternal = 0x010B,
    REQUEST_EVENT_GetRoomMemberDataInternal = 0x010C,
    REQUEST_EVENT_SetSignalingOptParam = 0x010D,
    REQUEST_EVENT_SignalingGetPingInfo = 0x0E01,

    ROOM_EVENT_MemberJoined = 0x1101,
    ROOM_EVENT_MemberLeft = 0x1102,
    ROOM_EVENT_Kickedout = 0x1103,
    ROOM_EVENT_RoomDestroyed = 0x1104,
    ROOM_EVENT_RoomOwnerChanged = 0x1105,
    ROOM_EVENT_UpdatedRoomDataInternal = 0x1106,
    ROOM_EVENT_UpdatedRoomMemberDataInternal = 0x1107,
    ROOM_EVENT_UpdatedSignalingOptParam = 0x1108,

    SIGNALING_EVENT_Dead = 0x5101,
    SIGNALING_EVENT_Established = 0x5102,
    SIGNALING_EVENT_NetInfoResult = 0x5103,

    CONTEXT_EVENT_StartOver = 0x6F01,
    CONTEXT_EVENT_Start = 0x6F02,
    CONTEXT_EVENT_Stop = 0x6F03,
};

enum : uint8_t {
    EVENT_CAUSE_LEAVE_ACTION = 1,
    EVENT_CAUSE_KICKOUT_ACTION = 2,
    EVENT_CAUSE_GRANT_OWNER_ACTION = 3,
    EVENT_CAUSE_SERVER_OPERATION = 4,
    EVENT_CAUSE_MEMBER_DISAPPEARED = 5,
    EVENT_CAUSE_SERVER_INTERNAL = 6,
    EVENT_CAUSE_CONNECTION_ERROR = 7,
    EVENT_CAUSE_NP_SIGNED_OUT = 8,
    EVENT_CAUSE_SYSTEM_ERROR = 9,
    EVENT_CAUSE_CONTEXT_ERROR = 10,
    EVENT_CAUSE_CONTEXT_ACTION = 11,
};

enum : uint8_t {
    SERVER_STATUS_AVAILABLE = 1,
};

struct IntAttr {
    AttributeId id;
    uint8_t padding[2];
    uint32_t num;
};
static_assert(sizeof(IntAttr) == 8); // verified

struct BinAttr {
    AttributeId id;
    uint8_t padding[2];
    Ptr<uint8_t> ptr;
    uint32_t size;
};
static_assert(sizeof(BinAttr) == 12); // verified

struct RangeFilter {
    uint32_t startIndex;
    uint32_t max;
};

struct IntSearchFilter {
    uint8_t searchOperator;
    uint8_t padding[3];
    IntAttr attr;
};
static_assert(sizeof(IntSearchFilter) == 12);

struct BinSearchFilter {
    uint8_t searchOperator;
    uint8_t padding[3];
    BinAttr attr;
};
static_assert(sizeof(BinSearchFilter) == 16);

struct Range {
    uint32_t startIndex;
    uint32_t total;
    uint32_t size;
};

struct Server {
    ServerId serverId;
    uint8_t status;
    uint8_t padding;
};

// Vita prefixes the PS3 layout with the server ID (SFxT reads worldId at +4 of 0x40-byte entries)
struct World {
    ServerId serverId;
    uint8_t padding0[2];
    WorldId worldId;
    uint32_t numOfLobby;
    uint32_t maxNumOfTotalLobbyMember;
    uint32_t curNumOfTotalLobbyMember;
    uint32_t curNumOfRoom;
    uint32_t curNumOfTotalRoomMember;
    uint8_t withEntitlementId;
    char entitlementId[32];
    uint8_t padding[3];
};
static_assert(sizeof(World) == 0x40); // verified
static_assert(offsetof(World, worldId) == 4); // verified

struct GroupLabel {
    uint8_t data[8];
};

struct RoomGroupConfig {
    uint32_t slotNum;
    uint8_t withLabel;
    GroupLabel label;
    uint8_t withPassword;
    uint8_t padding[2];
};

struct RoomGroup {
    uint8_t groupId;
    uint8_t withPassword;
    uint8_t withLabel;
    uint8_t padding;
    GroupLabel label;
    uint32_t slotNum;
    uint32_t curGroupMemberNum;
};
static_assert(sizeof(RoomGroup) == 20);

struct RoomMemberBinAttrInternal {
    uint64_t updateDate;
    BinAttr data;
    uint8_t padding[4];
};
static_assert(sizeof(RoomMemberBinAttrInternal) == 0x18); // verified

struct RoomMemberDataInternal {
    Ptr<RoomMemberDataInternal> next;
    np::SceNpId npId;
    uint64_t joinDate;
    RoomMemberId memberId;
    uint8_t teamId;
    uint8_t padding1;
    Ptr<RoomGroup> roomGroup;
    uint8_t natType;
    uint8_t padding2[3];
    FlagAttr flagAttr;
    Ptr<RoomMemberBinAttrInternal> roomMemberBinAttrInternal;
    uint32_t roomMemberBinAttrInternalNum;
};
static_assert(offsetof(RoomMemberDataInternal, joinDate) == 0x28); // verified
static_assert(offsetof(RoomMemberDataInternal, memberId) == 0x30); // verified
static_assert(offsetof(RoomMemberDataInternal, roomGroup) == 0x34); // verified
static_assert(offsetof(RoomMemberDataInternal, natType) == 0x38); // verified
static_assert(offsetof(RoomMemberDataInternal, flagAttr) == 0x3C); // verified
static_assert(offsetof(RoomMemberDataInternal, roomMemberBinAttrInternal) == 0x40); // verified
static_assert(sizeof(RoomMemberDataInternal) == 0x48);

struct RoomMemberDataInternalList {
    Ptr<RoomMemberDataInternal> members;
    uint32_t membersNum;
    Ptr<RoomMemberDataInternal> me;
    Ptr<RoomMemberDataInternal> owner;
};

struct RoomBinAttrInternal {
    uint64_t updateDate;
    RoomMemberId updateMemberId;
    uint8_t padding[2];
    BinAttr data;
};
static_assert(sizeof(RoomBinAttrInternal) == 24);

// Vita uses the newer (PS4 style) layout, not PS3's. Checked against SFxT's own code: it reads
// maxSlot/curMemberNum at +4/+6, roomId +0x18, owner +0x38, int attrs +0x48, bin attrs +0x58.
struct RoomDataExternal {
    Ptr<RoomDataExternal> next;
    uint16_t maxSlot;
    uint16_t curMemberNum;
    ServerId serverId;
    uint8_t padding1[2];
    WorldId worldId;
    LobbyId lobbyId;
    RoomId roomId;
    uint64_t passwordSlotMask;
    uint64_t joinedSlotMask;
    uint16_t publicSlotNum;
    uint16_t privateSlotNum;
    uint16_t openPublicSlotNum;
    uint16_t openPrivateSlotNum;
    Ptr<np::SceNpId> owner;
    Ptr<RoomGroup> roomGroup;
    uint32_t roomGroupNum;
    FlagAttr flagAttr;
    Ptr<IntAttr> roomSearchableIntAttrExternal;
    uint32_t roomSearchableIntAttrExternalNum;
    Ptr<BinAttr> roomSearchableBinAttrExternal;
    uint32_t roomSearchableBinAttrExternalNum;
    Ptr<BinAttr> roomBinAttrExternal;
    uint32_t roomBinAttrExternalNum;
};
static_assert(offsetof(RoomDataExternal, maxSlot) == 4); // verified
static_assert(offsetof(RoomDataExternal, roomId) == 0x18); // verified
static_assert(offsetof(RoomDataExternal, owner) == 0x38); // verified
static_assert(offsetof(RoomDataExternal, roomSearchableIntAttrExternal) == 0x48); // verified
static_assert(offsetof(RoomDataExternal, roomBinAttrExternal) == 0x58); // verified
static_assert(sizeof(RoomDataExternal) == 0x60);

// Only roomId (+0x10) and memberList.me (+0x38) are verified (all SFxT reads); the fields in
// between follow the external layout (joinedSlotMask added), the rest keeps PS3 order.
struct RoomDataInternal {
    ServerId serverId;
    uint8_t padding1[2];
    WorldId worldId;
    LobbyId lobbyId;
    RoomId roomId;
    uint64_t passwordSlotMask;
    uint64_t joinedSlotMask;
    uint32_t maxSlot;
    uint8_t padding2[4];
    RoomMemberDataInternalList memberList;
    Ptr<RoomGroup> roomGroup;
    uint32_t roomGroupNum;
    FlagAttr flagAttr;
    Ptr<RoomBinAttrInternal> roomBinAttrInternal;
    uint32_t roomBinAttrInternalNum;
};
static_assert(offsetof(RoomDataInternal, roomId) == 0x10); // verified
static_assert(offsetof(RoomDataInternal, memberList) + offsetof(RoomMemberDataInternalList, me) == 0x38); // verified

struct SignalingOptParam {
    uint8_t type;
    uint8_t flag;
    RoomMemberId hubMemberId;
    uint8_t reserved2[4];
};

struct RequestOptParam {
    Ptr<void> cbFunc;
    Ptr<void> cbFuncArg;
    uint32_t timeout;
    uint16_t appReqId;
    uint8_t padding[2];
};

struct GetWorldInfoListRequest {
    ServerId serverId;
};

struct GetWorldInfoListResponse {
    Ptr<World> world;
    uint32_t worldNum;
};

struct CreateJoinRoomRequest {
    WorldId worldId;
    uint8_t padding1[4];
    LobbyId lobbyId;
    uint32_t maxSlot;
    FlagAttr flagAttr;
    Ptr<BinAttr> roomBinAttrInternal;
    uint32_t roomBinAttrInternalNum;
    Ptr<IntAttr> roomSearchableIntAttrExternal;
    uint32_t roomSearchableIntAttrExternalNum;
    Ptr<BinAttr> roomSearchableBinAttrExternal;
    uint32_t roomSearchableBinAttrExternalNum;
    Ptr<BinAttr> roomBinAttrExternal;
    uint32_t roomBinAttrExternalNum;
    Ptr<uint8_t> roomPassword; // SceNpMatching2SessionPassword (8 bytes)
    Ptr<RoomGroupConfig> groupConfig;
    uint32_t groupConfigNum;
    Ptr<uint64_t> passwordSlotMask;
    Ptr<np::SceNpId> allowedUser;
    uint32_t allowedUserNum;
    Ptr<np::SceNpId> blockedUser;
    uint32_t blockedUserNum;
    Ptr<GroupLabel> joinRoomGroupLabel;
    Ptr<BinAttr> roomMemberBinAttrInternal;
    uint32_t roomMemberBinAttrInternalNum;
    uint8_t teamId;
    uint8_t padding2[3];
    Ptr<SignalingOptParam> sigOptParam;
};
static_assert(offsetof(CreateJoinRoomRequest, maxSlot) == 0x10); // verified
static_assert(offsetof(CreateJoinRoomRequest, roomBinAttrInternal) == 0x18); // verified
static_assert(offsetof(CreateJoinRoomRequest, roomSearchableIntAttrExternal) == 0x20); // verified
static_assert(offsetof(CreateJoinRoomRequest, roomSearchableBinAttrExternal) == 0x28); // verified
static_assert(offsetof(CreateJoinRoomRequest, roomBinAttrExternal) == 0x30); // verified
static_assert(offsetof(CreateJoinRoomRequest, allowedUserNum) == 0x4C); // verified
static_assert(offsetof(CreateJoinRoomRequest, roomMemberBinAttrInternal) == 0x5C); // verified
static_assert(offsetof(CreateJoinRoomRequest, roomMemberBinAttrInternalNum) == 0x60); // verified

struct CreateJoinRoomResponse {
    Ptr<RoomDataInternal> roomDataInternal;
};

struct JoinRoomRequest {
    RoomId roomId;
    Ptr<uint8_t> roomPassword;
    Ptr<GroupLabel> joinRoomGroupLabel;
    Ptr<BinAttr> roomMemberBinAttrInternal;
    uint32_t roomMemberBinAttrInternalNum;
    uint8_t optData[16]; // SceNpMatching2PresenceOptionData data
    uint32_t optDataLen;
    uint8_t teamId;
    uint8_t padding[3];
};

struct JoinRoomResponse {
    Ptr<RoomDataInternal> roomDataInternal;
};

struct LeaveRoomRequest {
    RoomId roomId;
    uint8_t optData[16];
    uint32_t optDataLen;
    uint8_t padding[4];
};

struct KickoutRoomMemberRequest {
    RoomId roomId;
    RoomMemberId target;
    uint8_t blockKickFlag;
    uint8_t padding;
    uint8_t optData[16];
    uint32_t optDataLen;
};

struct SearchRoomRequest {
    int32_t option;
    WorldId worldId;
    LobbyId lobbyId;
    RangeFilter rangeFilter;
    FlagAttr flagFilter;
    FlagAttr flagAttr;
    Ptr<IntSearchFilter> intFilter;
    uint32_t intFilterNum;
    Ptr<BinSearchFilter> binFilter;
    uint32_t binFilterNum;
    Ptr<AttributeId> attrId;
    uint32_t attrIdNum;
};

struct SearchRoomResponse {
    Range range;
    Ptr<RoomDataExternal> roomDataExternal;
};

struct SetRoomDataExternalRequest {
    RoomId roomId;
    Ptr<IntAttr> roomSearchableIntAttrExternal;
    uint32_t roomSearchableIntAttrExternalNum;
    Ptr<BinAttr> roomSearchableBinAttrExternal;
    uint32_t roomSearchableBinAttrExternalNum;
    Ptr<BinAttr> roomBinAttrExternal;
    uint32_t roomBinAttrExternalNum;
};

struct GetRoomDataExternalListRequest {
    Ptr<RoomId> roomId;
    uint32_t roomIdNum;
    Ptr<AttributeId> attrId;
    uint32_t attrIdNum;
};

struct GetRoomDataExternalListResponse {
    Ptr<RoomDataExternal> roomDataExternal;
    uint32_t roomDataExternalNum;
};

struct SetRoomDataInternalRequest {
    RoomId roomId;
    FlagAttr flagFilter;
    FlagAttr flagAttr;
    Ptr<BinAttr> roomBinAttrInternal;
    uint32_t roomBinAttrInternalNum;
    Ptr<void> passwordConfig;
    uint32_t passwordConfigNum;
    Ptr<uint64_t> passwordSlotMask;
    Ptr<RoomMemberId> ownerPrivilegeRank;
    uint32_t ownerPrivilegeRankNum;
    uint8_t padding[4];
};

struct SetRoomMemberDataInternalRequest {
    RoomId roomId;
    RoomMemberId memberId;
    uint8_t teamId;
    uint8_t padding[5];
    FlagAttr flagFilter;
    FlagAttr flagAttr;
    Ptr<BinAttr> roomMemberBinAttrInternal;
    uint32_t roomMemberBinAttrInternalNum;
};

// Room event payloads
struct RoomMemberUpdateInfo {
    Ptr<RoomMemberDataInternal> roomMemberDataInternal;
    uint8_t eventCause;
    uint8_t padding[3];
    uint8_t optData[16];
    uint32_t optDataLen;
};

struct RoomUpdateInfo {
    uint8_t eventCause;
    uint8_t padding[3];
    int32_t errorCode;
    uint8_t optData[16];
    uint32_t optDataLen;
};

struct RoomDataInternalUpdateInfo {
    Ptr<RoomDataInternal> newRoomDataInternal;
    Ptr<FlagAttr> newFlagAttr;
    Ptr<FlagAttr> prevFlagAttr;
    Ptr<uint64_t> newRoomPasswordSlotMask;
    Ptr<uint64_t> prevRoomPasswordSlotMask;
    Ptr<Ptr<RoomGroup>> newRoomGroup;
    uint32_t newRoomGroupNum;
    Ptr<Ptr<RoomBinAttrInternal>> newRoomBinAttrInternal;
    uint32_t newRoomBinAttrInternalNum;
};

struct RoomMemberDataInternalUpdateInfo {
    Ptr<RoomMemberDataInternal> newRoomMemberDataInternal;
    Ptr<FlagAttr> newFlagAttr;
    Ptr<FlagAttr> prevFlagAttr;
    Ptr<uint8_t> newTeamId;
    Ptr<Ptr<RoomMemberBinAttrInternal>> newRoomMemberBinAttrInternal;
    uint32_t newRoomMemberBinAttrInternalNum;
};

} // namespace np2
