/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
#include "RetiredJITArtifacts.h"

#include "GCAwareJITStubRoutine.h"
#include "Heap.h"
#include "InlineCacheHandler.h"
#include "Options.h"
#include "VM.h"

#if __has_include("HeapClientSet.h")
// Heap workstream landed: GCClient::Heap::server() resolution is meaningful
// (same gate as bytecode/JSThreadsSafepoint.cpp). THREADS-INTEGRATE(jit)
#define JSC_JIT_HAS_SHARED_HEAP_SERVER 1
#endif

// N6 shim (SPEC-jit section 2 / section 4.4): the epoch facility is owned by
// the heap workstream (heap/GCSafepointEpoch.h, SPEC-heap section 11). Bodies
// below compile against it iff it has landed; until then they are no-op
// leak-until-integration stubs. The leak is sound pre-integration: the phase-1
// GIL stub admits no concurrent retirement, and flag-off there are no callers
// (the Task-3 rerouting of resetStubAsJumpInAccess, initializeWithUnitHandler
// displacement, and jettison-time PropertyInlineCache::deref(VM&) is gated on
// Options::useJSThreads()). Ordering note recorded in INTEGRATE-jit.md: heap must land
// before Task 13's epoch tests run. THREADS-INTEGRATE(jit)
#if __has_include("GCSafepointEpoch.h")
#include "GCSafepointEpoch.h"
#define JSC_JIT_HAS_GC_SAFEPOINT_EPOCH 1
#endif

namespace JSC {

// R4-2 (review round 4): resolve the heap whose safepoint epoch governs the
// retired data's lifetime — the SERVER heap this VM's client attaches to.
// Under useSharedGCHeap a client VM's vm.heap is not the shared server and its
// local epoch does not track the real mutator population (see the header
// comment); for the 1:1 case server() == vm.heap. Gate fallback mirrors
// JSThreadsSafepoint.cpp: without HeapClientSet.h no foreign-client shared
// server can exist, so vm.heap is the only candidate.
[[maybe_unused]] static JSC::Heap& epochHeapFor(VM& vm)
{
#if defined(JSC_JIT_HAS_SHARED_HEAP_SERVER)
    return vm.clientHeap.server();
#else
    return vm.heap;
#endif
}

#if ENABLE(JIT)

#if defined(JSC_JIT_HAS_GC_SAFEPOINT_EPOCH)
namespace {

// Holds a retired handler chain until its retirement epoch expires. The
// destructor (run at expiry) derefs the chain: node payloads are pure data
// (G2), and each node's Ref<GCAwareJITStubRoutine> drops into the jettisoned-
// stub-routine machinery, so the machine code is deleted only after the GC's
// conservative scan of all mutator stacks proves it off-stack (R2, I7) - never
// by epoch expiry alone.
class RetiredHandlerChain final : public RetiredCallback {
public:
    explicit RetiredHandlerChain(RefPtr<InlineCacheHandler>&& head)
        : m_head(WTF::move(head))
    {
    }

private:
    RefPtr<InlineCacheHandler> m_head;
};

} // anonymous namespace
#endif // defined(JSC_JIT_HAS_GC_SAFEPOINT_EPOCH)

void RetiredJITArtifacts::retireHandlerChain(VM& vm, RefPtr<InlineCacheHandler>&& head)
{
    if (!head)
        return;

    // SPEC-jit section 4.4 hard rule: only GC-aware stub routines may ride a
    // retired chain; their executable memory is freed by the jettison + R2
    // scan path, never by epoch expiry.
    for (auto* cursor = head.get(); cursor; cursor = cursor->next()) {
        if (auto* routine = cursor->stubRoutine())
            RELEASE_ASSERT(routine->isGCAware());
    }

#if defined(JSC_JIT_HAS_GC_SAFEPOINT_EPOCH)
    epochHeapFor(vm).safepointEpoch().retire(std::unique_ptr<RetiredCallback>(new RetiredHandlerChain(WTF::move(head))));
#else
    // Leak-until-integration stub (N6): never free inline - a JIT'd reader on
    // another thread (post-GIL) could still hold a node pointer inside its
    // safepoint-free window. Pre-integration the GIL makes the leak the only
    // cost. THREADS-INTEGRATE(jit)
    UNUSED_PARAM(vm);
    (void)head.leakRef();
#endif
}

#endif // ENABLE(JIT)

void RetiredJITArtifacts::retire(VM& vm, std::unique_ptr<RetiredCallback>&& callback)
{
    if (!callback)
        return;

#if defined(JSC_JIT_HAS_GC_SAFEPOINT_EPOCH)
    epochHeapFor(vm).safepointEpoch().retire(WTF::move(callback));
#else
    // Leak-until-integration stub (N6). THREADS-INTEGRATE(jit)
    UNUSED_PARAM(vm);
    (void)callback.release();
#endif
}

} // namespace JSC
