/*
 * Copyright (C) 2012-2025 Apple Inc. All rights reserved.
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

#pragma once

#include <wtf/Platform.h>

#if ENABLE(C_LOOP)

#include "CLoopStack.h"
#include "CallFrame.h"
#include "CodeBlock.h"
#include "StackManagerInlines.h"
#include "VM.h"

namespace JSC {

ALWAYS_INLINE VM& CLoopStack::vm() const
{
    return stackManager().vm();
}

// Saves the interpreter SP into the calling thread's own segment (the
// offlineasm cloop emits this before every native/slow-path call), and
// republishes this thread's stack limit if another thread published its own
// in the meantime (GIL handoff), so the asm fast-path stack checks compare
// against the running thread's segment. See the class comment in CLoopStack.h.
ALWAYS_INLINE void CLoopStack::setCurrentStackPointer(void* sp)
{
    ThreadState& state = threadState();
    state.currentStackPointer = sp;
    if (stackManager().cloopStackLimit() != static_cast<void*>(state.end)) [[unlikely]]
        stackManager().setCLoopStackLimit(state.end);
}

inline bool CLoopStack::ensureCapacityFor(Register* newTopOfStack)
{
    ThreadState& state = threadState();
    bool success = true;
    if (newTopOfStack < state.end) [[unlikely]]
        success = grow(state, newTopOfStack);
    // This is the C++ slow path of the asm stack checks: republishing here
    // guarantees a stale published limit self-corrects on the first failing
    // check after a GIL handoff.
    if (stackManager().cloopStackLimit() != static_cast<void*>(state.end)) [[unlikely]]
        stackManager().setCLoopStackLimit(state.end);
    return success;
}

ALWAYS_INLINE StackManager& CLoopStack::stackManager() const
{
    return *std::bit_cast<StackManager*>(std::bit_cast<uintptr_t>(this) - StackManager::offsetOfCLoopStack());
}

} // namespace JSC

#endif // ENABLE(C_LOOP)
