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

#include "SceNpManager.h"

#include "util/types.h"

#include <io/state.h>
#include <kernel/state.h>
#include <np/state.h>
#include <util/log.h>

#include <np/functions.h>
#include <rpcn/client.h>
#include <rpcn/p2p.h>
#include <rpcn/vport.h>

#include <util/tracy.h>
TRACY_MODULE_NAME(SceNpManager);

enum SceNpErrorCode : uint32_t {
    SCE_NP_ERROR_ALREADY_INITIALIZED = 0x80550001,
    SCE_NP_ERROR_NOT_INITIALIZED = 0x80550002,
    SCE_NP_ERROR_INVALID_ARGUMENT = 0x80550003,
    SCE_NP_ERROR_UNKNOWN_PLATFORM_TYPE = 0x80550004
};

enum SceNpManagerErrorCode : uint32_t {
    SCE_NP_MANAGER_ERROR_ABORTED = 0x80550507,
    SCE_NP_MANAGER_ERROR_ALREADY_INITIALIZED = 0x80550501,
    SCE_NP_MANAGER_ERROR_OUT_OF_MEMORY = 0x80550504,
    SCE_NP_MANAGER_ERROR_NOT_INITIALIZED = 0x80550502,
    SCE_NP_MANAGER_ERROR_INVALID_ARGUMENT = 0x80550503,
    SCE_NP_MANAGER_ERROR_INVALID_STATE = 0x80550506,
    SCE_NP_MANAGER_ERROR_ID_NOT_AVAIL = 0x80550509
};

static bool rpcn_enabled(EmuEnvState &emuenv) {
    return emuenv.cfg.rpcn_enable;
}

static SceNpServiceState get_service_state(EmuEnvState &emuenv) {
    if (rpcn_enabled(emuenv))
        return (emuenv.np.rpcn && emuenv.np.rpcn->is_authenticated()) ? SCE_NP_SERVICE_STATE_ONLINE : SCE_NP_SERVICE_STATE_SIGNED_OUT;
    return emuenv.cfg.current_config.psn_signed_in ? SCE_NP_SERVICE_STATE_SIGNED_IN : SCE_NP_SERVICE_STATE_SIGNED_OUT;
}

static void rpcn_connect(EmuEnvState &emuenv) {
    if (!emuenv.np.rpcn) {
        rpcn::Config config;
        config.host = emuenv.cfg.rpcn_host;
        config.npid = emuenv.cfg.rpcn_npid;
        config.password = emuenv.cfg.rpcn_password;
        config.token = emuenv.cfg.rpcn_token;
        emuenv.np.rpcn = std::make_shared<rpcn::Client>(std::move(config));
    }

    const rpcn::State result = emuenv.np.rpcn->connect_and_login();
    if (result != rpcn::State::Authenticated) {
        LOG_ERROR("RPCN: unable to sign in: {}", rpcn::to_string(result));
        return;
    }

    // Signaling/P2P port, announced to RPCN so other room members can reach us
    np::self_online_id = emuenv.np.rpcn->npid();
    if (!emuenv.np.p2p)
        emuenv.np.p2p = std::make_shared<rpcn::P2P>(static_cast<uint16_t>(emuenv.cfg.rpcn_p2p_port));
    if (emuenv.np.p2p->ok()) {
        const auto &client = *emuenv.np.rpcn;
        emuenv.np.p2p->set_self_npid(client.npid());
        emuenv.np.p2p->set_rpcn(client.server_ipv4(), client.user_id(), client.local_ipv4());
        rpcn::VportHub::get().attach(emuenv.np.p2p);
    }
}

EXPORT(int, sceNpAuthAbortOAuthRequest) {
    TRACY_FUNC(sceNpAuthAbortOAuthRequest);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpAuthCreateOAuthRequest) {
    TRACY_FUNC(sceNpAuthCreateOAuthRequest);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpAuthDeleteOAuthRequest) {
    TRACY_FUNC(sceNpAuthDeleteOAuthRequest);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpAuthGetAuthorizationCode) {
    TRACY_FUNC(sceNpAuthGetAuthorizationCode);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpCheckCallback) {
    TRACY_FUNC(sceNpCheckCallback);

    const ThreadStateRef thread = emuenv.kernel.get_thread(thread_id);
    const SceNpServiceState state = get_service_state(emuenv);
    // Offline mode keeps the historical behaviour of reporting on every check.
    // With RPCN, report transitions only, as the real library does.
    if (!rpcn_enabled(emuenv) || state != emuenv.np.last_reported_state) {
        emuenv.np.last_reported_state = state;
        for (auto &[_, np_callback] : emuenv.np.cbs) {
            thread->call_guest(np_callback.pc, { static_cast<uint32_t>(state), 0, np_callback.data });
        }
    }

    auto &auth = emuenv.np.auth_state;
    std::vector<SceInt32> auth_ready;
    {
        const std::lock_guard<std::mutex> lock(auth.access_mutex);
        for (auto &[id, request] : auth.requests) {
            if (request.pending) {
                request.pending = false;
                auth_ready.push_back(id);
            }
        }
    }
    for (const SceInt32 id : auth_ready) {
        NpAuthRequest request;
        {
            // An earlier callback may have destroyed this request.
            const std::lock_guard<std::mutex> lock(auth.access_mutex);
            const auto it = auth.requests.find(id);
            if (it == auth.requests.end())
                continue;
            request = it->second;
        }
        // No real ticket is issued; report a 1-byte ticket as success.
        thread->call_guest(request.ticket_cb, { static_cast<uint32_t>(id), 1, request.cb_arg });
    }

    return STUBBED("Stub");
}

EXPORT(int, sceNpGetServiceState, SceNpServiceState *state) {
    TRACY_FUNC(sceNpGetServiceState, state);
    *state = get_service_state(emuenv);

    return STUBBED("Stub");
}

EXPORT(int, sceNpInit, np::CommunicationConfig *comm_config, void *dontcare) {
    TRACY_FUNC(sceNpInit, comm_config, dontcare);
    if (emuenv.np.inited) {
        return SCE_NP_ERROR_ALREADY_INITIALIZED;
    }

    const np::CommunicationID *comm_id = (comm_config) ? comm_config->comm_id.get(emuenv.mem) : nullptr;

    if (!init(emuenv.np, comm_id)) {
        return SCE_NP_ERROR_NOT_INITIALIZED;
    }

    if (comm_id)
        LOG_INFO("sceNpInit: communication id {}", std::string_view(comm_id->data, strnlen(comm_id->data, sizeof(comm_id->data))));

    emuenv.np.last_reported_state = SCE_NP_SERVICE_STATE_UNKNOWN;
    if (rpcn_enabled(emuenv))
        rpcn_connect(emuenv);

    return 0;
}

struct SceNpCountryCode {
    char data[2];
    char term;
    char padding[1];
};

// Country of the PSN account (ISO 3166-1 alpha-2, lowercase) and its language. Games use them for region
// filters (e.g. SFxT stores the country in a searchable room attribute).
EXPORT(int, sceNpManagerGetAccountRegion, SceNpCountryCode *countryCode, int *languageCode) {
    TRACY_FUNC(sceNpManagerGetAccountRegion, countryCode, languageCode);
    if (!emuenv.np.inited)
        return RET_ERROR(SCE_NP_ERROR_NOT_INITIALIZED);
    if (!countryCode || !languageCode)
        return RET_ERROR(SCE_NP_ERROR_INVALID_ARGUMENT);

    std::string country = emuenv.cfg.rpcn_country;
    if (country.size() != 2)
        country = "us";
    std::memset(countryCode, 0, sizeof(*countryCode));
    countryCode->data[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(country[0])));
    countryCode->data[1] = static_cast<char>(std::tolower(static_cast<unsigned char>(country[1])));
    *languageCode = emuenv.cfg.sys_lang;
    return 0;
}

EXPORT(int, sceNpManagerGetCachedParam) {
    TRACY_FUNC(sceNpManagerGetCachedParam);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpManagerGetChatRestrictionFlag, SceInt *isRestricted) {
    TRACY_FUNC(sceNpManagerGetChatRestrictionFlag, isRestricted);
    *isRestricted = 0; // User is never restricted
    return 0;
}

EXPORT(int, sceNpManagerGetContentRatingFlag, SceInt *isRestricted, SceInt *age) {
    TRACY_FUNC(sceNpManagerGetContentRatingFlag, isRestricted, age);
    *isRestricted = 0; // User is never restricted
    *age = 21; // Assume user is 21 years old
    return STUBBED("isRestricted = 0; age = 21; return 0;");
}

EXPORT(int, sceNpManagerGetNpId, np::SceNpId *id) {
    TRACY_FUNC(sceNpManagerGetNpId, id);
    const std::string &online_id = rpcn_enabled(emuenv) ? emuenv.cfg.rpcn_npid : emuenv.io.user_name;
    if (rpcn_enabled(emuenv) && !(emuenv.np.rpcn && emuenv.np.rpcn->is_authenticated()))
        return SCE_NP_MANAGER_ERROR_INVALID_STATE; // signed out
    if (online_id.length() > SCE_NP_ONLINEID_MAX_LENGTH) {
        LOG_ERROR("Your online ID has over 16 characters, try again with shorter name");
        return SCE_NP_MANAGER_ERROR_ID_NOT_AVAIL;
    }
    // Fill the unused stuffs to 0 (prevent some weird things happen)
    memset(id, 0, sizeof(np::SceNpId));
    strcpy(id->handle.data, online_id.c_str());
    id->isIdValid = true;
    id->opt.platformType[0] = 'p';
    id->opt.platformType[1] = 's';
    id->opt.platformType[2] = 'p';
    id->opt.platformType[3] = '2';
    return 0;
}

EXPORT(int, sceNpRegisterServiceStateCallback, Ptr<void> callback, Ptr<void> data) {
    TRACY_FUNC(sceNpRegisterServiceStateCallback, callback, data);
    const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
    SceUID cid = emuenv.kernel.get_next_uid();
    SceNpServiceStateCallback sceNpServiceStateCallback{
        .pc = callback.address(),
        .data = data.address()
    };
    emuenv.np.cbs.emplace(cid, sceNpServiceStateCallback);
    emuenv.np.state_cb_id = cid;
    return 0;
}

EXPORT(void, sceNpTerm) {
    TRACY_FUNC(sceNpTerm);
    if (!emuenv.np.inited) {
        LOG_WARN("NP library not initialized but termination got called");
        return;
    }
    deinit(emuenv.np);
}

EXPORT(int, sceNpUnregisterServiceStateCallback) {
    TRACY_FUNC(sceNpUnregisterServiceStateCallback);
    if (emuenv.np.state_cb_id) {
        emuenv.np.cbs.erase(emuenv.np.state_cb_id);
    }
    return 0;
}
