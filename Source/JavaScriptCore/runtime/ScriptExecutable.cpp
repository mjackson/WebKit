/*
 * Copyright (C) 2009-2022 Apple Inc. All rights reserved.
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

#include "CodeBlock.h"
#include "Debugger.h"
#include "EvalCodeBlock.h"
#include "FunctionCodeBlock.h"
#include "FunctionExecutableInlines.h"
#include "GlobalExecutable.h"
#include "IsoCellSetInlines.h"
#include "JIT.h"
#include "JSCellInlines.h"
#include "JSGlobalObjectInlines.h"
#include "JSObjectInlines.h"
#include "JSTemplateObjectDescriptor.h"
#include "LLIntEntrypoint.h"
#include "ModuleProgramCodeBlock.h"
#include "ParserError.h"
#include "ProgramCodeBlock.h"
#include "VMInlines.h"
#include "VMTraps.h"
#include <wtf/Atomics.h>
#include <wtf/RecursiveLockAdapter.h>
#include <wtf/Threading.h>

namespace JSC {

const ClassInfo ScriptExecutable::s_info = { "ScriptExecutable"_s, &ExecutableBase::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(ScriptExecutable) };

// UNGIL IT-8 (concurrent compilation/replacement): GIL-off lites of one VM
// share Executables, UnlinkedCodeBlocks and installed CodeBlocks, but
// CodeBlock creation/installation and tier-up finalization were written for a
// single mutator. Bring-up-grade serialization: one process-wide recursive
// lock, taken only under vm.gilOffWithProcessGate() (flag-off cost is one
// predicted-untaken branch at compilation-rate sites — not transition-rate, so
// bench-gate rung (iii) is unaffected either way). Recursive because
// prepareForExecutionImpl holds it across its installCode call and
// DFG::Plan::finalize holds it across the callback's installCode. Per the
// VM.h sticky-shared-server comment there is at most one gilOff VM per
// process, so the process-wide lock couples no GIL-on workers. Replace with
// per-Executable striping / install-CAS once GIL-off is accepted.
//
// Declared locally (not in a header) by bytecompiler/BytecodeGenerator.cpp and
// dfg/DFGPlan.cpp; keep the three declarations in sync.
RecursiveLock& gilOffCompilationLock()
{
    static RecursiveLock lock;
    return lock;
}

namespace {

// Stop-protocol-safe acquisition (W1/D9 convention, AB-17 status block in
// VMEntryScope.cpp): a lite blocked in a raw lock() is invisible to the
// GIL-off per-lite stop fan. If the lock holder parks at a safepoint inside
// the locked region while we block raw, the collector waits on us forever and
// the holder never resumes — deadlock. So contended acquisition spins on
// tryLock() and services ONLY NeedStopTheWorld between attempts: that parks
// us for the stop cycle (keeping the stop fan live) and can neither throw nor
// run JS, so it is safe at every call site including jettison-driven
// installCode, where throwing is not allowed.
class GILOffCompilationLocker {
    WTF_MAKE_NONCOPYABLE(GILOffCompilationLocker);
public:
    GILOffCompilationLocker(VM& vm, bool shouldLock)
        : m_shouldLock(shouldLock)
    {
        if (!m_shouldLock) [[likely]]
            return;
        RecursiveLock& lock = gilOffCompilationLock();
        if (lock.tryLock()) [[likely]]
            return;
        while (!lock.tryLock()) {
            handleTrapsForCurrentThreadIfNeeded(vm, VMTraps::NeedStopTheWorld);
            Thread::yield();
        }
    }

    ~GILOffCompilationLocker()
    {
        if (m_shouldLock) [[unlikely]]
            gilOffCompilationLock().unlock();
    }

private:
    bool m_shouldLock;
};

} // anonymous namespace

ScriptExecutable::ScriptExecutable(Structure* structure, VM& vm, const SourceCode& source, LexicallyScopedFeatures lexicallyScopedFeatures, DerivedContextType derivedContextType, bool isInArrowFunctionContext, bool isInsideOrdinaryFunction, EvalContextType evalContextType, Intrinsic intrinsic)
    : ExecutableBase(vm, structure)
    , m_source(source)
    , m_intrinsic(intrinsic)
    , m_features(NoFeatures)
    , m_lexicallyScopedFeatures(lexicallyScopedFeatures)
    , m_hasCapturedVariables(false)
    , m_neverInline(false)
    , m_neverOptimize(false)
    , m_neverFTLOptimize(false)
    , m_isArrowFunctionContext(isInArrowFunctionContext)
    , m_canUseOSRExitFuzzing(true)
    , m_codeForGeneratorBodyWasGenerated(false)
    , m_isInsideOrdinaryFunction(isInsideOrdinaryFunction)
    , m_derivedContextType(static_cast<unsigned>(derivedContextType))
    , m_evalContextType(static_cast<unsigned>(evalContextType))
{
}

void ScriptExecutable::destroy(JSCell* cell)
{
    static_cast<ScriptExecutable*>(cell)->ScriptExecutable::~ScriptExecutable();
}

void ScriptExecutable::clearCode(IsoCellSet& clearableCodeSet)
{
    m_jitCodeForCall = nullptr;
    m_jitCodeForConstruct = nullptr;
    m_jitCodeForCallWithArityCheck = CodePtr<JSEntryPtrTag>();
    m_jitCodeForConstructWithArityCheck = CodePtr<JSEntryPtrTag>();

    switch (type()) {
    case FunctionExecutableType: {
        FunctionExecutable* executable = static_cast<FunctionExecutable*>(this);
        executable->m_codeBlockForCall.clear();
        executable->m_codeBlockForConstruct.clear();
        break;
    }
    case EvalExecutableType: {
        EvalExecutable* executable = static_cast<EvalExecutable*>(this);
        executable->m_codeBlock.clear();
        executable->m_unlinkedCodeBlock.clear();
        break;
    }
    case ProgramExecutableType: {
        ProgramExecutable* executable = static_cast<ProgramExecutable*>(this);
        executable->m_codeBlock.clear();
        executable->m_unlinkedCodeBlock.clear();
        break;
    }
    case ModuleProgramExecutableType: {
        ModuleProgramExecutable* executable = static_cast<ModuleProgramExecutable*>(this);
        executable->m_codeBlock.clear();
        executable->m_unlinkedCodeBlock.clear();
        executable->m_moduleEnvironmentSymbolTable.clear();
        break;
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }

    ASSERT(&Heap::ScriptExecutableSpaceAndSets::clearableCodeSetFor(*subspace()) == &clearableCodeSet);
    clearableCodeSet.remove(this);
}

void ScriptExecutable::installCode(CodeBlock* codeBlock)
{
    installCode(codeBlock->vm(), codeBlock, codeBlock->codeType(), codeBlock->specializationKind(), Profiler::JettisonReason::NotJettisoned);
}

void ScriptExecutable::installCode(VM& vm, CodeBlock* genericCodeBlock, CodeType codeType, CodeSpecializationKind kind, Profiler::JettisonReason reason)
{
    // UNGIL IT-8 (R1a/R1c): installs arrive from any lite (mutator-side
    // jettison, worklist drain via DFG::Plan::finalize's callback,
    // prepareForExecutionImpl); the m_codeBlock* / m_jitCodeFor* store
    // sequence must not interleave with a sibling's install, nor with a
    // sibling reading these fields under the same lock in
    // prepareForExecutionImpl. Deliberately NOT locked:
    //  - JettisonDueToWeakReference / JettisonDueToOldAge: GC-end installs
    //    (Heap finalizeUnconditionalFinalizers -> CodeBlock::
    //    finalizeUnconditionally -> jettison) run in collector context with
    //    mutators suspended — already race-free, and acquiring here while a
    //    suspended mutator holds the lock would deadlock the collection.
    //  - any world-stopped context, same argument.
    // Recursive lock: the nested acquisition from prepareForExecutionImpl /
    // Plan::finalize is a cheap re-acquire.
    bool isGCDrivenInstall = reason == Profiler::JettisonReason::JettisonDueToWeakReference
        || reason == Profiler::JettisonReason::JettisonDueToOldAge;
    GILOffCompilationLocker compilationLocker(vm, vm.gilOffWithProcessGate() && !isGCDrivenInstall && !vm.heap.worldIsStopped());

    if (genericCodeBlock) {
        CODEBLOCK_LOG_EVENT(genericCodeBlock, "installCode", ());
        switch (reason) {
        case Profiler::JettisonReason::JettisonDueToWeakReference:
        case Profiler::JettisonReason::JettisonDueToOldAge: {
            if (genericCodeBlock && !vm.heap.isMarked(genericCodeBlock))
                genericCodeBlock = nullptr;
            break;
        }
        default:
            break;
        }
    }

    // UNGIL IT-8 (R1c, clear direction): the lock-free fast gate
    // (ScriptExecutable::prepareForExecution in CodeBlock.h: hasJITCodeFor
    // then codeBlockFor, plain loads) and call-link reads of m_jitCodeFor*
    // never take the compilation lock, so field-store ORDER is the only
    // writer-side guarantee we can give them. On clear, retract the gating
    // jit-code pointer FIRST (before replaceCodeBlockWith clears the
    // CodeBlock pointer below), so a sibling that still observes a non-null
    // jit-code pointer also still observes the matching CodeBlock. The
    // switch(kind) further down stores nullptr again — harmless.
    if (!genericCodeBlock && vm.gilOffWithProcessGate()) [[unlikely]] {
        switch (kind) {
        case CodeSpecializationKind::CodeForCall:
            m_jitCodeForCall = nullptr;
            m_jitCodeForCallWithArityCheck = nullptr;
            break;
        case CodeSpecializationKind::CodeForConstruct:
            m_jitCodeForConstruct = nullptr;
            m_jitCodeForConstructWithArityCheck = nullptr;
            break;
        }
        WTF::storeStoreFence();
    }

    CodeBlock* oldCodeBlock = nullptr;

    switch (codeType) {
    case GlobalCode: {
        ProgramExecutable* executable = uncheckedDowncast<ProgramExecutable>(this);
        ProgramCodeBlock* codeBlock = static_cast<ProgramCodeBlock*>(genericCodeBlock);
        
        ASSERT(kind == CodeSpecializationKind::CodeForCall);
        
        oldCodeBlock = executable->replaceCodeBlockWith(vm, codeBlock);
        break;
    }

    case ModuleCode: {
        ModuleProgramExecutable* executable = uncheckedDowncast<ModuleProgramExecutable>(this);
        ModuleProgramCodeBlock* codeBlock = static_cast<ModuleProgramCodeBlock*>(genericCodeBlock);

        ASSERT(kind == CodeSpecializationKind::CodeForCall);

        oldCodeBlock = executable->replaceCodeBlockWith(vm, codeBlock);
        break;
    }

    case EvalCode: {
        EvalExecutable* executable = uncheckedDowncast<EvalExecutable>(this);
        EvalCodeBlock* codeBlock = static_cast<EvalCodeBlock*>(genericCodeBlock);
        
        ASSERT(kind == CodeSpecializationKind::CodeForCall);
        
        oldCodeBlock = executable->replaceCodeBlockWith(vm, codeBlock);
        break;
    }
        
    case FunctionCode: {
        FunctionExecutable* executable = uncheckedDowncast<FunctionExecutable>(this);
        FunctionCodeBlock* codeBlock = static_cast<FunctionCodeBlock*>(genericCodeBlock);
        
        oldCodeBlock = executable->replaceCodeBlockWith(vm, kind, codeBlock);
        break;
    }
    }

    // UNGIL IT-8 (R1c, install direction): publish the CodeBlock pointer
    // stores above strictly BEFORE the jit-code pointer that gates the
    // lock-free fast path, so a sibling that observes a non-null jit-code
    // pointer observes the matching CodeBlock. KNOWN RESIDUAL: this is a
    // writer-side fence only; the fast-path reader does two independent plain
    // loads (hasJITCodeFor then codeBlockFor) with no acquire/dependency
    // ordering, so ARM64 load-load reordering can still pair a fresh jit-code
    // observation with a stale CodeBlock read. Closing that requires a
    // CodeBlock.h reader-side change (out of this change's scope) — recorded
    // for the next IT-8 round.
    if (genericCodeBlock && vm.gilOffWithProcessGate()) [[unlikely]]
        WTF::storeStoreFence();

    switch (kind) {
    case CodeSpecializationKind::CodeForCall:
        m_jitCodeForCall = genericCodeBlock ? genericCodeBlock->jitCode() : nullptr;
        m_jitCodeForCallWithArityCheck = nullptr;
        break;
    case CodeSpecializationKind::CodeForConstruct:
        m_jitCodeForConstruct = genericCodeBlock ? genericCodeBlock->jitCode() : nullptr;
        m_jitCodeForConstructWithArityCheck = nullptr;
        break;
    }

    auto& clearableCodeSet = Heap::ScriptExecutableSpaceAndSets::clearableCodeSetFor(*subspace());
    if (hasClearableCode())
        clearableCodeSet.add(this);
    else
        clearableCodeSet.remove(this);

    if (genericCodeBlock) {
        RELEASE_ASSERT(genericCodeBlock->ownerExecutable() == this);
        RELEASE_ASSERT(JITCode::isExecutableScript(genericCodeBlock->jitType()));

        genericCodeBlock->m_isJettisoned = false;
        
        dataLogLnIf(Options::verboseOSR(), "Installing ", *genericCodeBlock);
        
        if (vm.m_perBytecodeProfiler) [[unlikely]]
            vm.m_perBytecodeProfiler->ensureBytecodesFor(genericCodeBlock);
        
        Debugger* debugger = genericCodeBlock->globalObject()->debugger();
        if (debugger) [[unlikely]]
            debugger->registerCodeBlock(genericCodeBlock);
    }

    if (oldCodeBlock)
        oldCodeBlock->unlinkOrUpgradeIncomingCalls(vm, genericCodeBlock);

    vm.writeBarrier(this);
}

bool ScriptExecutable::hasClearableCode() const
{
    if (m_jitCodeForCall
        || m_jitCodeForConstruct
        || m_jitCodeForCallWithArityCheck
        || m_jitCodeForConstructWithArityCheck)
        return true;

    if (structure()->classInfoForCells() == FunctionExecutable::info()) {
        auto* executable = static_cast<const FunctionExecutable*>(this);
        if (executable->eitherCodeBlock())
            return true;

    } else if (structure()->classInfoForCells() == EvalExecutable::info()) {
        auto* executable = static_cast<const EvalExecutable*>(this);
        if (executable->m_codeBlock || executable->m_unlinkedCodeBlock)
            return true;

    } else if (structure()->classInfoForCells() == ProgramExecutable::info()) {
        auto* executable = static_cast<const ProgramExecutable*>(this);
        if (executable->m_codeBlock || executable->m_unlinkedCodeBlock)
            return true;

    } else if (structure()->classInfoForCells() == ModuleProgramExecutable::info()) {
        auto* executable = static_cast<const ModuleProgramExecutable*>(this);
        if (executable->m_codeBlock
            || executable->m_unlinkedCodeBlock
            || executable->m_moduleEnvironmentSymbolTable)
            return true;
    }
    return false;
}

CodeBlock* ScriptExecutable::newCodeBlockFor(CodeSpecializationKind kind, JSFunction* function, JSScope* scope)
{
    VM& vm = scope->vm();
    auto throwScope = DECLARE_THROW_SCOPE(vm);

    ASSERT(vm.heap.isDeferred());
    ASSERT(endColumn() != UINT_MAX);

    JSGlobalObject* globalObject = scope->realm();

    if (classInfo() == EvalExecutable::info()) {
        EvalExecutable* executable = uncheckedDowncast<EvalExecutable>(this);
        RELEASE_ASSERT(kind == CodeSpecializationKind::CodeForCall);
        RELEASE_ASSERT(!executable->m_codeBlock);
        RELEASE_ASSERT(!function);

        // FIXME: There might be a case that executable->unlinkedCodeBlock() will be a nullptr 
        // since ScriptExecutable::clearCode might be triggered due to limited memory usage. 
        // We should regenerate unlinkedCodeBlock if necessary for both EvalExecutable and ProgramExecutable.
        // See similar problem for ModuleProgramExecutable in https://bugs.webkit.org/show_bug.cgi?id=255044.
        RELEASE_AND_RETURN(throwScope, EvalCodeBlock::create(vm, executable, executable->unlinkedCodeBlock(), scope));
    }

    if (classInfo() == ProgramExecutable::info()) {
        ProgramExecutable* executable = uncheckedDowncast<ProgramExecutable>(this);
        RELEASE_ASSERT(kind == CodeSpecializationKind::CodeForCall);
        RELEASE_ASSERT(!executable->m_codeBlock);
        RELEASE_ASSERT(!function);
        RELEASE_AND_RETURN(throwScope, ProgramCodeBlock::create(vm, executable, executable->unlinkedCodeBlock(), scope));
    }

    if (classInfo() == ModuleProgramExecutable::info()) {
        ModuleProgramExecutable* executable = uncheckedDowncast<ModuleProgramExecutable>(this);
        RELEASE_ASSERT(kind == CodeSpecializationKind::CodeForCall);
        RELEASE_ASSERT(!executable->m_codeBlock);
        RELEASE_ASSERT(!function);

        UnlinkedModuleProgramCodeBlock* unlinkedCodeBlock = executable->getUnlinkedCodeBlock(globalObject);
        RETURN_IF_EXCEPTION(throwScope, nullptr);
        ASSERT(executable->unlinkedCodeBlock());
        RELEASE_AND_RETURN(throwScope, ModuleProgramCodeBlock::create(vm, executable, unlinkedCodeBlock, scope));
    }

    RELEASE_ASSERT(classInfo() == FunctionExecutable::info());
    RELEASE_ASSERT(function);
    FunctionExecutable* executable = uncheckedDowncast<FunctionExecutable>(this);
    RELEASE_ASSERT(!executable->codeBlockFor(kind));
    ParserError error;
    OptionSet<CodeGenerationMode> codeGenerationMode = globalObject->defaultCodeGenerationMode();
    // We continue using the same CodeGenerationMode for Generators because live generator objects can
    // keep the state which is only valid with the CodeBlock compiled with the same CodeGenerationMode.
    if (isGeneratorOrAsyncFunctionBodyParseMode(executable->parseMode())) {
        if (!m_codeForGeneratorBodyWasGenerated) {
            m_codeGenerationModeForGeneratorBody = codeGenerationMode;
            m_codeForGeneratorBodyWasGenerated = true;
        } else
            codeGenerationMode = m_codeGenerationModeForGeneratorBody;
    }
    UnlinkedFunctionCodeBlock* unlinkedCodeBlock = 
        executable->m_unlinkedExecutable->unlinkedCodeBlockFor(
            vm, executable->source(), kind, codeGenerationMode, error, 
            executable->parseMode());
    recordParse(
        executable->m_unlinkedExecutable->features(), 
        executable->m_unlinkedExecutable->lexicallyScopedFeatures(),
        executable->m_unlinkedExecutable->hasCapturedVariables(),
        lastLine(), endColumn());
    if (!unlinkedCodeBlock) {
        throwException(globalObject, throwScope, error.toErrorObject(globalObject, executable->source()));
        return nullptr;
    }
    RELEASE_AND_RETURN(throwScope, FunctionCodeBlock::create(vm, executable, unlinkedCodeBlock, scope));
}

CodeBlock* ScriptExecutable::newReplacementCodeBlockFor(
    CodeSpecializationKind kind)
{
    VM& vm = this->vm();
    if (classInfo() == EvalExecutable::info()) {
        RELEASE_ASSERT(kind == CodeSpecializationKind::CodeForCall);
        EvalExecutable* executable = uncheckedDowncast<EvalExecutable>(this);
        EvalCodeBlock* baseline = static_cast<EvalCodeBlock*>(
            executable->codeBlock()->baselineVersion());
        EvalCodeBlock* result = EvalCodeBlock::create(vm,
            CodeBlock::CopyParsedBlock, *baseline);
        result->setAlternative(vm, baseline);
        return result;
    }
    
    if (classInfo() == ProgramExecutable::info()) {
        RELEASE_ASSERT(kind == CodeSpecializationKind::CodeForCall);
        ProgramExecutable* executable = uncheckedDowncast<ProgramExecutable>(this);
        ProgramCodeBlock* baseline = static_cast<ProgramCodeBlock*>(
            executable->codeBlock()->baselineVersion());
        ProgramCodeBlock* result = ProgramCodeBlock::create(vm,
            CodeBlock::CopyParsedBlock, *baseline);
        result->setAlternative(vm, baseline);
        return result;
    }

    if (classInfo() == ModuleProgramExecutable::info()) {
        RELEASE_ASSERT(kind == CodeSpecializationKind::CodeForCall);
        ModuleProgramExecutable* executable = uncheckedDowncast<ModuleProgramExecutable>(this);
        ModuleProgramCodeBlock* baseline = static_cast<ModuleProgramCodeBlock*>(
            executable->codeBlock()->baselineVersion());
        ModuleProgramCodeBlock* result = ModuleProgramCodeBlock::create(vm,
            CodeBlock::CopyParsedBlock, *baseline);
        result->setAlternative(vm, baseline);
        return result;
    }

    RELEASE_ASSERT(classInfo() == FunctionExecutable::info());
    FunctionExecutable* executable = uncheckedDowncast<FunctionExecutable>(this);
    FunctionCodeBlock* baseline = static_cast<FunctionCodeBlock*>(
        executable->codeBlockFor(kind)->baselineVersion());
    FunctionCodeBlock* result = FunctionCodeBlock::create(vm,
        CodeBlock::CopyParsedBlock, *baseline);
    result->setAlternative(vm, baseline);
    return result;
}

static void setupLLInt(CodeBlock* codeBlock)
{
    LLInt::setEntrypoint(codeBlock);
}

static void setupJIT(VM& vm, CodeBlock* codeBlock)
{
#if ENABLE(JIT)
    CompilationResult result = JIT::compileSync(vm, codeBlock, JITCompilationMustSucceed);
    RELEASE_ASSERT(result == CompilationResult::CompilationSuccessful);
#else
    UNUSED_PARAM(vm);
    UNUSED_PARAM(codeBlock);
    UNREACHABLE_FOR_PLATFORM();
#endif
}

void ScriptExecutable::prepareForExecutionImpl(VM& vm, JSFunction* function, JSScope* scope, CodeSpecializationKind kind, CodeBlock*& resultCodeBlock)
{
    auto throwScope = DECLARE_THROW_SCOPE(vm);
    DeferGCForAWhile deferGC(vm);

    if (vm.getAndClearFailNextNewCodeBlock()) [[unlikely]] {
        JSGlobalObject* globalObject = scope->realm();
        throwException(globalObject, throwScope, createError(globalObject, "Forced Failure"_s));
        return;
    }

    // UNGIL IT-8 (R1): the caller's hasJITCodeFor() gate (CodeBlock.h
    // prepareForExecution) runs BEFORE this lock, so a sibling lite may have
    // generated and installed this executable's code while we waited. Adopt
    // the installed CodeBlock instead of re-generating: the re-generation
    // path RELEASE_ASSERTs !codeBlockFor(kind) (newCodeBlockFor) and would
    // clobber a CodeBlock the sibling is already running. Ordering notes:
    //  - lock taken AFTER DeferGCForAWhile, so the locked region cannot
    //    trigger a collection at its own allocation sites;
    //  - the failNextNewCodeBlock test-flag check stays ABOVE the lock and
    //    the adopt path, preserving its flag-off/GIL-on semantics (the flag
    //    is consumed only when this thread would actually create a new
    //    CodeBlock — an adopting thread leaves it armed).
    GILOffCompilationLocker compilationLocker(vm, vm.gilOffWithProcessGate());
    if (vm.gilOffWithProcessGate() && hasJITCodeFor(kind)) [[unlikely]] {
        if (classInfo() == FunctionExecutable::info())
            resultCodeBlock = uncheckedDowncast<FunctionExecutable>(this)->codeBlockFor(kind);
        else if (classInfo() == EvalExecutable::info())
            resultCodeBlock = uncheckedDowncast<EvalExecutable>(this)->codeBlock();
        else if (classInfo() == ProgramExecutable::info())
            resultCodeBlock = uncheckedDowncast<ProgramExecutable>(this)->codeBlock();
        else {
            RELEASE_ASSERT(classInfo() == ModuleProgramExecutable::info());
            resultCodeBlock = uncheckedDowncast<ModuleProgramExecutable>(this)->codeBlock();
        }
        RELEASE_ASSERT(resultCodeBlock);
        return;
    }

    CodeBlock* codeBlock = newCodeBlockFor(kind, function, scope);
    RETURN_IF_EXCEPTION(throwScope, void());

    ASSERT(codeBlock);
    resultCodeBlock = codeBlock;

    if (Options::validateBytecode())
        codeBlock->validate();

    bool installedUnlinkedBaselineCode = false;
#if ENABLE(JIT)
    if (RefPtr<BaselineJITCode> baselineRef = codeBlock->unlinkedCodeBlock()->m_unlinkedBaselineCode) {
        codeBlock->setupWithUnlinkedBaselineCode(baselineRef.releaseNonNull());
        installedUnlinkedBaselineCode = true;
    }
#endif
    if (!installedUnlinkedBaselineCode) {
        if (Options::useLLInt())
            setupLLInt(codeBlock);
        else
            setupJIT(vm, codeBlock);
    }

    installCode(vm, codeBlock, codeBlock->codeType(), codeBlock->specializationKind(), Profiler::JettisonReason::NotJettisoned);
}

ScriptExecutable* ScriptExecutable::topLevelExecutable()
{
    switch (type()) {
    case FunctionExecutableType:
        return uncheckedDowncast<FunctionExecutable>(this)->topLevelExecutable();
    default:
        return this;
    }
}

JSArray* ScriptExecutable::createTemplateObject(JSGlobalObject* globalObject, JSTemplateObjectDescriptor* descriptor)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    TemplateObjectMap& templateObjectMap = ensureTemplateObjectMap(vm);
    TemplateObjectMap::AddResult result;
    {
        Locker locker { cellLock() };
        result = templateObjectMap.add(descriptor->endOffset(), WriteBarrier<JSArray>());
    }
    if (JSArray* array = result.iterator->value.get())
        return array;
    JSArray* templateObject = descriptor->createTemplateObject(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    result.iterator->value.set(vm, this, templateObject);
    return templateObject;
}

auto ScriptExecutable::ensureTemplateObjectMapImpl(std::unique_ptr<TemplateObjectMap>& dest) -> TemplateObjectMap&
{
    if (dest)
        return *dest;
    auto result = makeUnique<TemplateObjectMap>();
    WTF::storeStoreFence();
    dest = WTF::move(result);
    return *dest;
}

auto ScriptExecutable::ensureTemplateObjectMap(VM& vm) -> TemplateObjectMap&
{
    switch (type()) {
    case FunctionExecutableType:
        return static_cast<FunctionExecutable*>(this)->ensureTemplateObjectMap(vm);
    case EvalExecutableType:
        return static_cast<EvalExecutable*>(this)->ensureTemplateObjectMap(vm);
    case ProgramExecutableType:
        return static_cast<ProgramExecutable*>(this)->ensureTemplateObjectMap(vm);
    case ModuleProgramExecutableType:
    default:
        ASSERT(type() == ModuleProgramExecutableType);
        return static_cast<ModuleProgramExecutable*>(this)->ensureTemplateObjectMap(vm);
    }
}

CodeBlockHash ScriptExecutable::hashFor(CodeSpecializationKind kind) const
{
    return CodeBlockHash(source(), kind);
}

std::optional<int> ScriptExecutable::overrideLineNumber(VM&) const
{
    if (inherits<FunctionExecutable>())
        return uncheckedDowncast<FunctionExecutable>(this)->overrideLineNumber();
    return std::nullopt;
}

unsigned ScriptExecutable::typeProfilingStartOffset() const
{
    if (inherits<FunctionExecutable>())
        return uncheckedDowncast<FunctionExecutable>(this)->functionStart();
    if (inherits<EvalExecutable>())
        return UINT_MAX;
    return 0;
}

unsigned ScriptExecutable::typeProfilingEndOffset() const
{
    if (inherits<FunctionExecutable>())
        return uncheckedDowncast<FunctionExecutable>(this)->functionEnd();
    if (inherits<EvalExecutable>())
        return UINT_MAX;
    return source().length() - 1;
}

void ScriptExecutable::recordParse(CodeFeatures features, LexicallyScopedFeatures lexicallyScopedFeatures, bool hasCapturedVariables, int lastLine, unsigned endColumn)
{
    switch (type()) {
    case FunctionExecutableType:
        // Since UnlinkedFunctionExecutable holds the information to calculate lastLine and endColumn, we do not need to remember them in ScriptExecutable's fields.
        uncheckedDowncast<FunctionExecutable>(this)->recordParse(features, lexicallyScopedFeatures, hasCapturedVariables);
        return;
    default:
        uncheckedDowncast<GlobalExecutable>(this)->recordParse(features, lexicallyScopedFeatures, hasCapturedVariables, lastLine, endColumn);
        return;
    }
}

int ScriptExecutable::lastLine() const
{
    switch (type()) {
    case FunctionExecutableType:
        return uncheckedDowncast<FunctionExecutable>(this)->lastLine();
    default:
        return uncheckedDowncast<GlobalExecutable>(this)->lastLine();
    }
    return 0;
}

unsigned ScriptExecutable::endColumn() const
{
    switch (type()) {
    case FunctionExecutableType:
        return uncheckedDowncast<FunctionExecutable>(this)->endColumn();
    default:
        return uncheckedDowncast<GlobalExecutable>(this)->endColumn();
    }
    return 0;
}

template<typename Visitor>
void ScriptExecutable::runConstraint(const ConcurrentJSLocker& locker, Visitor& visitor, CodeBlock* codeBlock)
{
    ASSERT(codeBlock);
    codeBlock->propagateTransitions(locker, visitor);
    codeBlock->determineLiveness(locker, visitor);
}

template void ScriptExecutable::runConstraint(const ConcurrentJSLocker&, AbstractSlotVisitor&, CodeBlock*);
template void ScriptExecutable::runConstraint(const ConcurrentJSLocker&, SlotVisitor&, CodeBlock*);

template<typename Visitor>
void ScriptExecutable::visitCodeBlockEdge(Visitor& visitor, CodeBlock* codeBlock)
{
    ASSERT(codeBlock);

    ConcurrentJSLocker locker(codeBlock->m_lock);

    if (codeBlock->shouldVisitStrongly(locker, visitor))
        visitor.appendUnbarriered(codeBlock);

    if (JSC::JITCode::isOptimizingJIT(codeBlock->jitType())) {
        // If we jettison ourselves we'll install our alternative, so make sure that it
        // survives GC even if we don't.
        visitor.append(codeBlock->m_alternative);
    }

    // NOTE: There are two sides to this constraint, with different requirements for correctness.
    // Because everything is ultimately protected with weak references and jettisoning, it's
    // always "OK" to claim that something is dead prematurely and it's "OK" to keep things alive.
    // But both choices could lead to bad perf - either recomp cycles or leaks.
    //
    // Determining CodeBlock liveness: This part is the most consequential. We want to keep the
    // output constraint active so long as we think that we may yet prove that the CodeBlock is
    // live but we haven't done it yet.
    //
    // Marking Structures if profitable: It's important that we do a pass of this. Logically, this
    // seems like it is a constraint of CodeBlock. But we have always first run this as a result
    // of the edge being marked even before we determine the liveness of the CodeBlock. This
    // allows a CodeBlock to mark itself by first proving that all of the Structures it weakly
    // depends on could be strongly marked. (This part is also called propagateTransitions.)
    //
    // As a weird caveat, we only fixpoint the constraints so long as the CodeBlock is not live.
    // This means that we may overlook structure marking opportunities created by other marking
    // that happens after the CodeBlock is marked. This was an accidental policy decision from a
    // long time ago, but it is probably OK, since it's only worthwhile to keep fixpointing the
    // structure marking if we still have unmarked structures after the first round. We almost
    // never will because we will mark-if-profitable based on the owning global object being
    // already marked. We mark it just in case that hadn't happened yet. And if the CodeBlock is
    // not yet marked because it weakly depends on a structure that we did not yet mark, then we
    // will keep fixpointing until the end.
    visitor.appendUnbarriered(codeBlock->globalObject());
    runConstraint(locker, visitor, codeBlock);
}

template void ScriptExecutable::visitCodeBlockEdge(AbstractSlotVisitor&, CodeBlock*);
template void ScriptExecutable::visitCodeBlockEdge(SlotVisitor&, CodeBlock*);

} // namespace JSC
