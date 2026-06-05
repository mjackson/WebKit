/*
 * Copyright (C) 2025 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "VMManager.h"

#include "JSCConfig.h"
#include "JSLock.h"
#include "VM.h"
#include "VMEntryScopeInlines.h"
#include "VMThreadContext.h"
#include "WasmDebugServerUtilities.h"
#include <wtf/Locker.h>
#include <wtf/RunLoop.h>

namespace JSC {

VM* VMManager::s_recentVM { nullptr };

VMManager& VMManager::singleton()
{
    static LazyNeverDestroyed<VMManager> manager;
    static std::once_flag onceKey;
    std::call_once(onceKey, [] {
        manager.construct();
    });
    return manager.get();
}

VMThreadContext::VMThreadContext() = default;
VMThreadContext::~VMThreadContext() = default;

bool VMManager::isValidVMSlow(VM* vm)
{
    bool found = false;
    forEachVM([&] (VM& nextVM) {
        if (vm == &nextVM) {
            s_recentVM = vm;
            found = true;
            return IterationStatus::Done;
        }
        return IterationStatus::Continue;
    });
    return found;
}

void VMManager::dumpVMs()
{
    unsigned i = 0;
    WTFLogAlways("Registered VMs:");
    forEachVM([&] (VM& nextVM) {
        WTFLogAlways("  [%u] VM %p", i++, &nextVM);
        return IterationStatus::Continue;
    });
}

void VMManager::iterateVMs(const Invocable<IterationStatus(VM&)> auto& functor) WTF_REQUIRES_LOCK(m_worldLock)
{
    for (auto* context = m_vmList.head(); context; context = context->next()) {
        VM& vm = *VM::fromThreadContext(context);
        IterationStatus status = functor(vm);
        if (status == IterationStatus::Done)
            return;
    }
}

VM* VMManager::findMatchingVMImpl(const ScopedLambda<VMManager::TestCallback>& test)
{
    Locker lock { m_worldLock };
    if (s_recentVM && test(*s_recentVM))
        return s_recentVM;

    VM* result = nullptr;
    iterateVMs(scopedLambda<IteratorCallback>([&] (VM& vm) {
        if (test(vm)) {
            result = &vm;
            s_recentVM = &vm;
            return IterationStatus::Done;
        }
        return IterationStatus::Continue;
    }));
    return result;
}

void VMManager::forEachVMImpl(const ScopedLambda<VMManager::IteratorCallback>& func)
{
    Locker lock { m_worldLock };
    iterateVMs(func);
}

VMManager::Error VMManager::forEachVMWithTimeoutImpl(Seconds timeout, const ScopedLambda<VMManager::IteratorCallback>& func)
{
    if (!m_worldLock.tryLockWithTimeout(timeout))
        return Error::TimedOut;

    Locker locker { AdoptLock, m_worldLock };
    iterateVMs(func);
    return Error::None;
}

void VMManager::Info::dump(PrintStream& out) const
{
    out.print("VMManager::Info(numberOfVMs:", numberOfVMs);
    out.print(", numberOfActiveVMs:", numberOfActiveVMs);
    out.print(", numberOfStoppedVMs:", numberOfStoppedVMs);
    out.print(", worldMode:", worldMode);
    out.print(", targetVM:", RawPointer(targetVM), ")");
}

auto VMManager::info() -> Info
{
    Info info;
    auto& manager = singleton();

    // The reason for locking here is so that we capture a consistent snapshot
    // of all the values in info.
    Locker lock { manager.m_worldLock };
    info.numberOfVMs = manager.m_numberOfVMs;
    info.numberOfActiveVMs = manager.m_numberOfActiveVMs;
    info.numberOfStoppedVMs = manager.m_numberOfStoppedVMs;
    info.worldMode = manager.m_worldMode;
    info.targetVM = manager.m_targetVM;
    return info;
}

void VMManager::setWasmDebuggerOnStop(StopTheWorldCallback callback)
{
    g_jscConfig.wasmDebuggerOnStop = callback;
}

void VMManager::setWasmDebuggerOnResume(PostResumeCallback callback)
{
    g_jscConfig.wasmDebuggerOnResume = callback;
}

void VMManager::setMemoryDebuggerCallback(StopTheWorldCallback callback)
{
    g_jscConfig.memoryDebuggerStopTheWorld = callback;
}

// THREADS-INTEGRATE(heap) manifest 5d (review round 4): file-local
// Atomic statics, deliberately NOT g_jscConfig slots — JSC::Config lives
// in the WTF::Config page that Config::finalize() (run from every VM
// constructor) mprotects read-only, and the sole installer
// (Heap::noteSharedServerSticky, second-client attach) always runs
// post-freeze; a config store would SIGSEGV at the ISS flip. seq_cst
// Atomic: the install happens-before the ISS flip publishes on the
// installing thread, and the hooks are inert (no-op unless ISS && GSP),
// so a load racing the install correctly no-ops.
static Atomic<void (*)(VM&)> s_gcWillParkInStopTheWorld { nullptr };
static Atomic<void (*)(VM&)> s_gcDidResumeFromStopTheWorld { nullptr };

void VMManager::setGCParkCallbacks(void (*willPark)(VM&), void (*didResume)(VM&))
{
    // Heap-owned hooks (JSC::Heap::gcWillParkInStopTheWorld /
    // gcDidResumeFromStopTheWorld); may be null (inert).
    s_gcWillParkInStopTheWorld.store(willPark);
    s_gcDidResumeFromStopTheWorld.store(didResume);
}

#if USE(BUN_JSC_ADDITIONS)
void VMManager::setJSDebuggerCallback(StopTheWorldCallback callback)
{
    g_jscConfig.jsDebuggerStopTheWorld = callback;
}
#endif

void VMManager::incrementActiveVMs(VM& vm) WTF_REQUIRES_LOCK(m_worldLock)
{
    RELEASE_ASSERT(m_worldMode != Mode::RunAll);

    if (!vm.traps().m_hasBeenCountedAsActive) {
        m_numberOfActiveVMs++;
        vm.traps().m_hasBeenCountedAsActive = true;
    }
}

void VMManager::decrementActiveVMs(VM& vm) WTF_REQUIRES_LOCK(m_worldLock)
{
    // We only need to track m_numberOfActiveVMs changes if we're in RunOne
    // mode. If we're running because the world was resumed with RunAll,
    // then m_numberOfActiveVMs is invalid, and resumeTheWorld() would set
    // it to a token value of invalidNumberOfActiveVMs (to aid debugging).
    if (m_worldMode == Mode::RunAll) {
        RELEASE_ASSERT(m_numberOfActiveVMs == invalidNumberOfActiveVMs);
        RELEASE_ASSERT(!vm.traps().m_hasBeenCountedAsActive);
    } else if (vm.traps().m_hasBeenCountedAsActive) {
        m_numberOfActiveVMs--;
        vm.traps().m_hasBeenCountedAsActive = false;
    }

    auto shouldResumeAll = [&] WTF_REQUIRES_LOCK(m_worldLock) {
        if (m_worldMode != Mode::RunAll && !m_numberOfActiveVMs)
            return true;
        if (m_worldMode == Mode::RunOne) {
            RELEASE_ASSERT(m_targetVM == &vm);
            return true;
        }
        return false;
    };

    if (shouldResumeAll()) {
        if (m_targetVM) {
            // There's a designated targetVM thread to continue in, but we don't have the
            // ability to just wake the desired one up. So, wake up all the threads and let
            // them sort themselves out.
            //
            // But if the targetVM thread is this thread, then pass the control to another
            // thread, any thread. That's because this thread is dying imminently.
            if (m_targetVM == &vm) {
                m_targetVM = nullptr;
                m_useRunOneMode = false;
            }
            m_worldConditionVariable.notifyAll();
        } else {
            // There's no designated targetVM thread. So, just waking up any one thread will do.
            m_worldConditionVariable.notifyOne();
        }
    }
}

CONCURRENT_SAFE void VMManager::requestStopAllInternal(StopReason reason)
{
    // StopReason is synonymous with "StopRequest".
    // From the client's perspective, it is the reason for a stop request.
    // From the VMManager's perspective, it is the type of stop request.
    auto requestBits = static_cast<StopRequestBits>(reason);
    m_pendingStopRequestBits.exchangeOr(requestBits);
    {
        Locker lock { m_worldLock };
        if (m_worldMode >= Mode::Stopping) {
            // THREADS-INTEGRATE(heap) manifest 5g(ii): a GC stop
            // requested while another stop is already in progress must
            // still (1) trap entered VMs — a RunOne targetVM keeps
            // executing through an in-progress debugger stop and would
            // otherwise never reach a poll — and (2) wake parked VMs so
            // their wait loops observe the new GC bit and run the 5g(i)
            // park hook (release heap access). Without this, a GC
            // requested during a non-GC stop hangs the §10.4 barrier.
            if (reason == StopReason::GC) [[unlikely]] {
                iterateVMs(scopedLambda<IteratorCallback>([&] (VM& vm) {
                    if (vm.isEntered()) {
                        vm.requestStop();
                        WTF::storeLoadFence();
                    }
                    return IterationStatus::Continue;
                }));
                m_worldConditionVariable.notifyAll();
            }
            return;
        }

        if (m_worldMode == Mode::RunAll) {
            // RunOne mode allows execution of 1 VM without resumeTheWorld(). We did not clear
            // the m_hasBeenCountedAsActive flags on each VM on resuming with RunOne. As a
            // result, m_numberOfActiveVMs is still valid in RunOne mode. We don't want
            // to reset m_numberOfActiveVMs to 0 here because we won't be re-calculating
            // it on stop like we do for RunAll mode.
            //
            // For RunAll mode, do want to reset m_numberOfActiveVMs, and incrementActiveVMs()
            // below will re-calculate the current true value of m_numberOfActiveVMs.
            m_numberOfActiveVMs = 0;
        }

        m_worldMode = Mode::Stopping;

        bool isWasmDebugger = reason == StopReason::WasmDebugger;

        // Have to use iterateVMs() instead of forEachVM() because we're already
        // holding the m_worldLock.
        iterateVMs(scopedLambda<IteratorCallback>([&] (VM& vm) {
            vm.requestStop();
            WTF::storeLoadFence();

            if (isWasmDebugger) [[unlikely]]
                dispatchStopHandler(vm);

            if (vm.isEntered()) {
                // incrementActiveVMs() relies on m_worldLock being held, which it
                // obviously is above. However, Clang is not smart enough to see this.
                // So, we need to suppress this warning here.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wthread-safety-analysis"
#endif
                incrementActiveVMs(vm);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
            }
            return IterationStatus::Continue;
        }));
    }
}

// Dispatch a callback to VM's RunLoop to handle Stop-The-World for idle VMs.
// Idle VMs (not executing code) never check traps, so they can't respond to requestStop().
// Dispatching to RunLoop ensures the callback executes when VM processes events, allowing
// idle VMs to call notifyVMStop(). Uses atomic flag to prevent duplicate dispatches.
void VMManager::dispatchStopHandler(VM& vm)
{
    if (vm.traps().m_hasDispatchedIdleStopHandler.exchange(true))
        return;

    // Use JSLock coordination pattern (like JSRunLoopTimer) to safely detect VM destruction.
    Ref<JSLock> apiLock = vm.apiLock();
    vm.runLoop().dispatch([apiLock = WTF::move(apiLock)]() {
        Locker locker { apiLock.get() };

        RefPtr<VM> vm = apiLock->vm();
        if (!vm)
            return;

        // Clear flag before VMEntryScope so new stop requests during VMEntryScope can dispatch.
        // Must clear before, not after: stops during destruction can't be handled by this scope.
        vm->traps().m_hasDispatchedIdleStopHandler.exchange(false);

        RELEASE_ASSERT(!vm->isEntered());
        VMEntryScope scope(*vm, nullptr);
    });
}

CONCURRENT_SAFE void VMManager::requestResumeAllInternal(StopReason reason)
{
    // StopReason is synonymous with StopRequest.
    // From the client's perspective, it is the reason for a stop request.
    // From the VMManager's perspective, it is the type of stop request.
    auto requestBits = static_cast<StopRequestBits>(reason);
    m_pendingStopRequestBits.exchangeAnd(~requestBits);

    // THREADS-INTEGRATE(heap) manifest 5e: VMs parked by the GC
    // keep-parked rule (5b) wait on m_worldConditionVariable with no
    // latched m_currentStopReason and (with no other stop in progress) no
    // targetVM — NOTHING else will ever wake them once the GC bit
    // clears. So for reason == GC, ALWAYS notifyAll under m_worldLock:
    // even if other stop bits remain pending (woken VMs re-evaluate
    // shouldStop() and re-park for the remaining reasons), and even if
    // resumeTheWorld() early-returns (RunOne mode, or already RunAll).
    // Getting this wrong (e.g. notifying only when the GC bit was the
    // last pending bit) is a silent shared-mode resume deadlock.
    if (reason == StopReason::GC) {
        Locker lock { m_worldLock };
        if (!hasPendingStopRequests())
            resumeTheWorld();
        m_worldConditionVariable.notifyAll();
        return;
    }

    if (hasPendingStopRequests())
        return; // There are still pending stop requests. Nothing more to do.

    Locker lock { m_worldLock };
    resumeTheWorld();
}

void VMManager::resumeTheWorld() WTF_REQUIRES_LOCK(m_worldLock)
{
    // We can call resumeTheWorld() more than once. Hence, we may already be in RunAll mode.
    if (m_worldMode == Mode::RunAll)
        return; // Already resumed. Nothing more to do.

    // If we're in RunOne mode, then we want to still call into notifyVMStop() all
    // the time. So, we don't want to resumeTheWorld() just yet as that will disable
    // all the stop checks yet.
    if (m_useRunOneMode)
        return;

    // Have to use iterateVMs() instead of forEachVM() because we're already
    // holding the m_worldLock.
    iterateVMs(scopedLambda<IteratorCallback>([&] (VM& vm) {
        vm.cancelStop();
        vm.traps().m_hasBeenCountedAsActive = false;
        return IterationStatus::Continue;
    }));

    m_targetVM = nullptr;
    m_numberOfActiveVMs = invalidNumberOfActiveVMs; // invalid when not Stopped.
    m_worldMode = Mode::RunAll;
    m_worldConditionVariable.notifyAll();
}

void VMManager::notifyVMStop(VM& vm, StopTheWorldEvent event)
{
    // Ensure VM is counted as active before executing the body of notifyVMStop.
    // This is needed for concurrent stop requests where entry services
    // may have missed the increment due to flag races with dispatch callbacks.
    // The guard in incrementActiveVMs makes this idempotent - if VM was already
    // counted (entry services succeeded), this does nothing.
    {
        Locker lock { m_worldLock };
        // Guard against a late-arriving notifyVMStop() after resumeTheWorld() has already
        // completed. Once the world is back in RunAll, touching the counters would corrupt
        // the m_numberOfStoppedVMs == m_numberOfActiveVMs invariant used in the following stop section.
        if (m_worldMode == Mode::RunAll)
            return;

        incrementActiveVMs(vm);
        ++m_numberOfStoppedVMs;

        // Must be inside this lock. Once m_numberOfStoppedVMs == m_numberOfActiveVMs,
        // the STW callback fires and the debugger assumes isStopped() on every VM.
        // A VM preempted between releasing this lock and calling setStopped() would
        // cause isStopped() to return false after the STW callback fires.
#if ENABLE(WEBASSEMBLY_DEBUGGER)
        if (Options::enableWasmDebugger()) [[unlikely]]
            vm.debugState()->setStopped();
#endif
    }

    // THREADS-INTEGRATE(heap) manifest 5a: GC park hook (entry side). Called
    // with NO m_worldLock held (the hook takes heap locks; L6). Heap-owned,
    // idempotent: no-op unless ISS && GSP && this VM's client holds access.
    if (auto willParkHook = s_gcWillParkInStopTheWorld.load()) [[unlikely]]
        willParkHook(vm);

    // Due to races, we may end up calling notifyVMStop() even when there is no stop to be serviced.
    // It should always be safe to call notifyVMStop() as many times as we like. The only cost is
    // is performance.
    //
    // In Mode::RunOne, we will call notifyVMStop() even if there are no requested stops. The code
    // below will simply determine that there's nothing to do and return back out. This is fine
    // since Mode::RunOne is only used by debuggers, and peek performance is not a concern.
    // We need to ensure that StopTheWorld VMTraps remained installed and that notifyVMStop() gets
    // called when in Mode::RunOne because new VM thread can be started, and we want those new
    // threads to also stop since they aren't the targetVM thread.


    for (;;) {
        {
            Locker lock { m_worldLock };

            RELEASE_ASSERT(m_numberOfStoppedVMs <= m_numberOfActiveVMs);

            auto fetchTopPriorityStopReason = [&] {
                auto pendingRequests = m_pendingStopRequestBits.loadRelaxed();
                // THREADS-INTEGRATE(heap) manifest 5c: the GC bit is never
                // latched/serviced by notifyVMStop — it only keeps VMs
                // parked (5b). The conductor resumes them via
                // requestResumeAll(GC) (5e).
                pendingRequests &= ~static_cast<StopRequestBits>(StopReason::GC);
                for (unsigned i = 0; i < NumberOfStopReasons; ++i) {
                    auto requestToCheck = static_cast<StopRequestBits>(1 << i);
                    if (pendingRequests & requestToCheck)
                        return static_cast<StopReason>(requestToCheck);
                }
                return StopReason::None;
            };

            auto shouldStop = [&] WTF_REQUIRES_LOCK(m_worldLock) {
                // THREADS-INTEGRATE(heap) manifest 5b: a pending GC stop
                // keeps every VM parked until requestResumeAll(GC) — the GC
                // bit is never latched into m_currentStopReason (5c).
                if (m_pendingStopRequestBits.loadRelaxed() & static_cast<StopRequestBits>(StopReason::GC))
                    return true;

                // 1. If the targetVM is already selected, and we're not the targetVM, then stop.
                //    We need to check this first because in RunOne mode, even if there is no more
                //    STW request to service, any VM that is not the targetVM still needs to stop.
                if (m_targetVM)
                    return m_targetVM != &vm;

                // 2. If there's no more STW requests, then we don't need to stop.
                //    This is superseded by the condition above during RunOne mode.
                if (m_currentStopReason == StopReason::None)
                    return false;

                // 3. We have a STW request. If not all active VMs are at the stopping point yet,
                //    then stop and wait for the last VM to stop.
                //    FIXME: rdar://173360944 Any VM may serve the STW callback, not just the last to stop,
                //    since once the counter lock above is released, any VM can observe m_numberOfStoppedVMs == m_numberOfActiveVMs.
                return m_numberOfStoppedVMs != m_numberOfActiveVMs;
            };

            // Fetch the top priority stop request and finish servicing it
            // before entertaining another one. THREADS-INTEGRATE(heap)
            // manifest 5f: the fetch precedes the FIRST shouldStop() AND
            // re-runs after every wake — a stop request that arrives while
            // we are parked must be latched by SOME stopped VM or no one
            // services it.
            bool calledGCParkHook = false;
            bool ranGCResumeHook = false;
            for (;;) {
                if (m_currentStopReason == StopReason::None)
                    m_currentStopReason = fetchTopPriorityStopReason();
                if (!shouldStop()) {
                    // THREADS-INTEGRATE(heap) manifest 5g(iii) (review round
                    // 3): if a GC park hook released this VM's heap access —
                    // EITHER the entry-side 5a insertion-A call (GC bit
                    // already pending when we parked) OR the mid-park 5g(i)
                    // call below — re-acquire it BEFORE leaving the wait
                    // loop. The dispatch below may run a non-GC STW callback
                    // (wasmDebuggerOnStop / memoryDebuggerStopTheWorld —
                    // stop reasons that never take the heap's GCL
                    // JSThreadsStopScope bracket) which reads, or even
                    // allocates from, the heap; with access still released a
                    // shared-mode conductor's §10.4 barrier would treat this
                    // VM as not-accessing and collect/sweep under the
                    // callback's feet (and any allocation would violate I2).
                    // The hook is idempotent via m_releasedByGCPark (a no-op
                    // when nothing was released, so gating on ranGCResumeHook
                    // rather than calledGCParkHook is safe AND necessary —
                    // the entry-side release never sets calledGCParkHook) and
                    // F8-blocks if a NEW GC stop pends; calledGCParkHook is
                    // reset and we re-evaluate (continue) so a GC bit that
                    // arrived during the re-acquire re-runs 5g(i) instead of
                    // leaving the new conductor's barrier waiting on us.
                    auto didResumeHook = s_gcDidResumeFromStopTheWorld.load();
                    if (!ranGCResumeHook && didResumeHook) [[unlikely]] {
                        ranGCResumeHook = true;
                        calledGCParkHook = false;
                        {
                            DropLockForScope dropper(lock);
                            didResumeHook(vm);
                        }
                        continue;
                    }
                    break;
                }
                // THREADS-INTEGRATE(heap) manifest 5g(i): a VM that parked
                // BEFORE the GC stop was requested (e.g. for a debugger
                // stop) still holds heap access, and its entry-side 5a hook
                // ran when no GC bit existed — it must release access NOW or
                // the conductor's §10.4 barrier never completes
                // (§10C(a)/(c)/(d)). The hook must run without m_worldLock
                // (it takes heap locks); after re-taking the lock we
                // re-evaluate (continue) rather than wait, because the 5e
                // resume-notify may have fired while the lock was dropped.
                // calledGCParkHook bounds this to one call per GC stop (the
                // hook is idempotent — m_releasedByGCPark — so a re-fire
                // would be harmless but would busy-spin the loop); 5g(iii)
                // resets it after the matching re-acquire so a LATER GC bit
                // in the same notifyVMStop invocation re-runs this block,
                // and this block resets ranGCResumeHook so the matching
                // 5g(iii) re-acquire runs again at the next loop exit.
                bool gcBitPending = m_pendingStopRequestBits.loadRelaxed() & static_cast<StopRequestBits>(StopReason::GC);
                auto willParkHook = s_gcWillParkInStopTheWorld.load();
                if (gcBitPending && !calledGCParkHook && willParkHook) [[unlikely]] {
                    calledGCParkHook = true;
                    ranGCResumeHook = false;
                    {
                        DropLockForScope dropper(lock);
                        willParkHook(vm);
                    }
                    continue;
                }
                m_worldConditionVariable.wait(m_worldLock);
            }

            // We can only get here under one the following possible circumstance:
            // 1. No targetVM thread was specified (therefore, any thread may service this stop)
            //    and this is the last thread that stopped. Or ...
            // 2. This is a subsequent iteration through this loop after context switches (see the
            //    m_worldConditionVariable.notifyAll() at the bottom of the loop). In which case,
            //    the targetVM thread is the only one that can get past the wait() above. Or ...
            // 3. We're executing in RunOne mode and entering this function due to a subsequent
            //    stop request. In that case, all other threads remained stopped, and only the
            //    targetVM thread is allowed to run.
            RELEASE_ASSERT(!m_targetVM || m_targetVM == &vm);

            // Now we can break out of the handler loop is there are no more requests.
            if (m_currentStopReason == StopReason::None) {
                if (m_useRunOneMode) {
                    m_worldMode = Mode::RunOne;
                    RELEASE_ASSERT(m_targetVM);
                } else if (m_worldMode != Mode::RunAll)
                    resumeTheWorld(); // Sets m_worldMode = Mode::RunAll.
                break; // Exit this loop.
            }

            m_targetVM = &vm;
            m_worldMode = Mode::Stopped;
        }

        auto status = STW_RESUME();
        switch (m_currentStopReason) {
        case StopReason::GC:
            RELEASE_ASSERT_NOT_REACHED();
        case StopReason::WasmDebugger:
            status = g_jscConfig.wasmDebuggerOnStop(vm, event);
            break;
        case StopReason::MemoryDebugger:
            status = g_jscConfig.memoryDebuggerStopTheWorld(vm, event);
            break;
#if USE(BUN_JSC_ADDITIONS)
        case StopReason::JSDebugger:
            status = g_jscConfig.jsDebuggerStopTheWorld(vm, event);
            break;
#endif
        case StopReason::None:
            RELEASE_ASSERT_NOT_REACHED();
        }

        if (status.first == IterationStatus::Done) {
            // Done servicing this request. We can't just exit the loop here yet because there
            // may be other requests that need to be serviced. So, we'll just clear the
            // current request and go back to the top of the loop to check if there are other
            // requests. It's safe to clear m_currentStopReason without acquiring m_worldLock
            // here because currently, all other VM threads are already stopped.
            // Same reason for why it's safe to set m_useRunOneMode here.
            auto requestBits = static_cast<StopRequestBits>(m_currentStopReason);
            m_pendingStopRequestBits.exchangeAnd(~requestBits);
            if (m_currentStopReason == StopReason::WasmDebugger)
                m_needsWasmDebuggerOnResume.store(true);
            m_currentStopReason = StopReason::None;

            // targetVM not being specified means that we should not change m_useRunOneMode.
            if (status.second)
                m_useRunOneMode = status.second != STW_RESUME_ALL_TOKEN;
        }

        if (status.second && status.second != STW_RESUME_ALL_TOKEN && status.second != m_targetVM) {
            // A context switch was requested. Wake all so that a context switch can occur, and
            // continue on the targetVM thread.
            Locker lock { m_worldLock };
            m_targetVM = status.second;
            m_worldConditionVariable.notifyAll();
        }
    }

    unsigned numberOfStoppedVMs = UINT_MAX;

    {
        Locker lock { m_worldLock };

        // If we get here, we're either transitioning to RunOne or Running mode.
        RELEASE_ASSERT(!m_targetVM || m_targetVM == &vm);

        numberOfStoppedVMs = --m_numberOfStoppedVMs;

#if ENABLE(WEBASSEMBLY_DEBUGGER)
        if (Options::enableWasmDebugger()) [[unlikely]] {
            // WasmAtomicsWaitBlocked: thread is still sleeping inside memory.atomic.wait and
            // may participate in multiple STW cycles. Keep stopData so each cycle shows the
            // correct state; waitForSync() calls clearStop() when the wait ends.
            if (event != StopTheWorldEvent::WasmAtomicsWaitBlocked)
                vm.debugState()->clearStop();
        }
#endif
    }

    // THREADS-INTEGRATE(heap) manifest 5a: GC park hook (resume side). NO
    // m_worldLock held. Heap-owned, idempotent: iff m_releasedByGCPark ->
    // re-acquire heap access (F8-blocking if a NEW stop pends), then clear.
    if (auto didResumeHook = s_gcDidResumeFromStopTheWorld.load()) [[unlikely]]
        didResumeHook(vm);

    // Call post-resume callback once when last VM exits and all VMs are running.
    if (!numberOfStoppedVMs && m_needsWasmDebuggerOnResume.exchange(false))
        g_jscConfig.wasmDebuggerOnResume();
}

void VMManager::notifyVMConstruction(VM& vm)
{
    bool needsStopping = false;
    {
        Locker locker { m_worldLock };
        s_recentVM = &vm;
        m_vmList.append(vm.threadContext());
        m_numberOfVMs++;
        needsStopping = m_worldMode != Mode::RunAll;
    }
    if (needsStopping) {
        // If a stop is in progress, we cannot proceed onto initializing (i.e. mutating)
        // the heap in the VM constructor. GlobalGC may be expecting a quiescent world
        // state at this point. So, go park this thread if needed.
        notifyVMStop(vm, StopTheWorldEvent::VMCreated); // Cannot be called while holding m_worldLock.

        Locker locker { m_worldLock };
        decrementActiveVMs(vm);
    }
}

void VMManager::notifyVMDestruction(VM& vm)
{
    bool worldIsStopped = false;
    {
        Locker locker { m_worldLock };
        if (s_recentVM == &vm)
            s_recentVM = nullptr;
        m_vmList.remove(vm.threadContext());
        m_numberOfVMs--;

        worldIsStopped = (m_worldMode != Mode::RunAll);
    }
    if (worldIsStopped) {
        // If a stop is in progress, some threads may have stopped, and may need to be
        // woken up.
        handleVMDestructionWhileWorldStopped(vm);
    }
}

void VMManager::notifyVMActivation(VM& vm)
{
    // The main concern for this notification is that if we are currently Stopping or Stopped,
    // then we need to block this newly activated VM from executing.
    bool needsStopping = false;
    {
        Locker lock { m_worldLock };
        s_recentVM = &vm;
        needsStopping = m_worldMode != Mode::RunAll;
    }
    if (needsStopping)
        notifyVMStop(vm, StopTheWorldEvent::VMActivated);
}

void VMManager::notifyVMDeactivation(VM& vm)
{
    // The main concern for this notification is that if we are currently Stopping or Stopped,
    // then we may need to wake up another thread to potentially service the StopTheWorld
    // request. That's because this may be the last thread that STW is waiting on.
    Locker lock { m_worldLock };
    decrementActiveVMs(vm);
}

void VMManager::handleVMDestructionWhileWorldStopped(VM& vm)
{
    Locker lock { m_worldLock };
    if (m_worldMode == Mode::RunAll) {
        // World has been resumed already. Nothing more to do.
        return;
    }

    if (!m_numberOfVMs) {
        // We're the last VM, and we're about to shutdown. So, there's nothing to
        // resume. Fix m_worldMode to reflect this.
        m_worldMode = Mode::RunAll;
        return;
    }

    // If we get here, then the world is either in Stopping / Stopped / RunOne state,
    // and there's at least one other VM thread in play out there. Wake them up so
    // that the right thread can take next step.
    if (m_targetVM) {
        // There's a designated targetVM thread to continue in, but we don't have the
        // ability to just wake the desired one up. So, wake up all the threads and let
        // them sort themselves out.
        //
        // But if the targetVM thread is this thread, then pass the control to another
        // thread, any thread. That's because this thread is dying imminently.
        if (m_targetVM == &vm) {
            m_targetVM = nullptr;
            m_useRunOneMode = false;
        }
        m_worldConditionVariable.notifyAll();
    } else {
        // There's no designated targetVM thread. So, just waking up any one thread will do.
        m_worldConditionVariable.notifyOne();
    }
}

} // namespace JSC
