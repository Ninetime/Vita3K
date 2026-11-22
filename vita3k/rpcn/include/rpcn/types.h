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

// Wire protocol definitions shared with the RPCN server (https://github.com/RipleyTom/rpcn).
// The enum orders are part of the protocol and must match the server exactly.

#include <cstdint>

namespace rpcn {

constexpr uint32_t PROTOCOL_VERSION = 36;
constexpr uint16_t DEFAULT_PORT = 31313;
constexpr uint16_t SIGNALING_PORT = 3657;
constexpr size_t HEADER_SIZE = 15; // u8 type, u16 command, u32 size, u64 id (all little-endian)
constexpr size_t COMMUNICATION_ID_SIZE = 12; // "NPWR01781_00"

enum class CommandType : uint16_t {
    Login,
    Terminate,
    Create,
    Delete,
    SendToken,
    SendResetToken,
    ResetPassword,
    ResetState,
    AddFriend,
    RemoveFriend,
    AddBlock,
    RemoveBlock,
    GetServerList,
    GetWorldList,
    CreateRoom,
    JoinRoom,
    LeaveRoom,
    SearchRoom,
    GetRoomDataExternalList,
    SetRoomDataExternal,
    GetRoomDataInternal,
    SetRoomDataInternal,
    GetRoomMemberDataInternal,
    SetRoomMemberDataInternal,
    SetUserInfo,
    PingRoomOwner,
    SendRoomMessage,
    RequestSignalingInfos,
    RequestTicket,
    SendMessage,
    GetBoardInfos,
    RecordScore,
    RecordScoreData,
    GetScoreData,
    GetScoreRange,
    GetScoreFriends,
    GetScoreNpid,
    GetNetworkTime,
    TusSetMultiSlotVariable,
    TusGetMultiSlotVariable,
    TusGetMultiUserVariable,
    TusGetFriendsVariable,
    TusAddAndGetVariable,
    TusTryAndSetVariable,
    TusDeleteMultiSlotVariable,
    TusSetData,
    TusGetData,
    TusGetMultiSlotDataStatus,
    TusGetMultiUserDataStatus,
    TusGetFriendsDataStatus,
    TusDeleteMultiSlotData,
    SetPresence,
    CreateRoomGUI,
    JoinRoomGUI,
    LeaveRoomGUI,
    GetRoomListGUI,
    SetRoomSearchFlagGUI,
    GetRoomSearchFlagGUI,
    SetRoomInfoGUI,
    GetRoomInfoGUI,
    QuickMatchGUI,
    SearchJoinRoomGUI,
    GetRoomMemberDataExternalList,
    UnlockTrophy,
    SyncTrophies,
    DeleteTrophies,
    GetTss,
    GetLobbyInfoList,
    JoinLobby,
    LeaveLobby,
    SetLobbyMemberDataInternal,
    GetLobbyMemberDataInternalList,
    SendLobbyChatMessage,
    SendLobbyInvitation,
};

enum class NotificationType : uint16_t {
    UserJoinedRoom,
    UserLeftRoom,
    RoomDestroyed,
    UpdatedRoomDataInternal,
    UpdatedRoomMemberDataInternal,
    FriendQuery,
    FriendNew,
    FriendLost,
    FriendStatus,
    RoomMessageReceived,
    MessageReceived,
    FriendPresenceChanged,
    SignalingHelper,
    MemberJoinedRoomGUI,
    MemberLeftRoomGUI,
    RoomDisappearedGUI,
    RoomOwnerChangedGUI,
    UserKickedGUI,
    QuickMatchCompleteGUI,
    MemberJoinedLobby,
    MemberLeftLobby,
    UpdatedLobbyMemberDataInternal,
    LobbyMessageReceived,
    LobbyInvitationReceived,
};

enum class PacketType : uint8_t {
    Request,
    Reply,
    Notification,
    ServerInfo,
};

enum class ErrorType : uint8_t {
    NoError,
    Malformed,
    Invalid,
    InvalidInput,
    TooSoon,
    LoginError,
    LoginAlreadyLoggedIn,
    LoginInvalidUsername,
    LoginInvalidPassword,
    LoginInvalidToken,
    CreationError,
    CreationExistingUsername,
    CreationBannedEmailProvider,
    CreationExistingEmail,
    RoomMissing,
    RoomAlreadyJoined,
    RoomFull,
    RoomPasswordMismatch,
    RoomPasswordMissing,
    RoomGroupNoJoinLabel,
    RoomGroupFull,
    RoomGroupJoinLabelNotFound,
    RoomGroupMaxSlotMismatch,
    Unauthorized,
    DbFail,
    EmailFail,
    NotFound,
    Blocked,
    AlreadyFriend,
    ScoreNotBest,
    ScoreInvalid,
    ScoreHasData,
    CondFail,
    Unsupported,
};

// Client-side connection state (not part of the wire protocol).
enum class State {
    Disconnected,
    Connected, // TLS up and protocol version verified
    Authenticated,
    FailureInput,
    FailureSsl,
    FailureResolve,
    FailureConnect,
    FailureProtocol,
    FailureLogin,
    FailureLoginAlreadyLoggedIn,
    FailureLoginUsername,
    FailureLoginPassword,
    FailureLoginToken,
    FailureOther,
};

const char *to_string(ErrorType error);
const char *to_string(State state);
const char *to_string(CommandType command);

} // namespace rpcn
