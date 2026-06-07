/*
 * Copyright (C) 2012-2018 Apple Inc. All rights reserved.
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

#include "BaselineJITRegisters.h"
#include "CallFrame.h"
#include "CallFrameShuffleData.h"
#include "CallLinkInfoBase.h"
#include "CallMode.h"
#include "CodeLocation.h"
#include "CodeOrigin.h"
#include "CodeSpecializationKind.h"
#include "Options.h"
#include "PolymorphicCallStubRoutine.h"
#include "WriteBarrier.h"
#include <wtf/Lock.h>
#include <wtf/RecursiveLockAdapter.h>
#include <wtf/ScopedLambda.h>

namespace JSC {
namespace DFG {
struct UnlinkedCallLinkInfo;
}

class CCallHelpers;
class ExecutableBase;
class FunctionCodeBlock;
class JSFunction;
class OptimizingCallLinkInfo;
class PolymorphicCallStubRoutine;
enum OpcodeID : unsigned;

struct CallFrameShuffleData;
struct UnlinkedCallLinkInfo;
struct BaselineUnlinkedCallLinkInfo;

using CompileTimeCallLinkInfo = Variant<OptimizingCallLinkInfo*, BaselineUnlinkedCallLinkInfo*, DFG::UnlinkedCallLinkInfo*>;

// SPEC-jit section 5.8 (Task 7): the single published call-link record.
//
// Guard/payload word-pair protocols are unsound under N mutators (a racing
// reader can pair a new guard with an old target), so with shared-memory
// threads enabled (Options::useJSThreads()) every JIT'd call fast path flows
// through ONE published pointer to an immutable record:
//
//   load r = m_record; if (!r) use the empty record (default call);
//   load c = r->comparand;
//   if (c == calleeGPR || (c & polymorphicCalleeMask)) {
//       store r->codeBlockToTransfer -> callee frame;
//       load t = r->target ONCE; call t;
//   } else use the empty record (default call);
//
// c == callee cell => monomorphic; c with bit 0 set (polymorphicCalleeMask)
// => always-call (virtual/polymorphic-stub dispatch, today's bit-test, G10);
// direct calls skip the comparand check entirely. All reads go THROUGH r so
// ARM64 readers are ordered by the address dependency (F2); a stale read
// observes a complete OLD record, which is benign.
//
// Records are immutable after publish (F6: fully initialize, then
// WTF::storeStoreFence(), then a single m_record pointer store), heap-allocated
// at link time, and freed only via RetiredJITArtifacts (SPEC-jit section 4.4)
// once every mutator has crossed a safepoint, except when the owning
// CallLinkInfo itself is destroyed (its code is already unreachable by then).
//
// GC: comparand is a RAW word - never dereferenced, never visited, no
// WriteBarrier. The legacy mirror fields (m_callee/m_codeBlock/
// m_monomorphicCallDestination; Direct: m_target/m_codeBlock) remain the sole
// GC roots/weak references and stay in sync with the record under the existing
// locks; visitWeak/unlinkOrUpgrade read the mirrors as today and additionally
// null or republish m_record on clear/relink.
struct CallLinkRecord {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(CallLinkRecord);

    uintptr_t comparand { 0 }; // Callee cell, or sentinel: bit 0 (CallLinkInfo::polymorphicCalleeMask) = always-call.
    CodePtr<JSEntryPtrTag> target { }; // Entrypoint (monomorphic/virtual/stub/direct).
    CodeBlock* codeBlockToTransfer { nullptr }; // Stored to the callee frame by the fast path.

    static constexpr ptrdiff_t offsetOfComparand() { return OBJECT_OFFSETOF(CallLinkRecord, comparand); }
    static constexpr ptrdiff_t offsetOfTarget() { return OBJECT_OFFSETOF(CallLinkRecord, target); }
    static constexpr ptrdiff_t offsetOfCodeBlockToTransfer() { return OBJECT_OFFSETOF(CallLinkRecord, codeBlockToTransfer); }
};

#if CPU(ADDRESS64)
static_assert(CallLinkRecord::offsetOfComparand() == 0);
static_assert(CallLinkRecord::offsetOfTarget() == 8);
static_assert(CallLinkRecord::offsetOfCodeBlockToTransfer() == 16);
static_assert(sizeof(CallLinkRecord) == 24);
#endif

class CallLinkInfo : public CallLinkInfoBase {
public:
    friend class LLIntOffsetsExtractor;

    static constexpr uint8_t maxProfiledArgumentCountIncludingThisForVarargs = UINT8_MAX;

    enum class Type : uint8_t {
        DataOnly,
        Optimizing,
    };

    enum class Mode : uint8_t {
        Init,
        Monomorphic,
        Polymorphic,
        Virtual,
    };

    static constexpr uintptr_t polymorphicCalleeMask = 1;

    // AB18-D / GIL-removal precondition 11 (docs/threads/INTEGRATE-jit.md):
    // gilOff, ALL slow-path call-link transition writers — and every
    // m_incomingCalls push/remove they perform — serialize on this single
    // process-wide lock: linkMonomorphicCall / linkPolymorphicCall /
    // linkDirectCall (bytecode/Repatch.cpp) and unlinkOrUpgradeImpl (both the
    // CallLinkInfo and DirectCallLinkInfo flavors, which covers the upgrade
    // relink push and the per-node work of the non-STW
    // ScriptExecutable::installCode drain). ONE lock, not per-CodeBlock pairs:
    // call linking is a rare slow path, and a single lock is deadlock-free by
    // construction (holders never reach a safepoint poll). AB17c F4: now a
    // RECURSIVE lock — destruction-context removers (~CallLinkInfoBase on a
    // lazy-sweep mutator, reachable from allocation inside a LOCKED linker,
    // e.g. linkPolymorphicCallImpl's stub allocation sweeping a dying
    // CodeBlock) must also serialize on it, and the recursive acquire is the
    // only deadlock-free way to admit them. The takeFrom HEAD-rewrite
    // residual was closed by the AB18-C locker in
    // CodeBlock::unlinkOrUpgradeIncomingCalls. Static member: no layout
    // change.
    static RecursiveLock s_callLinkSerializationLock;


    static CallType NODELETE callTypeFor(OpcodeID opcodeID);

    static bool isVarargsCallType(CallType callType)
    {
        switch (callType) {
        case CallVarargs:
        case ConstructVarargs:
        case TailCallVarargs:
            return true;

        default:
            return false;
        }
    }

    ~CallLinkInfo();
    
    static CodeSpecializationKind specializationKindFor(CallType callType)
    {
        return specializationFromIsConstruct(callType == Construct || callType == ConstructVarargs || callType == DirectConstruct);
    }
    CodeSpecializationKind specializationKind() const
    {
        return specializationKindFor(static_cast<CallType>(m_callType));
    }

    CallMode callMode() const
    {
        return callModeFor(static_cast<CallType>(m_callType));
    }

    bool isTailCall() const
    {
        return callMode() == CallMode::Tail;
    }
    
    NearCallMode nearCallMode() const
    {
        return isTailCall() ? NearCallMode::Tail : NearCallMode::Regular;
    }

    bool isVarargs() const
    {
        return isVarargsCallType(static_cast<CallType>(m_callType));
    }

    bool isLinked() const { return mode() != Mode::Init && mode() != Mode::Virtual; }
    void unlinkOrUpgradeImpl(VM&, CodeBlock* oldCodeBlock, CodeBlock* newCodeBlock);

#if ENABLE(JIT)
protected:
    static void emitFastPathImpl(CallLinkInfo*, CCallHelpers&, bool isTailCall, ScopedLambda<void()>&& prepareForTailCall);
public:
    static void emitDataICFastPath(CCallHelpers&);
    static void emitTailCallDataICFastPath(CCallHelpers&, ScopedLambda<void()>&& prepareForTailCall);

    static void emitFastPath(CCallHelpers&, CompileTimeCallLinkInfo);
    static void emitTailCallFastPath(CCallHelpers&, CompileTimeCallLinkInfo, ScopedLambda<void()>&& prepareForTailCall);
#endif

    void NODELETE revertCallToStub();

    void setMonomorphicCallee(VM&, JSCell*, JSObject* callee, CodeBlock*, CodePtr<JSEntryPtrTag>);
    void NODELETE clearCallee();
    JSObject* NODELETE callee();

    void setLastSeenCallee(VM&, const JSCell* owner, JSObject* callee);
    JSObject* NODELETE lastSeenCallee() const;
    bool NODELETE haveLastSeenCallee() const;
    
    void setExecutableDuringCompilation(ExecutableBase*);
    ExecutableBase* executable();
    
    void setStub(VM&, Ref<PolymorphicCallStubRoutine>&&);
    void clearStub();

    void setVirtualCall(VM&);

    void revertCall(VM&);

    PolymorphicCallStubRoutine* stub() const
    {
        // SPEC-jit section 5.8 (Task 7): flag-on, a non-Polymorphic
        // CallLinkInfo may still retain a displaced routine in m_stub purely
        // to keep the pointer published for racing JIT'd thunk readers (see
        // clearStub()); logically there is no stub then.
        if (Options::useJSThreads() && mode() != Mode::Polymorphic) [[unlikely]]
            return nullptr;
        return m_stub.get();
    }

    bool seenOnce()
    {
        return m_hasSeenShouldRepatch;
    }

    void clearSeen()
    {
        m_hasSeenShouldRepatch = false;
    }

    void setSeen()
    {
        m_hasSeenShouldRepatch = true;
    }

    bool hasSeenClosure()
    {
        return m_hasSeenClosure;
    }

    void setHasSeenClosure()
    {
        m_hasSeenClosure = true;
    }

    bool clearedByGC()
    {
        return m_clearedByGC;
    }
    
    bool clearedByVirtual()
    {
        return m_clearedByVirtual;
    }

    void setClearedByVirtual()
    {
        m_clearedByVirtual = true;
    }
    
    void setCallType(CallType callType)
    {
        m_callType = callType;
    }

    CallType callType()
    {
        return static_cast<CallType>(m_callType);
    }

    static constexpr ptrdiff_t offsetOfMaxArgumentCountIncludingThisForVarargs()
    {
        return OBJECT_OFFSETOF(CallLinkInfo, m_maxArgumentCountIncludingThisForVarargs);
    }

    uint32_t maxArgumentCountIncludingThisForVarargs()
    {
        return m_maxArgumentCountIncludingThisForVarargs;
    }
    
    void updateMaxArgumentCountIncludingThisForVarargs(unsigned argumentCountIncludingThisForVarargs)
    {
        if (m_maxArgumentCountIncludingThisForVarargs < argumentCountIncludingThisForVarargs)
            m_maxArgumentCountIncludingThisForVarargs = std::min<unsigned>(argumentCountIncludingThisForVarargs, maxProfiledArgumentCountIncludingThisForVarargs);
    }

    static constexpr ptrdiff_t offsetOfSlowPathCount()
    {
        return OBJECT_OFFSETOF(CallLinkInfo, m_slowPathCount);
    }

    static constexpr ptrdiff_t offsetOfCallee()
    {
        return OBJECT_OFFSETOF(CallLinkInfo, m_callee);
    }

    static constexpr ptrdiff_t offsetOfCodeBlock()
    {
        return OBJECT_OFFSETOF(CallLinkInfo, m_codeBlock);
    }

    static constexpr ptrdiff_t offsetOfMonomorphicCallDestination()
    {
        return OBJECT_OFFSETOF(CallLinkInfo, m_monomorphicCallDestination);
    }

    static constexpr ptrdiff_t offsetOfStub()
    {
        return OBJECT_OFFSETOF(CallLinkInfo, m_stub);
    }

    static constexpr ptrdiff_t offsetOfRecord()
    {
        return OBJECT_OFFSETOF(CallLinkInfo, m_record);
    }

    uint32_t slowPathCount()
    {
        return m_slowPathCount;
    }

    CodeOrigin codeOrigin() const { return m_codeOrigin; }

    template<typename Functor>
    void forEachDependentCell(const Functor& functor) const
    {
        if (isLinked()) {
            if (stub())
                stub()->forEachDependentCell(functor);
            else
                functor(m_callee.get());
        }
        if (haveLastSeenCallee())
            functor(lastSeenCallee());
    }

    void visitWeak(VM&);

    Type type() const { return static_cast<Type>(m_type); }

    Mode mode() const { return static_cast<Mode>(m_mode); }

    JSCell* owner() const { return m_owner; }

    JSCell* ownerForSlowPath(CallFrame* calleeFrame);

    JSGlobalObject* globalObjectForSlowPath(JSCell* owner);

    std::tuple<CodeBlock*, BytecodeIndex> retrieveCaller(JSCell* owner);

protected:
    CallLinkInfo(Type type, JSCell* owner, CodeOrigin codeOrigin)
        : CallLinkInfoBase(CallSiteType::CallLinkInfo)
        , m_type(static_cast<unsigned>(type))
        , m_owner(owner)
        , m_codeOrigin(codeOrigin)
    {
        ASSERT(type == this->type());
    }

    void reset(VM&);

    // SPEC-jit section 5.8 writers (F6): every transition (first link,
    // monomorphic upgrade, setVirtualCall/setStub) publishes a NEW immutable
    // record - init record -> storeStoreFence -> single m_record store - and
    // retires the replaced one via RetiredJITArtifacts (section 4.4). Unlink is
    // a single nullptr store (monotone; legal from a running slow path or
    // under STW). No-ops flag-off (I1: no records exist flag-off).
    void publishRecord(VM&, uintptr_t comparand, CodePtr<JSEntryPtrTag> target, CodeBlock* codeBlockToTransfer);
    void clearRecord(VM&);

    bool m_hasSeenShouldRepatch : 1 { false };
    bool m_hasSeenClosure : 1 { false };
    bool m_clearedByGC : 1 { false };
    bool m_clearedByVirtual : 1 { false };
    unsigned m_callType : 4 { CallType::None }; // CallType
    unsigned m_type : 1; // Type
    unsigned m_mode : 3 { static_cast<unsigned>(Mode::Init) }; // Mode
    uint8_t m_maxArgumentCountIncludingThisForVarargs { 0 }; // For varargs: the profiled maximum number of arguments. For direct: the number of stack slots allocated for arguments.
    uint32_t m_slowPathCount { 0 };

    CodeBlock* m_codeBlock { nullptr }; // This is weakly held. And cleared whenever m_monomorphicCallDestination is changed.
    CodePtr<JSEntryPtrTag> m_monomorphicCallDestination { nullptr };
    WriteBarrier<JSObject> m_callee;
    WriteBarrier<JSObject> m_lastSeenCallee;
    RefPtr<PolymorphicCallStubRoutine> m_stub;
    JSCell* m_owner { nullptr };
    CodeOrigin m_codeOrigin { };
    // SPEC-jit section 5.8 frozen placement: appended as the LAST member of the
    // CallLinkInfo data (one shared offset for the data-IC fast path emitted by
    // emitFastPathImpl, serving both DataOnlyCallLinkInfo - LLInt/Baseline
    // bytecode metadata, +8B per call op, unconditional per D7 - and
    // OptimizingCallLinkInfo). Null = unlinked. Flag-off this stays null
    // forever and no emitted sequence reads it (I1).
    CallLinkRecord* m_record { nullptr };
};

class DataOnlyCallLinkInfo final : public CallLinkInfo {
public:
    DataOnlyCallLinkInfo()
        : CallLinkInfo(Type::DataOnly, nullptr, CodeOrigin { })
    {
    }

    void initialize(VM&, CodeBlock*, CallType, CodeOrigin);
};

struct UnlinkedCallLinkInfo { };

struct BaselineUnlinkedCallLinkInfo : public JSC::UnlinkedCallLinkInfo {
    BytecodeIndex bytecodeIndex; // Currently, only used by baseline, so this can trivially produce a CodeOrigin.
    CodeLocationLabel<JSInternalPtrTag> doneLocation;

#if ENABLE(JIT)
    void setUpCall(CallLinkInfo::CallType) { }
#endif
};

#if ENABLE(JIT)

class DirectCallLinkInfo final : public CallLinkInfoBase {
    WTF_MAKE_NONCOPYABLE(DirectCallLinkInfo);
public:
    DirectCallLinkInfo(CodeOrigin codeOrigin, UseDataIC useDataIC, JSCell* owner, ExecutableBase* executable)
        : CallLinkInfoBase(CallSiteType::DirectCall)
        , m_useDataIC(useDataIC)
        , m_codeOrigin(codeOrigin)
        , m_owner(owner)
        , m_executable(executable)
    {
        // SPEC-jit I3/section 5.8: with shared-memory threads enabled, direct
        // calls must use data ICs - UseDataIC::No fast paths patch machine code
        // in place (repatchNearCall/replaceWithJump), which is forbidden under
        // concurrent execution (I2).
        RELEASE_ASSERT(!Options::useJSThreads() || useDataIC == UseDataIC::Yes);
    }

    ~DirectCallLinkInfo()
    {
        m_target = { };
        m_codeBlock = nullptr;
        // SPEC-jit section 5.8: a DirectCallLinkInfo is destroyed only once its
        // owning code is unreachable (post-R2 conservative scan / CodeBlock
        // sweep), so no JIT'd frame can still hold the record pointer (I16);
        // inline delete is sound here, unlike replacement/unlink which must go
        // through RetiredJITArtifacts.
        delete m_record;
        m_record = nullptr;
    }

    void setCallType(CallType callType)
    {
        m_callType = callType;
    }

    CallType callType()
    {
        return static_cast<CallType>(m_callType);
    }

    CallMode callMode() const
    {
        return callModeFor(static_cast<CallType>(m_callType));
    }

    bool isTailCall() const
    {
        return callMode() == CallMode::Tail;
    }

    CodeSpecializationKind specializationKind() const
    {
        auto callType = static_cast<CallType>(m_callType);
        return specializationFromIsConstruct(callType == DirectConstruct);
    }

    void setSlowPathStart(CodeLocationLabel<JSInternalPtrTag> slowPathStart)
    {
        m_slowPathStart = slowPathStart;
    }

    static constexpr ptrdiff_t offsetOfTarget() { return OBJECT_OFFSETOF(DirectCallLinkInfo, m_target); };
    static constexpr ptrdiff_t offsetOfCodeBlock() { return OBJECT_OFFSETOF(DirectCallLinkInfo, m_codeBlock); };
    static constexpr ptrdiff_t offsetOfRecord() { return OBJECT_OFFSETOF(DirectCallLinkInfo, m_record); };

    JSCell* owner() const { return m_owner; }

    void unlinkOrUpgradeImpl(VM&, CodeBlock* oldCodeBlock, CodeBlock* newCodeBlock);

    void visitWeak(VM&);

    CodeOrigin codeOrigin() const { return m_codeOrigin; }
    bool isDataIC() const { return m_useDataIC == UseDataIC::Yes; }

    MacroAssembler::JumpList emitDirectFastPath(CCallHelpers&);
    MacroAssembler::JumpList emitDirectTailCallFastPath(CCallHelpers&, ScopedLambda<void()>&& prepareForTailCall);
    void setCallTarget(CodeBlock*, CodeLocationLabel<JSEntryPtrTag>);
    void NODELETE setMaxArgumentCountIncludingThis(unsigned);
    unsigned maxArgumentCountIncludingThis() const { return m_maxArgumentCountIncludingThis; }

    void reset();

    void validateSpeculativeRepatchOnMainThread(VM&);

private:
    CodeLocationLabel<JSInternalPtrTag> slowPathStart() const { return m_slowPathStart; }
    CodeLocationLabel<JSInternalPtrTag> fastPathStart() const { return m_fastPathStart; }

    void initialize();
    void repatchSpeculatively();

    // SPEC-jit section 5.8 (direct calls are data-IC-only flag-on, I3): record
    // publish/unlink mirroring CallLinkInfo::publishRecord/clearRecord. Direct
    // records carry no comparand (the fast path skips the comparand check).
    // No-ops flag-off.
    void publishRecord(CodePtr<JSEntryPtrTag> target, CodeBlock* codeBlockToTransfer);
    void clearRecord();
    void retireRecord(CallLinkRecord*);

    CodeBlock* NODELETE retrieveCodeBlock(FunctionExecutable*);
    CodePtr<JSEntryPtrTag> retrieveCodePtr(const ConcurrentJSLocker&, CodeBlock*);

    CallType m_callType : 4;
    UseDataIC m_useDataIC : 1;
    unsigned m_maxArgumentCountIncludingThis { 0 };
    CodePtr<JSEntryPtrTag> m_target;
    CodeBlock* m_codeBlock { nullptr }; // This is weakly held. And cleared whenever m_target is changed.
    CodeOrigin m_codeOrigin { };
    CodeLocationLabel<JSInternalPtrTag> m_slowPathStart;
    CodeLocationLabel<JSInternalPtrTag> m_fastPathStart;
    CodeLocationDataLabelPtr<JSInternalPtrTag> m_codeBlockLocation;
    CodeLocationNearCall<JSInternalPtrTag> m_callLocation NO_UNIQUE_ADDRESS;
    JSCell* m_owner;
    ExecutableBase* m_executable { nullptr }; // This is weakly held. DFG / FTL CommonData already ensures this.
    // SPEC-jit section 5.8 frozen placement: LAST member. Null = unlinked;
    // flag-off this stays null forever (I1).
    CallLinkRecord* m_record { nullptr };
};

class OptimizingCallLinkInfo final : public CallLinkInfo {
public:
    friend class CallLinkInfo;

    OptimizingCallLinkInfo()
        : CallLinkInfo(Type::Optimizing, nullptr, CodeOrigin { })
    {
    }

    OptimizingCallLinkInfo(CodeOrigin codeOrigin, JSCell* owner)
        : CallLinkInfo(Type::Optimizing, owner, codeOrigin)
    {
    }

    void setUpCall(CallType callType)
    {
        m_callType = callType;
    }

    void NODELETE initializeFromDFGUnlinkedCallLinkInfo(VM&, const DFG::UnlinkedCallLinkInfo&, CodeBlock*);

private:
    void emitFastPath(CCallHelpers&);
    void emitTailCallFastPath(CCallHelpers&, ScopedLambda<void()>&& prepareForTailCall);

    CodeLocationNearCall<JSInternalPtrTag> m_callLocation NO_UNIQUE_ADDRESS;
};

#endif

inline JSCell* CallLinkInfo::ownerForSlowPath(CallFrame* calleeFrame)
{
    if (m_owner)
        return m_owner;

    // Right now, IC (Getter, Setter, Proxy IC etc.) / WasmToJS sets nullptr intentionally since we would like to share IC / WasmToJS thunk eventually.
    // However, in that case, each IC's data side will have CallLinkInfo.
    // At that time, they should have appropriate owner. So this is a hack only for now.
    // This should always works since IC only performs regular-calls and it never does tail-calls.
    return calleeFrame->callerFrame()->codeOwnerCell();
}

} // namespace JSC
