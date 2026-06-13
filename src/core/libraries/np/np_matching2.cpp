// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "core/libraries/np/np_handler.h"
#include "core/libraries/np/np_manager.h"
#include "core/libraries/np/np_matching2.h"
#include "core/libraries/np/np_types.h"
#include "core/libraries/system/userservice.h"
#include "shadnet.pb.h"

namespace Libraries::Np::NpMatching2 {

static bool g_initialized = false;
static OrbisNpMatching2ContextId contextId = 1;

struct NpMatching2ContextEvent {
    OrbisNpMatching2ContextId contextId;
    OrbisNpMatching2Event event;
    OrbisNpMatching2EventCause cause;
    int errorCode;
};

struct NpMatching2LobbyEvent {
    OrbisNpMatching2ContextId contextId;
    OrbisNpMatching2LobbyId lobbyId;
    OrbisNpMatching2Event event;
    void* data;
};

struct NpMatching2RoomEvent {
    OrbisNpMatching2ContextId contextId;
    OrbisNpMatching2RoomId roomId;
    OrbisNpMatching2Event event;
    void* data;
};

static std::mutex g_events_mutex;
static std::deque<NpMatching2ContextEvent> g_ctx_events;
static std::deque<NpMatching2LobbyEvent> g_lobby_events;
static std::deque<NpMatching2RoomEvent> g_room_events;
static std::mutex g_responses_mutex;
static std::deque<std::function<void()>> g_responses;

// Per-context info captured at CreateContext time: maps a context id to the
// owning user and its service label, so room requests can be routed to the
// right shadNet client.
struct ContextInfo {
    s32 user_id;
    s32 service_label;
};
static std::mutex g_contexts_mutex;
static std::map<OrbisNpMatching2ContextId, ContextInfo> g_contexts;

static void RegisterContext(OrbisNpMatching2ContextId ctxId, s32 user_id, s32 service_label) {
    std::scoped_lock lk{g_contexts_mutex};
    g_contexts[ctxId] = {user_id, service_label};
}

// Frame a serialized proto as [u32-LE size][bytes] (no com_id prefix — room
// commands are not com-id scoped, unlike score) and submit it to the shadNet
// server for the user that owns the context. Returns the submit packet id.
static u64 SubmitRoomRequest(OrbisNpMatching2ContextId ctxId, ShadNet::CommandType cmd,
                             const std::string& proto_bytes) {
    s32 user_id = -1;
    {
        std::scoped_lock lk{g_contexts_mutex};
        if (auto it = g_contexts.find(ctxId); it != g_contexts.end()) {
            user_id = it->second.user_id;
        }
    }
    if (user_id < 0) {
        LOG_WARNING(Lib_NpMatching2, "SubmitRoomRequest: unknown ctxId {}", ctxId);
        return 0;
    }

    std::vector<u8> payload;
    payload.reserve(4 + proto_bytes.size());
    const u32 sz = static_cast<u32>(proto_bytes.size());
    payload.push_back(static_cast<u8>(sz));
    payload.push_back(static_cast<u8>(sz >> 8));
    payload.push_back(static_cast<u8>(sz >> 16));
    payload.push_back(static_cast<u8>(sz >> 24));
    payload.insert(payload.end(), proto_bytes.begin(), proto_bytes.end());

    return Libraries::Np::NpHandler::GetInstance().SubmitMatchingRequest(user_id, cmd, payload);
}

struct OrbisNpMatching2CreateContextParameter {
    Libraries::Np::OrbisNpId* npId;
    void* npCommunicationId;
    void* npPassphrase;
    Libraries::Np::OrbisNpServiceLabel serviceLabel;
    u64 size;
};

static_assert(sizeof(OrbisNpMatching2CreateContextParameter) == 0x28);

int PS4_SYSV_ABI sceNpMatching2CreateContext(const OrbisNpMatching2CreateContextParameter* param,
                                             OrbisNpMatching2ContextId* ctxId) {
    LOG_DEBUG(Lib_NpMatching2, "called, npId = {}, serviceLabel = {}, size = {}",
              param->npId->handle.data, param->serviceLabel, param->size);

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!param || param->size != 0x28 || !ctxId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    *ctxId = contextId++;
    const s32 user_id =
        param->npId ? NpHandler::GetInstance().GetUserIdByOnlineId(param->npId->handle) : -1;
    RegisterContext(*ctxId, user_id, param->serviceLabel);

    return ORBIS_OK;
}

struct OrbisNpMatching2CreateContextParameterA {
    Libraries::UserService::OrbisUserServiceUserId userId;
    Libraries::Np::OrbisNpServiceLabel serviceLabel;
    u64 size;
};

static_assert(sizeof(OrbisNpMatching2CreateContextParameterA) == 16);

int PS4_SYSV_ABI sceNpMatching2CreateContextA(const OrbisNpMatching2CreateContextParameterA* param,
                                              OrbisNpMatching2ContextId* ctxId) {
    LOG_DEBUG(Lib_NpMatching2, "called, userId = {}, serviceLabel = {}, size = {}", param->userId,
              param->serviceLabel, param->size);

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!param || param->size != 0x10 || !ctxId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    *ctxId = contextId++;
    RegisterContext(*ctxId, param->userId, param->serviceLabel);

    return ORBIS_OK;
}

using OrbisNpMatching2RequestCallback = PS4_SYSV_ABI void (*)(OrbisNpMatching2ContextId,
                                                              OrbisNpMatching2RequestId,
                                                              OrbisNpMatching2Event, int, void*,
                                                              void*);

struct OrbisNpMatching2RequestOptParam {
    OrbisNpMatching2RequestCallback callback;
    void* arg;
    u32 timeout;
    u16 appId;
    u8 dummy[2];
};

static std::optional<OrbisNpMatching2RequestOptParam> defaultRequestOptParam = std::nullopt;

auto GetOptParam(OrbisNpMatching2RequestOptParam* requestOpt) {
    return requestOpt ? *requestOpt
                      : (defaultRequestOptParam ? defaultRequestOptParam
                                                : std::optional<OrbisNpMatching2RequestOptParam>{});
}

// A room request submitted to the shadNet server and awaiting its reply, keyed
// by the submit packet id. Holds what HandleRoomReply needs to fire the toolkit
// callback once the server answers.
struct PendingRoomRequest {
    OrbisNpMatching2ContextId ctxId;
    OrbisNpMatching2RequestId requestId;
    OrbisNpMatching2RequestCallback callback;
    void* arg;
};
static std::mutex g_pending_rooms_mutex;
static std::map<u64, PendingRoomRequest> g_pending_rooms;

static void StashRoomRequest(u64 pkt_id, const PendingRoomRequest& pending) {
    if (pkt_id == 0) {
        return; // request was not sent (no connected client)
    }
    std::scoped_lock lk{g_pending_rooms_mutex};
    g_pending_rooms[pkt_id] = pending;
}

struct OrbisNpMatching2CreateJoinRoomRequestA {
    u16 maxSlot;
    OrbisNpMatching2TeamId teamId;
    u8 pad[5];
    OrbisNpMatching2Flags flags;
    OrbisNpMatching2WorldId worldId;
    OrbisNpMatching2LobbyId lobbyId;
    void* roomPasswd;
    void* passwdSlotMask;
    void* groupConfig;
    u64 groupConfigs;
    void* joinGroupLabel;
    Libraries::Np::OrbisNpAccountId* allowedUser;
    u64 allowedUsers;
    Libraries::Np::OrbisNpAccountId* blockedUser;
    u64 blockedUsers;
    void* internalBinAttr;
    u64 internalBinAttrs;
    void* externalSearchIntAttr;
    u64 externalSearchIntAttrs;
    void* externalSearchBinAttr;
    u64 externalSearchBinAttrs;
    void* externalBinAttr;
    u64 externalBinAttrs;
    void* memberInternalBinAttr;
    u64 memberInternalBinAttrs;
    void* signalingParam;
};

static_assert(sizeof(OrbisNpMatching2CreateJoinRoomRequestA) == 184);

struct OrbisNpMatching2RoomDataInternal {
    u16 publicSlots;
    u16 privateSlots;
    u16 openPublicSlots;
    u16 openPrivateSlots;
    u16 maxSlot;
    OrbisNpMatching2ServerId serverId;
    OrbisNpMatching2WorldId worldId;
    OrbisNpMatching2LobbyId lobbyId;
    OrbisNpMatching2RoomId roomId;
    u64 passwdSlotMask;
    u64 joinedSlotMask;
    void* roomGroup;
    u64 roomGroups;
    OrbisNpMatching2Flags flags;
    u8 pad[4];
    void* internalBinAttr;
    u64 internalBinAttrs;
};

struct OrbisNpMatching2RoomMemberDataInternalA {
    OrbisNpMatching2RoomMemberDataInternalA* next;
    u64 joinDateTicks;
    Libraries::Np::OrbisNpPeerAddressA user;
    Libraries::Np::OrbisNpOnlineId onlineId;
    u8 pad[4];
    OrbisNpMatching2RoomMemberId memberId;
    OrbisNpMatching2TeamId teamId;
    OrbisNpMatching2NatType natType;
    OrbisNpMatching2Flags flags;
    void* roomGroup;
    void* roomMemberInternalBinAttr;
    u64 roomMemberInternalBinAttrs;
};

struct OrbisNpMatching2RoomMemberDataInternalListA {
    OrbisNpMatching2RoomMemberDataInternalA* members;
    u64 membersNum;
    OrbisNpMatching2RoomMemberDataInternalA* me;
    OrbisNpMatching2RoomMemberDataInternalA* owner;
};

struct OrbisNpMatching2CreateJoinRoomResponseA {
    OrbisNpMatching2RoomDataInternal* roomData;
    OrbisNpMatching2RoomMemberDataInternalListA members;
};

// JoinRoom shares the response shape with CreateJoinRoom.
using OrbisNpMatching2JoinRoomResponseA = OrbisNpMatching2CreateJoinRoomResponseA;

struct OrbisNpMatching2PresenceOptionData {
    u8 data[16];
    u64 len;
};

// Room info retrievable outside the room (search/list results). Layout mirrors the SDK's
// SceNpMatching2RoomDataExternalA exactly so the toolkit reads each field at the right offset.
struct OrbisNpMatching2RoomDataExternalA {
    OrbisNpMatching2RoomDataExternalA* next;
    u16 maxSlot;
    u16 curMemberNum;
    OrbisNpMatching2Flags flagAttr;
    OrbisNpMatching2ServerId serverId;
    u8 padding[2];
    OrbisNpMatching2WorldId worldId;
    OrbisNpMatching2LobbyId lobbyId;
    OrbisNpMatching2RoomId roomId;
    u64 passwordSlotMask;
    u64 joinedSlotMask;
    u16 publicSlotNum;
    u16 privateSlotNum;
    u16 openPublicSlotNum;
    u16 openPrivateSlotNum;
    Libraries::Np::OrbisNpPeerAddressA owner;
    Libraries::Np::OrbisNpOnlineId ownerOnlineId;
    void* roomGroup;
    u64 roomGroupNum;
    void* roomSearchableIntAttrExternal;
    u64 roomSearchableIntAttrExternalNum;
    void* roomSearchableBinAttrExternal;
    u64 roomSearchableBinAttrExternalNum;
    void* roomBinAttrExternal;
    u64 roomBinAttrExternalNum;
};
static_assert(sizeof(OrbisNpMatching2RoomDataExternalA) == 168);

struct OrbisNpMatching2Range {
    u32 start;
    u32 total;
    u32 results;
    u8 pad[4];
};

struct OrbisNpMatching2SearchRoomResponseA {
    OrbisNpMatching2Range range;
    OrbisNpMatching2RoomDataExternalA* roomDataExternal;
};

struct OrbisNpMatching2JoinRoomRequestA {
    OrbisNpMatching2RoomId roomId;
    void* roomPassword;
    void* joinRoomGroupLabel;
    void* roomMemberBinAttrInternal;
    u64 roomMemberBinAttrInternalNum;
    OrbisNpMatching2PresenceOptionData optData;
    OrbisNpMatching2TeamId teamId;
    u8 padding[3];
    OrbisNpMatching2Flags flagAttr;
    Libraries::Np::OrbisNpAccountId* blockedUser;
    u64 blockedUserNum;
};

// Fill an OrbisNpOnlineId from an npid handle string (truncated to the 16-byte field).
static Libraries::Np::OrbisNpOnlineId MakeOnlineId(const std::string& handle) {
    Libraries::Np::OrbisNpOnlineId id{};
    const size_t n = std::min<size_t>(handle.size(), ORBIS_NP_ONLINEID_MAX_LENGTH - 1);
    std::memcpy(id.data, handle.data(), n);
    return id;
}

int PS4_SYSV_ABI sceNpMatching2CreateJoinRoomA(OrbisNpMatching2ContextId ctxId,
                                               OrbisNpMatching2CreateJoinRoomRequestA* request,
                                               OrbisNpMatching2RequestOptParam* requestOpt,
                                               OrbisNpMatching2RequestId* requestId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    LOG_DEBUG(Lib_NpMatching2,
              "maxSlot = {}, teamId = {}, worldId = {}, lobbyId = {}, groupConfig = {}, "
              "joinGroupLabel = {}",
              request->maxSlot, request->teamId, request->worldId, request->lobbyId,
              request->groupConfig, request->joinGroupLabel);

    static OrbisNpMatching2RequestId id = 10;
    *requestId = id++;

    // Register the room on the shadNet server, then stash the request so HandleRoomReply
    // can fire the toolkit callback with the real room once the server answers.
    auto optParam = GetOptParam(requestOpt);
    {
        shadnet::CreateRoomRequest pb;
        pb.set_req_id(*requestId);
        pb.set_max_slots(request->maxSlot);
        pb.set_team_id(request->teamId);
        pb.set_world_id(request->worldId);
        pb.set_lobby_id(request->lobbyId);
        pb.set_flags(request->flags);
        pb.set_group_config_count(static_cast<u32>(request->groupConfigs));
        pb.set_allowed_user_count(static_cast<u32>(request->allowedUsers));
        pb.set_blocked_user_count(static_cast<u32>(request->blockedUsers));
        pb.set_internal_bin_attr_count(static_cast<u32>(request->internalBinAttrs));
        pb.set_join_group_label_present(request->joinGroupLabel != nullptr);
        pb.set_room_password_present(request->roomPasswd != nullptr);
        // sig_type/sig_flag/sig_main_member come from signalingParam, whose layout is not
        // modeled yet; left at 0 until signaling is wired.
        const u64 pkt_id =
            SubmitRoomRequest(ctxId, ShadNet::CommandType::CreateRoom, pb.SerializeAsString());
        LOG_INFO(Lib_NpMatching2,
                 "CreateJoinRoom: sent to shadNet, ctxId={} reqId={} maxSlot={} pkt_id={}", ctxId,
                 *requestId, request->maxSlot, pkt_id);
        if (optParam) {
            StashRoomRequest(pkt_id, {ctxId, *requestId, optParam->callback, optParam->arg});
        }
    }
    return ORBIS_OK;
}

using OrbisNpMatching2ContextCallback = PS4_SYSV_ABI void (*)(OrbisNpMatching2ContextId contextId,
                                                              OrbisNpMatching2Event event,
                                                              OrbisNpMatching2EventCause cause,
                                                              int errorCode, void* userdata);

std::function<void(const NpMatching2ContextEvent*)> npMatching2ContextCallback = nullptr;

int PS4_SYSV_ABI sceNpMatching2RegisterContextCallback(OrbisNpMatching2ContextCallback callback,
                                                       void* userdata) {
    LOG_DEBUG(Lib_NpMatching2, "called, userdata = {}", userdata);

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }

    npMatching2ContextCallback = [callback, userdata](auto arg) {
        callback(arg->contextId, arg->event, arg->cause, arg->errorCode, userdata);
    };

    return ORBIS_OK;
}

using OrbisNpMatching2LobbyEventCallback =
    PS4_SYSV_ABI void (*)(OrbisNpMatching2ContextId contextId, OrbisNpMatching2LobbyId lobbyId,
                          OrbisNpMatching2Event event, void* data, void* userdata);

std::function<void(const NpMatching2LobbyEvent*)> npMatching2LobbyCallback = nullptr;

int PS4_SYSV_ABI sceNpMatching2RegisterLobbyEventCallback(
    OrbisNpMatching2ContextId ctxId, OrbisNpMatching2LobbyEventCallback callback, void* userdata) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, userdata = {}", ctxId, userdata);

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }

    npMatching2LobbyCallback = [callback, userdata](auto arg) {
        callback(arg->contextId, arg->lobbyId, arg->event, arg->data, userdata);
    };

    return ORBIS_OK;
}

using OrbisNpMatching2RoomEventCallback = PS4_SYSV_ABI void (*)(OrbisNpMatching2ContextId contextId,
                                                                OrbisNpMatching2RoomId roomId,
                                                                OrbisNpMatching2Event event,
                                                                void* data, void* userdata);

std::function<void(const NpMatching2RoomEvent*)> npMatching2RoomCallback = nullptr;

int PS4_SYSV_ABI sceNpMatching2RegisterRoomEventCallback(OrbisNpMatching2ContextId ctxId,
                                                         OrbisNpMatching2RoomEventCallback callback,
                                                         void* userdata) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, userdata = {}", ctxId, userdata);

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }

    npMatching2RoomCallback = [callback, userdata](auto arg) {
        callback(arg->contextId, arg->roomId, arg->event, arg->data, userdata);
    };

    return ORBIS_OK;
}

struct OrbisNpMatching2SignalingEvent {
    OrbisNpMatching2ContextId contextId;
    OrbisNpMatching2RoomId roomId;
    OrbisNpMatching2RoomMemberId roomMemberId;
    OrbisNpMatching2Event event;
    int errorCode;
};

using OrbisNpMatching2SignalingCallback =
    PS4_SYSV_ABI void (*)(OrbisNpMatching2ContextId contextId, OrbisNpMatching2RoomId roomId,
                          OrbisNpMatching2RoomMemberId roomMemberId, OrbisNpMatching2Event event,
                          int errorCode, void* userdata);

std::function<void(const OrbisNpMatching2SignalingEvent*)> npMatching2SignalingCallback = nullptr;

int PS4_SYSV_ABI sceNpMatching2RegisterSignalingCallback(OrbisNpMatching2ContextId ctxId,
                                                         OrbisNpMatching2SignalingCallback callback,
                                                         void* userdata) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, userdata = {}", ctxId, userdata);

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }

    npMatching2SignalingCallback = [callback, userdata](auto arg) {
        callback(arg->contextId, arg->roomId, arg->roomMemberId, arg->event, arg->errorCode,
                 userdata);
    };

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2ContextStart(OrbisNpMatching2ContextId ctxId, u64 timeout) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, timeout = {}", ctxId, timeout);

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }

    std::scoped_lock lk{g_events_mutex};
    if (EmulatorSettings.IsConnectedToNetwork() && EmulatorSettings.IsShadNetEnabled()) {
        g_ctx_events.emplace_back(ctxId, ORBIS_NP_MATCHING2_CONTEXT_EVENT_STARTED,
                                  ORBIS_NP_MATCHING2_EVENT_CAUSE_CONTEXT_ACTION, 0);
    } else {
        // error confirmed with a real console disconnected from the internet
        constexpr int ORBIS_NET_ERROR_RESOLVER_ETIMEDOUT = 0x804101e2;
        g_ctx_events.emplace_back(ctxId, ORBIS_NP_MATCHING2_CONTEXT_EVENT_START_OVER,
                                  ORBIS_NP_MATCHING2_EVENT_CAUSE_CONTEXT_ERROR,
                                  ORBIS_NET_ERROR_RESOLVER_ETIMEDOUT);
    }

    return ORBIS_OK;
}

// Extract the [u32-LE size][proto] framing common to all room replies.
static bool ExtractReplyProto(const std::vector<u8>& body, const u8** out, u32* out_sz) {
    if (body.size() < 4) {
        return false;
    }
    const u32 sz = u32(body[0]) | (u32(body[1]) << 8) | (u32(body[2]) << 16) | (u32(body[3]) << 24);
    if (body.size() < 4 + static_cast<size_t>(sz)) {
        return false;
    }
    *out = body.data() + 4;
    *out_sz = sz;
    return true;
}

// Build a RoomDataInternal from a server-side MatchingRoomDataInternal.
static OrbisNpMatching2RoomDataInternal BuildRoomDataInternal(
    const shadnet::MatchingRoomDataInternal& rd) {
    OrbisNpMatching2RoomDataInternal room{};
    room.publicSlots = static_cast<u16>(rd.public_slots());
    room.privateSlots = static_cast<u16>(rd.private_slots());
    room.openPublicSlots = static_cast<u16>(rd.open_public_slots());
    room.openPrivateSlots = static_cast<u16>(rd.open_private_slots());
    room.maxSlot = static_cast<u16>(rd.max_slot());
    room.serverId = static_cast<OrbisNpMatching2ServerId>(rd.server_id());
    room.worldId = rd.world_id();
    room.lobbyId = rd.lobby_id();
    room.roomId = rd.room_id();
    room.passwdSlotMask = rd.passwd_slot_mask();
    room.joinedSlotMask = rd.joined_slot_mask();
    room.flags = rd.flags();
    return room;
}

// Fire the toolkit callback for a CreateJoinRoom / JoinRoom reply. Both carry the same
// CreateJoinRoomResponse details (room + member list); only the event differs.
static void DispatchCreateJoinResponse(const PendingRoomRequest& pending,
                                       OrbisNpMatching2Event event,
                                       const shadnet::CreateJoinRoomResponse& details) {
    // Resolve the local user behind this context so the "me" member carries our real
    // online ID / account ID (the server only echoes npid strings for members).
    s32 user_id = -1;
    {
        std::scoped_lock lk{g_contexts_mutex};
        if (auto it = g_contexts.find(pending.ctxId); it != g_contexts.end()) {
            user_id = it->second.user_id;
        }
    }
    const u32 me_member_id = details.me_member_id();

    std::scoped_lock lk{g_responses_mutex};
    g_responses.emplace_back([pending, event, details, user_id, me_member_id]() {
        // Build the room and the member list on this (callback) thread's stack so their
        // addresses stay valid for the synchronous callback.
        OrbisNpMatching2RoomDataInternal room = BuildRoomDataInternal(details.room_data());

        std::vector<OrbisNpMatching2RoomMemberDataInternalA> members;
        members.reserve(details.members_size());
        for (const auto& m : details.members()) {
            OrbisNpMatching2RoomMemberDataInternalA mem{};
            mem.user.platform = Libraries::Np::OrbisNpPlatformType::PS4;
            mem.memberId = static_cast<OrbisNpMatching2RoomMemberId>(m.member_id());
            mem.teamId = static_cast<OrbisNpMatching2TeamId>(m.team_id());
            mem.natType = 1;
            if (m.member_id() == me_member_id && user_id >= 0) {
                auto& np = Libraries::Np::NpHandler::GetInstance();
                mem.onlineId = np.GetOnlineId(user_id);
                mem.user.accountId = np.GetAccountId(user_id);
            } else {
                mem.onlineId = MakeOnlineId(m.npid());
                mem.user.accountId = 0;
            }
            members.push_back(mem);
        }
        // Link the singly-linked list (vector storage is stable for the call's duration).
        for (size_t i = 0; i + 1 < members.size(); ++i) {
            members[i].next = &members[i + 1];
        }

        OrbisNpMatching2RoomMemberDataInternalA* me = nullptr;
        OrbisNpMatching2RoomMemberDataInternalA* owner = nullptr;
        for (auto& mem : members) {
            if (mem.memberId == details.me_member_id()) {
                me = &mem;
            }
            if (mem.memberId == details.owner_member_id()) {
                owner = &mem;
            }
        }
        OrbisNpMatching2RoomMemberDataInternalA* head = members.empty() ? nullptr : &members[0];

        OrbisNpMatching2CreateJoinRoomResponseA resp{
            &room, {head, members.size(), me ? me : head, owner ? owner : head}};
        pending.callback(pending.ctxId, pending.requestId, event, 0, &resp, pending.arg);
    });
}

// Called (from np_handler's reader thread) when a room command reply arrives from the
// shadNet server. Looks up the pending request and queues the toolkit callback so it
// fires on the NP-callback thread via ProcessEvents.
void HandleRoomReply(u16 cmd, u64 pkt_id, u8 error, const std::vector<u8>& body) {
    PendingRoomRequest pending;
    {
        std::scoped_lock lk{g_pending_rooms_mutex};
        auto it = g_pending_rooms.find(pkt_id);
        if (it == g_pending_rooms.end()) {
            LOG_WARNING(Lib_NpMatching2, "HandleRoomReply: no pending request for pkt_id={} cmd={}",
                        pkt_id, cmd);
            return;
        }
        pending = it->second;
        g_pending_rooms.erase(it);
    }

    // Map the server command to the request-event the toolkit is waiting on.
    OrbisNpMatching2Event event;
    switch (static_cast<ShadNet::CommandType>(cmd)) {
    case ShadNet::CommandType::CreateRoom:
        event = ORBIS_NP_MATCHING2_REQUEST_EVENT_CREATE_JOIN_ROOM_A;
        break;
    case ShadNet::CommandType::JoinRoom:
        event = ORBIS_NP_MATCHING2_REQUEST_EVENT_JOIN_ROOM_A;
        break;
    case ShadNet::CommandType::GetRoomList:
        event = ORBIS_NP_MATCHING2_REQUEST_EVENT_SEARCH_ROOM_A;
        break;
    default:
        LOG_WARNING(Lib_NpMatching2, "HandleRoomReply: unhandled cmd={}", cmd);
        return;
    }

    // On a server error, notify the toolkit with a failure code and no data.
    if (error != static_cast<u8>(ShadNet::ErrorType::NoError)) {
        LOG_WARNING(Lib_NpMatching2, "HandleRoomReply: server error={} cmd={} reqId={}", error, cmd,
                    pending.requestId);
        std::scoped_lock lk{g_responses_mutex};
        g_responses.emplace_back([pending, event]() {
            pending.callback(pending.ctxId, pending.requestId, event,
                             ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT, nullptr, pending.arg);
        });
        return;
    }

    const u8* proto = nullptr;
    u32 sz = 0;
    if (!ExtractReplyProto(body, &proto, &sz)) {
        LOG_WARNING(Lib_NpMatching2, "HandleRoomReply: short reply body ({} bytes) cmd={}",
                    body.size(), cmd);
        return;
    }

    if (event == ORBIS_NP_MATCHING2_REQUEST_EVENT_CREATE_JOIN_ROOM_A) {
        shadnet::CreateRoomReply rep;
        if (!rep.ParseFromArray(proto, sz)) {
            LOG_WARNING(Lib_NpMatching2, "HandleRoomReply: failed to parse CreateRoomReply");
            return;
        }
        LOG_INFO(Lib_NpMatching2,
                 "CreateJoinRoom reply: roomId={} memberId={} maxSlots={} curMembers={}",
                 rep.room_id(), rep.member_id(), rep.max_slots(), rep.cur_members());
        DispatchCreateJoinResponse(pending, event, rep.details());
    } else if (event == ORBIS_NP_MATCHING2_REQUEST_EVENT_JOIN_ROOM_A) {
        shadnet::JoinRoomReply rep;
        if (!rep.ParseFromArray(proto, sz)) {
            LOG_WARNING(Lib_NpMatching2, "HandleRoomReply: failed to parse JoinRoomReply");
            return;
        }
        LOG_INFO(Lib_NpMatching2, "JoinRoom reply: roomId={} memberId={} maxSlots={} curMembers={}",
                 rep.room_id(), rep.member_id(), rep.max_slots(), rep.cur_members());
        DispatchCreateJoinResponse(pending, event, rep.details());
    } else { // SEARCH_ROOM_A (GetRoomList)
        shadnet::GetRoomListReply rep;
        if (!rep.ParseFromArray(proto, sz)) {
            LOG_WARNING(Lib_NpMatching2, "HandleRoomReply: failed to parse GetRoomListReply");
            return;
        }
        LOG_INFO(Lib_NpMatching2, "SearchRoom reply: {} room(s)", rep.rooms_size());
        std::scoped_lock lk{g_responses_mutex};
        g_responses.emplace_back([pending, rep]() {
            std::vector<OrbisNpMatching2RoomDataExternalA> rooms;
            rooms.reserve(rep.rooms_size());
            for (const auto& r : rep.rooms()) {
                OrbisNpMatching2RoomDataExternalA room{};
                room.maxSlot = static_cast<u16>(r.max_slot());
                room.curMemberNum = static_cast<u16>(r.cur_members());
                room.flagAttr = r.flags();
                room.serverId = static_cast<OrbisNpMatching2ServerId>(r.server_id());
                room.worldId = r.world_id();
                room.lobbyId = r.lobby_id();
                room.roomId = r.room_id();
                room.passwordSlotMask = r.passwd_slot_mask();
                room.joinedSlotMask = r.joined_slot_mask();
                room.publicSlotNum = static_cast<u16>(r.public_slots());
                room.privateSlotNum = static_cast<u16>(r.private_slots());
                room.openPublicSlotNum = static_cast<u16>(r.open_public_slots());
                room.openPrivateSlotNum = static_cast<u16>(r.open_private_slots());
                room.owner.platform = Libraries::Np::OrbisNpPlatformType::PS4;
                room.ownerOnlineId = MakeOnlineId(r.owner_npid());
                LOG_INFO(Lib_NpMatching2,
                         "SearchRoom result: roomId={} serverId={} worldId={} lobbyId={} maxSlot={} "
                         "curMembers={} flags={:#x} owner='{}'",
                         room.roomId, room.serverId, room.worldId, room.lobbyId, room.maxSlot,
                         room.curMemberNum, room.flagAttr, room.ownerOnlineId.data);
                rooms.push_back(room);
            }
            for (size_t i = 0; i + 1 < rooms.size(); ++i) {
                rooms[i].next = &rooms[i + 1];
            }

            const u32 count = static_cast<u32>(rooms.size());
            OrbisNpMatching2SearchRoomResponseA resp{};
            resp.range = {0, count, count, {}};
            resp.roomDataExternal = rooms.empty() ? nullptr : &rooms[0];
            LOG_INFO(Lib_NpMatching2, "SearchRoom: firing callback cb={} arg={} count={}",
                     fmt::ptr(reinterpret_cast<void*>(pending.callback)), pending.arg, count);
            pending.callback(pending.ctxId, pending.requestId,
                             ORBIS_NP_MATCHING2_REQUEST_EVENT_SEARCH_ROOM_A, 0, &resp, pending.arg);
            LOG_INFO(Lib_NpMatching2, "SearchRoom: callback returned");
        });
    }
}

void ProcessEvents() {
    {
        std::scoped_lock lk{g_events_mutex};

        if (npMatching2ContextCallback) {
            while (!g_ctx_events.empty()) {
                npMatching2ContextCallback(&g_ctx_events.front());
                g_ctx_events.pop_front();
            }
        }
        if (npMatching2LobbyCallback) {
            while (!g_lobby_events.empty()) {
                npMatching2LobbyCallback(&g_lobby_events.front());
                g_lobby_events.pop_front();
            }
        }
        if (npMatching2RoomCallback) {
            while (!g_room_events.empty()) {
                npMatching2RoomCallback(&g_room_events.front());
                g_room_events.pop_front();
            }
        }
    }

    std::scoped_lock lk{g_responses_mutex};
    while (!g_responses.empty()) {
        (g_responses.front())();
        g_responses.pop_front();
    }
}

struct OrbisNpMatching2InitializeParameter {
    u64 poolSize;
    //
};

int PS4_SYSV_ABI sceNpMatching2Initialize(OrbisNpMatching2InitializeParameter* param) {
    LOG_DEBUG(Lib_NpMatching2, "called");

    if (g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_ALREADY_INITIALIZED;
    }

    g_initialized = true;
    Libraries::Np::NpManager::RegisterNpCallback("NpMatching2", ProcessEvents);

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2Terminate() {
    LOG_DEBUG(Lib_NpMatching2, "called");

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }

    g_initialized = false;
    Libraries::Np::NpManager::DeregisterNpCallback("NpMatching2");

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SetDefaultRequestOptParam(
    OrbisNpMatching2ContextId ctxId, OrbisNpMatching2RequestOptParam* requestOpt) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}", ctxId);

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!requestOpt) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    defaultRequestOptParam = *requestOpt;

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2GetServerId(OrbisNpMatching2ContextId ctxId,
                                           OrbisNpMatching2ServerId* serverId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}", ctxId);

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!serverId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    *serverId = 0xac;

    return ORBIS_OK;
}

struct OrbisNpMatching2GetWorldInfoListRequest {
    OrbisNpMatching2ServerId serverId;
};

struct OrbisNpMatching2World {
    OrbisNpMatching2World* next;
    OrbisNpMatching2WorldId worldId;
    u32 lobbiesNum;
    u32 maxLobbyMembersNum;
    u32 lobbyMembersNum;
    u32 roomsNum;
    u32 roomMembersNum;
    u8 pad[3];
};

struct OrbisNpMatching2GetWorldInfoListResponse {
    OrbisNpMatching2World* world;
    u64 worldNum;
};

int PS4_SYSV_ABI sceNpMatching2GetWorldInfoList(OrbisNpMatching2ContextId ctxId,
                                                OrbisNpMatching2GetWorldInfoListRequest* request,
                                                OrbisNpMatching2RequestOptParam* requestOpt,
                                                OrbisNpMatching2RequestId* requestId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, request.serverId = {}, requestOpt = {}", ctxId,
              request ? request->serverId : 0xFFFF, fmt::ptr(requestOpt));

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    static OrbisNpMatching2RequestId id = 1;
    *requestId = id++;

    if (auto optParam = GetOptParam(requestOpt); optParam) {
        LOG_DEBUG(Lib_NpMatching2, "optParam.timeout = {}, optParam.appId = {}", optParam->timeout,
                  optParam->appId);
        auto reqIdCopy = *requestId;
        std::scoped_lock lk{g_responses_mutex};
        g_responses.emplace_back([=]() {
            OrbisNpMatching2World w{nullptr, 1, 10, 0, 10, 0, {}};
            OrbisNpMatching2GetWorldInfoListResponse resp{&w, 1};
            optParam->callback(ctxId, reqIdCopy,
                               ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_WORLD_INFO_LIST, 0, &resp,
                               optParam->arg);
        });
    }

    return ORBIS_OK;
}

struct OrbisNpMatching2LeaveRoomRequest {
    OrbisNpMatching2RoomId roomId;
    OrbisNpMatching2PresenceOptionData optData;
};

int PS4_SYSV_ABI sceNpMatching2LeaveRoom(OrbisNpMatching2ContextId ctxId,
                                         OrbisNpMatching2LeaveRoomRequest* request,
                                         OrbisNpMatching2RequestOptParam* requestOpt,
                                         OrbisNpMatching2RequestId* requestId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    static OrbisNpMatching2RequestId id = 500;
    *requestId = id++;

    if (auto optParam = GetOptParam(requestOpt); optParam) {
        LOG_DEBUG(Lib_NpMatching2, "optParam.timeout = {}, optParam.appId = {}", optParam->timeout,
                  optParam->appId);
        std::scoped_lock lk{g_responses_mutex};
        auto reqIdCopy = *requestId;
        g_responses.emplace_back([=]() {
            optParam->callback(ctxId, reqIdCopy, ORBIS_NP_MATCHING2_REQUEST_EVENT_LEAVE_ROOM, 0,
                               nullptr, optParam->arg);
        });
    }

    return ORBIS_OK;
}

struct OrbisNpMatching2RangeFilter {
    u32 start;
    u32 max;
};

struct OrbisNpMatching2SearchRoomRequest {
    int option;
    OrbisNpMatching2WorldId worldId;
    OrbisNpMatching2LobbyId lobbyId;
    OrbisNpMatching2RangeFilter rangeFilter;
    OrbisNpMatching2Flags flags1;
    OrbisNpMatching2Flags flags2;
    void* intFilter;
    u64 intFilters;
    void* binFilter;
    u64 binFilters;
    OrbisNpMatching2AttributeId* attr;
    u64 attrs;
};

int PS4_SYSV_ABI sceNpMatching2SearchRoom(OrbisNpMatching2ContextId ctxId,
                                          OrbisNpMatching2SearchRoomRequest* request,
                                          OrbisNpMatching2RequestOptParam* requestOpt,
                                          OrbisNpMatching2RequestId* requestId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    static OrbisNpMatching2RequestId id = 1;
    *requestId = id++;

    LOG_INFO(Lib_NpMatching2,
             "SearchRoom req: option={:#x} worldId={} lobbyId={} range.start={} range.max={} "
             "flagFilter={:#x} flagAttr={:#x} intFilters={} binFilters={} attrIds={}",
             request->option, request->worldId, request->lobbyId, request->rangeFilter.start,
             request->rangeFilter.max, request->flags1, request->flags2, request->intFilters,
             request->binFilters, request->attrs);

    auto optParam = GetOptParam(requestOpt);

    // Ask the shadNet server for the live room list (CmdGetRoomList ignores any body).
    // HandleRoomReply(cmd=GetRoomList) turns the reply into a SearchRoom response.
    const u64 pkt_id = SubmitRoomRequest(ctxId, ShadNet::CommandType::GetRoomList, std::string{});
    LOG_INFO(Lib_NpMatching2, "SearchRoom: sent GetRoomList to shadNet, ctxId={} reqId={} pkt_id={}",
             ctxId, *requestId, pkt_id);

    if (optParam) {
        if (pkt_id != 0) {
            StashRoomRequest(pkt_id, {ctxId, *requestId, optParam->callback, optParam->arg});
        } else {
            // Offline / no connected client: return an empty result so the toolkit doesn't hang.
            std::scoped_lock lk{g_responses_mutex};
            auto reqIdCopy = *requestId;
            g_responses.emplace_back([=]() {
                OrbisNpMatching2SearchRoomResponseA resp{{0, 0, 0, {}}, nullptr};
                optParam->callback(ctxId, reqIdCopy,
                                   ORBIS_NP_MATCHING2_REQUEST_EVENT_SEARCH_ROOM_A, 0, &resp,
                                   optParam->arg);
            });
        }
    }

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2JoinRoomA(OrbisNpMatching2ContextId ctxId,
                                         OrbisNpMatching2JoinRoomRequestA* request,
                                         OrbisNpMatching2RequestOptParam* requestOpt,
                                         OrbisNpMatching2RequestId* requestId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    static OrbisNpMatching2RequestId id = 300;
    *requestId = id++;

    auto optParam = GetOptParam(requestOpt);

    shadnet::JoinRoomRequest pb;
    pb.set_room_id(request->roomId);
    pb.set_req_id(*requestId);
    pb.set_team_id(request->teamId);
    pb.set_join_flags(request->flagAttr);
    pb.set_blocked_user_count(static_cast<u32>(request->blockedUserNum));
    pb.set_room_password_present(request->roomPassword != nullptr);
    pb.set_join_group_label_present(request->joinRoomGroupLabel != nullptr);

    const u64 pkt_id =
        SubmitRoomRequest(ctxId, ShadNet::CommandType::JoinRoom, pb.SerializeAsString());
    LOG_INFO(Lib_NpMatching2, "JoinRoom: sent to shadNet, ctxId={} roomId={} reqId={} pkt_id={}",
             ctxId, request->roomId, *requestId, pkt_id);

    if (optParam) {
        StashRoomRequest(pkt_id, {ctxId, *requestId, optParam->callback, optParam->arg});
    }

    return ORBIS_OK;
}

struct OrbisNpMatching2SetUserInfoRequest {
    OrbisNpMatching2ServerId serverId;
    u8 padding[6];
    void* userBinAttr;
    u64 userBinAttrs;
};

int PS4_SYSV_ABI sceNpMatching2SetUserInfo(OrbisNpMatching2ContextId ctxId,
                                           OrbisNpMatching2SetUserInfoRequest* request,
                                           OrbisNpMatching2RequestOptParam* requestOpt,
                                           OrbisNpMatching2RequestId* requestId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    static OrbisNpMatching2RequestId id = 100;
    *requestId = id++;

    if (auto optParam = GetOptParam(requestOpt); optParam) {
        LOG_DEBUG(Lib_NpMatching2, "optParam.timeout = {}, optParam.appId = {}", optParam->timeout,
                  optParam->appId);
        std::scoped_lock lk{g_responses_mutex};
        auto reqIdCopy = *requestId;
        g_responses.emplace_back([=]() {
            optParam->callback(ctxId, reqIdCopy, ORBIS_NP_MATCHING2_REQUEST_EVENT_SET_USER_INFO, 0,
                               nullptr, optParam->arg);
        });
    }

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SendRoomMessage(OrbisNpMatching2ContextId ctxId, void* request,
                                               OrbisNpMatching2RequestOptParam* requestOpt,
                                               OrbisNpMatching2RequestId* requestId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    static OrbisNpMatching2RequestId id = 1000;
    *requestId = id++;

    if (auto optParam = GetOptParam(requestOpt); optParam) {
        LOG_DEBUG(Lib_NpMatching2, "optParam.timeout = {}, optParam.appId = {}", optParam->timeout,
                  optParam->appId);
        std::scoped_lock lk{g_responses_mutex};
        auto reqIdCopy = *requestId;
        g_responses.emplace_back([=]() {
            optParam->callback(ctxId, reqIdCopy, ORBIS_NP_MATCHING2_REQUEST_EVENT_SEND_ROOM_MESSAGE,
                               0, nullptr, optParam->arg);
        });
    }

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SetRoomDataExternal(OrbisNpMatching2ContextId ctxId, void* request,
                                                   OrbisNpMatching2RequestOptParam* requestOpt,
                                                   OrbisNpMatching2RequestId* requestId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    static OrbisNpMatching2RequestId id = 800;
    *requestId = id++;

    if (auto optParam = GetOptParam(requestOpt); optParam) {
        LOG_DEBUG(Lib_NpMatching2, "optParam.timeout = {}, optParam.appId = {}", optParam->timeout,
                  optParam->appId);
        std::scoped_lock lk{g_responses_mutex};
        auto reqIdCopy = *requestId;
        g_responses.emplace_back([=]() {
            optParam->callback(ctxId, reqIdCopy,
                               ORBIS_NP_MATCHING2_REQUEST_EVENT_SET_ROOM_DATA_EXTERNAL, 0, nullptr,
                               optParam->arg);
        });
    }

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SetRoomDataInternal(OrbisNpMatching2ContextId ctxId, void* request,
                                                   OrbisNpMatching2RequestOptParam* requestOpt,
                                                   OrbisNpMatching2RequestId* requestId) {
    LOG_DEBUG(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!g_initialized) {
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    static OrbisNpMatching2RequestId id = 200;
    *requestId = id++;

    if (auto optParam = GetOptParam(requestOpt); optParam) {
        LOG_DEBUG(Lib_NpMatching2, "optParam.timeout = {}, optParam.appId = {}", optParam->timeout,
                  optParam->appId);
        std::scoped_lock lk{g_responses_mutex};
        auto reqIdCopy = *requestId;
        g_responses.emplace_back([=]() {
            optParam->callback(ctxId, reqIdCopy,
                               ORBIS_NP_MATCHING2_REQUEST_EVENT_SET_ROOM_DATA_INTERNAL, 0, nullptr,
                               optParam->arg);
        });
    }

    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("10t3e5+JPnU", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2Initialize);
    LIB_FUNCTION("Mqp3lJ+sjy4", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2Terminate);
    LIB_FUNCTION("YfmpW719rMo", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2CreateContext);
    LIB_FUNCTION("ajvzc8e2upo", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2CreateContextA);
    LIB_FUNCTION("V6KSpKv9XJE", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2CreateJoinRoomA);
    LIB_FUNCTION("gQ6cUriNpgs", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2JoinRoomA);
    LIB_FUNCTION("fQQfP87I7hs", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2RegisterContextCallback);
    LIB_FUNCTION("4Nj7u5B5yCA", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2RegisterLobbyEventCallback);
    LIB_FUNCTION("p+2EnxmaAMM", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2RegisterRoomEventCallback);
    LIB_FUNCTION("0UMeWRGnZKA", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2RegisterSignalingCallback);
    LIB_FUNCTION("7vjNQ6Z1op0", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2ContextStart);
    LIB_FUNCTION("LhCPctIICxQ", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2GetServerId);
    LIB_FUNCTION("rJNPJqDCpiI", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2GetWorldInfoList);
    LIB_FUNCTION("BD6kfx442Do", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2LeaveRoom);
    LIB_FUNCTION("+8e7wXLmjds", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SetDefaultRequestOptParam);
    LIB_FUNCTION("VqZX7POg2Mk", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SearchRoom);
    LIB_FUNCTION("Iw2h0Jrrb5U", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SendRoomMessage);
    LIB_FUNCTION("meEjIdbjAA0", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SetUserInfo);
    LIB_FUNCTION("q7GK98-nYSE", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SetRoomDataExternal);
    LIB_FUNCTION("S9D8JSYIrjE", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SetRoomDataInternal);
};

} // namespace Libraries::Np::NpMatching2