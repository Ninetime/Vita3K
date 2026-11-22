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

#include <kernel/state.h>
#include <np/common.h>

#include <map>
#include <mutex>
#include <optional>
#include <set>

// NP Lookup. Online IDs map one to one onto NP IDs (as on RPCN), so lookups complete locally.
namespace {

enum : uint32_t {
    SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED = 0x80550702,
    SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT = 0x80550704,
    SCE_NP_COMMUNITY_ERROR_ABORTED = 0x80550707,
    SCE_NP_COMMUNITY_ERROR_INVALID_ID = 0x8055070e,
    SCE_NP_COMMUNITY_ERROR_TRANSACTION_ALREADY_END = 0x80550712,
};

struct LookupRequest {
    SceInt32 title_ctx = 0;
    std::optional<SceInt32> result;
};

struct LookupState {
    std::mutex mutex;
    bool inited = false;
    std::set<SceInt32> title_contexts;
    std::map<SceInt32, LookupRequest> requests;
};

LookupState &lookup() {
    static LookupState s;
    return s;
}

int lookup_npid(const char *export_name, SceInt32 requestId, const np::SceNpOnlineId *onlineId, np::SceNpId *npId, bool async) {
    std::lock_guard lock(lookup().mutex);
    const auto it = lookup().requests.find(requestId);
    if (it == lookup().requests.end())
        return RET_ERROR(SCE_NP_COMMUNITY_ERROR_INVALID_ID);
    if (!onlineId || !npId)
        return RET_ERROR(SCE_NP_COMMUNITY_ERROR_INVALID_ARGUMENT);
    if (it->second.result)
        return RET_ERROR(SCE_NP_COMMUNITY_ERROR_TRANSACTION_ALREADY_END);
    memset(npId, 0, sizeof(*npId));
    npId->handle = *onlineId;
    npId->handle.term = 0;
    it->second.result = 0;
    LOG_INFO("NP Lookup: {} -> NP ID", std::string(onlineId->data, strnlen(onlineId->data, sizeof(onlineId->data))));
    return 0;
}

} // namespace

EXPORT(int, sceNpBandwidthTestAbort) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBandwidthTestGetStatus) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBandwidthTestInitStart) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpBandwidthTestShutdown) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpLookupAbortRequest, SceInt32 requestId) {
    std::lock_guard lock(lookup().mutex);
    const auto it = lookup().requests.find(requestId);
    if (it == lookup().requests.end())
        return RET_ERROR(SCE_NP_COMMUNITY_ERROR_INVALID_ID);
    if (!it->second.result)
        it->second.result = static_cast<SceInt32>(SCE_NP_COMMUNITY_ERROR_ABORTED);
    return 0;
}

EXPORT(int, sceNpLookupAvatarImage) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpLookupAvatarImageAsync) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpLookupCreateRequest, SceInt32 titleCtxId) {
    std::lock_guard lock(lookup().mutex);
    if (!lookup().title_contexts.contains(titleCtxId))
        return RET_ERROR(SCE_NP_COMMUNITY_ERROR_INVALID_ID);
    const SceUID id = emuenv.kernel.get_next_uid();
    lookup().requests[id] = { titleCtxId, std::nullopt };
    return id;
}

EXPORT(int, sceNpLookupCreateTitleCtx) {
    std::lock_guard lock(lookup().mutex);
    if (!lookup().inited)
        return RET_ERROR(SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED);
    const SceUID id = emuenv.kernel.get_next_uid();
    lookup().title_contexts.insert(id);
    return id;
}

EXPORT(int, sceNpLookupDeleteRequest, SceInt32 requestId) {
    std::lock_guard lock(lookup().mutex);
    if (!lookup().requests.erase(requestId))
        return RET_ERROR(SCE_NP_COMMUNITY_ERROR_INVALID_ID);
    return 0;
}

EXPORT(int, sceNpLookupDeleteTitleCtx, SceInt32 titleCtxId) {
    std::lock_guard lock(lookup().mutex);
    if (!lookup().title_contexts.erase(titleCtxId))
        return RET_ERROR(SCE_NP_COMMUNITY_ERROR_INVALID_ID);
    std::erase_if(lookup().requests, [titleCtxId](const auto &r) { return r.second.title_ctx == titleCtxId; });
    return 0;
}

EXPORT(int, sceNpLookupInit) {
    std::lock_guard lock(lookup().mutex);
    lookup().inited = true;
    return 0;
}

EXPORT(int, sceNpLookupNpId, SceInt32 requestId, const np::SceNpOnlineId *onlineId, np::SceNpId *npId) {
    return lookup_npid(export_name, requestId, onlineId, npId, false);
}

EXPORT(int, sceNpLookupNpIdAsync, SceInt32 requestId, const np::SceNpOnlineId *onlineId, np::SceNpId *npId) {
    return lookup_npid(export_name, requestId, onlineId, npId, true);
}

EXPORT(int, sceNpLookupPollAsync, SceInt32 requestId, SceInt32 *result) {
    std::lock_guard lock(lookup().mutex);
    const auto it = lookup().requests.find(requestId);
    if (it == lookup().requests.end())
        return RET_ERROR(SCE_NP_COMMUNITY_ERROR_INVALID_ID);
    if (!it->second.result)
        return 1;
    if (result)
        *result = *it->second.result;
    return 0;
}

EXPORT(int, sceNpLookupSetTimeout) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpLookupTerm) {
    std::lock_guard lock(lookup().mutex);
    lookup().inited = false;
    lookup().title_contexts.clear();
    lookup().requests.clear();
    return 0;
}

EXPORT(int, sceNpLookupUserProfile) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpLookupUserProfileAsync) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpLookupWaitAsync, SceInt32 requestId, SceInt32 *result) {
    return CALL_EXPORT(sceNpLookupPollAsync, requestId, result);
}

EXPORT(int, sceNpWordFilterAbortRequest) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpWordFilterCensorComment) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpWordFilterCensorCommentAsync) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpWordFilterCreateRequest) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpWordFilterCreateTitleCtx) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpWordFilterDeleteRequest) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpWordFilterDeleteTitleCtx) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpWordFilterInit) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpWordFilterPollAsync) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpWordFilterSanitizeComment) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpWordFilterSanitizeCommentAsync) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpWordFilterSetTimeout) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpWordFilterTerm) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpWordFilterWaitAsync) {
    return UNIMPLEMENTED();
}
