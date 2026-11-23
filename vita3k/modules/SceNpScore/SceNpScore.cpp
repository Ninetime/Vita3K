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

// NP Score (leaderboards) over RPCN, sharing the boards of the PS3 version of a game when it uses the same
// communication ID. Argument orders and array sizes follow the firmware module (np_ranking.suprx).

#include <module/module.h>

#include <kernel/state.h>

#include <np/common.h>
#include <np/state.h>
#include <rpcn/client.h>
#include <rpcn/pb.h>

#include <thread>

namespace {

namespace pb = rpcn::pb;

// Error codes (SceNpCommunity)
constexpr uint32_t SCORE_ERROR_ALREADY_INITIALIZED = 0x80550701;
constexpr uint32_t SCORE_ERROR_NOT_INITIALIZED = 0x80550702;
constexpr uint32_t SCORE_ERROR_INVALID_ARGUMENT = 0x80550704;
constexpr uint32_t SCORE_ERROR_NO_LOGIN = 0x80550705;
constexpr uint32_t SCORE_ERROR_TOO_MANY_OBJECTS = 0x80550706;
constexpr uint32_t SCORE_ERROR_ABORTED = 0x80550707;
constexpr uint32_t SCORE_ERROR_BAD_RESPONSE = 0x80550708;
constexpr uint32_t SCORE_ERROR_INSUFFICIENT_ARGUMENT = 0x8055070c;
constexpr uint32_t SCORE_ERROR_INVALID_ID = 0x8055070e;
constexpr uint32_t SCORE_ERROR_TRANSACTION_ALREADY_END = 0x80550712;
constexpr uint32_t SCORE_ERROR_INVALID_ALIGNMENT = 0x80550714;
constexpr uint32_t SCORE_ERROR_NO_RESOURCE = 0x80550716;
constexpr uint32_t SCORE_SERVER_ERROR_FORBIDDEN = 0x80550806;
constexpr uint32_t SCORE_SERVER_ERROR_INTERNAL_SERVER_ERROR = 0x80550807;
constexpr uint32_t SCORE_SERVER_ERROR_GAME_RANKING_NOT_FOUND = 0x80550812;
constexpr uint32_t SCORE_SERVER_ERROR_NOT_BEST_SCORE = 0x80550815;
constexpr uint32_t SCORE_SERVER_ERROR_RANKING_BOARD_MASTER_NOT_FOUND = 0x80550817;
constexpr uint32_t SCORE_SERVER_ERROR_INVALID_SCORE = 0x80550823;
constexpr uint32_t SCORE_SERVER_ERROR_GAME_DATA_ALREADY_EXISTS = 0x8055082c;

constexpr size_t MAX_CONTEXTS = 32;
constexpr uint32_t MAX_RANKS_PER_REQUEST = 100; // checked by the firmware
constexpr size_t GAME_INFO_MAX_SIZE = 189; // checked by the firmware

struct SceNpScoreRankData {
    np::SceNpId npId;
    char onlineName[49]; // SceNpOnlineName on the PS3; reserved in the Vita SDK
    SceUChar8 pad0[3];
    SceInt32 pcId;
    SceUInt32 serialRank;
    SceUInt32 rank;
    SceUInt32 highestRank;
    SceInt32 hasGameData;
    SceUChar8 pad1[4];
    SceInt64 scoreValue;
    SceUInt64 recordDate; // SceRtcTick
};
static_assert(sizeof(SceNpScoreRankData) == 128); // firmware: rankArraySize == arrayNum * 128

struct SceNpScorePlayerRankData {
    SceInt32 hasData;
    SceUChar8 pad0[4];
    SceNpScoreRankData rankData;
};
static_assert(sizeof(SceNpScorePlayerRankData) == 136); // SFxT passes arrayNum * 0x88

struct SceNpScoreComment {
    char utf8Comment[64];
};
static_assert(sizeof(SceNpScoreComment) == 64); // firmware: commentArraySize == arrayNum * 64

struct SceNpScoreGameInfo {
    SceSize infoSize;
    SceUChar8 pad[4];
    SceUChar8 data[GAME_INFO_MAX_SIZE];
    SceUChar8 pad2[3];
};
static_assert(sizeof(SceNpScoreGameInfo) == 200); // firmware: infoArraySize == arrayNum * 200

NpTusRequest *get_request(NpTusState &score, SceInt32 requestId) {
    const auto it = score.requests.find(requestId);
    if (it == score.requests.end() || !score.title_contexts.contains(it->second.title_context_id))
        return nullptr;
    return &it->second;
}

std::string npid_string(const np::SceNpId &id) {
    return std::string(id.handle.data, strnlen(id.handle.data, sizeof(id.handle.data)));
}

std::optional<SceInt32> score_error(const std::vector<uint8_t> &reply) {
    if (reply.empty())
        return static_cast<SceInt32>(SCORE_SERVER_ERROR_INTERNAL_SERVER_ERROR);
    switch (static_cast<rpcn::ErrorType>(reply[0])) {
    case rpcn::ErrorType::NoError: return std::nullopt;
    case rpcn::ErrorType::ScoreNotBest: return static_cast<SceInt32>(SCORE_SERVER_ERROR_NOT_BEST_SCORE);
    case rpcn::ErrorType::ScoreInvalid: return static_cast<SceInt32>(SCORE_SERVER_ERROR_INVALID_SCORE);
    case rpcn::ErrorType::ScoreHasData: return static_cast<SceInt32>(SCORE_SERVER_ERROR_GAME_DATA_ALREADY_EXISTS);
    case rpcn::ErrorType::NotFound: return static_cast<SceInt32>(SCORE_SERVER_ERROR_RANKING_BOARD_MASTER_NOT_FOUND);
    case rpcn::ErrorType::Unauthorized: return static_cast<SceInt32>(SCORE_SERVER_ERROR_FORBIDDEN);
    default: return static_cast<SceInt32>(SCORE_SERVER_ERROR_INTERNAL_SERVER_ERROR);
    }
}

// Protobuf after the error byte ("u32 length + message")
std::optional<pb::Reader> reply_message(const std::vector<uint8_t> &reply, std::vector<uint8_t> &storage) {
    if (reply.size() < 5)
        return std::nullopt;
    uint32_t len;
    memcpy(&len, reply.data() + 1, 4);
    if (5 + static_cast<size_t>(len) > reply.size())
        return std::nullopt;
    storage.assign(reply.begin() + 5, reply.begin() + 5 + len);
    pb::Reader r(storage);
    return r.ok() ? std::optional<pb::Reader>(r) : std::nullopt;
}

using ReplyHandler = std::function<SceInt32(const std::vector<uint8_t> &reply)>;

// 0 with the result, 1 while the request is still running (poll only), or an error code
uint32_t get_async_result(NpTusState &score, SceInt32 requestId, SceInt32 *result, bool wait) {
    while (true) {
        {
            std::lock_guard lock(score.access_mutex);
            if (!score.inited)
                return SCORE_ERROR_NOT_INITIALIZED;
            const auto *request = get_request(score, requestId);
            if (!request)
                return SCORE_ERROR_INVALID_ID;
            if (request->result) {
                *result = *request->result;
                return 0;
            }
            if (!wait)
                return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// Sends `body` (after the communication ID) and runs `handler` on the reply. Synchronous calls wait for it and
// return the request result; asynchronous ones return 0 and the result goes to PollAsync/WaitAsync.
int score_send(EmuEnvState &emuenv, const char *export_name, SceInt32 requestId, rpcn::CommandType command, const pb::Writer &msg, bool async, ReplyHandler handler) {
    auto &score = emuenv.np.score_state;
    std::string com_id;
    {
        std::lock_guard lock(score.access_mutex);
        if (!score.inited)
            return RET_ERROR(SCORE_ERROR_NOT_INITIALIZED);
        auto *request = get_request(score, requestId);
        if (!request)
            return RET_ERROR(SCORE_ERROR_INVALID_ID);
        if (request->result)
            return RET_ERROR(SCORE_ERROR_TRANSACTION_ALREADY_END);
        com_id = score.title_contexts[request->title_context_id];
    }
    const auto client = emuenv.np.rpcn;
    if (!client || !client->is_authenticated())
        return RET_ERROR(SCORE_ERROR_NO_LOGIN);

    std::vector<uint8_t> payload(rpcn::COMMUNICATION_ID_SIZE, 0);
    memcpy(payload.data(), com_id.data(), std::min<size_t>(com_id.size(), rpcn::COMMUNICATION_ID_SIZE));
    const uint32_t len = static_cast<uint32_t>(msg.data().size());
    payload.insert(payload.end(), reinterpret_cast<const uint8_t *>(&len), reinterpret_cast<const uint8_t *>(&len) + 4);
    payload.insert(payload.end(), msg.data().begin(), msg.data().end());

    const bool sent = client->request_async(command, payload, [&score, requestId, handler = std::move(handler), name = std::string(export_name), com_id](rpcn::CommandType, std::vector<uint8_t> reply) {
        std::lock_guard lock(score.access_mutex);
        auto *request = get_request(score, requestId);
        if (!request || request->result)
            return; // deleted or aborted meanwhile
        const auto error = score_error(reply);
        request->result = error ? *error : handler(reply);
        LOG_INFO("NP Score: {} ({}) finished with {}", name, com_id, log_hex(static_cast<uint32_t>(*request->result)));
    });
    if (!sent)
        return RET_ERROR(SCORE_ERROR_NO_LOGIN);
    if (async)
        return 0;

    SceInt32 result = 0;
    if (const uint32_t err = get_async_result(score, requestId, &result, true))
        return RET_ERROR(err);
    return result;
}

void fill_rank_data(SceNpScoreRankData &out, const pb::Reader &in) {
    np::fill_server_npid(out.npId, in.string(1));
    const std::string online_name = in.string(2);
    std::strncpy(out.onlineName, online_name.c_str(), sizeof(out.onlineName) - 1);
    out.pcId = in.i32(3);
    out.serialRank = out.rank = out.highestRank = in.u32(4);
    out.scoreValue = static_cast<SceInt64>(in.u64(5));
    out.hasGameData = in.boolean(6) ? 1 : 0;
    out.recordDate = in.u64(7);
}

struct RankingOutput {
    void *rankArray;
    SceSize rankArraySize;
    bool player_rank_data; // SceNpScorePlayerRankData rows (by NP ID), else SceNpScoreRankData
    SceNpScoreComment *commentArray;
    SceSize commentArraySize;
    SceNpScoreGameInfo *infoArray;
    SceSize infoArraySize;
    SceUInt32 arrayNum;
    SceUInt64 *lastSortDate;
    SceUInt32 *totalRecord;
};

// Fills the arrays from a GetScoreResponse. Rows keep the order of the request (by NP ID: one per requested
// player, empty for players without a score). Returns the number of rows, or GAME_RANKING_NOT_FOUND when nobody
// has a score, as RPCS3 does.
SceInt32 handle_score_response(const std::vector<uint8_t> &reply, const RankingOutput &out) {
    std::vector<uint8_t> storage;
    const auto msg = reply_message(reply, storage);
    if (!msg)
        return static_cast<SceInt32>(SCORE_ERROR_BAD_RESPONSE);

    const auto ranks = msg->messages(1);
    const auto comments = msg->strings(2);
    const auto infos = msg->messages(3);
    const uint32_t rows = std::min<uint32_t>(static_cast<uint32_t>(ranks.size()), out.arrayNum);

    uint32_t registered = 0;
    for (uint32_t i = 0; i < rows; i++) {
        if (ranks[i].u64(7) == 0)
            continue; // no score for this player
        registered++;
        if (out.player_rank_data) {
            auto &row = static_cast<SceNpScorePlayerRankData *>(out.rankArray)[i];
            row.hasData = 1;
            fill_rank_data(row.rankData, ranks[i]);
        } else {
            fill_rank_data(static_cast<SceNpScoreRankData *>(out.rankArray)[i], ranks[i]);
        }
    }

    if (out.commentArray) {
        for (uint32_t i = 0; i < std::min<uint32_t>(static_cast<uint32_t>(comments.size()), out.arrayNum); i++)
            std::strncpy(out.commentArray[i].utf8Comment, comments[i].c_str(), sizeof(out.commentArray[i].utf8Comment) - 1);
    }
    if (out.infoArray) {
        for (uint32_t i = 0; i < std::min<uint32_t>(static_cast<uint32_t>(infos.size()), out.arrayNum); i++) {
            const auto data = infos[i].bytes(1);
            const size_t size = std::min(data.size(), GAME_INFO_MAX_SIZE);
            out.infoArray[i].infoSize = static_cast<SceSize>(size);
            memcpy(out.infoArray[i].data, data.data(), size);
        }
    }
    if (out.lastSortDate)
        *out.lastSortDate = msg->u64(4);
    if (out.totalRecord)
        *out.totalRecord = msg->u32(5);

    return registered ? static_cast<SceInt32>(rows) : static_cast<SceInt32>(SCORE_SERVER_ERROR_GAME_RANKING_NOT_FOUND);
}

// Validates and clears the output arrays as the firmware does; 0 or an error code
uint32_t prepare_output(const RankingOutput &out, size_t row_size) {
    if (!out.rankArray || out.arrayNum == 0)
        return SCORE_ERROR_INSUFFICIENT_ARGUMENT;
    if (out.arrayNum > MAX_RANKS_PER_REQUEST)
        return SCORE_ERROR_INVALID_ARGUMENT;
    if (out.rankArraySize != out.arrayNum * row_size)
        return SCORE_ERROR_INVALID_ALIGNMENT;
    if (out.commentArray && out.commentArraySize != out.arrayNum * sizeof(SceNpScoreComment))
        return SCORE_ERROR_INVALID_ALIGNMENT;
    if (out.infoArray && out.infoArraySize != out.arrayNum * sizeof(SceNpScoreGameInfo))
        return SCORE_ERROR_INVALID_ALIGNMENT;

    memset(out.rankArray, 0, out.rankArraySize);
    if (out.commentArray)
        memset(out.commentArray, 0, out.commentArraySize);
    if (out.infoArray)
        memset(out.infoArray, 0, out.infoArraySize);
    return 0;
}

int record_score(EmuEnvState &emuenv, const char *export_name, SceInt32 reqId, SceUInt32 boardId, SceInt64 score,
    const SceNpScoreComment *scoreComment, const SceNpScoreGameInfo *gameInfo, SceUInt32 *tmpRank, const SceUInt64 *compareDate, const void *option, bool async) {
    if (option || (gameInfo && gameInfo->infoSize > GAME_INFO_MAX_SIZE))
        return RET_ERROR(SCORE_ERROR_INVALID_ARGUMENT);
    if (compareDate)
        LOG_WARN_ONCE("NP Score: RecordScore compareDate is not supported");

    pb::Writer w;
    w.u32(1, boardId);
    w.i32(2, 0); // pcId
    w.i64(3, score);
    if (scoreComment)
        w.string(4, std::string_view(scoreComment->utf8Comment, strnlen(scoreComment->utf8Comment, sizeof(scoreComment->utf8Comment))));
    if (gameInfo && gameInfo->infoSize)
        w.bytes(5, gameInfo->data, gameInfo->infoSize);
    LOG_INFO("NP Score: RecordScore board {} score {}", boardId, score);

    return score_send(emuenv, export_name, reqId, rpcn::CommandType::RecordScore, w, async, [tmpRank](const std::vector<uint8_t> &reply) -> SceInt32 {
        if (reply.size() < 5)
            return static_cast<SceInt32>(SCORE_ERROR_BAD_RESPONSE);
        uint32_t rank;
        memcpy(&rank, reply.data() + 1, 4);
        if (tmpRank)
            *tmpRank = rank;
        return 0;
    });
}

void add_output_flags(pb::Writer &w, uint32_t comment_field, const RankingOutput &out) {
    w.boolean(comment_field, out.commentArray != nullptr);
    w.boolean(comment_field + 1, out.infoArray != nullptr);
}

int get_ranking_by_range(EmuEnvState &emuenv, const char *export_name, SceInt32 reqId, SceUInt32 boardId, SceUInt32 startSerialRank, const RankingOutput &out, const void *option, bool async) {
    if (option || startSerialRank == 0)
        return RET_ERROR(SCORE_ERROR_INVALID_ARGUMENT);
    if (const uint32_t err = prepare_output(out, sizeof(SceNpScoreRankData)))
        return RET_ERROR(err);

    pb::Writer w;
    w.u32(1, boardId);
    w.u32(2, startSerialRank);
    w.u32(3, out.arrayNum);
    add_output_flags(w, 4, out);
    LOG_INFO("NP Score: GetRankingByRange board {} from rank {} ({} rows)", boardId, startSerialRank, out.arrayNum);

    return score_send(emuenv, export_name, reqId, rpcn::CommandType::GetScoreRange, w, async, [out](const std::vector<uint8_t> &reply) {
        return handle_score_response(reply, out);
    });
}

int get_ranking_by_npid(EmuEnvState &emuenv, const char *export_name, SceInt32 reqId, SceUInt32 boardId, const np::SceNpId *npIdArray, SceSize npIdArraySize,
    const RankingOutput &out, const void *option, bool async) {
    if (option)
        return RET_ERROR(SCORE_ERROR_INVALID_ARGUMENT);
    if (!npIdArray)
        return RET_ERROR(SCORE_ERROR_INSUFFICIENT_ARGUMENT);
    if (npIdArraySize != out.arrayNum * sizeof(np::SceNpId))
        return RET_ERROR(SCORE_ERROR_INVALID_ALIGNMENT);
    if (const uint32_t err = prepare_output(out, sizeof(SceNpScorePlayerRankData)))
        return RET_ERROR(err);

    pb::Writer w;
    w.u32(1, boardId);
    for (SceUInt32 i = 0; i < out.arrayNum; i++) {
        pb::Writer id;
        id.string(1, npid_string(npIdArray[i]));
        id.i32(2, 0);
        w.message(2, id);
    }
    add_output_flags(w, 3, out);
    LOG_INFO("NP Score: GetRankingByNpId board {} ({} players)", boardId, out.arrayNum);

    return score_send(emuenv, export_name, reqId, rpcn::CommandType::GetScoreNpid, w, async, [out](const std::vector<uint8_t> &reply) {
        return handle_score_response(reply, out);
    });
}

int get_friends_ranking(EmuEnvState &emuenv, const char *export_name, SceInt32 reqId, SceUInt32 boardId, SceInt32 includeSelf, const RankingOutput &out, const void *option, bool async) {
    if (option)
        return RET_ERROR(SCORE_ERROR_INVALID_ARGUMENT);
    if (const uint32_t err = prepare_output(out, sizeof(SceNpScoreRankData)))
        return RET_ERROR(err);

    pb::Writer w;
    w.u32(1, boardId);
    w.boolean(2, includeSelf != 0);
    w.u32(3, out.arrayNum);
    add_output_flags(w, 4, out);
    LOG_INFO("NP Score: GetFriendsRanking board {} ({} rows)", boardId, out.arrayNum);

    return score_send(emuenv, export_name, reqId, rpcn::CommandType::GetScoreFriends, w, async, [out](const std::vector<uint8_t> &reply) {
        return handle_score_response(reply, out);
    });
}

} // namespace

#define RANKING_ARGS                                                                                           \
    void *rankArray, SceSize rankArraySize, SceNpScoreComment *commentArray, SceSize commentArraySize,         \
        SceNpScoreGameInfo *infoArray, SceSize infoArraySize, SceUInt32 arrayNum, SceUInt64 *lastSortDate, \
        SceUInt32 *totalRecord, const void *option
#define RANKING_OUTPUT(player_rows) \
    RankingOutput { rankArray, rankArraySize, player_rows, commentArray, commentArraySize, infoArray, infoArraySize, arrayNum, lastSortDate, totalRecord }

EXPORT(int, sceNpScoreAbortRequest, SceInt32 reqId) {
    auto &score = emuenv.np.score_state;
    std::lock_guard lock(score.access_mutex);
    if (!score.inited)
        return RET_ERROR(SCORE_ERROR_NOT_INITIALIZED);
    auto *request = get_request(score, reqId);
    if (!request)
        return RET_ERROR(SCORE_ERROR_INVALID_ID);
    if (!request->result)
        request->result = static_cast<SceInt32>(SCORE_ERROR_ABORTED);
    return 0;
}

EXPORT(int, sceNpScoreCensorComment) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpScoreCensorCommentAsync) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpScoreChangeModeForOtherSaveDataOwners) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpScoreCreateRequest, SceInt32 titleCtxId) {
    auto &score = emuenv.np.score_state;
    std::lock_guard lock(score.access_mutex);
    if (!score.inited)
        return RET_ERROR(SCORE_ERROR_NOT_INITIALIZED);
    if (!score.title_contexts.contains(titleCtxId))
        return RET_ERROR(SCORE_ERROR_INVALID_ID);
    if (score.requests.size() >= MAX_CONTEXTS)
        return RET_ERROR(SCORE_ERROR_TOO_MANY_OBJECTS);

    const SceUID requestId = emuenv.kernel.get_next_uid();
    if (requestId <= 0 || !score.requests.try_emplace(requestId, NpTusRequest{ titleCtxId, std::nullopt }).second)
        return RET_ERROR(SCORE_ERROR_NO_RESOURCE);
    return requestId;
}

EXPORT(int, sceNpScoreCreateTitleCtx, const np::CommunicationID *communicationId, const np::SceNpCommunicationPassphrase *passphrase, const np::SceNpId *selfNpId) {
    auto &score = emuenv.np.score_state;
    std::lock_guard lock(score.access_mutex);
    if (!score.inited)
        return RET_ERROR(SCORE_ERROR_NOT_INITIALIZED);
    if (!communicationId || !passphrase || !selfNpId)
        return RET_ERROR(SCORE_ERROR_INSUFFICIENT_ARGUMENT);
    if (score.title_contexts.size() >= MAX_CONTEXTS)
        return RET_ERROR(SCORE_ERROR_TOO_MANY_OBJECTS);

    const SceUID titleCtxId = emuenv.kernel.get_next_uid();
    const std::string com_id = fmt::format("{}_{:02}", std::string(communicationId->data, strnlen(communicationId->data, 9)), communicationId->num);
    if (titleCtxId <= 0 || !score.title_contexts.try_emplace(titleCtxId, com_id).second)
        return RET_ERROR(SCORE_ERROR_NO_RESOURCE);
    LOG_INFO("NP Score: title context {} for {}", titleCtxId, com_id);
    return titleCtxId;
}

EXPORT(int, sceNpScoreDeleteRequest, SceInt32 reqId) {
    auto &score = emuenv.np.score_state;
    std::lock_guard lock(score.access_mutex);
    if (!score.inited)
        return RET_ERROR(SCORE_ERROR_NOT_INITIALIZED);
    if (!score.requests.erase(reqId))
        return RET_ERROR(SCORE_ERROR_INVALID_ID);
    return 0;
}

EXPORT(int, sceNpScoreDeleteTitleCtx, SceInt32 titleCtxId) {
    auto &score = emuenv.np.score_state;
    std::lock_guard lock(score.access_mutex);
    if (!score.inited)
        return RET_ERROR(SCORE_ERROR_NOT_INITIALIZED);
    if (!score.title_contexts.erase(titleCtxId))
        return RET_ERROR(SCORE_ERROR_INVALID_ID);
    std::erase_if(score.requests, [titleCtxId](const auto &request) { return request.second.title_context_id == titleCtxId; });
    return 0;
}

EXPORT(int, sceNpScoreGetBoardInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpScoreGetBoardInfoAsync) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpScoreGetFriendsRanking, SceInt32 reqId, SceUInt32 boardId, SceInt32 includeSelf, RANKING_ARGS) {
    return get_friends_ranking(emuenv, export_name, reqId, boardId, includeSelf, RANKING_OUTPUT(false), option, false);
}

EXPORT(int, sceNpScoreGetFriendsRankingAsync, SceInt32 reqId, SceUInt32 boardId, SceInt32 includeSelf, RANKING_ARGS) {
    return get_friends_ranking(emuenv, export_name, reqId, boardId, includeSelf, RANKING_OUTPUT(false), option, true);
}

EXPORT(int, sceNpScoreGetGameData) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpScoreGetGameDataAsync) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpScoreGetRankingByNpId, SceInt32 reqId, SceUInt32 boardId, const np::SceNpId *npIdArray, SceSize npIdArraySize, RANKING_ARGS) {
    return get_ranking_by_npid(emuenv, export_name, reqId, boardId, npIdArray, npIdArraySize, RANKING_OUTPUT(true), option, false);
}

EXPORT(int, sceNpScoreGetRankingByNpIdAsync, SceInt32 reqId, SceUInt32 boardId, const np::SceNpId *npIdArray, SceSize npIdArraySize, RANKING_ARGS) {
    return get_ranking_by_npid(emuenv, export_name, reqId, boardId, npIdArray, npIdArraySize, RANKING_OUTPUT(true), option, true);
}

EXPORT(int, sceNpScoreGetRankingByNpIdPcId) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpScoreGetRankingByNpIdPcIdAsync) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpScoreGetRankingByRange, SceInt32 reqId, SceUInt32 boardId, SceUInt32 startSerialRank, RANKING_ARGS) {
    return get_ranking_by_range(emuenv, export_name, reqId, boardId, startSerialRank, RANKING_OUTPUT(false), option, false);
}

EXPORT(int, sceNpScoreGetRankingByRangeAsync, SceInt32 reqId, SceUInt32 boardId, SceUInt32 startSerialRank, RANKING_ARGS) {
    return get_ranking_by_range(emuenv, export_name, reqId, boardId, startSerialRank, RANKING_OUTPUT(false), option, true);
}

EXPORT(int, sceNpScoreInit, SceInt32 threadPriority, SceInt32 cpuAffinityMask, const void *option) {
    auto &score = emuenv.np.score_state;
    std::lock_guard lock(score.access_mutex);
    if (score.inited)
        return RET_ERROR(SCORE_ERROR_ALREADY_INITIALIZED);
    if (option)
        return RET_ERROR(SCORE_ERROR_INVALID_ARGUMENT);
    score.title_contexts.clear();
    score.requests.clear();
    score.inited = true;
    return 0;
}

EXPORT(int, sceNpScorePollAsync, SceInt32 reqId, SceInt32 *result) {
    if (!result)
        return RET_ERROR(SCORE_ERROR_INVALID_ARGUMENT);
    const uint32_t ret = get_async_result(emuenv.np.score_state, reqId, result, false);
    return ret > 1 ? RET_ERROR(ret) : static_cast<int>(ret);
}

EXPORT(int, sceNpScoreRecordGameData) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpScoreRecordGameDataAsync) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpScoreRecordScore, SceInt32 reqId, SceUInt32 boardId, SceInt64 score, const SceNpScoreComment *scoreComment,
    const SceNpScoreGameInfo *gameInfo, SceUInt32 *tmpRank, const SceUInt64 *compareDate, const void *option) {
    return record_score(emuenv, export_name, reqId, boardId, score, scoreComment, gameInfo, tmpRank, compareDate, option, false);
}

EXPORT(int, sceNpScoreRecordScoreAsync, SceInt32 reqId, SceUInt32 boardId, SceInt64 score, const SceNpScoreComment *scoreComment,
    const SceNpScoreGameInfo *gameInfo, SceUInt32 *tmpRank, const SceUInt64 *compareDate, const void *option) {
    return record_score(emuenv, export_name, reqId, boardId, score, scoreComment, gameInfo, tmpRank, compareDate, option, true);
}

EXPORT(int, sceNpScoreSanitizeComment) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpScoreSanitizeCommentAsync) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpScoreSetPlayerCharacterId) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpScoreSetTimeout) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpScoreTerm) {
    auto &score = emuenv.np.score_state;
    std::lock_guard lock(score.access_mutex);
    if (!score.inited)
        return RET_ERROR(SCORE_ERROR_NOT_INITIALIZED);
    score.title_contexts.clear();
    score.requests.clear();
    score.inited = false;
    return 0;
}

EXPORT(int, sceNpScoreWaitAsync, SceInt32 reqId, SceInt32 *result) {
    if (!result)
        return RET_ERROR(SCORE_ERROR_INVALID_ARGUMENT);
    const uint32_t ret = get_async_result(emuenv.np.score_state, reqId, result, true);
    return ret ? RET_ERROR(ret) : 0;
}
