/*
 * Copyright (C) 2016-2025 Apple Inc. All rights reserved.
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

#include "StackAlignment.h"
#include "TopExceptionScope.h"
#include "VM.h"
#include <wtf/SetForScope.h>

namespace JSC {

struct EntryFrame;
class PropertyInlineCache;

class SuspendExceptionScope {
public:
    // UNGIL §A.1.3 mode split (obligation-10 audit): suspend/restore the
    // CURRENT thread's exception words — GIL-off the raw VM-block members
    // are inert spare storage and the live words are the lite's
    // (group3Primitives()); GIL-on this aliases the VM block, bit-identical.
    // The SetForScope write-back requires ctor and dtor to resolve the same
    // storage: this scope stays on one thread inside a stable (thread, lite)
    // window, like every other exception scope.
    SuspendExceptionScope(VM& vm)
        : m_vm(vm)
        , m_exceptionWasSet(vm.group3Primitives().m_exception)
        , m_savedException(vm.group3Primitives().m_exception, nullptr)
        , m_savedLastException(vm.group3Primitives().m_lastException, nullptr)
    {
        if (m_exceptionWasSet)
            m_vm.trapsForCurrentThread().clearTrap(VMTraps::NeedExceptionHandling); // Same storage domain as the words above.
    }
    ~SuspendExceptionScope()
    {
        if (m_exceptionWasSet)
            m_vm.trapsForCurrentThread().fireTrap(VMTraps::NeedExceptionHandling);
    }
private:
    VM& m_vm;
    bool m_exceptionWasSet;
    SetForScope<Exception*> m_savedException;
    SetForScope<Exception*> m_savedLastException;
};

class TopCallFrameSetter {
public:
    TopCallFrameSetter(VM& currentVM, CallFrame* callFrame)
        : vm(currentVM)
        , oldCallFrame(currentVM.group3Primitives().topCallFrame) // UNGIL §A.1.3 mode split.
    {
        currentVM.group3Primitives().topCallFrame = callFrame;
#if ASSERT_ENABLED
        m_primitivesAtConstruction = &currentVM.group3Primitives();
#endif
    }

    ~TopCallFrameSetter()
    {
        // Rematerialized per §A.1.2; same thread+VM => same lite as ctor.
        // Assert that holds so a §F.5 foreign-lite window restoring into
        // different storage than the ctor wrote is loud, not silent.
        ASSERT(&vm.group3Primitives() == m_primitivesAtConstruction);
        vm.group3Primitives().topCallFrame = oldCallFrame;
    }
private:
    VM& vm;
    CallFrame* oldCallFrame;
#if ASSERT_ENABLED
    VMLitePrimitives* m_primitivesAtConstruction;
#endif
};

ALWAYS_INLINE static void assertStackPointerIsAligned()
{
#ifndef NDEBUG
#if CPU(X86) && !OS(WINDOWS)
    uintptr_t stackPointer;

    __asm__("movl %%esp,%0" : "=r"(stackPointer));
    ASSERT(!(stackPointer % stackAlignmentBytes()));
#endif
#endif
}

class SlowPathFrameTracer {
public:
    ALWAYS_INLINE SlowPathFrameTracer(VM& vm, CallFrame* callFrame)
    {
        ASSERT(callFrame);
        // UNGIL §A.1.3 (U-T1): GIL-off, doVMEntry publishes topEntryFrame through
        // the current thread's VMLitePrimitives (LowLevelInterpreter.asm gilOff
        // branch); the VM-block word is inert spare storage. GIL-on this selects
        // the VM block — bit-identical to the old direct access.
        VMLitePrimitives& primitives = vm.group3Primitives();
        ASSERT(reinterpret_cast<void*>(callFrame) < reinterpret_cast<void*>(primitives.topEntryFrame));
        assertStackPointerIsAligned();
        primitives.topCallFrame = callFrame;
    }
};

// This class should be used instead of SlowPathFrameTracer *only* in contexts where the sole
// reason the topCallFrame is accessed is to update ShadowChicken. In other words, in contexts
// where a JS exception cannot be thrown.
class WasmSlowPathWithoutCallFrameTracer {
public:
    ALWAYS_INLINE WasmSlowPathWithoutCallFrameTracer(VM& vm)
    {
        // Wasm frames don't participate in ShadowChicken.
        if (vm.shadowChicken()) [[unlikely]]
            vm.group3Primitives().topCallFrame = nullptr; // UNGIL §A.1.3 mode split.
    }
};

class NativeCallFrameTracer {
public:
    ALWAYS_INLINE NativeCallFrameTracer(VM& vm, CallFrame* callFrame)
    {
        ASSERT(callFrame);
        VMLitePrimitives& primitives = vm.group3Primitives(); // UNGIL §A.1.3 mode split.
        ASSERT(reinterpret_cast<void*>(callFrame) < reinterpret_cast<void*>(primitives.topEntryFrame));
        assertStackPointerIsAligned();
        primitives.topCallFrame = callFrame;
    }
};

class WasmOperationPrologueCallFrameTracer {
public:
    ALWAYS_INLINE WasmOperationPrologueCallFrameTracer(VM& vm, CallFrame* callFrame, void* returnPC)
    {
        ASSERT(callFrame);
        VMLitePrimitives& primitives = vm.group3Primitives(); // UNGIL §A.1.3 mode split.
        ASSERT(reinterpret_cast<void*>(callFrame) < reinterpret_cast<void*>(primitives.topEntryFrame));
        assertStackPointerIsAligned();
        primitives.topCallFrame = callFrame;
        // maybeReturnPC is a SPEC-vmstate §6.3 relocated member (VM.h:560),
        // deliberately NOT in VMLitePrimitives — stays a direct VM access.
        vm.maybeReturnPC = returnPC;
    }
};

class JITOperationPrologueCallFrameTracer {
public:
    ALWAYS_INLINE JITOperationPrologueCallFrameTracer(VM& vm, CallFrame* callFrame)
#if ASSERT_ENABLED
        : m_vm(vm)
#endif
    {
        UNUSED_PARAM(vm);
        UNUSED_PARAM(callFrame);
        ASSERT(callFrame);
        ASSERT(reinterpret_cast<void*>(callFrame) < reinterpret_cast<void*>(vm.group3Primitives().topEntryFrame));
        assertStackPointerIsAligned();
#if USE(BUILTIN_FRAME_ADDRESS)
        // If ASSERT_ENABLED and USE(BUILTIN_FRAME_ADDRESS), prepareCallOperation() will put the frame pointer into vm.topCallFrame.
        // We can ensure here that a call to prepareCallOperation() (or its equivalent) is not missing by comparing vm.topCallFrame to
        // the result of __builtin_frame_address which is passed in as callFrame.
        // UNGIL §A.1.3: compare against the mode-selected storage. NOTE: this
        // assert will (correctly) fire under gilOff JIT until the emission side
        // of prepareCallOperation (AssemblyHelpers.h:82, raw &vm.topCallFrame
        // store) is converted per U-T4 — that is the next unconverted publisher,
        // and masking it by reading the VM block here would re-create the
        // split-brain this change removes.
        ASSERT(vm.group3Primitives().topCallFrame == callFrame);
        vm.group3Primitives().topCallFrame = callFrame;
#endif
    }

#if ASSERT_ENABLED
    ~JITOperationPrologueCallFrameTracer()
    {
        // Fill vm.topCallFrame with invalid value when leaving from JIT operation functions.
        m_vm.group3Primitives().topCallFrame = std::bit_cast<CallFrame*>(static_cast<uintptr_t>(0x0badbeef0badbeefULL)); // UNGIL §A.1.3: poison the storage LLInt/JIT actually read.
    }

    VM& m_vm;
#endif
};

class ICSlowPathCallFrameTracer {
public:
    inline ICSlowPathCallFrameTracer(VM&, CallFrame*, PropertyInlineCache*);

#if ASSERT_ENABLED
    ~ICSlowPathCallFrameTracer()
    {
        // Fill vm.topCallFrame with invalid value when leaving from JIT operation functions.
        m_vm.group3Primitives().topCallFrame = std::bit_cast<CallFrame*>(static_cast<uintptr_t>(0x0badbeef0badbeefULL)); // UNGIL §A.1.3 mode split.
    }

    VM& m_vm;
#endif
};


} // namespace JSC
