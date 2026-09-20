// xrsim - action sets, actions, suggested bindings (oculus/touch_controller, khr/simple_controller), action state,
// action spaces, haptics. The simulated device is a pair of Touch controllers driven by the script.
#include "rt_api.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace xs {

namespace {

constexpr int V_SELECT = 100;       // simple_controller select/click: trigger > 0.5
constexpr int V_MENU_SIMPLE = 101;  // simple_controller menu/click: left menu / right system
constexpr int P_GRIP = 0, P_AIM = 1, P_GRIP_SURFACE = 2;

struct CompDef { const char* sub; SrcKind kind; int comp; const char* label; };

const CompDef kTouchLeft[] = {
    {"input/x/click", SK_BOOL, C_X, "X Button"}, {"input/x/touch", SK_BOOL, C_X_TOUCH, "X Button Touch"},
    {"input/y/click", SK_BOOL, C_Y, "Y Button"}, {"input/y/touch", SK_BOOL, C_Y_TOUCH, "Y Button Touch"},
    {"input/menu/click", SK_BOOL, C_MENU, "Menu Button"},
    {"input/squeeze/value", SK_FLOAT, C_SQUEEZE, "Grip"},
    {"input/trigger/value", SK_FLOAT, C_TRIGGER, "Trigger"}, {"input/trigger/touch", SK_BOOL, C_TRIGGER_TOUCH, "Trigger Touch"},
    {"input/thumbstick", SK_VEC2, C_STICK_X, "Thumbstick"}, {"input/thumbstick/x", SK_FLOAT, C_STICK_X, "Thumbstick X"},
    {"input/thumbstick/y", SK_FLOAT, C_STICK_Y, "Thumbstick Y"}, {"input/thumbstick/click", SK_BOOL, C_STICK_CLICK, "Thumbstick Press"},
    {"input/thumbstick/touch", SK_BOOL, C_STICK_TOUCH, "Thumbstick Touch"},
    {"input/thumbrest/touch", SK_BOOL, C_THUMBREST_TOUCH, "Thumbrest Touch"},
    {"input/grip/pose", SK_POSE, P_GRIP, "Grip Pose"}, {"input/aim/pose", SK_POSE, P_AIM, "Aim Pose"},
    {"input/grip_surface/pose", SK_POSE, P_GRIP_SURFACE, "Grip Surface Pose"},
    {"output/haptic", SK_HAPTIC, 0, "Haptic"},
};
const CompDef kTouchRight[] = {
    {"input/a/click", SK_BOOL, C_A, "A Button"}, {"input/a/touch", SK_BOOL, C_A_TOUCH, "A Button Touch"},
    {"input/b/click", SK_BOOL, C_B, "B Button"}, {"input/b/touch", SK_BOOL, C_B_TOUCH, "B Button Touch"},
    {"input/system/click", SK_BOOL, C_SYSTEM, "System Button"},
    {"input/squeeze/value", SK_FLOAT, C_SQUEEZE, "Grip"},
    {"input/trigger/value", SK_FLOAT, C_TRIGGER, "Trigger"}, {"input/trigger/touch", SK_BOOL, C_TRIGGER_TOUCH, "Trigger Touch"},
    {"input/thumbstick", SK_VEC2, C_STICK_X, "Thumbstick"}, {"input/thumbstick/x", SK_FLOAT, C_STICK_X, "Thumbstick X"},
    {"input/thumbstick/y", SK_FLOAT, C_STICK_Y, "Thumbstick Y"}, {"input/thumbstick/click", SK_BOOL, C_STICK_CLICK, "Thumbstick Press"},
    {"input/thumbstick/touch", SK_BOOL, C_STICK_TOUCH, "Thumbstick Touch"},
    {"input/thumbrest/touch", SK_BOOL, C_THUMBREST_TOUCH, "Thumbrest Touch"},
    {"input/grip/pose", SK_POSE, P_GRIP, "Grip Pose"}, {"input/aim/pose", SK_POSE, P_AIM, "Aim Pose"},
    {"input/grip_surface/pose", SK_POSE, P_GRIP_SURFACE, "Grip Surface Pose"},
    {"output/haptic", SK_HAPTIC, 0, "Haptic"},
};
const CompDef kSimple[] = {
    {"input/select/click", SK_BOOL, V_SELECT, "Select"}, {"input/menu/click", SK_BOOL, V_MENU_SIMPLE, "Menu"},
    {"input/grip/pose", SK_POSE, P_GRIP, "Grip Pose"}, {"input/aim/pose", SK_POSE, P_AIM, "Aim Pose"},
    {"input/grip_surface/pose", SK_POSE, P_GRIP_SURFACE, "Grip Surface Pose"},
    {"output/haptic", SK_HAPTIC, 0, "Haptic"},
};

struct ProfileDef {
    const char* path;
    const char* label;
    const CompDef* comps[2];
    size_t count[2];
};
const ProfileDef kProfiles[] = {
    {"/interaction_profiles/oculus/touch_controller", "Oculus Touch Controller", {kTouchLeft, kTouchRight},
     {sizeof(kTouchLeft) / sizeof(CompDef), sizeof(kTouchRight) / sizeof(CompDef)}},
    {"/interaction_profiles/khr/simple_controller", "Khronos Simple Controller", {kSimple, kSimple},
     {sizeof(kSimple) / sizeof(CompDef), sizeof(kSimple) / sizeof(CompDef)}},
};

const char* const kTopLevelPaths[] = {"/user/hand/left", "/user/hand/right", "/user/head", "/user/gamepad", "/user/treadmill"};

const ProfileDef* FindProfile(const std::string& p) {
    for (const ProfileDef& d : kProfiles)
        if (p == d.path) return &d;
    return nullptr;
}

bool Compatible(XrActionType t, SrcKind k) {
    switch (t) {
    case XR_ACTION_TYPE_BOOLEAN_INPUT:
    case XR_ACTION_TYPE_FLOAT_INPUT: return k == SK_BOOL || k == SK_FLOAT;
    case XR_ACTION_TYPE_VECTOR2F_INPUT: return k == SK_VEC2;
    case XR_ACTION_TYPE_POSE_INPUT: return k == SK_POSE;
    case XR_ACTION_TYPE_VIBRATION_OUTPUT: return k == SK_HAPTIC;
    default: return false;
    }
}

// Resolves a suggested binding path (identifier may be omitted: ".../trigger" -> ".../trigger/value").
bool Resolve(const ProfileDef& prof, const std::string& path, XrActionType type, std::string& resolved, int& hand, const CompDef*& def) {
    std::string sub;
    if (path.rfind("/user/hand/left/", 0) == 0) { hand = 0; sub = path.substr(16); }
    else if (path.rfind("/user/hand/right/", 0) == 0) { hand = 1; sub = path.substr(17); }
    else return false;
    const std::vector<std::string> tries = type == XR_ACTION_TYPE_FLOAT_INPUT ? std::vector<std::string>{sub, sub + "/value", sub + "/click"}
                                         : type == XR_ACTION_TYPE_POSE_INPUT  ? std::vector<std::string>{sub, sub + "/pose"}
                                                                              : std::vector<std::string>{sub, sub + "/click", sub + "/value"};
    for (const std::string& t : tries) {
        for (size_t i = 0; i < prof.count[hand]; ++i) {
            const CompDef& c = prof.comps[hand][i];
            if (t == c.sub && Compatible(type, c.kind)) {
                resolved = std::string(hand == 0 ? "/user/hand/left/" : "/user/hand/right/") + c.sub;
                def = &c;
                return true;
            }
        }
    }
    return false;
}

bool ValidName(const char* s, size_t max) {
    const size_t n = strnlen(s, max);
    if (n == 0 || n >= max) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
    }
    return true;
}

bool IsTopLevel(const std::string& p) {
    for (const char* t : kTopLevelPaths)
        if (p == t) return true;
    return false;
}

int HandOfPath(Instance* inst, XrPath p) {
    const std::string s = inst->PathString(p);
    return s == "/user/hand/left" ? 0 : s == "/user/hand/right" ? 1 : -1;
}

float ReadComp(const Snapshot& sn, int hand, int comp) {
    if (comp == V_SELECT) return sn.comp[hand][C_TRIGGER] > 0.5f ? 1.0f : 0.0f;
    if (comp == V_MENU_SIMPLE) return sn.comp[hand][hand == 0 ? C_MENU : C_SYSTEM];
    return sn.comp[hand][comp];
}

bool Focused(Session* s) { return s->state == XR_SESSION_STATE_FOCUSED; }

struct GetCtx { Session* s; Action* a; int slot; };
XrResult ValidateGet(XrSession session, const XrActionStateGetInfo* gi, XrActionType type, GetCtx& c, const char* fn) {
    c.s = GetSession(session);
    if (!c.s) return FailImpl(XR_ERROR_HANDLE_INVALID, fn, "session");
    if (!gi || gi->type != XR_TYPE_ACTION_STATE_GET_INFO) return FailImpl(XR_ERROR_VALIDATION_FAILURE, fn, "getInfo");
    c.a = GetAction(gi->action);
    if (!c.a || c.a->set->inst != c.s->inst) return FailImpl(XR_ERROR_HANDLE_INVALID, fn, "action");
    if (c.a->type != type) return FailImpl(XR_ERROR_ACTION_TYPE_MISMATCH, fn, "action '%s'", c.a->name.c_str());
    if (!c.s->attached || !c.a->set->attached) return FailImpl(XR_ERROR_ACTIONSET_NOT_ATTACHED, fn, "action set '%s'", c.a->set->name.c_str());
    if (gi->subactionPath != XR_NULL_PATH && gi->subactionPath > c.s->inst->pathStr.size())
        return FailImpl(XR_ERROR_PATH_INVALID, fn, "subactionPath");
    c.slot = gi->subactionPath == XR_NULL_PATH ? 0 : c.a->SlotOf(gi->subactionPath);
    if (c.slot < 0) return FailImpl(XR_ERROR_PATH_UNSUPPORTED, fn, "subactionPath not declared for '%s'", c.a->name.c_str());
    return XR_SUCCESS;
}

} // namespace

int Action::SlotOf(XrPath p) const {
    for (size_t i = 0; i < subPaths.size(); ++i)
        if (subPaths[i] == p) return (int)i + 1;
    return -1;
}

void DestroyActionSetObject(ActionSet* as) {
    Instance* inst = as->inst;
    for (Action* a : as->actions) {
        for (auto& [prof, list] : inst->suggested)
            list.erase(std::remove_if(list.begin(), list.end(), [&](const Binding& b) { return b.action == a; }), list.end());
        if (inst->session)
            for (Space* sp : inst->session->spaces)
                if (sp->action == a) sp->action = nullptr;
        Unregister(a->handle);
    }
    as->actions.clear();
    auto& v = inst->actionSets;
    v.erase(std::remove(v.begin(), v.end(), as), v.end());
    Unregister(as->handle);
}

XrResult XRAPI_CALL xrCreateActionSet(XrInstance instance, const XrActionSetCreateInfo* ci, XrActionSet* actionSet) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Instance* inst = GetInstance(instance);
    if (!inst) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "instance");
    if (!ci || !actionSet || ci->type != XR_TYPE_ACTION_SET_CREATE_INFO) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "createInfo");
    if (!ValidName(ci->actionSetName, XR_MAX_ACTION_SET_NAME_SIZE)) return XS_FAIL(XR_ERROR_NAME_INVALID, "'%.64s'", ci->actionSetName);
    const size_t ln = strnlen(ci->localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
    if (ln == 0 || ln >= XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE) return XS_FAIL(XR_ERROR_LOCALIZED_NAME_INVALID, "localized name");
    for (ActionSet* o : inst->actionSets) {
        if (o->name == ci->actionSetName) return XS_FAIL(XR_ERROR_NAME_DUPLICATED, "'%s'", ci->actionSetName);
        if (o->loc == ci->localizedActionSetName) return XS_FAIL(XR_ERROR_LOCALIZED_NAME_DUPLICATED, "'%s'", ci->localizedActionSetName);
    }
    auto as = std::make_unique<ActionSet>();
    as->inst = inst;
    as->name = ci->actionSetName;
    as->loc = ci->localizedActionSetName;
    as->priority = ci->priority;
    inst->actionSets.push_back(as.get());
    *actionSet = (XrActionSet)Register(std::move(as));
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrDestroyActionSet(XrActionSet actionSet) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    ActionSet* as = GetActionSet(actionSet);
    if (!as) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "actionSet");
    DestroyActionSetObject(as);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrCreateAction(XrActionSet actionSet, const XrActionCreateInfo* ci, XrAction* action) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    ActionSet* as = GetActionSet(actionSet);
    if (!as) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "actionSet");
    if (!ci || !action || ci->type != XR_TYPE_ACTION_CREATE_INFO) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "createInfo");
    if (as->attached) return XS_FAIL(XR_ERROR_ACTIONSETS_ALREADY_ATTACHED, "set '%s' is attached", as->name.c_str());
    switch (ci->actionType) {
    case XR_ACTION_TYPE_BOOLEAN_INPUT: case XR_ACTION_TYPE_FLOAT_INPUT: case XR_ACTION_TYPE_VECTOR2F_INPUT:
    case XR_ACTION_TYPE_POSE_INPUT: case XR_ACTION_TYPE_VIBRATION_OUTPUT: break;
    default: return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "actionType %d", (int)ci->actionType);
    }
    if (!ValidName(ci->actionName, XR_MAX_ACTION_NAME_SIZE)) return XS_FAIL(XR_ERROR_NAME_INVALID, "'%.64s'", ci->actionName);
    const size_t ln = strnlen(ci->localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    if (ln == 0 || ln >= XR_MAX_LOCALIZED_ACTION_NAME_SIZE) return XS_FAIL(XR_ERROR_LOCALIZED_NAME_INVALID, "localized name");
    for (Action* o : as->actions) {
        if (o->name == ci->actionName) return XS_FAIL(XR_ERROR_NAME_DUPLICATED, "'%s'", ci->actionName);
        if (o->loc == ci->localizedActionName) return XS_FAIL(XR_ERROR_LOCALIZED_NAME_DUPLICATED, "'%s'", ci->localizedActionName);
    }
    if (ci->countSubactionPaths && !ci->subactionPaths) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "subactionPaths == NULL");
    std::vector<XrPath> subs;
    for (uint32_t i = 0; i < ci->countSubactionPaths; ++i) {
        const XrPath p = ci->subactionPaths[i];
        if (p == XR_NULL_PATH || p > as->inst->pathStr.size()) return XS_FAIL(XR_ERROR_PATH_INVALID, "subactionPaths[%u]", i);
        if (!IsTopLevel(as->inst->PathString(p)) || std::find(subs.begin(), subs.end(), p) != subs.end())
            return XS_FAIL(XR_ERROR_PATH_UNSUPPORTED, "subaction path '%s'", as->inst->PathString(p).c_str());
        subs.push_back(p);
    }
    auto a = std::make_unique<Action>();
    a->set = as;
    a->name = ci->actionName;
    a->loc = ci->localizedActionName;
    a->type = ci->actionType;
    a->subPaths = subs;
    a->vals.resize(subs.size() + 1);
    as->actions.push_back(a.get());
    *action = (XrAction)Register(std::move(a));
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrDestroyAction(XrAction action) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Action* a = GetAction(action);
    if (!a) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "action");
    Instance* inst = a->set->inst;
    for (auto& [prof, list] : inst->suggested)
        list.erase(std::remove_if(list.begin(), list.end(), [&](const Binding& b) { return b.action == a; }), list.end());
    if (inst->session)
        for (Space* sp : inst->session->spaces)
            if (sp->action == a) sp->action = nullptr;
    auto& v = a->set->actions;
    v.erase(std::remove(v.begin(), v.end(), a), v.end());
    Unregister(a->handle);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(XrInstance instance, const XrInteractionProfileSuggestedBinding* sb) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Instance* inst = GetInstance(instance);
    if (!inst) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "instance");
    if (!sb || sb->type != XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING || sb->countSuggestedBindings == 0 || !sb->suggestedBindings)
        return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "suggestedBindings");
    if (sb->interactionProfile == XR_NULL_PATH || sb->interactionProfile > inst->pathStr.size())
        return XS_FAIL(XR_ERROR_PATH_INVALID, "interactionProfile");
    const std::string profName = inst->PathString(sb->interactionProfile);
    const ProfileDef* prof = FindProfile(profName);
    if (!prof) return XS_FAIL(XR_ERROR_PATH_UNSUPPORTED, "interaction profile '%s' (xrsim: oculus/touch_controller, khr/simple_controller)", profName.c_str());
    std::vector<Binding> list;
    for (uint32_t i = 0; i < sb->countSuggestedBindings; ++i) {
        const XrActionSuggestedBinding& b = sb->suggestedBindings[i];
        Action* a = GetAction(b.action);
        if (!a || a->set->inst != inst) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "suggestedBindings[%u].action", i);
        if (a->set->attached) return XS_FAIL(XR_ERROR_ACTIONSETS_ALREADY_ATTACHED, "set '%s'", a->set->name.c_str());
        if (b.binding == XR_NULL_PATH || b.binding > inst->pathStr.size()) return XS_FAIL(XR_ERROR_PATH_INVALID, "suggestedBindings[%u].binding", i);
        std::string resolved;
        int hand = -1;
        const CompDef* def = nullptr;
        if (!Resolve(*prof, inst->PathString(b.binding), a->type, resolved, hand, def))
            return XS_FAIL(XR_ERROR_PATH_UNSUPPORTED, "'%s' for action '%s' in %s", inst->PathString(b.binding).c_str(), a->name.c_str(), profName.c_str());
        list.push_back({a, inst->GetPath(resolved), hand, def->kind, def->comp});
    }
    inst->suggested[sb->interactionProfile] = std::move(list);
    Log("suggested %u binding(s) for %s", sb->countSuggestedBindings, profName.c_str());
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrAttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* ai) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    if (!ai || ai->type != XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO || ai->countActionSets == 0 || !ai->actionSets)
        return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "attachInfo");
    if (s->attached) return XS_FAIL(XR_ERROR_ACTIONSETS_ALREADY_ATTACHED, "already attached");
    std::vector<ActionSet*> sets;
    for (uint32_t i = 0; i < ai->countActionSets; ++i) {
        ActionSet* as = GetActionSet(ai->actionSets[i]);
        if (!as || as->inst != s->inst) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "actionSets[%u]", i);
        sets.push_back(as);
    }
    for (ActionSet* as : sets) {
        as->attached = true;
        for (Action* a : as->actions) a->vals.assign(a->subPaths.size() + 1, ActionValue{});
    }
    s->attached = true;
    s->attachedSets = sets;
    InputOnAttach(s);
    return XR_SUCCESS;
}

void InputOnAttach(Session* s) {
    Instance* inst = s->inst;
    s->profile = XR_NULL_PATH;
    for (const ProfileDef& d : kProfiles) {
        auto it = inst->pathIds.find(d.path);
        if (it == inst->pathIds.end()) continue;
        auto sg = inst->suggested.find(it->second);
        bool relevant = false;
        if (sg != inst->suggested.end())
            for (const Binding& b : sg->second)
                relevant = relevant || std::find(s->attachedSets.begin(), s->attachedSets.end(), b.action->set) != s->attachedSets.end();
        if (relevant) {
            s->profile = it->second;
            break;
        }
    }
    Log("attached %zu action set(s); current interaction profile: %s", s->attachedSets.size(),
        s->profile ? inst->PathString(s->profile).c_str() : "(none)");
    XrEventDataInteractionProfileChanged ev{XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED};
    ev.session = (XrSession)s->handle;
    inst->PushEvent(&ev, sizeof(ev));
}

XrResult XRAPI_CALL xrGetCurrentInteractionProfile(XrSession session, XrPath topLevelUserPath, XrInteractionProfileState* st) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    if (!st || st->type != XR_TYPE_INTERACTION_PROFILE_STATE) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "interactionProfile");
    if (!s->attached) return XS_FAIL(XR_ERROR_ACTIONSET_NOT_ATTACHED, "no action sets attached");
    if (topLevelUserPath == XR_NULL_PATH || topLevelUserPath > s->inst->pathStr.size()) return XS_FAIL(XR_ERROR_PATH_INVALID, "topLevelUserPath");
    const std::string p = s->inst->PathString(topLevelUserPath);
    if (!IsTopLevel(p)) return XS_FAIL(XR_ERROR_PATH_UNSUPPORTED, "'%s' is not a top level user path", p.c_str());
    st->interactionProfile = HandOfPath(s->inst, topLevelUserPath) >= 0 ? s->profile : XR_NULL_PATH;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrSyncActions(XrSession session, const XrActionsSyncInfo* si) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    if (!si || si->type != XR_TYPE_ACTIONS_SYNC_INFO || (si->countActiveActionSets && !si->activeActionSets))
        return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "syncInfo");
    Instance* inst = s->inst;
    struct Active { ActionSet* set; XrPath sub; };
    std::vector<Active> active;
    for (uint32_t i = 0; i < si->countActiveActionSets; ++i) {
        const XrActiveActionSet& aas = si->activeActionSets[i];
        ActionSet* as = GetActionSet(aas.actionSet);
        if (!as || as->inst != inst) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "activeActionSets[%u]", i);
        if (!s->attached || std::find(s->attachedSets.begin(), s->attachedSets.end(), as) == s->attachedSets.end())
            return XS_FAIL(XR_ERROR_ACTIONSET_NOT_ATTACHED, "set '%s'", as->name.c_str());
        if (aas.subactionPath != XR_NULL_PATH) {
            if (aas.subactionPath > inst->pathStr.size()) return XS_FAIL(XR_ERROR_PATH_INVALID, "activeActionSets[%u].subactionPath", i);
            if (!IsTopLevel(inst->PathString(aas.subactionPath)))
                return XS_FAIL(XR_ERROR_PATH_UNSUPPORTED, "subactionPath '%s'", inst->PathString(aas.subactionPath).c_str());
        }
        active.push_back({as, aas.subactionPath});
    }
    const bool focused = Focused(s);
    const XrTime now = NowXr();
    const Snapshot& sn = s->cur;
    const std::vector<Binding>* binds = nullptr;
    if (s->profile) {
        auto it = inst->suggested.find(s->profile);
        if (it != inst->suggested.end()) binds = &it->second;
    }
    for (ActionSet* as : s->attachedSets) {
        for (Action* a : as->actions) {
            for (size_t slot = 0; slot < a->vals.size(); ++slot) {
                ActionValue& v = a->vals[slot];
                const XrPath slotPath = slot == 0 ? XR_NULL_PATH : a->subPaths[slot - 1];
                ActionValue nv;
                nv.lastChange = v.lastChange;
                bool any = false;
                if (focused && binds) {
                    for (const Binding& b : *binds) {
                        if (b.action != a) continue;
                        const XrPath userPath = inst->GetPath(b.hand == 0 ? "/user/hand/left" : "/user/hand/right");
                        if (slotPath != XR_NULL_PATH && slotPath != userPath) continue;
                        bool setActive = false;
                        for (const Active& ac : active)
                            setActive = setActive || (ac.set == as && (ac.sub == XR_NULL_PATH || ac.sub == userPath));
                        if (!setActive) continue;
                        any = true;
                        if (b.kind == SK_BOOL || b.kind == SK_FLOAT) {
                            const float val = ReadComp(sn, b.hand, b.comp);
                            if (a->type == XR_ACTION_TYPE_BOOLEAN_INPUT) nv.b = nv.b || (b.kind == SK_BOOL ? val != 0 : val > 0.5f);
                            else if (std::fabs(val) > std::fabs(nv.f)) nv.f = val;
                        } else if (b.kind == SK_VEC2) {
                            const float x = sn.comp[b.hand][C_STICK_X], y = sn.comp[b.hand][C_STICK_Y];
                            if (x * x + y * y > nv.x * nv.x + nv.y * nv.y) { nv.x = x; nv.y = y; }
                        }
                    }
                }
                nv.active = any;
                const bool differs = nv.b != v.b || nv.f != v.f || nv.x != v.x || nv.y != v.y;
                nv.changed = nv.active && v.active && differs;
                if (nv.changed || (nv.active && !v.active)) nv.lastChange = now;
                v = nv;
            }
        }
    }
    return focused ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
}

XrResult XRAPI_CALL xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo* gi, XrActionStateBoolean* st) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    GetCtx c;
    XrResult r = ValidateGet(session, gi, XR_ACTION_TYPE_BOOLEAN_INPUT, c, __func__);
    if (XR_FAILED(r)) return r;
    if (!st || st->type != XR_TYPE_ACTION_STATE_BOOLEAN) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "state");
    const ActionValue& v = c.a->vals[(size_t)c.slot];
    st->currentState = v.b ? XR_TRUE : XR_FALSE;
    st->changedSinceLastSync = v.changed ? XR_TRUE : XR_FALSE;
    st->lastChangeTime = v.active ? v.lastChange : 0;
    st->isActive = v.active ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo* gi, XrActionStateFloat* st) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    GetCtx c;
    XrResult r = ValidateGet(session, gi, XR_ACTION_TYPE_FLOAT_INPUT, c, __func__);
    if (XR_FAILED(r)) return r;
    if (!st || st->type != XR_TYPE_ACTION_STATE_FLOAT) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "state");
    const ActionValue& v = c.a->vals[(size_t)c.slot];
    st->currentState = v.f;
    st->changedSinceLastSync = v.changed ? XR_TRUE : XR_FALSE;
    st->lastChangeTime = v.active ? v.lastChange : 0;
    st->isActive = v.active ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrGetActionStateVector2f(XrSession session, const XrActionStateGetInfo* gi, XrActionStateVector2f* st) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    GetCtx c;
    XrResult r = ValidateGet(session, gi, XR_ACTION_TYPE_VECTOR2F_INPUT, c, __func__);
    if (XR_FAILED(r)) return r;
    if (!st || st->type != XR_TYPE_ACTION_STATE_VECTOR2F) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "state");
    const ActionValue& v = c.a->vals[(size_t)c.slot];
    st->currentState = {v.x, v.y};
    st->changedSinceLastSync = v.changed ? XR_TRUE : XR_FALSE;
    st->lastChangeTime = v.active ? v.lastChange : 0;
    st->isActive = v.active ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrGetActionStatePose(XrSession session, const XrActionStateGetInfo* gi, XrActionStatePose* st) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    GetCtx c;
    XrResult r = ValidateGet(session, gi, XR_ACTION_TYPE_POSE_INPUT, c, __func__);
    if (XR_FAILED(r)) return r;
    if (!st || st->type != XR_TYPE_ACTION_STATE_POSE) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "state");
    st->isActive = c.a->vals[(size_t)c.slot].active ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

bool InputPoseForAction(Session* s, Action* a, XrPath subPath, XrTime t, Pose& out, bool& active) {
    active = false;
    if (!a || !s->attached || !a->set->attached || !s->profile) return false;
    const int slot = subPath == XR_NULL_PATH ? 0 : a->SlotOf(subPath);
    if (slot < 0 || (size_t)slot >= a->vals.size()) return false;
    auto it = s->inst->suggested.find(s->profile);
    if (it == s->inst->suggested.end()) return false;
    const int wantHand = subPath == XR_NULL_PATH ? -1 : HandOfPath(s->inst, subPath);
    for (const Binding& b : it->second) {
        if (b.action != a || b.kind != SK_POSE || (wantHand >= 0 && b.hand != wantHand)) continue;
        const Snapshot& sn = s->SnapshotAt(t);
        out = sn.hand[b.hand]; // grip, aim and grip_surface share the simulated controller pose
        active = a->vals[(size_t)slot].active && sn.handTracked[b.hand];
        return true;
    }
    return false;
}

XrResult XRAPI_CALL xrEnumerateBoundSourcesForAction(XrSession session, const XrBoundSourcesForActionEnumerateInfo* info, uint32_t cap,
                                                     uint32_t* count, XrPath* sources) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    if (!info || info->type != XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "enumerateInfo");
    Action* a = GetAction(info->action);
    if (!a || a->set->inst != s->inst) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "action");
    if (!s->attached || !a->set->attached) return XS_FAIL(XR_ERROR_ACTIONSET_NOT_ATTACHED, "set '%s'", a->set->name.c_str());
    std::vector<XrPath> list;
    auto it = s->inst->suggested.find(s->profile);
    if (s->profile && it != s->inst->suggested.end())
        for (const Binding& b : it->second)
            if (b.action == a && std::find(list.begin(), list.end(), b.path) == list.end()) list.push_back(b.path);
    return TwoCall(cap, count, sources, list);
}

XrResult XRAPI_CALL xrGetInputSourceLocalizedName(XrSession session, const XrInputSourceLocalizedNameGetInfo* info, uint32_t cap,
                                                  uint32_t* count, char* buffer) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    if (!info || info->type != XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "getInfo");
    const XrInputSourceLocalizedNameFlags all = XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT |
                                                XR_INPUT_SOURCE_LOCALIZED_NAME_INTERACTION_PROFILE_BIT | XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT;
    if (info->whichComponents == 0 || (info->whichComponents & ~all)) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "whichComponents");
    if (!s->attached) return XS_FAIL(XR_ERROR_ACTIONSET_NOT_ATTACHED, "no action sets attached");
    if (info->sourcePath == XR_NULL_PATH || info->sourcePath > s->inst->pathStr.size()) return XS_FAIL(XR_ERROR_PATH_INVALID, "sourcePath");
    const std::string p = s->inst->PathString(info->sourcePath);
    const ProfileDef* prof = s->profile ? FindProfile(s->inst->PathString(s->profile)) : nullptr;
    int hand = -1;
    const CompDef* def = nullptr;
    std::string resolved;
    if (!prof || !Resolve(*prof, p, XR_ACTION_TYPE_FLOAT_INPUT, resolved, hand, def)) {
        if (!prof || (!Resolve(*prof, p, XR_ACTION_TYPE_POSE_INPUT, resolved, hand, def) &&
                      !Resolve(*prof, p, XR_ACTION_TYPE_VECTOR2F_INPUT, resolved, hand, def) &&
                      !Resolve(*prof, p, XR_ACTION_TYPE_VIBRATION_OUTPUT, resolved, hand, def)))
            return XS_FAIL(XR_ERROR_PATH_UNSUPPORTED, "'%s'", p.c_str());
    }
    std::string name;
    auto add = [&](const std::string& part) { name += (name.empty() ? "" : " ") + part; };
    if (info->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT) add(hand == 0 ? "Left Hand" : "Right Hand");
    if (info->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_INTERACTION_PROFILE_BIT) add(prof->label);
    if (info->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT) add(def->label);
    return TwoCallString(cap, count, buffer, name);
}

namespace {
XrResult HapticCommon(XrSession session, const XrHapticActionInfo* info, Session*& s, const char* fn) {
    s = GetSession(session);
    if (!s) return FailImpl(XR_ERROR_HANDLE_INVALID, fn, "session");
    if (!info || info->type != XR_TYPE_HAPTIC_ACTION_INFO) return FailImpl(XR_ERROR_VALIDATION_FAILURE, fn, "hapticActionInfo");
    Action* a = GetAction(info->action);
    if (!a || a->set->inst != s->inst) return FailImpl(XR_ERROR_HANDLE_INVALID, fn, "action");
    if (a->type != XR_ACTION_TYPE_VIBRATION_OUTPUT) return FailImpl(XR_ERROR_ACTION_TYPE_MISMATCH, fn, "action '%s'", a->name.c_str());
    if (!s->attached || !a->set->attached) return FailImpl(XR_ERROR_ACTIONSET_NOT_ATTACHED, fn, "set '%s'", a->set->name.c_str());
    if (info->subactionPath != XR_NULL_PATH && a->SlotOf(info->subactionPath) < 0)
        return FailImpl(XR_ERROR_PATH_UNSUPPORTED, fn, "subactionPath not declared for '%s'", a->name.c_str());
    return XR_SUCCESS;
}
} // namespace

XrResult XRAPI_CALL xrApplyHapticFeedback(XrSession session, const XrHapticActionInfo* info, const XrHapticBaseHeader* fb) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = nullptr;
    XrResult r = HapticCommon(session, info, s, __func__);
    if (XR_FAILED(r)) return r;
    if (!fb || fb->type != XR_TYPE_HAPTIC_VIBRATION) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "hapticFeedback.type");
    const auto* v = reinterpret_cast<const XrHapticVibration*>(fb);
    ++s->haptics;
    if (Verbose()) Log("haptic: duration %lld ns, frequency %.1f, amplitude %.2f", (long long)v->duration, v->frequency, v->amplitude);
    return Focused(s) ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
}

XrResult XRAPI_CALL xrStopHapticFeedback(XrSession session, const XrHapticActionInfo* info) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = nullptr;
    XrResult r = HapticCommon(session, info, s, __func__);
    if (XR_FAILED(r)) return r;
    return Focused(s) ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
}

} // namespace xs
