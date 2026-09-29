#include "research_diagnostics.h"
#include "stereo_task_fix.h"
#include <windows.h>
#include <intrin.h>
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

namespace StereoTaskFix
{
namespace
{
constexpr std::uintptr_t kPopWrap = 0x2DCCFE0;
constexpr std::uintptr_t kPopLoop = 0x2DCD110;
constexpr std::uintptr_t kSkipRule = 0xEE35030;
constexpr std::uintptr_t kInvoker = 0x2DC1D60;
constexpr std::uintptr_t kWrapRuleReturn = 0x2DCD079;
constexpr std::uintptr_t kLoopRuleReturn = 0x2DCD169;
constexpr std::uintptr_t kActorCallback = 0xF3B50;
// The waiting actor's sit motion is issued by this distinct
// task, +F3A20 -> +5DC6760 -> A60 component +1274D0 -> holder/graph.
constexpr std::uintptr_t kMotionCallback = 0xF3A20;
constexpr std::uintptr_t kMotionJob = 0x5DC6760;
// Repairing that task starts the sit, but the same actor phase stays starved. Native
// +5DC7160 calls actor +1050: timed states and attached component updates.
constexpr std::uintptr_t kStateCallback = 0xF3C40;
constexpr std::uintptr_t kStateJob = 0x5DC7160;
using PopFn = void* (*)(void*, std::uint8_t);
using SkipFn = std::uint8_t (*)(void*);
PopFn g_popWrap{}, g_popLoop{};
SkipFn g_skip{};
using InvokeFn = void (*)(void*, const std::uint32_t*);
InvokeFn g_invoke{};
std::atomic_uint64_t g_present{};
std::uintptr_t g_base{};
bool (*g_stereoActive)(){};
LogFn g_log{};
std::atomic_bool g_ready{};
struct PopContext { void* batch; std::uint8_t restricted; unsigned depth; };
thread_local PopContext g_context{};

constexpr std::size_t kSlots = 512;
struct HolderState
{
    std::atomic_uint64_t calls{}, zeroDt{};
    std::atomic_uintptr_t holder{}, graph{}, graphVtable{}, graphStep{};
    std::atomic_uint32_t mode{}, paused{}, controlMode{}, controlState{}, controlCount{}, controlDirty{};
};
struct Slot
{
    std::atomic_uintptr_t task{}, actor{}, callback{};
    std::atomic_uint32_t interval{};
    std::atomic_uint64_t deferred[2]{}, returned[2]{}, invoked[2]{}, dtZero[2]{};
    std::atomic_uint64_t ruleRun[2][2]{}, ruleSkip[2][2]{};
    std::atomic_uint64_t noContext[2]{}, seen[2]{}, first[2]{}, last[2]{};
    std::atomic_uint32_t preCounter[2]{}, preFlags[2]{}, dt[2]{}, kind[2]{};
    std::atomic_uintptr_t caller[2]{}, batch[2]{};
    std::atomic_uint32_t batchFlags[2]{};
    std::atomic_uint64_t stages[2][7]{}; // job, actor update, request driver, component list, AI comp, mode holder, mode tray
    std::atomic_uint64_t scaledZero[2]{}, updateDone[2]{}, listDisabled[2]{}, holderPaused[2]{};
    std::atomic_uint32_t scaledDt[2]{}, actorFlags[2]{}, requestMode[2]{}, requestPhase[2]{}, requestPending[2]{};
    std::atomic_uint32_t holderMode[2]{}, holderDt[2]{}, motion[2]{}, motionAfter[2]{}, listState[2]{};
    std::atomic_uintptr_t actualActor[2]{};
    HolderState holders[2][4]{};
};
Slot g_slots[kSlots]{};
struct StageScope { Slot* slot; int phase; void* aiComp; };
thread_local StageScope g_stage{};
using StepFn = void (*)(void*, const std::uint32_t*);
using UpdateFn = std::uintptr_t (*)(void*, const std::uint32_t*);
using TrayFn = std::uint8_t (*)(void*);
StepFn g_job{}, g_request{}, g_list{}, g_ai{}, g_holder{};
UpdateFn g_update{};
TrayFn g_tray{};
constexpr std::uintptr_t kJob = 0x5DC7060, kUpdate = 0xF3750, kRequest = 0x10EFA0;
constexpr std::uintptr_t kList = 0x5EB9410, kAi = 0x5E67ED0, kHolder = 0x11D620, kTray = 0x89B7580;

std::atomic_uint64_t g_bypassed[2]{}, g_returned[2]{}, g_overflow{};
constexpr std::uintptr_t kTargetResolver = 0xA157420;
using ResolveFn = void (*)(void*, void*);
ResolveFn g_resolve{};
constexpr std::size_t kTargetSlots = 128;
struct SceneTarget
{
    std::atomic_uintptr_t node{}, handle{}, actor{}, actorVtable{};
    std::atomic_uint32_t index{}, count{}, id{}, status{}, classRva{}, flagsF1F2{};
    std::atomic_uint64_t samples[2]{}, waits[2]{}, first[2]{}, last[2]{};
    std::atomic_uint32_t firstMotion[2]{}, lastMotion[2]{};
};
SceneTarget g_targets[kTargetSlots]{};
std::atomic_uint64_t g_targetOverflow{}, g_targetReadErrors{};

// Observe the motion writer and its adjacent actor phases.
// Raw invocation identity remains available for any other motion writer.
constexpr std::size_t kAnySlots = 8192;
struct AnyTask
{
    std::atomic_uint32_t ready{};
    std::uintptr_t task{}, callback{}, object{};
    std::atomic_uint32_t interval{}, kind{}, batchFlags{};
    std::atomic_uint64_t run[2][2]{}, skip[2][2]{}, dropped[2]{}, bypassed[2]{}, invoked[2]{}, first[2]{}, last[2]{};
    std::atomic_bool motionWriter{};
};
AnyTask g_any[kAnySlots]{};
std::atomic_uint64_t g_anyOverflow{};
thread_local AnyTask* g_currentTask{};
thread_local void* g_invokingTask{};
std::atomic_bool g_anyStarted{};
AnyTask* ObserveTask(void* task, int phase)
{
    if (!task) return nullptr;
    if (g_stereoActive()) g_anyStarted.store(true);
    if (!g_anyStarted.load()) return nullptr;
    __try
    {
        const auto* b = static_cast<const std::uint8_t*>(task);
        const auto key = reinterpret_cast<std::uintptr_t>(task);
        const auto callback = *reinterpret_cast<const std::uintptr_t*>(b + 8);
        // Retain the motion writer's two neighboring actor phases
        // without filling history with transient unrelated scheduler tasks.
        if (callback != g_base + kMotionCallback && callback != g_base + kActorCallback && callback != g_base + kStateCallback) return nullptr;
        const auto object = *reinterpret_cast<const std::uintptr_t*>(b + 0x28);
        const auto start = ((key >> 4) ^ (callback >> 4) ^ (object >> 4)) * 0x9E3779B97F4A7C15ull >> 51;
        for (std::size_t n = 0; n < 32; ++n)
        {
            auto& t = g_any[(start + n) & (kAnySlots - 1)];
            auto state = t.ready.load(std::memory_order_acquire);
            if (!state && t.ready.compare_exchange_strong(state, 1))
            {
                t.task = key; t.callback = callback; t.object = object;
                t.ready.store(2, std::memory_order_release);
                state = 2;
            }
            while (state == 1) { _mm_pause(); state = t.ready.load(std::memory_order_acquire); }
            if (state != 2 || t.task != key || t.callback != callback || t.object != object) continue;
            t.interval.store(b[0x49]);
            t.kind.store(*reinterpret_cast<const std::uint32_t*>(b + 0x30));
            const auto present = g_present.load();
            std::uint64_t zero{}; t.first[phase].compare_exchange_strong(zero, present);
            t.last[phase].store(present);
            return &t;
        }
        g_anyOverflow.fetch_add(1);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}
bool IsSceneActor(std::uintptr_t actor)
{
    if (!actor) return false;
    for (const auto& t : g_targets)
        if (t.node.load() && t.actor.load() == actor) return true;
    return false;
}
constexpr std::uintptr_t kMotionSetter = 0x5C163E0;
using MotionSetterFn = std::uintptr_t (*)(void*, std::uint32_t);
MotionSetterFn g_motionSetter{};
struct MotionEvent
{
    std::atomic_bool ready{};
    std::uint64_t present{};
    std::uintptr_t actor{}, task{}, callback{}, object{}, actorScope{}, caller{};
    std::uint32_t phase{}, before{}, requested{}, after{}, thread{};
    void* stack[24]{};
    unsigned frames{};
};
constexpr std::size_t kMotionEvents = 256;
MotionEvent g_motionEvents[kMotionEvents]{};
std::atomic_uint32_t g_motionCount{}, g_motionLogged{};
std::uintptr_t MotionSetter(void* actor, std::uint32_t motion)
{
    // Direct actor argument, independent of any guessed AI holder or TLS actor.
    const bool capture = g_ready.load(std::memory_order_acquire) && IsSceneActor(reinterpret_cast<std::uintptr_t>(actor));
    MotionEvent* e{};
    if (capture)
    {
        const auto index = g_motionCount.fetch_add(1);
        if (index < kMotionEvents)
        {
            e = &g_motionEvents[index];
            e->present = g_present.load(); e->phase = g_stereoActive() ? 0 : 1;
            e->actor = reinterpret_cast<std::uintptr_t>(actor);
            e->requested = motion; e->thread = GetCurrentThreadId();
            e->caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
            if (g_currentTask)
            {
                e->task = g_currentTask->task; e->callback = g_currentTask->callback; e->object = g_currentTask->object;
                g_currentTask->motionWriter.store(true);
            }
            else if (g_invokingTask)
            {
                // Preserve raw task identity even if the bounded history table fills.
                e->task = reinterpret_cast<std::uintptr_t>(g_invokingTask);
                __try
                {
                    const auto* b = static_cast<const std::uint8_t*>(g_invokingTask);
                    e->callback = *reinterpret_cast<const std::uintptr_t*>(b + 8);
                    e->object = *reinterpret_cast<const std::uintptr_t*>(b + 0x28);
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            e->actorScope = g_stage.slot ? g_stage.slot->actor.load() : 0;
            __try { e->before = *reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(actor) + 0x7E4); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            e->frames = CaptureStackBackTrace(0, 24, e->stack, nullptr);
        }
    }
    const auto result = g_motionSetter(actor, motion);
    if (e)
    {
        __try { e->after = *reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(actor) + 0x7E4); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        e->ready.store(true, std::memory_order_release);
    }
    return result;
}

// Read only after the game's own resolver has filled the vector. Each
// 16-byte reference is {entity id, handle}; native +5DC3B40..49 resolves
// its cached ActorHandle through [handle+8]. Enumerate ALL entries.
void CaptureTargets(void* action)
{
    if (!g_ready.load(std::memory_order_acquire) || !action) return;
    __try
    {
        const auto* node = static_cast<const std::uint8_t*>(action);
        const auto vtable = *reinterpret_cast<const std::uintptr_t*>(node);
        const auto type = vtable - g_base;
        if (type != 0x354B820 && type != 0x354BE80) return;
        const auto status = *reinterpret_cast<const std::uint32_t*>(node + 0x480);
        const auto count = *reinterpret_cast<const std::uint32_t*>(node + 0x490);
        const auto* refs = *reinterpret_cast<const std::uint8_t* const*>(node + 0x488);
        if (!refs || count > 128) { if (count > 128) g_targetReadErrors.fetch_add(1); return; }
        const auto present = g_present.load(std::memory_order_relaxed);
        const int phase = g_stereoActive() ? 0 : 1;
        const auto key = reinterpret_cast<std::uintptr_t>(action);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const auto id = *reinterpret_cast<const std::uint32_t*>(refs + i * 16);
            const auto handle = *reinterpret_cast<const std::uintptr_t*>(refs + i * 16 + 8);
            const auto actor = handle ? *reinterpret_cast<const std::uintptr_t*>(handle + 8) : 0;
            SceneTarget* target{};
            for (auto& t : g_targets)
            {
                auto found = t.node.load(std::memory_order_acquire);
                if (found == key && t.index.load() == i) { target = &t; break; }
                if (!found && t.node.compare_exchange_strong(found, key))
                { t.index.store(i); target = &t; break; }
            }
            if (!target) { g_targetOverflow.fetch_add(1); continue; }
            target->handle.store(handle); target->actor.store(actor);
            target->id.store(id); target->status.store(status); target->count.store(count);
            target->classRva.store(static_cast<std::uint32_t>(type));
            std::uint32_t motion{};
            if (actor)
            {
                target->actorVtable.store(*reinterpret_cast<const std::uintptr_t*>(actor) - g_base);
                motion = *reinterpret_cast<const std::uint32_t*>(actor + 0x7E4);
                target->flagsF1F2.store(*reinterpret_cast<const std::uint8_t*>(actor + 0xF1) |
                    (static_cast<unsigned>(*reinterpret_cast<const std::uint8_t*>(actor + 0xF2)) << 8));
            }
            if (!target->samples[phase].fetch_add(1))
            { target->first[phase].store(present); target->firstMotion[phase].store(motion); }
            target->last[phase].store(present); target->lastMotion[phase].store(motion);
            if (!status) target->waits[phase].fetch_add(1);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_targetReadErrors.fetch_add(1); }
}
void ResolveTargets(void* action, void* reference)
{
    g_resolve(action, reference);
    CaptureTargets(action);
}


// The caller already owns this live task. Only actor jobs with an actual
// frame-skip interval need the repair; unrelated task families are unchanged.
bool ReadActor(void* task, std::uintptr_t& actor, std::uint8_t& interval)
{
    if (!task) return false;
    __try
    {
        const auto* bytes = static_cast<const std::uint8_t*>(task);
        std::uintptr_t callback{};
        std::memcpy(&callback, bytes + 8, sizeof(callback));
        if (callback != g_base + kActorCallback && callback != g_base + kMotionCallback && callback != g_base + kStateCallback) return false;
        interval = bytes[0x49];
        std::memcpy(&actor, bytes + 0x28, sizeof(actor));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

Slot* Find(void* task, std::uintptr_t actor, std::uint8_t interval)
{
    const auto key = reinterpret_cast<std::uintptr_t>(task);
    auto index = static_cast<std::size_t>((key >> 4) * 0x9E3779B97F4A7C15ull >> 55);
    for (std::size_t probe = 0; probe < kSlots; ++probe, index = (index + 1) & (kSlots - 1))
    {
        auto& slot = g_slots[index];
        auto found = slot.task.load(std::memory_order_relaxed);
        if (found == key || (!found && slot.task.compare_exchange_strong(found, key)) || found == key)
        {
            slot.actor.store(actor, std::memory_order_relaxed);
            slot.callback.store(*reinterpret_cast<const std::uintptr_t*>(static_cast<const std::uint8_t*>(task) + 8) - g_base);
            slot.interval.store(interval, std::memory_order_relaxed);
            return &slot;
        }
    }
    g_overflow.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}
void Seen(Slot& slot, int phase)
{
    const auto present = g_present.load(std::memory_order_relaxed);
    if (!slot.seen[phase].fetch_add(1, std::memory_order_relaxed)) slot.first[phase].store(present);
    slot.last[phase].store(present, std::memory_order_relaxed);
}
void Record(void* task, std::uintptr_t actor, std::uint8_t interval, bool deferred, int phase)
{
    if (auto* slot = Find(task, actor, interval))
    {
        (deferred ? slot->deferred[phase] : slot->returned[phase]).fetch_add(1, std::memory_order_relaxed);
        Seen(*slot, phase);
    }
}

// The batch's restricted-pass rejection must precede the stateful skip rule.
// Otherwise a render-only pass consumes the due counter or banks an extra dt.
// Return SKIP through the native completion path without touching task state;
// the unrestricted pass then runs the original rule at native cadence.
std::uint8_t Skip(void* task)
{
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    Slot* slot{};
    std::uintptr_t actor{};
    std::uint8_t interval{}, counter{}, flags{}, batchFlags{};
    bool actorStateValid{};
    const bool ready = g_ready.load(std::memory_order_acquire);
    const bool stereo = ready && g_stereoActive();
    const int phase = stereo ? 0 : 1;
    const bool knownCaller = caller == g_base + kWrapRuleReturn || caller == g_base + kLoopRuleReturn;
    const bool context = g_context.batch && knownCaller;
    const int restricted = context && g_context.restricted ? 1 : 0;
    if (ready && ReadActor(task, actor, interval))
    {
        if constexpr (ResearchDiagnostics::Enabled) slot = Find(task, actor, interval);
        __try
        {
            const auto* bytes = static_cast<const std::uint8_t*>(task);
            counter = bytes[0x4A];
            flags = bytes[0x38];
            if (context) batchFlags = *static_cast<volatile std::uint8_t*>(g_context.batch);
            actorStateValid = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { slot = nullptr; }
        if (slot)
        {
            Seen(*slot, phase);
            slot->preCounter[phase].store(counter, std::memory_order_relaxed);
            slot->preFlags[phase].store(flags, std::memory_order_relaxed);
            slot->caller[phase].store(caller - g_base, std::memory_order_relaxed);
            slot->batch[phase].store(reinterpret_cast<std::uintptr_t>(g_context.batch), std::memory_order_relaxed);
            slot->batchFlags[phase].store(batchFlags, std::memory_order_relaxed);
            if (!context) slot->noContext[phase].fetch_add(1, std::memory_order_relaxed);

        }
    }
    auto* any = ResearchDiagnostics::Enabled && ready ? ObserveTask(task, phase) : nullptr;
    if (actorStateValid && interval && stereo && context && restricted && !(batchFlags & 0x10))
    {
        const int path = caller == g_base + kLoopRuleReturn ? 1 : 0;
        if constexpr (ResearchDiagnostics::Enabled) {
            g_bypassed[path].fetch_add(1, std::memory_order_relaxed);
            Record(task, actor, interval, true, phase);
        }
        if (any) { any->bypassed[phase].fetch_add(1); any->batchFlags.store(batchFlags); }
        return 1;
    }
    const auto skipped = g_skip(task);
    if (any)
    {
        (skipped ? any->skip[phase][restricted] : any->run[phase][restricted]).fetch_add(1);
        if (context)
        {
            const auto bf = *static_cast<volatile std::uint8_t*>(g_context.batch);
            any->batchFlags.store(bf);
            if (!skipped && restricted && !(bf & 0x10)) any->dropped[phase].fetch_add(1);
        }
    }
    if (slot)
        (skipped ? slot->ruleSkip[phase][restricted] : slot->ruleRun[phase][restricted]).fetch_add(1, std::memory_order_relaxed);

    return skipped;
}

void Invoke(void* task, const std::uint32_t* dt)
{
    std::uintptr_t actor{};
    std::uint8_t interval{};
    const auto saved = g_stage;
    auto* savedTask = g_currentTask;
    auto* savedInvokingTask = g_invokingTask;
    g_invokingTask = task;
    g_currentTask = g_ready.load(std::memory_order_acquire) ? ObserveTask(task, g_stereoActive() ? 0 : 1) : nullptr;
    if (g_currentTask) g_currentTask->invoked[g_stereoActive() ? 0 : 1].fetch_add(1);
    g_stage = {};
    if (g_ready.load(std::memory_order_acquire) && ReadActor(task, actor, interval))
        if (auto* slot = Find(task, actor, interval))
        {
            const int phase = g_stereoActive() ? 0 : 1;
            g_stage = {slot, phase, nullptr};
            Seen(*slot, phase);
            slot->invoked[phase].fetch_add(1, std::memory_order_relaxed);
            __try
            {
                const auto value = *dt;
                if (!value) slot->dtZero[phase].fetch_add(1, std::memory_order_relaxed);
                slot->dt[phase].store(value, std::memory_order_relaxed);
                slot->kind[phase].store(*reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(task) + 0x30), std::memory_order_relaxed);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    __try { g_invoke(task, dt); }
    __finally { g_stage = saved; g_currentTask = savedTask; g_invokingTask = savedInvokingTask; }
}

void Job(void* actor, const std::uint32_t* dt)
{
    if (auto* s = g_stage.slot)
    {
        s->stages[g_stage.phase][0].fetch_add(1, std::memory_order_relaxed);
        s->actualActor[g_stage.phase].store(reinterpret_cast<std::uintptr_t>(actor), std::memory_order_relaxed);
    }
    g_job(actor, dt);
}
std::uintptr_t Update(void* actor, const std::uint32_t* dt)
{
    auto* s = g_stage.slot; const int ph = g_stage.phase;
    if (s)
    {
        s->stages[ph][1].fetch_add(1, std::memory_order_relaxed);
        __try
        {
            auto* b = static_cast<const std::uint8_t*>(actor);
            const auto value = *dt;
            s->scaledDt[ph].store(value);
            if (!value) s->scaledZero[ph].fetch_add(1);
            s->actorFlags[ph].store(b[0xF1] | (static_cast<unsigned>(b[0xF2]) << 8));
            s->motion[ph].store(*reinterpret_cast<const std::uint32_t*>(b + 0x7E4));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    const auto result = g_update(actor, dt);
    if (s)
        __try
        {
            auto* b = static_cast<const std::uint8_t*>(actor);
            if (b[0x1A84]) s->updateDone[ph].fetch_add(1);
            s->motionAfter[ph].store(*reinterpret_cast<const std::uint32_t*>(b + 0x7E4));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    return result;
}
void Request(void* comp, const std::uint32_t* dt)
{
    if (auto* s = g_stage.slot)
    {
        const int ph = g_stage.phase;
        s->stages[ph][2].fetch_add(1, std::memory_order_relaxed);
        __try
        {
            auto* b = static_cast<const std::uint8_t*>(comp);
            s->requestMode[ph].store(*reinterpret_cast<const std::uint32_t*>(b + 0x24));
            s->requestPhase[ph].store(*reinterpret_cast<const std::uint32_t*>(b + 0x2C));
            s->requestPending[ph].store(b[0x9C]);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_request(comp, dt);
}
void List(void* list, const std::uint32_t* dt)
{
    if (auto* s = g_stage.slot)
    {
        const int ph = g_stage.phase;
        s->stages[ph][3].fetch_add(1, std::memory_order_relaxed);
        __try
        {
            auto* b = static_cast<const std::uint8_t*>(list);
            if (b[0xF9]) s->listDisabled[ph].fetch_add(1);
            s->listState[ph].store(*reinterpret_cast<const std::uint32_t*>(b + 0xF4));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_list(list, dt);
}
void Ai(void* comp, const std::uint32_t* dt)
{
    if (auto* s = g_stage.slot) s->stages[g_stage.phase][4].fetch_add(1, std::memory_order_relaxed);
    void* saved = g_stage.aiComp;
    g_stage.aiComp = comp;
    __try { g_ai(comp, dt); }
    __finally { g_stage.aiComp = saved; }
}
void Holder(void* holder, const std::uint32_t* dt)
{
    if (auto* s = g_stage.slot)
        __try
        {
            // Only the AIMode holder, identified by the native call at +5E67F6F.
            if (g_stage.aiComp && holder == *reinterpret_cast<void**>(static_cast<std::uint8_t*>(g_stage.aiComp) + 0x58))
            {
                const int ph = g_stage.phase;
                auto* b = static_cast<const std::uint8_t*>(holder);
                s->stages[ph][5].fetch_add(1);
                if (b[0x49]) s->holderPaused[ph].fetch_add(1);
                s->holderMode[ph].store(*reinterpret_cast<const std::uint32_t*>(b + 0x20));
                s->holderDt[ph].store(*dt);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (auto* s = g_stage.slot)
        __try
        {
            constexpr unsigned offsets[] = {0x68, 0x58, 0x60, 0x70};
            for (unsigned i = 0; g_stage.aiComp && i < 4; ++i)
                if (holder == *reinterpret_cast<void**>(static_cast<std::uint8_t*>(g_stage.aiComp) + offsets[i]))
                {
                    auto& h = s->holders[g_stage.phase][i];
                    const auto* b = static_cast<const std::uint8_t*>(holder);
                    h.calls.fetch_add(1); if (!*dt) h.zeroDt.fetch_add(1);
                    h.holder.store(reinterpret_cast<std::uintptr_t>(holder));
                    h.mode.store(*reinterpret_cast<const std::uint32_t*>(b + 0x20)); h.paused.store(b[0x49]);
                    const auto* ctl = *reinterpret_cast<const std::uint8_t* const*>(b + 0x30);
                    h.controlMode.store(ctl ? *reinterpret_cast<const std::uint32_t*>(ctl + 0x10) : 0);
                    h.controlState.store(ctl ? *reinterpret_cast<const std::uint32_t*>(ctl + 0x1C) : 0);
                    h.controlCount.store(ctl ? *reinterpret_cast<const std::uint32_t*>(ctl + 0x480) : 0);
                    h.controlDirty.store(ctl ? ctl[0x918] : 0);
                    const auto graph = *reinterpret_cast<const std::uintptr_t*>(b + 0x38);
                    const auto vt = graph ? *reinterpret_cast<const std::uintptr_t*>(graph) : 0;
                    h.graph.store(graph); h.graphVtable.store(vt ? vt - g_base : 0);
                    h.graphStep.store(vt ? *reinterpret_cast<const std::uintptr_t*>(vt + 0x58) - g_base : 0);
                }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_holder(holder, dt);
}
std::uint8_t Tray(void* tray)
{
    if (auto* s = g_stage.slot) s->stages[g_stage.phase][6].fetch_add(1, std::memory_order_relaxed);
    return g_tray(tray);
}

void* Pop(PopFn original, void* batch, std::uint8_t restricted)
{
    const auto saved = g_context;
    g_context = {batch, restricted, saved.depth + 1};
    void* task{};
    __try { task = original(batch, restricted); }
    __finally { g_context = saved; }
    // The wrapper can tail-call the loop hook. Count the returned task only
    // at the outermost pop, before the scheduler can execute/free that task.
    if (ResearchDiagnostics::Enabled && !saved.depth && g_ready.load(std::memory_order_acquire))
    {
        std::uintptr_t actor{};
        std::uint8_t interval{};
        if (ReadActor(task, actor, interval))
        {
            const int phase = g_stereoActive() ? 0 : 1;
            g_returned[phase].fetch_add(1, std::memory_order_relaxed);
            Record(task, actor, interval, false, phase);
        }
    }
    return task;
}
void* PopWrap(void* batch, std::uint8_t restricted) { return Pop(g_popWrap, batch, restricted); }
void* PopLoop(void* batch, std::uint8_t restricted) { return Pop(g_popLoop, batch, restricted); }

constexpr std::uint8_t kWrapBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0xB6, 0xEA, 0x48, 0x8B, 0xD9,
    0xF0, 0x80, 0x09, 0x40, 0x0F, 0xB6, 0x01, 0xA8, 0x08, 0x0F, 0x84, 0xF0, 0x00, 0x00, 0x00, 0x0F,
    0xB6, 0x01, 0xA8, 0x04, 0x0F, 0x84, 0xE5, 0x00, 0x00, 0x00, 0x8B, 0x49, 0x2C, 0x8D, 0x41, 0xFF,
    0x85, 0xC1, 0x0F, 0x84, 0xD7, 0x00, 0x00, 0x00, 0x48, 0x89, 0x74, 0x24, 0x30, 0x48, 0x89, 0x7C,
    0x24, 0x38, 0x48, 0x8D, 0x4B, 0x40, 0xFF, 0x15, 0x24, 0x94, 0x29, 0x00, 0x0F, 0xB7, 0x43, 0x06,
    0x3B, 0x43, 0x70, 0x0F, 0x83, 0x85, 0x00, 0x00, 0x00, 0x0F, 0xB7, 0x4B, 0x06, 0x48, 0x8B, 0x43,
    0x68, 0x48, 0x8B, 0x3C, 0xC8, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x66, 0xF0, 0x0F, 0xC1, 0x43, 0x06,
    0x48, 0x8D, 0x4B, 0x40, 0xFF, 0x15, 0xEE, 0x93, 0x29, 0x00, 0x0F, 0xB6, 0x47, 0x38, 0xD0, 0xE8,
    0xA8, 0x01, 0x74, 0x0D, 0x48, 0x8B, 0xD7, 0x48, 0x8B, 0xCB, 0xE8, 0xC1, 0x01, 0x00, 0x00, 0xEB,
    0xB1, 0x48, 0x8B, 0xCF, 0xE8, 0x37, 0xEE, 0xFE, 0xFF, 0x84, 0xC0, 0x74, 0x0D, 0x48, 0x8B, 0xD7,
    0x48, 0x8B, 0xCB, 0xE8, 0xA8, 0x01, 0x00, 0x00, 0xEB, 0x98, 0x40, 0x84, 0xED, 0x74, 0x17, 0x0F,
    0xB6, 0x03, 0xA8, 0x10, 0x75, 0x10, 0x48, 0x8B, 0xD7, 0x48, 0x8B, 0xCB, 0xE8, 0x8F, 0x01, 0x00,
    0x00, 0xE9, 0x7C, 0xFF, 0xFF, 0xFF, 0x48, 0x8B, 0xC7, 0x48, 0x8B, 0x7C, 0x24, 0x38, 0x48, 0x8B,
    0x74, 0x24, 0x30, 0x48, 0x8B, 0x5C, 0x24, 0x40, 0x48, 0x83, 0xC4, 0x20, 0x5D, 0xC3, 0x83, 0x7B,
    0x70, 0x00, 0x75, 0x1D, 0x66, 0x83, 0x7B, 0x0A, 0x00, 0x75, 0x16, 0x0F, 0xB6, 0x03, 0xA8, 0x20,
    0x75, 0x0F, 0x0F, 0xB6, 0x03, 0xA8, 0x01, 0x75, 0x08, 0x48, 0x8B, 0xCB, 0xE8, 0x2F, 0x02, 0x00,
    0x00, 0x48, 0x8D, 0x4B, 0x40, 0xFF, 0x15, 0x5D, 0x93, 0x29, 0x00, 0x33, 0xC0, 0xEB, 0xBA, 0x40,
    0x0F, 0xB6, 0xD5, 0x48, 0x8B, 0xCB, 0x48, 0x8B, 0x5C, 0x24, 0x40, 0x48, 0x83, 0xC4, 0x20, 0x5D,
    0xE9, 0x0B, 0x00, 0x00, 0x00,
};
constexpr std::uint8_t kLoopBytes[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x0F,
    0xB6, 0xF2, 0x48, 0x89, 0x7C, 0x24, 0x30, 0x48, 0x8B, 0xD9, 0xBD, 0xE7, 0xFF, 0x00, 0x00, 0x90,
    0xB9, 0x01, 0x00, 0x00, 0x00, 0x66, 0xF0, 0x0F, 0xC1, 0x4B, 0x06, 0x0F, 0xB7, 0xC1, 0x3B, 0x43,
    0x70, 0x0F, 0x83, 0x9D, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x43, 0x68, 0x0F, 0xB7, 0xC9, 0x48, 0x8B,
    0x3C, 0xC8, 0x0F, 0xB6, 0x47, 0x38, 0xD0, 0xE8, 0xA8, 0x01, 0x74, 0x05, 0x0F, 0xB6, 0x03, 0xEB,
    0x20, 0x48, 0x8B, 0xCF, 0xE8, 0x47, 0xED, 0xFE, 0xFF, 0x84, 0xC0, 0x74, 0x05, 0x0F, 0xB6, 0x03,
    0xEB, 0x0F, 0x40, 0x84, 0xF6, 0x74, 0x68, 0x0F, 0xB6, 0x03, 0xA8, 0x10, 0x75, 0x61, 0x0F, 0xB6,
    0x03, 0xA8, 0x08, 0x74, 0x0A, 0x48, 0x8D, 0x4B, 0x40, 0xFF, 0x15, 0xC1, 0x92, 0x29, 0x00, 0xB8,
    0x01, 0x00, 0x00, 0x00, 0x66, 0xF0, 0x0F, 0xC1, 0x43, 0x08, 0x66, 0xFF, 0xC0, 0x0F, 0xB7, 0xC0,
    0x3B, 0x43, 0x70, 0x75, 0x1C, 0x66, 0x21, 0x6F, 0x38, 0x0F, 0xB6, 0x03, 0xA8, 0x20, 0x75, 0x15,
    0x0F, 0xB6, 0x03, 0xA8, 0x01, 0x75, 0x0E, 0x48, 0x8B, 0xCB, 0xE8, 0x51, 0x01, 0x00, 0x00, 0xEB,
    0x04, 0x66, 0x21, 0x6F, 0x38, 0x0F, 0xB6, 0x03, 0xA8, 0x08, 0x0F, 0x84, 0x60, 0xFF, 0xFF, 0xFF,
    0x48, 0x8D, 0x4B, 0x40, 0xFF, 0x15, 0x6E, 0x92, 0x29, 0x00, 0xE9, 0x51, 0xFF, 0xFF, 0xFF, 0x48,
    0x8B, 0xC7, 0xEB, 0x2E, 0x83, 0xC8, 0xFF, 0x66, 0xF0, 0x0F, 0xC1, 0x43, 0x06, 0x83, 0x7B, 0x70,
    0x00, 0x75, 0x1D, 0x66, 0x83, 0x7B, 0x0A, 0x00, 0x75, 0x16, 0x0F, 0xB6, 0x03, 0xA8, 0x20, 0x75,
    0x0F, 0x0F, 0xB6, 0x03, 0xA8, 0x01, 0x75, 0x08, 0x48, 0x8B, 0xCB, 0xE8, 0x00, 0x01, 0x00, 0x00,
    0x33, 0xC0, 0x48, 0x8B, 0x7C, 0x24, 0x30, 0x48, 0x8B, 0x5C, 0x24, 0x38, 0x48, 0x8B, 0x6C, 0x24,
    0x40, 0x48, 0x83, 0xC4, 0x20, 0x5E, 0xC3,
};
constexpr std::uint8_t kRuleBytes[] = {
    0x48, 0x89, 0xCA, 0x0F, 0xB6, 0x49, 0x4A, 0x44, 0x0F, 0xB6, 0x4A, 0x49, 0x44, 0x38, 0xC9, 0x73,
    0x0B, 0x0F, 0xB6, 0x42, 0x38, 0xC0, 0xE8, 0x06, 0xA8, 0x01, 0x74, 0x3C, 0x44, 0x0F, 0xB7, 0x42,
    0x38, 0x41, 0x0F, 0xB6, 0xC0, 0xC0, 0xE8, 0x07, 0xA8, 0x01, 0x75, 0x2C, 0x41, 0x0F, 0xB6, 0xC0,
    0xC0, 0xE8, 0x06, 0xA8, 0x01, 0x74, 0x05, 0x44, 0x38, 0xC9, 0x72, 0x06, 0xC6, 0x42, 0x4A, 0x00,
    0xEB, 0x05, 0xFE, 0xC1, 0x88, 0x4A, 0x4A, 0xB8, 0xBF, 0xFF, 0x00, 0x00, 0x66, 0x41, 0x21, 0xC0,
    0x66, 0x44, 0x89, 0x42, 0x38, 0x30, 0xC0, 0xC3, 0xFE, 0xC1, 0x88, 0x4A, 0x4A, 0x48, 0x8B, 0x05,
    0xFC, 0x67, 0x12, 0xF6, 0x8B, 0x48, 0x08, 0xB8, 0x7F, 0xFF, 0x00, 0x00, 0x01, 0x4A, 0x4C, 0x66,
    0x21, 0x42, 0x38, 0xB0, 0x01, 0xC3,
};
constexpr std::uint8_t kThunkBytes[] = {
    0xE9, 0x7B, 0x91, 0x07, 0x0C,
};

constexpr std::uint8_t kInvokerBytes[] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x8B, 0x0A, 0x85, 0xC9, 0x75, 0x0E, 0x48,
    0x8D, 0x4B, 0x08, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xE9, 0x63, 0xBF, 0x9A, 0xFE, 0xF6, 0x43, 0x39,
    0x01, 0x74, 0x04, 0x33, 0xC0, 0xEB, 0x03, 0x8B, 0x43, 0x4C, 0x03, 0xC1, 0x89, 0x44, 0x24, 0x38,
    0x48, 0x8B, 0x43, 0x30, 0x48, 0x83, 0xC0, 0xFE, 0x48, 0x83, 0xF8, 0x08, 0x77, 0x75, 0x48, 0x8D,
    0x0D, 0x5B, 0xE2, 0x23, 0xFD, 0x8B, 0x84, 0x81, 0x2C, 0x1E, 0xDC, 0x02, 0x48, 0x03, 0xC1, 0xFF,
    0xE0, 0x48, 0x63, 0x4B, 0x10, 0x48, 0x03, 0x4B, 0x28, 0xEB, 0x5C, 0x48, 0x8B, 0x4B, 0x28, 0xFF,
    0x53, 0x08, 0xC7, 0x43, 0x4C, 0x00, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3, 0x48,
    0x63, 0x4B, 0x10, 0x48, 0x03, 0x4B, 0x28, 0xFF, 0x53, 0x08, 0xC7, 0x43, 0x4C, 0x00, 0x00, 0x00,
    0x00, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3, 0xFF, 0x53, 0x08, 0xC7, 0x43, 0x4C, 0x00, 0x00, 0x00,
    0x00, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3, 0x48, 0x8B, 0x4B, 0x08, 0x48, 0x8D, 0x54, 0x24, 0x38,
    0x48, 0x8B, 0x01, 0xFF, 0x50, 0x08, 0xC7, 0x43, 0x4C, 0x00, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4,
    0x20, 0x5B, 0xC3, 0x48, 0x8B, 0x4B, 0x28, 0x48, 0x8D, 0x54, 0x24, 0x38, 0xFF, 0x53, 0x08, 0xC7,
    0x43, 0x4C, 0x00, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3, 0x13, 0x1E, 0xDC, 0x02,
    0xB1, 0x1D, 0xDC, 0x02, 0x13, 0x1E, 0xDC, 0x02, 0xBB, 0x1D, 0xDC, 0x02, 0xBB, 0x1D, 0xDC, 0x02,
    0xCF, 0x1D, 0xDC, 0x02, 0xBB, 0x1D, 0xDC, 0x02, 0xE7, 0x1D, 0xDC, 0x02, 0xF7, 0x1D, 0xDC, 0x02,
};

constexpr std::uint8_t kJobBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xD3, 0x48, 0x89, 0xCF, 0xE8, 0xDB, 0x91, 0xE3, 0xFF, 0x66, 0x0F, 0x6E};
constexpr std::uint8_t kUpdateBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0xF2, 0x48, 0x8B, 0xD9};
constexpr std::uint8_t kRequestBytes[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x66, 0x0F, 0x6E, 0x0A, 0x0F, 0x57, 0xC0, 0x0F, 0x5B, 0xC9, 0x48, 0x8B, 0xD9, 0xF3, 0x0F, 0x59, 0x0D, 0xAD};
constexpr std::uint8_t kListBytes[] = {0x56, 0x48, 0x83, 0xEC, 0x20, 0x80, 0xB9, 0xF9, 0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0xD6, 0x75, 0x6D, 0x48, 0x89, 0x7C, 0x24, 0x38, 0x48, 0x8D};
constexpr std::uint8_t kAiBytes[] = {0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x79, 0x10, 0x48, 0x89, 0xD5, 0x48, 0x89};
constexpr std::uint8_t kHolderBytes[] = {0x40, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x48, 0x89, 0x5C, 0x24, 0x50, 0x48, 0x89, 0x6C, 0x24};
constexpr std::uint8_t kTrayBytes[] = {0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xCB, 0xE8, 0x23, 0xFD, 0xE4, 0xFE, 0x84, 0xC0, 0x74, 0x11, 0x48, 0x8D, 0x8B, 0xF8, 0x00, 0x00, 0x00};

constexpr std::uint8_t kStateCallbackBytes[] = {0xE9, 0x1B, 0x35, 0xCD, 0x05};
constexpr std::uint8_t kStateJobBytes[] = {0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x02, 0x48, 0x89, 0xCB, 0x89, 0x44, 0x24, 0x30, 0xE8, 0x6D, 0x29, 0xE3, 0xFF, 0x3D, 0x8A, 0x5F, 0x02, 0x01, 0x75, 0x13, 0x48, 0x8D, 0x54, 0x24, 0x38, 0x48, 0x89, 0xD9, 0xE8, 0xC9, 0x92, 0xE3, 0xFF, 0x8B, 0x10, 0x89, 0x54, 0x24, 0x30, 0x48, 0x8B, 0x03, 0x48, 0x8D, 0x54, 0x24, 0x30, 0x48, 0x89, 0xD9, 0xFF, 0x90, 0x50, 0x10, 0x00, 0x00, 0x84, 0xC0, 0x75, 0x0C, 0x48, 0x8D, 0x8B, 0xF0, 0x16, 0x00, 0x00, 0xE8, 0x72, 0xDA, 0x06, 0x09, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3};
constexpr std::uint8_t kMotionCallbackBytes[] = {0xE9, 0x3B, 0x2D, 0xCD, 0x05};
constexpr std::uint8_t kMotionJobBytes[] = {0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x02, 0x48, 0x89, 0xCB, 0x89, 0x44, 0x24, 0x30, 0xE8, 0x6D, 0x33, 0xE3, 0xFF, 0x3D, 0x8A, 0x5F, 0x02, 0x01, 0x75, 0x13, 0x48, 0x8D, 0x54, 0x24, 0x38, 0x48, 0x89, 0xD9, 0xE8, 0xC9, 0x9C, 0xE3, 0xFF, 0x8B, 0x10, 0x89, 0x54, 0x24, 0x30, 0x48, 0x8B, 0x03, 0x48, 0x89, 0xD9, 0xFF, 0x90, 0x60, 0x0A, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x0D, 0x48, 0x8D, 0x54, 0x24, 0x30, 0x48, 0x89, 0xC1, 0xE8, 0x25, 0x0D, 0x36, 0xFA, 0x48, 0x8B, 0x03, 0x48, 0x89, 0xD9, 0xFF, 0x90, 0x90, 0x0E, 0x00, 0x00, 0x48, 0x89, 0xC3, 0x48, 0x85, 0xC0, 0x74, 0x22, 0xF6, 0x80, 0x84, 0x00, 0x00, 0x00, 0x02, 0x74, 0x08, 0x48, 0x89, 0xC1, 0xE8, 0x30, 0x6B, 0x0C, 0x00, 0xF6, 0x83, 0x84, 0x00, 0x00, 0x00, 0x01, 0x74, 0x08, 0x48, 0x89, 0xD9, 0xE8, 0x7F, 0xF5, 0x35, 0xFA, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3};

constexpr std::uint8_t kMotionSetterBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0xB6, 0x81, 0x4C, 0x0F, 0x00, 0x00, 0x89, 0xD7, 0x48, 0x89, 0xCB, 0xA8, 0x08};

constexpr std::uint8_t kTargetResolverBytes[] = {0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x48, 0x89, 0x5C, 0x24, 0x58, 0x48, 0x89, 0xD6};

bool Matches(std::uintptr_t rva, const std::uint8_t* expected, std::size_t size)
{
    __try { return std::memcmp(reinterpret_cast<const void*>(g_base + rva), expected, size) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

bool Install(std::uintptr_t exeBase, bool (*stereoActive)(), LogFn log)
{
    g_base = exeBase;
    g_stereoActive = stereoActive;
    g_log = log;
    if (!exeBase || !stereoActive || !log) return false;
    if (!Matches(kStateCallback, kStateCallbackBytes, sizeof(kStateCallbackBytes)) ||
        !Matches(kStateJob, kStateJobBytes, sizeof(kStateJobBytes)) ||
        !Matches(kMotionCallback, kMotionCallbackBytes, sizeof(kMotionCallbackBytes)) ||
        !Matches(kMotionJob, kMotionJobBytes, sizeof(kMotionJobBytes)) ||
        !Matches(kMotionSetter, kMotionSetterBytes, sizeof(kMotionSetterBytes)) ||
        !Matches(kTargetResolver, kTargetResolverBytes, sizeof(kTargetResolverBytes)) ||
        !Matches(kJob, kJobBytes, sizeof(kJobBytes)) ||
        !Matches(kUpdate, kUpdateBytes, sizeof(kUpdateBytes)) ||
        !Matches(kRequest, kRequestBytes, sizeof(kRequestBytes)) ||
        !Matches(kList, kListBytes, sizeof(kListBytes)) ||
        !Matches(kAi, kAiBytes, sizeof(kAiBytes)) ||
        !Matches(kHolder, kHolderBytes, sizeof(kHolderBytes)) ||
        !Matches(kTray, kTrayBytes, sizeof(kTrayBytes)) ||
        !Matches(kInvoker, kInvokerBytes, sizeof(kInvokerBytes)) ||
        !Matches(0x2DCCFE0, kWrapBytes, sizeof(kWrapBytes)) ||
        !Matches(0x2DCD110, kLoopBytes, sizeof(kLoopBytes)) ||
        !Matches(0xEE35030, kRuleBytes, sizeof(kRuleBytes)) ||
        !Matches(0x2DBBEB0, kThunkBytes, sizeof(kThunkBytes)))
    {
        log("[TASKFIX] INSTALL FAILED: retail scheduler bytes differ; no scheduler patches applied");
        return false;
    }
    const auto init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
    {
        log("[TASKFIX] INSTALL FAILED: MinHook init %s", MH_StatusToString(init));
        return false;
    }
    struct Hook { std::uintptr_t rva; void* detour; void** original; };
    const Hook hooks[] = {
        {kMotionSetter, reinterpret_cast<void*>(&MotionSetter), reinterpret_cast<void**>(&g_motionSetter)},
        {kTargetResolver, reinterpret_cast<void*>(&ResolveTargets), reinterpret_cast<void**>(&g_resolve)},
        {kPopWrap, reinterpret_cast<void*>(&PopWrap), reinterpret_cast<void**>(&g_popWrap)},
        {kPopLoop, reinterpret_cast<void*>(&PopLoop), reinterpret_cast<void**>(&g_popLoop)},
        {kSkipRule, reinterpret_cast<void*>(&Skip), reinterpret_cast<void**>(&g_skip)},
        {kInvoker, reinterpret_cast<void*>(&Invoke), reinterpret_cast<void**>(&g_invoke)},
        {kJob, reinterpret_cast<void*>(&Job), reinterpret_cast<void**>(&g_job)},
        {kUpdate, reinterpret_cast<void*>(&Update), reinterpret_cast<void**>(&g_update)},
        {kRequest, reinterpret_cast<void*>(&Request), reinterpret_cast<void**>(&g_request)},
        {kList, reinterpret_cast<void*>(&List), reinterpret_cast<void**>(&g_list)},
        {kAi, reinterpret_cast<void*>(&Ai), reinterpret_cast<void**>(&g_ai)},
        {kHolder, reinterpret_cast<void*>(&Holder), reinterpret_cast<void**>(&g_holder)},
        {kTray, reinterpret_cast<void*>(&Tray), reinterpret_cast<void**>(&g_tray)},
    };
    // Keep any created trampolines for process lifetime, even on partial
    // failure. g_ready remains false so partially enabled hooks cannot mutate
    // tasks. Never free a trampoline that a worker may already be executing.
    for (const auto& hook : hooks)
    {
        if (!ResearchDiagnostics::Enabled && hook.rva != kPopWrap && hook.rva != kPopLoop && hook.rva != kSkipRule) continue;
        const auto result = MH_CreateHook(reinterpret_cast<void*>(exeBase + hook.rva), hook.detour, hook.original);
        if (result != MH_OK)
        {
            log("[TASKFIX] INSTALL FAILED: create +0x%llX %s", static_cast<unsigned long long>(hook.rva), MH_StatusToString(result));
            return false;
        }
    }
    for (const auto& hook : hooks)
    {
        if (!ResearchDiagnostics::Enabled && hook.rva != kPopWrap && hook.rva != kPopLoop && hook.rva != kSkipRule) continue;
        const auto result = MH_QueueEnableHook(reinterpret_cast<void*>(exeBase + hook.rva));
        if (result != MH_OK)
        {
            log("[TASKFIX] INSTALL FAILED: queue +0x%llX %s", static_cast<unsigned long long>(hook.rva), MH_StatusToString(result));
            return false;
        }
    }
    const auto result = MH_ApplyQueued();
    if (result != MH_OK)
    {
        log("[TASKFIX] INSTALL FAILED: enable %s", MH_StatusToString(result));
        return false;
    }
    g_ready.store(true, std::memory_order_release);
    log("[TASKFIX] INSTALLED v6 ACTOR-PHASE-FIX: reject restricted pass BEFORE skip rule for +F3A20 motion, +F3B50 update, +F3C40 state tasks; native cadence/dt; all actors; mono passive");
    return true;
}

void Tick(std::uint64_t present)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    g_present.store(present, std::memory_order_relaxed);
    if (!g_ready.load(std::memory_order_acquire)) return;
    static std::uint64_t lastPresent{};
    static bool lastStereo{};
    const bool stereo = g_stereoActive();
    if (stereo == lastStereo && present - lastPresent < 300) return;
    lastPresent = present;
    lastStereo = stereo;
    const auto wrap = g_bypassed[0].load(std::memory_order_relaxed);
    const auto loop = g_bypassed[1].load(std::memory_order_relaxed);
    if (!stereo && !wrap && !loop) return;
    g_log("[TASKFIX] present=%llu stereo=%u bypassed wrap/loop=%llu/%llu returned stereo/mono=%llu/%llu overflow=%llu (lifetime totals; returned=queue output, not job completion)",
        static_cast<unsigned long long>(present), stereo ? 1u : 0u,
        static_cast<unsigned long long>(wrap), static_cast<unsigned long long>(loop),
        static_cast<unsigned long long>(g_returned[0].load()), static_cast<unsigned long long>(g_returned[1].load()),
        static_cast<unsigned long long>(g_overflow.load()));
    g_log("[SCENETARGET] present=%llu overflow=%llu readErrors=%llu",
        static_cast<unsigned long long>(present), static_cast<unsigned long long>(g_targetOverflow.load()), static_cast<unsigned long long>(g_targetReadErrors.load()));
    g_log("[ANYTASK] overflow=%llu motionEvents=%u eventCapacity=%zu exeBase=%llX", static_cast<unsigned long long>(g_anyOverflow.load()), g_motionCount.load(), kMotionEvents, static_cast<unsigned long long>(g_base));
    for (unsigned n = g_motionLogged.load(); n < (std::min)(g_motionCount.load(), static_cast<unsigned>(kMotionEvents)); ++n)
    {
        const auto& e = g_motionEvents[n];
        if (!e.ready.load(std::memory_order_acquire)) break;
        g_log("[MOTIONWRITE] event=%u present=%llu phase=%s actor=%llX motion=%X requested=%X after=%X task=%llX callback=%llX object=%llX actorScope=%llX caller=%llX tid=%u frames=%u (callback/caller/stack absolute; subtract exeBase)", n, e.present, e.phase ? "MONO" : "STEREO", e.actor, e.before, e.requested, e.after, e.task, e.callback, e.object, e.actorScope, e.caller, e.thread, e.frames);
        for (unsigned f = 0; f < e.frames; ++f) g_log("[MOTIONSTACK] event=%u frame=%u pc=%llX", n, f, reinterpret_cast<std::uintptr_t>(e.stack[f]));
        g_motionLogged.store(n + 1);
    }
    for (const auto& t : g_any)
    {
        if (t.ready.load(std::memory_order_acquire) != 2) continue;
        if (!t.motionWriter.load() && !(t.interval.load() && (t.dropped[0].load() + t.bypassed[0].load()) >= 20)) continue;
        for (int ph = 0; ph < 2; ++ph)
            if (t.last[ph].load())
                g_log("[ANYTASK] task=%llX callback=%llX object=%llX interval=%u kind=%u phase=%s first/last=%llu/%llu RUN normal/restricted=%llu/%llu SKIP normal/restricted=%llu/%llu discarded=%llu bypassed=%llu invoked=%llu batchFlags=%X motionWriter=%u (callback absolute)", t.task, t.callback, t.object, t.interval.load(), t.kind.load(), ph ? "MONO" : "STEREO", t.first[ph].load(), t.last[ph].load(), t.run[ph][0].load(), t.run[ph][1].load(), t.skip[ph][0].load(), t.skip[ph][1].load(), t.dropped[ph].load(), t.bypassed[ph].load(), t.invoked[ph].load(), t.batchFlags.load(), t.motionWriter.load() ? 1u : 0u);
    }
    for (const auto& t : g_targets)
    {
        const auto node = t.node.load();
        if (!node) continue;
        const auto actor = t.actor.load();
        g_log("[SCENETARGET] node=%llX class=%s target=%u/%u id=%X handle=%llX actor=%llX actorVtable=%llX status=%u flagsF1F2=%04X stereo samples/waits=%llu/%llu first/last=%llu/%llu motion=%X->%X mono samples/waits=%llu/%llu first/last=%llu/%llu motion=%X->%X",
            static_cast<unsigned long long>(node), t.classRva.load() == 0x354B820 ? "PlayMotion" : "Wait", t.index.load(), t.count.load(), t.id.load(),
            static_cast<unsigned long long>(t.handle.load()), static_cast<unsigned long long>(actor), static_cast<unsigned long long>(t.actorVtable.load()), t.status.load(), t.flagsF1F2.load(),
            static_cast<unsigned long long>(t.samples[0].load()), static_cast<unsigned long long>(t.waits[0].load()), static_cast<unsigned long long>(t.first[0].load()), static_cast<unsigned long long>(t.last[0].load()), t.firstMotion[0].load(), t.lastMotion[0].load(),
            static_cast<unsigned long long>(t.samples[1].load()), static_cast<unsigned long long>(t.waits[1].load()), static_cast<unsigned long long>(t.first[1].load()), static_cast<unsigned long long>(t.last[1].load()), t.firstMotion[1].load(), t.lastMotion[1].load());
        unsigned matches{};
        for (const auto& slot : g_slots)
            if (actor && slot.task.load() && slot.actor.load() == actor)
            {
                ++matches;
                g_log("[TARGETTASK] node=%llX target=%u actor=%llX task=%llX interval=%u stereo returned/invoked=%llu/%llu mono returned/invoked=%llu/%llu (see AICHAIN for same task)",
                    static_cast<unsigned long long>(node), t.index.load(), static_cast<unsigned long long>(actor), static_cast<unsigned long long>(slot.task.load()), slot.interval.load(),
                    static_cast<unsigned long long>(slot.returned[0].load()), static_cast<unsigned long long>(slot.invoked[0].load()),
                    static_cast<unsigned long long>(slot.returned[1].load()), static_cast<unsigned long long>(slot.invoked[1].load()));
            }
        if (!matches) g_log("[TARGETTASK] node=%llX target=%u actor=%llX NO MATCH in actor-task trace", static_cast<unsigned long long>(node), t.index.load(), static_cast<unsigned long long>(actor));
    }
    // Log every observed actor, including interval zero and tasks with no
    // repairs. Lifetime ranking would hide the conversation tasks.
    for (const auto& slot : g_slots)
    {
        const auto task = slot.task.load();
        if (!task) continue;
        for (int phase = 0; phase < 2; ++phase)
        {
            if (!slot.seen[phase].load()) continue;
            g_log("[TASKTRACE] task=%llX actor=%llX callback=%llX interval=%u phase=%s first/last=%llu/%llu RUN normal/restricted=%llu/%llu SKIP normal/restricted=%llu/%llu bypassed=%llu returned=%llu invoked=%llu dtZero=%llu dt=%u kind=%u noContext=%llu caller=%llX batch=%llX batchFlags=%02X counter=%u flags=%02X",
                static_cast<unsigned long long>(task), static_cast<unsigned long long>(slot.actor.load()), static_cast<unsigned long long>(slot.callback.load()), slot.interval.load(), phase ? "MONO" : "STEREO",
                static_cast<unsigned long long>(slot.first[phase].load()), static_cast<unsigned long long>(slot.last[phase].load()),
                static_cast<unsigned long long>(slot.ruleRun[phase][0].load()), static_cast<unsigned long long>(slot.ruleRun[phase][1].load()),
                static_cast<unsigned long long>(slot.ruleSkip[phase][0].load()), static_cast<unsigned long long>(slot.ruleSkip[phase][1].load()),
                static_cast<unsigned long long>(slot.deferred[phase].load()),
                static_cast<unsigned long long>(slot.returned[phase].load()), static_cast<unsigned long long>(slot.invoked[phase].load()),
                static_cast<unsigned long long>(slot.dtZero[phase].load()), slot.dt[phase].load(), slot.kind[phase].load(),
                static_cast<unsigned long long>(slot.noContext[phase].load()), static_cast<unsigned long long>(slot.caller[phase].load()),
                static_cast<unsigned long long>(slot.batch[phase].load()), slot.batchFlags[phase].load(), slot.preCounter[phase].load(), slot.preFlags[phase].load());
            if (IsSceneActor(slot.actor.load()))
                for (unsigned i = 0; i < 4; ++i)
                {
                    const auto& h = slot.holders[phase][i];
                    constexpr unsigned offsets[] = {0x68, 0x58, 0x60, 0x70};
                    g_log("[HOLDERS] actor=%llX phase=%s compOffset=%X calls=%llu zeroDt=%llu holder=%llX mode=%X paused=%u ctlMode=%X ctlState=%u ctlCount=%u ctlDirty=%u graph=%llX graphVtable=%llX graphStep=%llX", slot.actor.load(), phase ? "MONO" : "STEREO", offsets[i], h.calls.load(), h.zeroDt.load(), h.holder.load(), h.mode.load(), h.paused.load(), h.controlMode.load(), h.controlState.load(), h.controlCount.load(), h.controlDirty.load(), h.graph.load(), h.graphVtable.load(), h.graphStep.load());
                }
            g_log("[AICHAIN] task=%llX actor=%llX actualActor=%llX phase=%s job/update/request/list/AI/holder/tray=%llu/%llu/%llu/%llu/%llu/%llu/%llu scaledDt=%u zeroDt=%llu flagsF1F2=%04X updateDone=%llu listDisabled=%llu listState=%u reqMode=%X reqPhase=%u pending=%u holderMode=%X holderDt=%u holderPaused=%llu motion=%X->%X",
                static_cast<unsigned long long>(task), static_cast<unsigned long long>(slot.actor.load()), static_cast<unsigned long long>(slot.actualActor[phase].load()), phase ? "MONO" : "STEREO",
                static_cast<unsigned long long>(slot.stages[phase][0].load()), static_cast<unsigned long long>(slot.stages[phase][1].load()),
                static_cast<unsigned long long>(slot.stages[phase][2].load()), static_cast<unsigned long long>(slot.stages[phase][3].load()),
                static_cast<unsigned long long>(slot.stages[phase][4].load()), static_cast<unsigned long long>(slot.stages[phase][5].load()),
                static_cast<unsigned long long>(slot.stages[phase][6].load()), slot.scaledDt[phase].load(), static_cast<unsigned long long>(slot.scaledZero[phase].load()),
                slot.actorFlags[phase].load(), static_cast<unsigned long long>(slot.updateDone[phase].load()), static_cast<unsigned long long>(slot.listDisabled[phase].load()),
                slot.listState[phase].load(), slot.requestMode[phase].load(), slot.requestPhase[phase].load(), slot.requestPending[phase].load(),
                slot.holderMode[phase].load(), slot.holderDt[phase].load(), static_cast<unsigned long long>(slot.holderPaused[phase].load()),
                slot.motion[phase].load(), slot.motionAfter[phase].load());
        }
    }
}
} // namespace StereoTaskFix
