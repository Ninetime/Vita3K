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

// NP Matching2 over RPCN. Requests are encoded to RPCN's protobuf messages and sent
// asynchronously; replies and server notifications are turned back into guest structures
// and delivered to the game's callbacks on a dedicated guest thread.

#include <module/module.h>

#include "np2_types.h"

#include <kernel/state.h>
#include <mem/functions.h>
#include <np/state.h>
#include <rpcn/client.h>
#include <rpcn/p2p.h>
#include <rpcn/pb.h>
#include <util/log.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <thread>

using namespace np2;
namespace pb = rpcn::pb;

namespace {

struct GuestCallback {
    Address fn = 0;
    Address arg = 0;
};

struct Context {
    std::string com_id; // "NPWR01781_00"
    bool started = false;
    ServerId server_id = 0;
};

struct RoomInfo {
    ContextId ctx_id = 0;
    RoomMemberId my_member_id = 0;
    std::map<RoomMemberId, std::string> members; // member ID -> NP ID
};

struct Matching2State {
    std::mutex mutex;
    bool inited = false;
    std::map<ContextId, Context> contexts;
    ContextId next_ctx_id = 1;
    RequestId next_req_id = 1;
    std::set<RequestId> aborted;
    std::map<RoomId, RoomInfo> rooms;

    GuestCallback context_cb;
    GuestCallback room_event_cb;
    GuestCallback signaling_cb;

    // Callback delivery
    Address trampoline = 0;
    ThreadStateRef cb_thread;
    uint32_t generation = 0;
    std::mutex job_mutex;
    std::condition_variable job_cv;
    std::deque<std::function<void()>> jobs;
};

// Never destroyed: the worker thread may still be running at process exit
Matching2State &state() {
    static auto *s = new Matching2State;
    return *s;
}

// Thumb trampoline run by the callback thread. r0 points to { fn, r0, r1, r2, r3, s0, s1, s2, s3 }:
// loads the registers and four stack words, then calls fn.
constexpr std::array<uint16_t, 22> TRAMPOLINE = {
    0xB510, // push {r4, lr}
    0xB084, // sub sp, #16
    0x1C04, // mov r4, r0
    0x6960, 0x9000, // ldr r0, [r4, #20]; str r0, [sp]
    0x69A0, 0x9001, // ldr r0, [r4, #24]; str r0, [sp, #4]
    0x69E0, 0x9002, // ldr r0, [r4, #28]; str r0, [sp, #8]
    0x6A20, 0x9003, // ldr r0, [r4, #32]; str r0, [sp, #12]
    0x6860, // ldr r0, [r4, #4]
    0x68A1, // ldr r1, [r4, #8]
    0x68E2, // ldr r2, [r4, #12]
    0x6923, // ldr r3, [r4, #16]
    0x6824, // ldr r4, [r4]
    0x47A0, // blx r4
    0xB004, // add sp, #16
    0xBD10, // pop {r4, pc}
    0xBF00, // nop
};

// Calls a guest function on the callback thread. Only used from the worker thread.
uint32_t call_guest(EmuEnvState &emuenv, Address fn, std::array<uint32_t, 4> regs, std::array<uint32_t, 4> stack = {}) {
    auto &st = state();
    if (!fn || !st.cb_thread)
        return 0;
    const Address block = alloc(emuenv.mem, 9 * sizeof(uint32_t), "np2_cb_args");
    if (!block)
        return 0;
    uint32_t *words = Ptr<uint32_t>(block).get(emuenv.mem);
    words[0] = fn;
    std::copy(regs.begin(), regs.end(), words + 1);
    std::copy(stack.begin(), stack.end(), words + 5);
    const uint32_t ret = st.cb_thread->call_and_wait(block);
    free(emuenv.mem, block);
    return ret;
}

// Bump allocator over one guest block holding a callback's event data
class Arena {
public:
    Arena(EmuEnvState &emuenv, uint32_t size)
        : mem(emuenv.mem)
        , size(size) {
        base = alloc(mem, size, "np2_event_data");
        if (base)
            memset(Ptr<uint8_t>(base).get(mem), 0, size);
    }
    ~Arena() {
        if (base)
            free(mem, base);
    }
    Arena(const Arena &) = delete;
    Arena &operator=(const Arena &) = delete;

    template <typename T>
    std::pair<T *, Address> make(uint32_t count = 1) {
        offset = (offset + 7) & ~7u;
        const uint32_t bytes = static_cast<uint32_t>(sizeof(T)) * count;
        if (!base || offset + bytes > size) {
            LOG_ERROR("NP Matching2: event data does not fit in {} bytes", size);
            return { nullptr, 0 };
        }
        const Address addr = base + offset;
        offset += bytes;
        return { Ptr<T>(addr).get(mem), addr };
    }

    Address copy(const std::vector<uint8_t> &data) {
        if (data.empty())
            return 0;
        auto [p, addr] = make<uint8_t>(static_cast<uint32_t>(data.size()));
        if (p)
            memcpy(p, data.data(), data.size());
        return addr;
    }

    MemState &mem;

private:
    uint32_t size;
    Address base = 0;
    uint32_t offset = 0;
};

constexpr uint32_t EVENT_DATA_SIZE = 64 * 1024;

std::shared_ptr<rpcn::Client> rpcn_client(EmuEnvState &emuenv) {
    auto client = emuenv.np.rpcn;
    return client && client->is_authenticated() ? client : nullptr;
}

std::string my_npid(EmuEnvState &emuenv) {
    return emuenv.np.rpcn ? emuenv.np.rpcn->npid() : std::string{};
}

std::string com_id_string(const np::CommunicationID &id) {
    return fmt::format("{}_{:02}", std::string(id.data, strnlen(id.data, 9)), id.num);
}

// ---- Guest -> protobuf -------------------------------------------------------------------

void put_bin_attr(MemState &mem, pb::Writer &w, uint32_t field, const BinAttr &attr) {
    pb::Writer a;
    a.wrapped(1, attr.id);
    if (attr.ptr && attr.size)
        a.bytes(2, attr.ptr.get(mem), attr.size);
    else
        a.bytes(2, nullptr, 0);
    w.message(field, a);
}

void put_bin_attrs(MemState &mem, pb::Writer &w, uint32_t field, Ptr<BinAttr> attrs, uint32_t num) {
    if (!attrs)
        return;
    const BinAttr *p = attrs.get(mem);
    for (uint32_t i = 0; i < num; i++)
        put_bin_attr(mem, w, field, p[i]);
}

void put_int_attrs(MemState &mem, pb::Writer &w, uint32_t field, Ptr<IntAttr> attrs, uint32_t num) {
    if (!attrs)
        return;
    const IntAttr *p = attrs.get(mem);
    for (uint32_t i = 0; i < num; i++) {
        pb::Writer a;
        a.wrapped(1, p[i].id);
        a.u32(2, p[i].num);
        w.message(field, a);
    }
}

void put_opt_data(pb::Writer &w, uint32_t field, const uint8_t (&data)[16], uint32_t len) {
    pb::Writer o;
    o.bytes(1, data, 16);
    o.u32(2, len);
    w.message(field, o);
}

std::vector<uint8_t> with_com_id(const std::string &com_id, const pb::Writer &msg) {
    std::vector<uint8_t> out(rpcn::COMMUNICATION_ID_SIZE, 0);
    memcpy(out.data(), com_id.data(), std::min<size_t>(com_id.size(), rpcn::COMMUNICATION_ID_SIZE));
    const auto &body = msg.data();
    const uint32_t len = static_cast<uint32_t>(body.size());
    const auto *lp = reinterpret_cast<const uint8_t *>(&len);
    out.insert(out.end(), lp, lp + 4);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// ---- Protobuf -> guest -------------------------------------------------------------------

// Reads "u32 length + protobuf" at offset into storage; advances offset
std::optional<pb::Reader> read_message(const std::vector<uint8_t> &data, size_t &offset, std::vector<uint8_t> &storage) {
    if (offset + 4 > data.size())
        return std::nullopt;
    uint32_t len;
    memcpy(&len, data.data() + offset, 4);
    offset += 4;
    if (offset + len > data.size())
        return std::nullopt;
    storage.assign(data.begin() + offset, data.begin() + offset + len);
    offset += len;
    pb::Reader r(storage);
    if (!r.ok())
        return std::nullopt;
    return r;
}

void fill_npid(np::SceNpId &id, const std::string &npid) {
    np::fill_server_npid(id, npid);
}

// From an RPCN UserInfo {npId = 1, onlineName = 2, avatarUrl = 3, platformType = 4}
void fill_user_npid(np::SceNpId &id, const pb::Reader &user) {
    const std::string npid = user.string(1);
    np::remember_platform(npid, user.u32(4));
    fill_npid(id, npid);
}

void fill_bin_attr(Arena &arena, BinAttr &out, const pb::Reader &in) {
    out.id = static_cast<AttributeId>(in.wrapped(1));
    const auto data = in.bytes(2);
    out.ptr = Ptr<uint8_t>(arena.copy(data));
    out.size = static_cast<uint32_t>(data.size());
}

void fill_room_group(RoomGroup &g, const pb::Reader &in) {
    g.groupId = static_cast<uint8_t>(in.wrapped(1));
    g.withPassword = in.boolean(2);
    const auto label = in.bytes(3);
    g.withLabel = !label.empty();
    memcpy(g.label.data, label.data(), std::min<size_t>(label.size(), sizeof(g.label.data)));
    g.slotNum = in.u32(4);
    g.curGroupMemberNum = in.u32(5);
}

// Array of RoomGroup from a repeated field; returns its address
Address make_room_groups(Arena &arena, const std::vector<pb::Reader> &groups) {
    if (groups.empty())
        return 0;
    auto [g, addr] = arena.make<RoomGroup>(static_cast<uint32_t>(groups.size()));
    if (!g)
        return 0;
    for (size_t i = 0; i < groups.size(); i++)
        fill_room_group(g[i], groups[i]);
    return addr;
}

void fill_member(Arena &arena, RoomMemberDataInternal &out, const pb::Reader &in) {
    fill_user_npid(out.npId, in.message(1));
    out.joinDate = in.u64(2);
    out.memberId = static_cast<RoomMemberId>(in.u32(3));
    out.teamId = static_cast<uint8_t>(in.wrapped(4));
    if (in.has(5))
        out.roomGroup = Ptr<RoomGroup>(make_room_groups(arena, { in.message(5) }));
    out.natType = static_cast<uint8_t>(in.wrapped(6));
    out.flagAttr = in.u32(7);
    const auto bins = in.messages(8);
    if (!bins.empty()) {
        auto [attrs, addr] = arena.make<RoomMemberBinAttrInternal>(static_cast<uint32_t>(bins.size()));
        if (attrs) {
            for (size_t i = 0; i < bins.size(); i++) {
                attrs[i].updateDate = bins[i].u64(1);
                fill_bin_attr(arena, attrs[i].data, bins[i].message(2));
            }
            out.roomMemberBinAttrInternal = Ptr<RoomMemberBinAttrInternal>(addr);
            out.roomMemberBinAttrInternalNum = static_cast<uint32_t>(bins.size());
        }
    }
}

Address make_member(Arena &arena, const pb::Reader &in) {
    auto [m, addr] = arena.make<RoomMemberDataInternal>();
    if (m)
        fill_member(arena, *m, in);
    return addr;
}

// Returns the guest address of the RoomDataInternal; my_member_id receives our own member ID
Address make_room_data_internal(Arena &arena, const pb::Reader &in, const std::string &npid, RoomMemberId *my_member_id = nullptr) {
    auto [room, room_addr] = arena.make<RoomDataInternal>();
    if (!room)
        return 0;
    room->serverId = static_cast<ServerId>(in.wrapped(1));
    room->worldId = in.u32(2);
    room->lobbyId = in.u64(3);
    room->roomId = in.u64(4);
    room->passwordSlotMask = in.u64(5);
    room->maxSlot = in.u32(6);
    room->joinedSlotMask = 0;

    const auto members = in.messages(7);
    const RoomMemberId owner_id = static_cast<RoomMemberId>(in.wrapped(8));
    if (!members.empty()) {
        auto [list, list_addr] = arena.make<RoomMemberDataInternal>(static_cast<uint32_t>(members.size()));
        if (list) {
            room->memberList.members = Ptr<RoomMemberDataInternal>(list_addr);
            room->memberList.membersNum = static_cast<uint32_t>(members.size());
            for (size_t i = 0; i < members.size(); i++) {
                const Address addr = list_addr + static_cast<Address>(i * sizeof(RoomMemberDataInternal));
                fill_member(arena, list[i], members[i]);
                // Member IDs are 0x10 + slot index (RPCN and PSN)
                if (list[i].memberId >= 0x10 && list[i].memberId < 0x10 + 64)
                    room->joinedSlotMask |= 1ull << (list[i].memberId - 0x10);
                if (i + 1 < members.size())
                    list[i].next = Ptr<RoomMemberDataInternal>(addr + sizeof(RoomMemberDataInternal));
                if (members[i].message(1).string(1) == npid) {
                    room->memberList.me = Ptr<RoomMemberDataInternal>(addr);
                    if (my_member_id)
                        *my_member_id = list[i].memberId;
                }
                if (list[i].memberId == owner_id)
                    room->memberList.owner = Ptr<RoomMemberDataInternal>(addr);
            }
        }
    }

    const auto groups = in.messages(9);
    room->roomGroup = Ptr<RoomGroup>(make_room_groups(arena, groups));
    room->roomGroupNum = room->roomGroup ? static_cast<uint32_t>(groups.size()) : 0;
    room->flagAttr = in.u32(10);

    const auto bins = in.messages(11);
    if (!bins.empty()) {
        auto [b, b_addr] = arena.make<RoomBinAttrInternal>(static_cast<uint32_t>(bins.size()));
        if (b) {
            for (size_t i = 0; i < bins.size(); i++) {
                b[i].updateDate = bins[i].u64(1);
                b[i].updateMemberId = static_cast<RoomMemberId>(bins[i].wrapped(2));
                fill_bin_attr(arena, b[i].data, bins[i].message(3));
            }
            room->roomBinAttrInternal = Ptr<RoomBinAttrInternal>(b_addr);
            room->roomBinAttrInternalNum = static_cast<uint32_t>(bins.size());
        }
    }
    return room_addr;
}

// Builds a linked array of RoomDataExternal; returns the first element's address
Address make_room_data_external_list(Arena &arena, const std::vector<pb::Reader> &rooms) {
    if (rooms.empty())
        return 0;
    auto [list, list_addr] = arena.make<RoomDataExternal>(static_cast<uint32_t>(rooms.size()));
    if (!list)
        return 0;
    for (size_t i = 0; i < rooms.size(); i++) {
        const auto &in = rooms[i];
        auto &out = list[i];
        if (i + 1 < rooms.size())
            out.next = Ptr<RoomDataExternal>(list_addr + static_cast<Address>((i + 1) * sizeof(RoomDataExternal)));
        out.serverId = static_cast<ServerId>(in.wrapped(1));
        out.worldId = in.u32(2);
        out.publicSlotNum = static_cast<uint16_t>(in.wrapped(3));
        out.privateSlotNum = static_cast<uint16_t>(in.wrapped(4));
        out.lobbyId = in.u64(5);
        out.roomId = in.u64(6);
        out.openPublicSlotNum = static_cast<uint16_t>(in.wrapped(7));
        out.maxSlot = static_cast<uint16_t>(in.wrapped(8));
        out.openPrivateSlotNum = static_cast<uint16_t>(in.wrapped(9));
        out.curMemberNum = static_cast<uint16_t>(in.wrapped(10));
        out.passwordSlotMask = in.u64(11);
        out.joinedSlotMask = out.curMemberNum >= 64 ? ~0ull : ((1ull << out.curMemberNum) - 1);
        if (in.has(12)) {
            auto [owner, owner_addr] = arena.make<np::SceNpId>();
            if (owner) {
                fill_user_npid(*owner, in.message(12));
                out.owner = Ptr<np::SceNpId>(owner_addr);
            }
        }
        const auto groups = in.messages(13);
        out.roomGroup = Ptr<RoomGroup>(make_room_groups(arena, groups));
        out.roomGroupNum = out.roomGroup ? static_cast<uint32_t>(groups.size()) : 0;
        out.flagAttr = in.u32(14);

        const auto ints = in.messages(15);
        if (!ints.empty()) {
            auto [a, a_addr] = arena.make<IntAttr>(static_cast<uint32_t>(ints.size()));
            if (a) {
                for (size_t j = 0; j < ints.size(); j++) {
                    a[j].id = static_cast<AttributeId>(ints[j].wrapped(1));
                    a[j].num = ints[j].u32(2);
                }
                out.roomSearchableIntAttrExternal = Ptr<IntAttr>(a_addr);
                out.roomSearchableIntAttrExternalNum = static_cast<uint32_t>(ints.size());
            }
        }
        auto bin_list = [&](uint32_t field, Ptr<BinAttr> &ptr, uint32_t &num) {
            const auto bins = in.messages(field);
            if (bins.empty())
                return;
            auto [b, b_addr] = arena.make<BinAttr>(static_cast<uint32_t>(bins.size()));
            if (!b)
                return;
            for (size_t j = 0; j < bins.size(); j++)
                fill_bin_attr(arena, b[j], bins[j]);
            ptr = Ptr<BinAttr>(b_addr);
            num = static_cast<uint32_t>(bins.size());
        };
        bin_list(16, out.roomSearchableBinAttrExternal, out.roomSearchableBinAttrExternalNum);
        bin_list(17, out.roomBinAttrExternal, out.roomBinAttrExternalNum);
    }
    return list_addr;
}

void fill_opt_data(const pb::Reader &in, uint8_t (&data)[16], uint32_t &len) {
    const auto bytes = in.bytes(1);
    memcpy(data, bytes.data(), std::min<size_t>(bytes.size(), 16));
    len = in.u32(2);
}

// IDs from a "repeated uint16" (wrapper message) field
std::vector<AttributeId> wrapped_ids(const pb::Reader &in, uint32_t field) {
    std::vector<AttributeId> ids;
    for (const auto &m : in.messages(field))
        ids.push_back(static_cast<AttributeId>(m.u32(1)));
    return ids;
}

// ---- Callback delivery -------------------------------------------------------------------

void queue_job(std::function<void()> job) {
    auto &st = state();
    {
        std::lock_guard lock(st.job_mutex);
        st.jobs.push_back(std::move(job));
    }
    st.job_cv.notify_one();
}

void call_context_callback(EmuEnvState &emuenv, ContextId ctx_id, M2Event event, uint8_t cause, int32_t error) {
    GuestCallback cb;
    {
        std::lock_guard lock(state().mutex);
        cb = state().context_cb;
    }
    LOG_INFO("NP Matching2: context {} event {} cause {} error {}", ctx_id, log_hex(event), cause, log_hex(static_cast<uint32_t>(error)));
    call_guest(emuenv, cb.fn, { ctx_id, event, cause, static_cast<uint32_t>(error) }, { cb.arg });
}

void call_room_event(EmuEnvState &emuenv, ContextId ctx_id, RoomId room_id, M2Event event, Address data) {
    GuestCallback cb;
    {
        std::lock_guard lock(state().mutex);
        cb = state().room_event_cb;
    }
    LOG_INFO("NP Matching2: room {} event {}", room_id, log_hex(event));
    call_guest(emuenv, cb.fn, { ctx_id, 0, static_cast<uint32_t>(room_id), static_cast<uint32_t>(room_id >> 32) }, { event, data, cb.arg });
}

struct PendingRequest {
    ContextId ctx_id;
    RequestId req_id;
    M2Event event;
    GuestCallback cb;
};

void call_request_callback(EmuEnvState &emuenv, const PendingRequest &req, int32_t error, Address data) {
    {
        std::lock_guard lock(state().mutex);
        if (state().aborted.erase(req.req_id)) {
            LOG_INFO("NP Matching2: request {} was aborted", req.req_id);
            return;
        }
    }
    LOG_INFO("NP Matching2: request {} event {} done, error {}", req.req_id, log_hex(req.event), log_hex(static_cast<uint32_t>(error)));
    call_guest(emuenv, req.cb.fn, { req.ctx_id, req.req_id, req.event, static_cast<uint32_t>(error) }, { data, req.cb.arg });
}

int32_t map_server_error(uint8_t error) {
    switch (static_cast<rpcn::ErrorType>(error)) {
    case rpcn::ErrorType::NoError: return 0;
    case rpcn::ErrorType::RoomMissing: return static_cast<int32_t>(SERVER_ERROR_NO_SUCH_ROOM);
    case rpcn::ErrorType::RoomAlreadyJoined: return static_cast<int32_t>(SERVER_ERROR_ALREADY_JOINED);
    case rpcn::ErrorType::RoomFull: return static_cast<int32_t>(SERVER_ERROR_ROOM_FULL);
    case rpcn::ErrorType::RoomPasswordMismatch: return static_cast<int32_t>(SERVER_ERROR_PASSWORD_MISMATCH);
    case rpcn::ErrorType::Unauthorized: return static_cast<int32_t>(SERVER_ERROR_FORBIDDEN);
    default: return static_cast<int32_t>(SERVER_ERROR_NO_SUCH_ROOM);
    }
}

// Converts a successful reply into guest event data inside the arena and returns its address
using ReplyHandler = std::function<Address(Arena &, const std::vector<uint8_t> &reply)>;

// Sends a request; on reply the handler builds the event data and the request callback runs
int send_request(EmuEnvState &emuenv, const char *export_name, ContextId ctx_id, Ptr<RequestOptParam> opt, Ptr<RequestId> assigned_req_id,
    M2Event event, rpcn::CommandType command, std::vector<uint8_t> payload, ReplyHandler handler) {
    auto &st = state();
    PendingRequest req{ ctx_id, 0, event, {} };
    {
        std::lock_guard lock(st.mutex);
        if (!st.inited)
            return RET_ERROR(ERROR_NOT_INITIALIZED);
        const auto it = st.contexts.find(ctx_id);
        if (it == st.contexts.end())
            return RET_ERROR(ERROR_CONTEXT_NOT_FOUND);
        if (!it->second.started)
            return RET_ERROR(ERROR_CONTEXT_NOT_STARTED);
        req.req_id = st.next_req_id++;
        if (st.next_req_id == 0)
            st.next_req_id = 1;
    }
    if (opt) {
        const RequestOptParam *o = opt.get(emuenv.mem);
        req.cb = { o->cbFunc.address(), o->cbFuncArg.address() };
    } else {
        LOG_WARN("NP Matching2: {} called without a request callback", export_name);
    }
    if (assigned_req_id)
        *assigned_req_id.get(emuenv.mem) = req.req_id;

    const auto client = rpcn_client(emuenv);
    if (!client) {
        queue_job([&emuenv, req] { call_request_callback(emuenv, req, static_cast<int32_t>(SERVER_ERROR_FORBIDDEN), 0); });
        return 0;
    }

    client->request_async(command, payload, [&emuenv, req, handler = std::move(handler)](rpcn::CommandType, std::vector<uint8_t> reply) {
        queue_job([&emuenv, req, handler, reply = std::move(reply)] {
            if (reply.empty()) {
                LOG_ERROR("NP Matching2: request {} lost its connection", req.req_id);
                call_request_callback(emuenv, req, static_cast<int32_t>(SERVER_ERROR_FORBIDDEN), 0);
                return;
            }
            const int32_t error = map_server_error(reply[0]);
            if (error) {
                LOG_WARN("NP Matching2: request {} failed: {}", req.req_id, rpcn::to_string(static_cast<rpcn::ErrorType>(reply[0])));
                call_request_callback(emuenv, req, error, 0);
                return;
            }
            Arena arena(emuenv, EVENT_DATA_SIZE);
            const Address data = handler ? handler(arena, reply) : 0;
            call_request_callback(emuenv, req, 0, data);
        });
    });
    return 0;
}

// ---- Signaling ---------------------------------------------------------------------------

ContextId context_for_room(RoomId room_id) {
    auto &st = state();
    std::lock_guard lock(st.mutex);
    const auto it = st.rooms.find(room_id);
    if (it != st.rooms.end())
        return it->second.ctx_id;
    return st.contexts.empty() ? 0 : st.contexts.begin()->first;
}


// Vita SceNpSignalingConnectionInfo codes (SFxT uses RTT and PEER_NPID)
enum SignalingInfoCode : int32_t {
    SIGNALING_CONN_INFO_RTT = 1,
    SIGNALING_CONN_INFO_BANDWIDTH = 2,
    SIGNALING_CONN_INFO_PEER_NPID = 3,
    SIGNALING_CONN_INFO_PEER_ADDRESS = 4,
    SIGNALING_CONN_INFO_MAPPED_ADDRESS = 5,
    SIGNALING_CONN_INFO_PACKET_LOSS = 6,
};

// Signaling callback events, same values the firmware library uses
constexpr uint32_t SIGNALING_EVENT_Dead = 0x5101;
constexpr uint32_t SIGNALING_EVENT_Established = 0x5102;

std::shared_ptr<rpcn::P2P> p2p(EmuEnvState &emuenv) {
    auto port = emuenv.np.p2p;
    return port && port->ok() ? port : nullptr;
}

// SignalingAddr { bytes ip = 1; uint16 port = 2; }
bool read_signaling_addr(const pb::Reader &in, uint32_t &addr, uint16_t &port) {
    const auto ip = in.bytes(1);
    if (ip.size() != 4) {
        LOG_WARN("NP Matching2: unsupported signaling address ({} byte IP)", ip.size());
        return false;
    }
    memcpy(&addr, ip.data(), 4);
    port = static_cast<uint16_t>(in.wrapped(2));
    return true;
}

// Remembers the NP IDs of a room's members (RoomDataInternal protobuf)
void record_members(RoomId room_id, ContextId ctx_id, RoomMemberId me, const pb::Reader &room) {
    std::lock_guard lock(state().mutex);
    auto &info = state().rooms[room_id];
    info.ctx_id = ctx_id;
    info.my_member_id = me;
    info.members.clear();
    for (const auto &m : room.messages(7))
        info.members[static_cast<RoomMemberId>(m.u32(3))] = m.message(1).string(1);
}

std::string member_npid(RoomId room_id, RoomMemberId member_id) {
    std::lock_guard lock(state().mutex);
    const auto room = state().rooms.find(room_id);
    if (room == state().rooms.end())
        return {};
    const auto it = room->second.members.find(member_id);
    return it == room->second.members.end() ? std::string{} : it->second;
}

void connect_member(EmuEnvState &emuenv, RoomId room_id, RoomMemberId member_id, uint32_t addr, uint16_t port) {
    const auto port_p2p = p2p(emuenv);
    const std::string npid = member_npid(room_id, member_id);
    if (!port_p2p || npid.empty()) {
        LOG_WARN("NP Matching2: cannot connect to member {} of room {}", member_id, room_id);
        return;
    }
    port_p2p->connect_member(npid, room_id, member_id, addr, port);
}

void call_signaling_callback(EmuEnvState &emuenv, const rpcn::SignalingEvent &ev) {
    GuestCallback cb;
    {
        std::lock_guard lock(state().mutex);
        cb = state().signaling_cb;
    }
    const ContextId ctx_id = context_for_room(ev.room_id);
    const uint32_t event = ev.established ? SIGNALING_EVENT_Established : SIGNALING_EVENT_Dead;
    LOG_INFO("NP Matching2: signaling with member {} of room {}: {}", ev.member_id, ev.room_id, ev.established ? "established" : "dead");
    // (ctxId, roomId, peerMemberId, event, errorCode, arg); the Dead error codes are not known yet
    call_guest(emuenv, cb.fn, { ctx_id, 0, static_cast<uint32_t>(ev.room_id), static_cast<uint32_t>(ev.room_id >> 32) },
        { ev.member_id, event, 0, cb.arg });
}

// ---- Notifications -----------------------------------------------------------------------

Address make_member_update(Arena &arena, const pb::Reader &info) {
    auto [upd, addr] = arena.make<RoomMemberUpdateInfo>();
    if (!upd)
        return 0;
    upd->roomMemberDataInternal = Ptr<RoomMemberDataInternal>(make_member(arena, info.message(1)));
    upd->eventCause = static_cast<uint8_t>(info.wrapped(2));
    fill_opt_data(info.message(3), upd->optData, upd->optDataLen);
    return addr;
}

Address make_room_data_internal_update(Arena &arena, const pb::Reader &msg, const std::string &npid) {
    auto [upd, addr] = arena.make<RoomDataInternalUpdateInfo>();
    if (!upd)
        return 0;
    const Address room_addr = make_room_data_internal(arena, msg.message(1), npid);
    upd->newRoomDataInternal = Ptr<RoomDataInternal>(room_addr);
    RoomDataInternal *room = upd->newRoomDataInternal.get(arena.mem);
    if (!room)
        return addr;
    upd->newFlagAttr = Ptr<FlagAttr>(room_addr + offsetof(RoomDataInternal, flagAttr));
    if (auto [prev, prev_addr] = arena.make<FlagAttr>(); prev) {
        *prev = msg.u32(2);
        upd->prevFlagAttr = Ptr<FlagAttr>(prev_addr);
    }
    upd->newRoomPasswordSlotMask = Ptr<uint64_t>(room_addr + offsetof(RoomDataInternal, passwordSlotMask));
    if (auto [prev, prev_addr] = arena.make<uint64_t>(); prev) {
        *prev = msg.u64(3);
        upd->prevRoomPasswordSlotMask = Ptr<uint64_t>(prev_addr);
    }
    const auto ids = wrapped_ids(msg, 5);
    if (!ids.empty() && room->roomBinAttrInternal) {
        auto [ptrs, ptrs_addr] = arena.make<Ptr<RoomBinAttrInternal>>(static_cast<uint32_t>(ids.size()));
        if (ptrs) {
            uint32_t count = 0;
            const RoomBinAttrInternal *attrs = room->roomBinAttrInternal.get(arena.mem);
            for (uint32_t i = 0; i < room->roomBinAttrInternalNum && count < ids.size(); i++)
                if (std::find(ids.begin(), ids.end(), attrs[i].data.id) != ids.end())
                    ptrs[count++] = Ptr<RoomBinAttrInternal>(room->roomBinAttrInternal.address() + i * sizeof(RoomBinAttrInternal));
            upd->newRoomBinAttrInternal = Ptr<Ptr<RoomBinAttrInternal>>(ptrs_addr);
            upd->newRoomBinAttrInternalNum = count;
        }
    }
    return addr;
}

Address make_member_data_internal_update(Arena &arena, const pb::Reader &msg) {
    auto [upd, addr] = arena.make<RoomMemberDataInternalUpdateInfo>();
    if (!upd)
        return 0;
    const Address member_addr = make_member(arena, msg.message(1));
    upd->newRoomMemberDataInternal = Ptr<RoomMemberDataInternal>(member_addr);
    RoomMemberDataInternal *member = upd->newRoomMemberDataInternal.get(arena.mem);
    if (!member)
        return addr;
    upd->newFlagAttr = Ptr<FlagAttr>(member_addr + offsetof(RoomMemberDataInternal, flagAttr));
    if (auto [prev, prev_addr] = arena.make<FlagAttr>(); prev) {
        *prev = msg.u32(2);
        upd->prevFlagAttr = Ptr<FlagAttr>(prev_addr);
    }
    upd->newTeamId = Ptr<uint8_t>(member_addr + offsetof(RoomMemberDataInternal, teamId));
    const auto ids = wrapped_ids(msg, 4);
    if (!ids.empty() && member->roomMemberBinAttrInternal) {
        auto [ptrs, ptrs_addr] = arena.make<Ptr<RoomMemberBinAttrInternal>>(static_cast<uint32_t>(ids.size()));
        if (ptrs) {
            uint32_t count = 0;
            const RoomMemberBinAttrInternal *attrs = member->roomMemberBinAttrInternal.get(arena.mem);
            for (uint32_t i = 0; i < member->roomMemberBinAttrInternalNum && count < ids.size(); i++)
                if (std::find(ids.begin(), ids.end(), attrs[i].data.id) != ids.end())
                    ptrs[count++] = Ptr<RoomMemberBinAttrInternal>(member->roomMemberBinAttrInternal.address() + i * sizeof(RoomMemberBinAttrInternal));
            upd->newRoomMemberBinAttrInternal = Ptr<Ptr<RoomMemberBinAttrInternal>>(ptrs_addr);
            upd->newRoomMemberBinAttrInternalNum = count;
        }
    }
    return addr;
}

void handle_notification(EmuEnvState &emuenv, const rpcn::Notification &n) {
    using rpcn::NotificationType;
    std::vector<uint8_t> storage;
    size_t offset = 0;

    switch (n.type) {
    case NotificationType::UserJoinedRoom: {
        const auto msg = read_message(n.data, offset, storage);
        if (!msg)
            break;
        const RoomId room_id = msg->u64(1);
        const auto member = msg->message(2).message(1);
        const auto member_id = static_cast<RoomMemberId>(member.u32(3));
        {
            std::lock_guard lock(state().mutex);
            if (const auto it = state().rooms.find(room_id); it != state().rooms.end())
                it->second.members[member_id] = member.message(1).string(1);
        }
        uint32_t addr;
        uint16_t port;
        if (msg->has(3) && read_signaling_addr(msg->message(3), addr, port))
            connect_member(emuenv, room_id, member_id, addr, port);
        Arena arena(emuenv, EVENT_DATA_SIZE);
        const Address data = make_member_update(arena, msg->message(2));
        call_room_event(emuenv, context_for_room(room_id), room_id, ROOM_EVENT_MemberJoined, data);
        break;
    }
    case NotificationType::SignalingHelper: {
        // A peer wants to connect to us: send it our address
        const auto msg = read_message(n.data, offset, storage);
        uint32_t addr;
        uint16_t port;
        const auto port_p2p = p2p(emuenv);
        if (msg && port_p2p && read_signaling_addr(msg->message(2), addr, port))
            port_p2p->send_info(msg->string(1), addr, port);
        break;
    }
    case NotificationType::UserLeftRoom:
    case NotificationType::RoomDestroyed:
    case NotificationType::UpdatedRoomDataInternal:
    case NotificationType::UpdatedRoomMemberDataInternal: {
        if (n.data.size() < 8)
            break;
        RoomId room_id;
        memcpy(&room_id, n.data.data(), 8);
        offset = 8;
        const auto msg = read_message(n.data, offset, storage);
        if (!msg)
            break;
        const ContextId ctx_id = context_for_room(room_id);
        Arena arena(emuenv, EVENT_DATA_SIZE);
        if (n.type == NotificationType::UserLeftRoom) {
            {
                std::lock_guard lock(state().mutex);
                if (const auto it = state().rooms.find(room_id); it != state().rooms.end())
                    it->second.members.erase(static_cast<RoomMemberId>(msg->message(1).u32(3)));
            }
            call_room_event(emuenv, ctx_id, room_id, ROOM_EVENT_MemberLeft, make_member_update(arena, *msg));
        } else if (n.type == NotificationType::RoomDestroyed) {
            auto [upd, addr] = arena.make<RoomUpdateInfo>();
            if (!upd)
                break;
            upd->eventCause = static_cast<uint8_t>(msg->wrapped(1));
            upd->errorCode = msg->i32(2);
            fill_opt_data(msg->message(3), upd->optData, upd->optDataLen);
            {
                std::lock_guard lock(state().mutex);
                state().rooms.erase(room_id);
            }
            if (const auto port_p2p = p2p(emuenv))
                port_p2p->disconnect_room(room_id);
            call_room_event(emuenv, ctx_id, room_id, ROOM_EVENT_RoomDestroyed, addr);
        } else if (n.type == NotificationType::UpdatedRoomDataInternal) {
            call_room_event(emuenv, ctx_id, room_id, ROOM_EVENT_UpdatedRoomDataInternal, make_room_data_internal_update(arena, *msg, my_npid(emuenv)));
        } else {
            call_room_event(emuenv, ctx_id, room_id, ROOM_EVENT_UpdatedRoomMemberDataInternal, make_member_data_internal_update(arena, *msg));
        }
        break;
    }
    default:
        LOG_INFO("NP Matching2: ignoring RPCN notification {} ({} bytes)", static_cast<int>(n.type), n.data.size());
        break;
    }
}

void worker_main(EmuEnvState &emuenv, uint32_t generation) {
    auto &st = state();
    while (true) {
        std::deque<std::function<void()>> jobs;
        {
            std::unique_lock lock(st.job_mutex);
            st.job_cv.wait_for(lock, std::chrono::milliseconds(20), [&] { return !st.jobs.empty() || st.generation != generation; });
            if (st.generation != generation)
                return;
            jobs.swap(st.jobs);
        }
        for (auto &job : jobs)
            job();

        if (const auto client = emuenv.np.rpcn) {
            for (const auto &n : client->take_notifications())
                handle_notification(emuenv, n);
        }
        if (const auto port_p2p = p2p(emuenv)) {
            for (const auto &ev : port_p2p->take_events())
                call_signaling_callback(emuenv, ev);
        }
    }
}

bool start_worker(EmuEnvState &emuenv) {
    auto &st = state();
    if (!st.trampoline) {
        st.trampoline = alloc(emuenv.mem, sizeof(TRAMPOLINE), "np2_cb_trampoline");
        if (!st.trampoline)
            return false;
        memcpy(Ptr<uint8_t>(st.trampoline).get(emuenv.mem), TRAMPOLINE.data(), sizeof(TRAMPOLINE));
    }
    st.cb_thread = emuenv.kernel.create_thread(emuenv.mem, "SceNpMatching2Callback", Ptr<const void>(st.trampoline | 1),
        SCE_KERNEL_DEFAULT_PRIORITY_USER, SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, 0x8000, 0);
    if (!st.cb_thread)
        return false;
    uint32_t generation;
    {
        std::lock_guard lock(st.job_mutex);
        generation = ++st.generation;
        st.jobs.clear();
    }
    std::thread(worker_main, std::ref(emuenv), generation).detach();
    return true;
}

void stop_worker() {
    auto &st = state();
    {
        std::lock_guard lock(st.job_mutex);
        ++st.generation;
        st.jobs.clear();
    }
    st.job_cv.notify_all();
}

std::string context_com_id(ContextId ctx_id) {
    std::lock_guard lock(state().mutex);
    const auto it = state().contexts.find(ctx_id);
    return it == state().contexts.end() ? std::string{} : it->second.com_id;
}

} // namespace

// ---- Exports -----------------------------------------------------------------------------

EXPORT(int, sceNpMatching2Init, uint32_t pool_size, int32_t thread_priority, int32_t cpu_affinity_mask, uint32_t thread_stack_size) {
    auto &st = state();
    std::lock_guard lock(st.mutex);
    if (st.inited)
        return RET_ERROR(ERROR_ALREADY_INITIALIZED);
    LOG_INFO("NP Matching2: init (pool {}, priority {}, stack {}){}", log_hex(pool_size), log_hex(thread_priority), log_hex(thread_stack_size),
        emuenv.cfg.rpcn_enable ? " using RPCN" : "");
    if (!start_worker(emuenv))
        return RET_ERROR(ERROR_OUT_OF_MEMORY);
    if (!st.inited) {
        emuenv.np.app_stop_hooks.push_back([] {
            // The app's memory and threads are gone: forget them
            auto &st = state();
            std::lock_guard lock(st.mutex);
            st.inited = false;
            st.contexts.clear();
            st.rooms.clear();
            st.aborted.clear();
            st.context_cb = st.room_event_cb = st.signaling_cb = {};
            stop_worker();
            st.trampoline = 0;
            st.cb_thread = {};
        });
    }
    st.inited = true;
    st.contexts.clear();
    st.rooms.clear();
    st.aborted.clear();
    st.context_cb = st.room_event_cb = st.signaling_cb = {};
    return 0;
}

EXPORT(int, sceNpMatching2Term) {
    auto &st = state();
    std::lock_guard lock(st.mutex);
    if (!st.inited)
        return RET_ERROR(ERROR_NOT_INITIALIZED);
    st.inited = false;
    st.contexts.clear();
    st.rooms.clear();
    stop_worker();
    return 0;
}

EXPORT(int, sceNpMatching2CreateContext, Ptr<const np::SceNpId> np_id, Ptr<const np::CommunicationID> comm_id, Ptr<const void> passphrase, Ptr<ContextId> ctx_id) {
    auto &st = state();
    std::lock_guard lock(st.mutex);
    if (!st.inited)
        return RET_ERROR(ERROR_NOT_INITIALIZED);
    if (!np_id || !comm_id || !passphrase || !ctx_id)
        return RET_ERROR(ERROR_INVALID_ARGUMENT);
    const ContextId id = st.next_ctx_id++;
    Context ctx;
    ctx.com_id = com_id_string(*comm_id.get(emuenv.mem));
    st.contexts[id] = ctx;
    *ctx_id.get(emuenv.mem) = id;
    LOG_INFO("NP Matching2: created context {} for {}", id, ctx.com_id);
    return 0;
}

EXPORT(int, sceNpMatching2DestroyContext, ContextId ctx_id) {
    auto &st = state();
    std::lock_guard lock(st.mutex);
    if (!st.inited)
        return RET_ERROR(ERROR_NOT_INITIALIZED);
    if (!st.contexts.erase(ctx_id))
        return RET_ERROR(ERROR_CONTEXT_NOT_FOUND);
    return 0;
}

EXPORT(int, sceNpMatching2RegisterContextCallback, Address cb, Address arg) {
    auto &st = state();
    std::lock_guard lock(st.mutex);
    if (!st.inited)
        return RET_ERROR(ERROR_NOT_INITIALIZED);
    st.context_cb = { cb, arg };
    return 0;
}

EXPORT(int, sceNpMatching2RegisterRoomEventCallback, ContextId ctx_id, Address cb, Address arg) {
    auto &st = state();
    std::lock_guard lock(st.mutex);
    if (!st.inited)
        return RET_ERROR(ERROR_NOT_INITIALIZED);
    st.room_event_cb = { cb, arg };
    return 0;
}

EXPORT(int, sceNpMatching2RegisterSignalingCallback, ContextId ctx_id, Address cb, Address arg) {
    auto &st = state();
    std::lock_guard lock(st.mutex);
    if (!st.inited)
        return RET_ERROR(ERROR_NOT_INITIALIZED);
    st.signaling_cb = { cb, arg };
    return 0;
}

EXPORT(int, sceNpMatching2RegisterRoomMessageCallback) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2RegisterLobbyEventCallback) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2RegisterLobbyMessageCallback) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2ContextStart, ContextId ctx_id) {
    auto &st = state();
    std::string com_id;
    {
        std::lock_guard lock(st.mutex);
        if (!st.inited)
            return RET_ERROR(ERROR_NOT_INITIALIZED);
        const auto it = st.contexts.find(ctx_id);
        if (it == st.contexts.end())
            return RET_ERROR(ERROR_CONTEXT_NOT_FOUND);
        if (it->second.started)
            return RET_ERROR(ERROR_CONTEXT_ALREADY_STARTED);
        com_id = it->second.com_id;
    }

    queue_job([&emuenv, ctx_id, com_id] {
        const auto client = rpcn_client(emuenv);
        ServerId server_id = 0;
        if (client) {
            if (const auto servers = client->get_server_list(com_id); servers && !servers->empty())
                server_id = servers->front();
        }
        if (!server_id) {
            LOG_ERROR("NP Matching2: no RPCN server for {}, context {} cannot start", com_id, ctx_id);
            call_context_callback(emuenv, ctx_id, CONTEXT_EVENT_StartOver, EVENT_CAUSE_CONTEXT_ERROR, static_cast<int32_t>(ERROR_SERVER_NOT_FOUND));
            return;
        }
        {
            std::lock_guard lock(state().mutex);
            const auto it = state().contexts.find(ctx_id);
            if (it == state().contexts.end())
                return;
            it->second.started = true;
            it->second.server_id = server_id;
        }
        LOG_INFO("NP Matching2: context {} started on server {}", ctx_id, server_id);
        call_context_callback(emuenv, ctx_id, CONTEXT_EVENT_Start, EVENT_CAUSE_CONTEXT_ACTION, 0);
    });
    return 0;
}

EXPORT(int, sceNpMatching2AbortContextStart) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2ContextStop, ContextId ctx_id) {
    auto &st = state();
    {
        std::lock_guard lock(st.mutex);
        if (!st.inited)
            return RET_ERROR(ERROR_NOT_INITIALIZED);
        const auto it = st.contexts.find(ctx_id);
        if (it == st.contexts.end())
            return RET_ERROR(ERROR_CONTEXT_NOT_FOUND);
        if (!it->second.started)
            return RET_ERROR(ERROR_CONTEXT_NOT_STARTED);
        it->second.started = false;
    }
    queue_job([&emuenv, ctx_id] { call_context_callback(emuenv, ctx_id, CONTEXT_EVENT_Stop, EVENT_CAUSE_CONTEXT_ACTION, 0); });
    return 0;
}

EXPORT(int, sceNpMatching2GetServerLocal, ContextId ctx_id, Ptr<Server> server) {
    auto &st = state();
    std::lock_guard lock(st.mutex);
    if (!st.inited)
        return RET_ERROR(ERROR_NOT_INITIALIZED);
    if (!server)
        return RET_ERROR(ERROR_INVALID_ARGUMENT);
    const auto it = st.contexts.find(ctx_id);
    if (it == st.contexts.end())
        return RET_ERROR(ERROR_CONTEXT_NOT_FOUND);
    if (!it->second.started)
        return RET_ERROR(ERROR_CONTEXT_NOT_STARTED);
    Server *s = server.get(emuenv.mem);
    s->serverId = it->second.server_id;
    s->status = SERVER_STATUS_AVAILABLE;
    return 0;
}

EXPORT(int, sceNpMatching2SetDefaultRequestOptParam) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2AbortRequest, ContextId ctx_id, RequestId req_id) {
    auto &st = state();
    std::lock_guard lock(st.mutex);
    if (!st.inited)
        return RET_ERROR(ERROR_NOT_INITIALIZED);
    st.aborted.insert(req_id);
    return 0;
}

EXPORT(int, sceNpMatching2GetWorldInfoList, ContextId ctx_id, Ptr<const GetWorldInfoListRequest> req, Ptr<RequestOptParam> opt, Ptr<RequestId> assigned_req_id) {
    if (!req)
        return RET_ERROR(ERROR_INVALID_ARGUMENT);
    const ServerId server_id = req.get(emuenv.mem)->serverId;
    std::vector<uint8_t> payload(rpcn::COMMUNICATION_ID_SIZE, 0);
    const std::string com_id = context_com_id(ctx_id);
    memcpy(payload.data(), com_id.data(), std::min<size_t>(com_id.size(), rpcn::COMMUNICATION_ID_SIZE));
    payload.push_back(static_cast<uint8_t>(server_id));
    payload.push_back(static_cast<uint8_t>(server_id >> 8));

    return send_request(emuenv, export_name, ctx_id, opt, assigned_req_id, REQUEST_EVENT_GetWorldInfoList, rpcn::CommandType::GetWorldList, payload,
        [server_id](Arena &arena, const std::vector<uint8_t> &reply) -> Address {
            if (reply.size() < 5)
                return 0;
            uint32_t count;
            memcpy(&count, reply.data() + 1, 4);
            count = std::min<uint32_t>(count, static_cast<uint32_t>((reply.size() - 5) / 4));
            auto [resp, resp_addr] = arena.make<GetWorldInfoListResponse>();
            if (!resp)
                return 0;
            if (count) {
                auto [worlds, worlds_addr] = arena.make<World>(count);
                if (worlds) {
                    for (uint32_t i = 0; i < count; i++) {
                        worlds[i].serverId = server_id;
                        memcpy(&worlds[i].worldId, reply.data() + 5 + i * 4, 4);
                    }
                    resp->world = Ptr<World>(worlds_addr);
                    resp->worldNum = count;
                }
            }
            LOG_INFO("NP Matching2: {} world(s)", count);
            return resp_addr;
        });
}

EXPORT(int, sceNpMatching2CreateJoinRoom, ContextId ctx_id, Ptr<const CreateJoinRoomRequest> req_ptr, Ptr<RequestOptParam> opt, Ptr<RequestId> assigned_req_id) {
    if (!req_ptr)
        return RET_ERROR(ERROR_INVALID_ARGUMENT);
    auto &mem = emuenv.mem;
    const CreateJoinRoomRequest &req = *req_ptr.get(mem);

    pb::Writer w;
    w.u32(1, req.worldId);
    w.u64(2, req.lobbyId);
    w.u32(3, req.maxSlot);
    w.u32(4, req.flagAttr);
    put_bin_attrs(mem, w, 5, req.roomBinAttrInternal, req.roomBinAttrInternalNum);
    put_int_attrs(mem, w, 6, req.roomSearchableIntAttrExternal, req.roomSearchableIntAttrExternalNum);
    put_bin_attrs(mem, w, 7, req.roomSearchableBinAttrExternal, req.roomSearchableBinAttrExternalNum);
    put_bin_attrs(mem, w, 8, req.roomBinAttrExternal, req.roomBinAttrExternalNum);
    if (req.roomPassword)
        w.bytes(9, req.roomPassword.get(mem), 8);
    if (req.groupConfig) {
        const RoomGroupConfig *g = req.groupConfig.get(mem);
        for (uint32_t i = 0; i < req.groupConfigNum; i++) {
            pb::Writer gw;
            gw.u32(1, g[i].slotNum);
            if (g[i].withLabel)
                gw.bytes(2, g[i].label.data, 8);
            gw.boolean(3, g[i].withPassword);
            w.message(10, gw);
        }
    }
    if (req.passwordSlotMask)
        w.u64(11, *req.passwordSlotMask.get(mem));
    auto put_users = [&](uint32_t field, Ptr<np::SceNpId> users, uint32_t num) {
        if (!users)
            return;
        const np::SceNpId *u = users.get(mem);
        for (uint32_t i = 0; i < num; i++)
            w.string(field, std::string(u[i].handle.data, strnlen(u[i].handle.data, sizeof(u[i].handle.data))));
    };
    put_users(12, req.allowedUser, req.allowedUserNum);
    put_users(13, req.blockedUser, req.blockedUserNum);
    if (req.joinRoomGroupLabel)
        w.bytes(14, req.joinRoomGroupLabel.get(mem)->data, 8);
    put_bin_attrs(mem, w, 15, req.roomMemberBinAttrInternal, req.roomMemberBinAttrInternalNum);
    w.wrapped(16, req.teamId);
    if (req.sigOptParam) {
        const SignalingOptParam *s = req.sigOptParam.get(mem);
        LOG_INFO("NP Matching2: CreateJoinRoom signaling type {} flag {} hub {}", s->type, s->flag, s->hubMemberId);
        pb::Writer sw;
        sw.wrapped(1, s->type);
        sw.wrapped(2, s->flag);
        sw.wrapped(3, s->hubMemberId);
        w.message(17, sw);
    }

    const std::string com_id = context_com_id(ctx_id);
    return send_request(emuenv, export_name, ctx_id, opt, assigned_req_id, REQUEST_EVENT_CreateJoinRoom, rpcn::CommandType::CreateRoom, with_com_id(com_id, w),
        [&emuenv, ctx_id](Arena &arena, const std::vector<uint8_t> &reply) -> Address {
            size_t offset = 1;
            std::vector<uint8_t> storage;
            const auto msg = read_message(reply, offset, storage);
            if (!msg)
                return 0;
            auto [resp, resp_addr] = arena.make<CreateJoinRoomResponse>();
            if (!resp)
                return 0;
            RoomMemberId me = 0;
            resp->roomDataInternal = Ptr<RoomDataInternal>(make_room_data_internal(arena, msg->message(1), my_npid(emuenv), &me));
            if (const RoomDataInternal *r = resp->roomDataInternal.get(arena.mem)) {
                LOG_INFO("NP Matching2: created room {} (member id {})", r->roomId, me);
                record_members(r->roomId, ctx_id, me, msg->message(1));
            }
            return resp_addr;
        });
}

EXPORT(int, sceNpMatching2JoinRoom, ContextId ctx_id, Ptr<const JoinRoomRequest> req_ptr, Ptr<RequestOptParam> opt, Ptr<RequestId> assigned_req_id) {
    if (!req_ptr)
        return RET_ERROR(ERROR_INVALID_ARGUMENT);
    auto &mem = emuenv.mem;
    const JoinRoomRequest &req = *req_ptr.get(mem);

    pb::Writer w;
    w.u64(1, req.roomId);
    if (req.roomPassword)
        w.bytes(2, req.roomPassword.get(mem), 8);
    if (req.joinRoomGroupLabel)
        w.bytes(3, req.joinRoomGroupLabel.get(mem)->data, 8);
    put_bin_attrs(mem, w, 4, req.roomMemberBinAttrInternal, req.roomMemberBinAttrInternalNum);
    put_opt_data(w, 5, req.optData, req.optDataLen);
    w.wrapped(6, req.teamId);

    const std::string com_id = context_com_id(ctx_id);
    return send_request(emuenv, export_name, ctx_id, opt, assigned_req_id, REQUEST_EVENT_JoinRoom, rpcn::CommandType::JoinRoom, with_com_id(com_id, w),
        [&emuenv, ctx_id](Arena &arena, const std::vector<uint8_t> &reply) -> Address {
            size_t offset = 1;
            std::vector<uint8_t> storage;
            const auto msg = read_message(reply, offset, storage);
            if (!msg)
                return 0;
            auto [resp, resp_addr] = arena.make<JoinRoomResponse>();
            if (!resp)
                return 0;
            RoomMemberId me = 0;
            resp->roomDataInternal = Ptr<RoomDataInternal>(make_room_data_internal(arena, msg->message(1), my_npid(emuenv), &me));
            if (const RoomDataInternal *r = resp->roomDataInternal.get(arena.mem)) {
                LOG_INFO("NP Matching2: joined room {} (member id {}, {} member(s))", r->roomId, me, r->memberList.membersNum);
                const RoomId room_id = r->roomId;
                record_members(room_id, ctx_id, me, msg->message(1));
                // Matching2SignalingInfo { uint16 member_id = 1; SignalingAddr addr = 2; }
                for (const auto &sig : msg->messages(2)) {
                    uint32_t addr;
                    uint16_t port;
                    if (read_signaling_addr(sig.message(2), addr, port))
                        connect_member(emuenv, room_id, static_cast<RoomMemberId>(sig.wrapped(1)), addr, port);
                }
            }
            return resp_addr;
        });
}

EXPORT(int, sceNpMatching2LeaveRoom, ContextId ctx_id, Ptr<const LeaveRoomRequest> req_ptr, Ptr<RequestOptParam> opt, Ptr<RequestId> assigned_req_id) {
    if (!req_ptr)
        return RET_ERROR(ERROR_INVALID_ARGUMENT);
    const LeaveRoomRequest &req = *req_ptr.get(emuenv.mem);
    pb::Writer w;
    w.u64(1, req.roomId);
    put_opt_data(w, 2, req.optData, req.optDataLen);
    const RoomId room_id = req.roomId;

    const std::string com_id = context_com_id(ctx_id);
    return send_request(emuenv, export_name, ctx_id, opt, assigned_req_id, REQUEST_EVENT_LeaveRoom, rpcn::CommandType::LeaveRoom, with_com_id(com_id, w),
        [&emuenv, room_id](Arena &, const std::vector<uint8_t> &) -> Address {
            LOG_INFO("NP Matching2: left room {}", room_id);
            if (const auto port_p2p = p2p(emuenv))
                port_p2p->disconnect_room(room_id);
            std::lock_guard lock(state().mutex);
            state().rooms.erase(room_id);
            return 0;
        });
}

EXPORT(int, sceNpMatching2SearchRoom, ContextId ctx_id, Ptr<const SearchRoomRequest> req_ptr, Ptr<RequestOptParam> opt, Ptr<RequestId> assigned_req_id) {
    if (!req_ptr)
        return RET_ERROR(ERROR_INVALID_ARGUMENT);
    auto &mem = emuenv.mem;
    const SearchRoomRequest &req = *req_ptr.get(mem);
    static bool dumped = false;
    if (!dumped) {
        dumped = true;
    }

    pb::Writer w;
    w.i32(1, req.option);
    w.u32(2, req.worldId);
    w.u64(3, req.lobbyId);
    w.u32(4, req.rangeFilter.startIndex);
    w.u32(5, req.rangeFilter.max);
    w.u32(6, req.flagFilter);
    w.u32(7, req.flagAttr);
    if (req.intFilter) {
        const IntSearchFilter *f = req.intFilter.get(mem);
        for (uint32_t i = 0; i < req.intFilterNum; i++) {
            pb::Writer a;
            a.wrapped(1, f[i].attr.id);
            a.u32(2, f[i].attr.num);
            pb::Writer fw;
            fw.wrapped(1, f[i].searchOperator);
            fw.message(2, a);
            w.message(8, fw);
        }
    }
    if (req.binFilter) {
        const BinSearchFilter *f = req.binFilter.get(mem);
        for (uint32_t i = 0; i < req.binFilterNum; i++) {
            pb::Writer fw;
            fw.wrapped(1, f[i].searchOperator);
            put_bin_attr(mem, fw, 2, f[i].attr);
            w.message(9, fw);
        }
    }
    if (req.attrId) {
        const AttributeId *ids = req.attrId.get(mem);
        for (uint32_t i = 0; i < req.attrIdNum; i++)
            w.wrapped(10, ids[i]);
    }

    const std::string com_id = context_com_id(ctx_id);
    return send_request(emuenv, export_name, ctx_id, opt, assigned_req_id, REQUEST_EVENT_SearchRoom, rpcn::CommandType::SearchRoom, with_com_id(com_id, w),
        [](Arena &arena, const std::vector<uint8_t> &reply) -> Address {
            size_t offset = 1;
            std::vector<uint8_t> storage;
            const auto msg = read_message(reply, offset, storage);
            if (!msg)
                return 0;
            auto [resp, resp_addr] = arena.make<SearchRoomResponse>();
            if (!resp)
                return 0;
            const auto rooms = msg->messages(3);
            resp->range.startIndex = msg->u32(1);
            resp->range.total = msg->u32(2);
            resp->range.size = static_cast<uint32_t>(rooms.size());
            resp->roomDataExternal = Ptr<RoomDataExternal>(make_room_data_external_list(arena, rooms));
            LOG_INFO("NP Matching2: SearchRoom found {} room(s) (total {})", rooms.size(), resp->range.total);
            return resp_addr;
        });
}

EXPORT(int, sceNpMatching2GetRoomDataExternalList, ContextId ctx_id, Ptr<const GetRoomDataExternalListRequest> req_ptr, Ptr<RequestOptParam> opt, Ptr<RequestId> assigned_req_id) {
    if (!req_ptr)
        return RET_ERROR(ERROR_INVALID_ARGUMENT);
    auto &mem = emuenv.mem;
    const GetRoomDataExternalListRequest &req = *req_ptr.get(mem);
    pb::Writer w;
    if (req.roomId) {
        const RoomId *ids = req.roomId.get(mem);
        for (uint32_t i = 0; i < req.roomIdNum; i++)
            w.u64_always(1, ids[i]);
    }
    if (req.attrId) {
        const AttributeId *ids = req.attrId.get(mem);
        for (uint32_t i = 0; i < req.attrIdNum; i++)
            w.wrapped(2, ids[i]);
    }

    const std::string com_id = context_com_id(ctx_id);
    return send_request(emuenv, export_name, ctx_id, opt, assigned_req_id, REQUEST_EVENT_GetRoomDataExternalList, rpcn::CommandType::GetRoomDataExternalList, with_com_id(com_id, w),
        [](Arena &arena, const std::vector<uint8_t> &reply) -> Address {
            size_t offset = 1;
            std::vector<uint8_t> storage;
            const auto msg = read_message(reply, offset, storage);
            if (!msg)
                return 0;
            auto [resp, resp_addr] = arena.make<GetRoomDataExternalListResponse>();
            if (!resp)
                return 0;
            const auto rooms = msg->messages(1);
            resp->roomDataExternal = Ptr<RoomDataExternal>(make_room_data_external_list(arena, rooms));
            resp->roomDataExternalNum = static_cast<uint32_t>(rooms.size());
            return resp_addr;
        });
}

EXPORT(int, sceNpMatching2SetRoomDataExternal, ContextId ctx_id, Ptr<const SetRoomDataExternalRequest> req_ptr, Ptr<RequestOptParam> opt, Ptr<RequestId> assigned_req_id) {
    if (!req_ptr)
        return RET_ERROR(ERROR_INVALID_ARGUMENT);
    auto &mem = emuenv.mem;
    const SetRoomDataExternalRequest &req = *req_ptr.get(mem);
    pb::Writer w;
    w.u64(1, req.roomId);
    put_int_attrs(mem, w, 2, req.roomSearchableIntAttrExternal, req.roomSearchableIntAttrExternalNum);
    put_bin_attrs(mem, w, 3, req.roomSearchableBinAttrExternal, req.roomSearchableBinAttrExternalNum);
    put_bin_attrs(mem, w, 4, req.roomBinAttrExternal, req.roomBinAttrExternalNum);

    const std::string com_id = context_com_id(ctx_id);
    return send_request(emuenv, export_name, ctx_id, opt, assigned_req_id, REQUEST_EVENT_SetRoomDataExternal, rpcn::CommandType::SetRoomDataExternal, with_com_id(com_id, w), nullptr);
}

EXPORT(int, sceNpMatching2SetRoomDataInternal, ContextId ctx_id, Ptr<const SetRoomDataInternalRequest> req_ptr, Ptr<RequestOptParam> opt, Ptr<RequestId> assigned_req_id) {
    if (!req_ptr)
        return RET_ERROR(ERROR_INVALID_ARGUMENT);
    auto &mem = emuenv.mem;
    const SetRoomDataInternalRequest &req = *req_ptr.get(mem);
    pb::Writer w;
    w.u64(1, req.roomId);
    w.u32(2, req.flagFilter);
    w.u32(3, req.flagAttr);
    put_bin_attrs(mem, w, 4, req.roomBinAttrInternal, req.roomBinAttrInternalNum);
    if (req.passwordConfig)
        LOG_WARN("NP Matching2: SetRoomDataInternal password config is not supported");
    if (req.passwordSlotMask)
        w.u64_always(6, *req.passwordSlotMask.get(mem));
    if (req.ownerPrivilegeRank) {
        const RoomMemberId *ranks = req.ownerPrivilegeRank.get(mem);
        for (uint32_t i = 0; i < req.ownerPrivilegeRankNum; i++)
            w.wrapped(7, ranks[i]);
    }

    const std::string com_id = context_com_id(ctx_id);
    return send_request(emuenv, export_name, ctx_id, opt, assigned_req_id, REQUEST_EVENT_SetRoomDataInternal, rpcn::CommandType::SetRoomDataInternal, with_com_id(com_id, w), nullptr);
}

EXPORT(int, sceNpMatching2SetRoomMemberDataInternal, ContextId ctx_id, Ptr<const SetRoomMemberDataInternalRequest> req_ptr, Ptr<RequestOptParam> opt, Ptr<RequestId> assigned_req_id) {
    if (!req_ptr)
        return RET_ERROR(ERROR_INVALID_ARGUMENT);
    auto &mem = emuenv.mem;
    const SetRoomMemberDataInternalRequest &req = *req_ptr.get(mem);
    pb::Writer w;
    w.u64(1, req.roomId);
    w.wrapped(2, req.memberId);
    w.wrapped(3, req.teamId);
    put_bin_attrs(mem, w, 4, req.roomMemberBinAttrInternal, req.roomMemberBinAttrInternalNum);

    const std::string com_id = context_com_id(ctx_id);
    return send_request(emuenv, export_name, ctx_id, opt, assigned_req_id, REQUEST_EVENT_SetRoomMemberDataInternal, rpcn::CommandType::SetRoomMemberDataInternal, with_com_id(com_id, w), nullptr);
}

EXPORT(int, sceNpMatching2KickoutRoomMember, ContextId ctx_id, Ptr<const KickoutRoomMemberRequest> req_ptr) {
    if (req_ptr)
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2GetLobbyInfoList) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2GetMemoryInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2GetRoomDataInternal) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2GetRoomMemberDataExternalList) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2GetRoomMemberDataInternal) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2GetRoomMemberIdListLocal) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2GetRoomPasswordLocal) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2GetSignalingOptParamLocal) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2GetUserInfoList) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2GrantRoomOwner) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2JoinLobby) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2LeaveLobby) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2SendLobbyChatMessage) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2SendRoomChatMessage) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2SendRoomMessage) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2SetSignalingOptParam) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2SetUserInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2SignalingCancelPeerNetInfo) {
    return UNIMPLEMENTED();
}

// Union of rtt / bandwidth / SceNpId / {addr, port} / packetLoss
struct SignalingConnectionInfo {
    uint8_t data[sizeof(np::SceNpId)];
};

EXPORT(int, sceNpMatching2SignalingGetConnectionInfo, ContextId ctx_id, RoomId room_id, RoomMemberId member_id, int32_t code, SignalingConnectionInfo *info) {
    if (!state().inited)
        return RET_ERROR(ERROR_NOT_INITIALIZED);
    if (!info)
        return RET_ERROR(ERROR_INVALID_ARGUMENT);
    const std::string npid = member_npid(room_id, member_id);
    if (npid.empty())
        return RET_ERROR(ERROR_ROOM_MEMBER_NOT_FOUND);
    const auto port_p2p = p2p(emuenv);
    const auto peer = port_p2p ? port_p2p->peer(npid) : std::nullopt;
    memset(info, 0, sizeof(*info));
    switch (code) {
    case SIGNALING_CONN_INFO_RTT: {
        const uint32_t rtt = peer ? peer->rtt_us : 0;
        memcpy(info->data, &rtt, 4);
        break;
    }
    case SIGNALING_CONN_INFO_BANDWIDTH: {
        const uint32_t bandwidth = 100'000'000; // same assumption as RPCS3
        memcpy(info->data, &bandwidth, 4);
        break;
    }
    case SIGNALING_CONN_INFO_PEER_NPID:
        fill_npid(*reinterpret_cast<np::SceNpId *>(info->data), npid);
        break;
    case SIGNALING_CONN_INFO_PEER_ADDRESS:
    case SIGNALING_CONN_INFO_MAPPED_ADDRESS: {
        // { SceNetInAddr addr; SceNetInPort_t port (network order) }
        const bool peer_addr = code == SIGNALING_CONN_INFO_PEER_ADDRESS;
        const uint32_t addr = peer ? (peer_addr ? peer->addr : peer->mapped_addr) : 0;
        const uint16_t port = peer ? (peer_addr ? peer->port : peer->mapped_port) : 0;
        const uint16_t port_be = static_cast<uint16_t>((port >> 8) | (port << 8));
        memcpy(info->data, &addr, 4);
        memcpy(info->data + 4, &port_be, 2);
        break;
    }
    case SIGNALING_CONN_INFO_PACKET_LOSS:
        break;
    default:
        return RET_ERROR(ERROR_INVALID_ARGUMENT);
    }
    return 0;
}

EXPORT(int, sceNpMatching2SignalingGetConnectionStatus, ContextId ctx_id, RoomId room_id, RoomMemberId member_id, int32_t *conn_status, uint32_t *peer_addr, uint16_t *peer_port) {
    if (!state().inited)
        return RET_ERROR(ERROR_NOT_INITIALIZED);
    if (!conn_status)
        return RET_ERROR(ERROR_INVALID_ARGUMENT);
    *conn_status = static_cast<int32_t>(rpcn::SignalingStatus::Inactive);
    const std::string npid = member_npid(room_id, member_id);
    if (npid.empty())
        return RET_ERROR(ERROR_ROOM_MEMBER_NOT_FOUND);
    const auto port_p2p = p2p(emuenv);
    const auto peer = port_p2p ? port_p2p->peer(npid) : std::nullopt;
    if (!peer)
        return 0;
    *conn_status = static_cast<int32_t>(peer->status);
    if (peer_addr)
        *peer_addr = peer->addr;
    if (peer_port)
        *peer_port = static_cast<uint16_t>((peer->port >> 8) | (peer->port << 8)); // network order
    return 0;
}

EXPORT(int, sceNpMatching2SignalingGetLocalNetInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2SignalingGetPeerNetInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2SignalingGetPeerNetInfoResult) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpMatching2SignalingGetPingInfo) {
    return UNIMPLEMENTED();
}
