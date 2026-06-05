/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ThreadAtomics.h"
#include "LockObject.h"

#include "JSCInlines.h"
#include "JSLock.h"
#include "JSPromise.h"
#include "ObjectConstructor.h"
#include "ThreadManager.h"
#include "ThreadObject.h"
#include <wtf/HashSet.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>

namespace JSC {

// ---------------- own-data-property helpers ----------------

enum class OwnPropertyKind : uint8_t { Missing, Data, Accessor };

// Returns Missing with an exception pending for the two rejected receiver
// classes (see the 4.5 atomicity comment below); every caller
// RETURN_IF_EXCEPTIONs immediately after this call, so a gated receiver can
// never fall into a Missing-create path.
static OwnPropertyKind getOwnPropertyForAtomics(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, JSValue& value, unsigned& attributes)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // Gate 1: reentrant receivers. After this check, the method-table probe
    // below runs no user JS (other exotic getOwnPropertySlot implementations
    // may reify lazy properties or allocate, but never call out to JS), and
    // atomicsStoreOnProperty's isExtensible() is a plain structure-flag read.
    if (object->type() == ProxyObjectType || object->type() == GlobalProxyType) [[unlikely]] {
        throwTypeError(globalObject, scope, "Atomics property operations cannot be performed on a Proxy"_s);
        return OwnPropertyKind::Missing;
    }

    PropertySlot slot(object, PropertySlot::InternalMethodType::GetOwnProperty);
    bool hasProperty = object->methodTable()->getOwnPropertySlot(object, globalObject, propertyName, slot);
    RETURN_IF_EXCEPTION(scope, OwnPropertyKind::Missing);
    if (!hasProperty)
        return OwnPropertyKind::Missing;
    attributes = slot.attributes();
    if (!slot.isValue())
        return OwnPropertyKind::Accessor;

    // Gate 2: the own data property must be plain structure/butterfly
    // storage, so the read here and the later putDirect/putDirectIndex hit
    // the SAME slot. The probe above may have reified a lazy property (e.g.
    // function name/length), so the re-validation runs after it on purpose.
    if (std::optional<uint32_t> index = parseIndex(propertyName)) {
        if (!object->canGetIndexQuickly(index.value())) [[unlikely]] {
            throwTypeError(globalObject, scope, "Atomics property operations require a plain data property"_s);
            return OwnPropertyKind::Missing;
        }
        value = object->getIndexQuickly(index.value());
        attributes = 0; // Butterfly elements are writable/enumerable/configurable.
        return OwnPropertyKind::Data;
    }
    unsigned structureAttributes = 0;
    PropertyOffset offset = object->structure()->get(vm, propertyName, structureAttributes);
    if (!isValidOffset(offset)) [[unlikely]] {
        throwTypeError(globalObject, scope, "Atomics property operations require a plain data property"_s);
        return OwnPropertyKind::Missing;
    }
    attributes = structureAttributes;
    value = object->getDirect(offset);
    return OwnPropertyKind::Data;
}

static bool sameValueZeroForAtomics(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    if (a.isNumber() && b.isNumber()) {
        double x = a.asNumber();
        double y = b.asNumber();
        if (std::isnan(x) && std::isnan(y))
            return true;
        return x == y;
    }
    return sameValue(globalObject, a, b);
}

// SPEC-api 4.5: every property op is "one atomic step". Under the phase-1 GIL
// that holds only if NO user JS can run between the own-property read and the
// write below (operand coercions happen before the read). Two receiver
// classes would break that mechanically, so getOwnPropertyForAtomics rejects
// them with TypeError up front (landed deviation; rationale recorded in
// docs/threads/INTEGRATE-api.md "Landed deviations" — the frozen 4.5 table
// does not enumerate exotic receivers):
//
// 1. Reentrant receivers — ProxyObject / JSGlobalProxy: their
//    getOwnPropertySlot (and isExtensible) run arbitrary trap JS, which can
//    reach a GIL-dropping park site (join, cond.wait, contended lock.hold,
//    property Atomics.wait) mid-step; another thread could then mutate the
//    property between a CAS/RMW's read and its write — a cross-thread TOCTOU
//    that would falsify the advertised CAS atomicity.
//
// 2. Exotic own data properties not backed by plain structure/butterfly
//    storage — e.g. JSArray "length", RegExpObject "lastIndex", StringObject
//    indexed chars, sparse-map indices, global var-scope bindings: the method
//    table reports them as own data properties, but putDirect/putDirectIndex
//    would install a DUPLICATE shadow property next to the exotic one (an
//    object state no sequential JS program can create, violating THREAD.md's
//    indistinguishable-heap requirement).
//
// After these gates the read is a non-reentrant structure/butterfly probe and
// the write targets exactly the probed slot. Post-GIL these bodies re-home
// onto the object-model atomic slot CAS/RMW helpers (OM §9.5) per Deviation
// 12 (UNOWNED chartered workstream); the §7 signatures are frozen so only the
// bodies change.
// THREADS-INTEGRATE(api): Dev 12 re-freeze point (atomic slot CAS/RMW).

// Writes an EXISTING own data property's value, preserving its attributes.
// putDirect/putDirectMayBeIndex default to attributes 0, and putDirectInternal
// (PutModeDefineOwnProperty) performs an attribute-change Structure transition
// whenever newAttributes != currentAttributes — which would silently strip
// DontEnum/DontDelete/ReadOnly. 4.5 ops only ever change the value.
static void putExistingOwnDataPropertyForAtomics(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, JSValue value, unsigned attributes)
{
    unsigned preservedAttributes = attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
    if (std::optional<uint32_t> index = parseIndex(propertyName)) {
        object->putDirectIndex(globalObject, index.value(), value, preservedAttributes, PutDirectIndexLikePutDirect);
        return;
    }
    object->putDirect(globalObject->vm(), propertyName, value, preservedAttributes);
}

JSValue atomicsLoadOnProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value;
    unsigned attributes = 0;
    auto kind = getOwnPropertyForAtomics(globalObject, object, propertyName, value, attributes);
    RETURN_IF_EXCEPTION(scope, { });
    if (kind != OwnPropertyKind::Data) {
        throwTypeError(globalObject, scope, "Atomics.load: object has no own property"_s);
        return { };
    }
    return value;
}

JSValue atomicsStoreOnProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue existing;
    unsigned attributes = 0;
    auto kind = getOwnPropertyForAtomics(globalObject, object, propertyName, existing, attributes);
    RETURN_IF_EXCEPTION(scope, { });
    switch (kind) {
    case OwnPropertyKind::Accessor:
        throwTypeError(globalObject, scope, "Atomics.store: property is an accessor"_s);
        return { };
    case OwnPropertyKind::Data:
        if (attributes & PropertyAttribute::ReadOnly) {
            throwTypeError(globalObject, scope, "Atomics.store: property is not writable"_s);
            return { };
        }
        break;
    case OwnPropertyKind::Missing: {
        bool extensible = object->isExtensible(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (!extensible) {
            throwTypeError(globalObject, scope, "Atomics.store: cannot add a property to a non-extensible object"_s);
            return { };
        }
        break;
    }
    }
    if (kind == OwnPropertyKind::Data)
        putExistingOwnDataPropertyForAtomics(globalObject, object, propertyName, value, attributes);
    else
        object->putDirectMayBeIndex(globalObject, propertyName, value); // Fresh data property: writable/enumerable/configurable.
    RETURN_IF_EXCEPTION(scope, { });
    return value;
}

JSValue atomicsCompareExchangeOnProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, JSValue expected, JSValue replacement)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue current;
    unsigned attributes = 0;
    auto kind = getOwnPropertyForAtomics(globalObject, object, propertyName, current, attributes);
    RETURN_IF_EXCEPTION(scope, { });
    if (kind != OwnPropertyKind::Data) {
        throwTypeError(globalObject, scope, "Atomics.compareExchange: object has no own data property"_s);
        return { };
    }
    // 4.5 "stores rep" inherits store's writability rule (same as exchange's
    // "store but requires own data k"): putExistingOwnDataPropertyForAtomics
    // uses putDirect define-semantics, which would replace a ReadOnly slot's
    // value in place — a heap state no sequential JS program can create
    // (THREAD.md indistinguishable-heap requirement; a lock word CASed on a
    // later-frozen object must fail, not keep mutating). Thrown
    // unconditionally, matching store/exchange (not only when SVZ matches).
    if (attributes & PropertyAttribute::ReadOnly) {
        throwTypeError(globalObject, scope, "Atomics.compareExchange: property is not writable"_s);
        return { };
    }
    // SVZ, not ===: NaN compares equal to NaN and +0 to -0, so CAS retry
    // loops over NaN-valued properties terminate (4.5 table).
    bool equal = sameValueZeroForAtomics(globalObject, current, expected);
    RETURN_IF_EXCEPTION(scope, { }); // String comparison can resolve ropes (OOM).
    if (equal) {
        putExistingOwnDataPropertyForAtomics(globalObject, object, propertyName, replacement, attributes);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return current;
}

JSValue atomicsRMWOnProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, AtomicsRMWOp op, JSValue operand)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (op == AtomicsRMWOp::Exchange) {
        // 4.5: "store but requires own data k" — inherits store's accessor /
        // non-writable TypeErrors, but never creates the property.
        JSValue current;
        unsigned attributes = 0;
        auto kind = getOwnPropertyForAtomics(globalObject, object, propertyName, current, attributes);
        RETURN_IF_EXCEPTION(scope, { });
        if (kind != OwnPropertyKind::Data) {
            throwTypeError(globalObject, scope, "Atomics.exchange: object has no own data property"_s);
            return { };
        }
        if (attributes & PropertyAttribute::ReadOnly) {
            throwTypeError(globalObject, scope, "Atomics.exchange: property is not writable"_s);
            return { };
        }
        putExistingOwnDataPropertyForAtomics(globalObject, object, propertyName, operand, attributes);
        RETURN_IF_EXCEPTION(scope, { });
        return current;
    }

    // Numeric RMW family: convert the operand first (may run JS), then
    // perform the read-modify-write as one atomic step under the GIL.
    double operandNumber = 0;
    int32_t operandInt = 0;
    bool isBitwise = op == AtomicsRMWOp::And || op == AtomicsRMWOp::Or || op == AtomicsRMWOp::Xor;
    if (isBitwise) {
        operandInt = operand.toInt32(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    } else {
        operandNumber = operand.toNumber(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }

    JSValue current;
    unsigned attributes = 0;
    auto kind = getOwnPropertyForAtomics(globalObject, object, propertyName, current, attributes);
    RETURN_IF_EXCEPTION(scope, { });
    if (kind != OwnPropertyKind::Data) {
        throwTypeError(globalObject, scope, "Atomics RMW: object has no own data property"_s);
        return { };
    }
    // 4.5 "stores result" inherits store's writability rule (see the
    // compareExchange comment above): a ReadOnly slot must never be
    // mutated in place. Checked before the stored-value type check, so a
    // frozen non-number slot reports the writability error.
    if (attributes & PropertyAttribute::ReadOnly) {
        throwTypeError(globalObject, scope, "Atomics RMW: property is not writable"_s);
        return { };
    }
    if (!current.isNumber()) {
        throwTypeError(globalObject, scope, "Atomics RMW: stored value is not a number"_s);
        return { };
    }

    JSValue newValue;
    switch (op) {
    case AtomicsRMWOp::Add:
        newValue = jsNumber(current.asNumber() + operandNumber);
        break;
    case AtomicsRMWOp::Sub:
        newValue = jsNumber(current.asNumber() - operandNumber);
        break;
    case AtomicsRMWOp::And:
        newValue = jsNumber(JSC::toInt32(current.asNumber()) & operandInt);
        break;
    case AtomicsRMWOp::Or:
        newValue = jsNumber(JSC::toInt32(current.asNumber()) | operandInt);
        break;
    case AtomicsRMWOp::Xor:
        newValue = jsNumber(JSC::toInt32(current.asNumber()) ^ operandInt);
        break;
    case AtomicsRMWOp::Exchange:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }
    putExistingOwnDataPropertyForAtomics(globalObject, object, propertyName, newValue, attributes);
    RETURN_IF_EXCEPTION(scope, { });
    return current;
}

// ---------------- property waiter table (SPEC-api 5.6) ----------------

class PropertyWaiter final : public ThreadSafeRefCounted<PropertyWaiter> {
public:
    enum State : uint8_t { Waiting, Notified, TimedOut, Terminated };
    enum class Kind : uint8_t { Sync, Async };

    static Ref<PropertyWaiter> create(Kind kind) { return adoptRef(*new PropertyWaiter(kind)); }

    Kind kind;
    std::atomic<uint8_t> state { Waiting }; // flipped exactly once, under the list lock
    Condition condition; // sync
    RefPtr<AsyncTicket> ticket; // async

private:
    explicit PropertyWaiter(Kind kind)
        : kind(kind)
    {
    }
};

class PropertyWaiterList final : public ThreadSafeRefCounted<PropertyWaiterList> {
public:
    static Ref<PropertyWaiterList> create() { return adoptRef(*new PropertyWaiterList()); }

    Lock listLock; // rank 3
    Deque<Ref<PropertyWaiter>> waiters WTF_GUARDED_BY_LOCK(listLock);
    // GC-protects the waited-on object while the list is non-empty; created
    // and cleared only under the JSLock (SPEC-api 5.10).
    Strong<Unknown> cellProtect;
    RefPtr<UniquedStringImpl> uidProtect;

private:
    PropertyWaiterList() = default;
};

using PropertyWaiterKey = std::pair<JSCell*, UniquedStringImpl*>;

class PropertyWaiterTable final {
    WTF_MAKE_NONCOPYABLE(PropertyWaiterTable);
public:
    static PropertyWaiterTable& singleton()
    {
        static LazyNeverDestroyed<PropertyWaiterTable> table;
        static std::once_flag onceKey;
        std::call_once(onceKey, [&] {
            table.construct();
        });
        return table;
    }

    Lock m_lock; // rank 2
    UncheckedKeyHashMap<PropertyWaiterKey, Ref<PropertyWaiterList>> m_lists WTF_GUARDED_BY_LOCK(m_lock);
    // Cells that already carry the per-cell teardown-sweep finalizer (round-4
    // fix, companion to D5 — see sweepCellAtFinalization below). One entry
    // per live waited-on cell; removed when the finalizer runs, so a recycled
    // cell address re-registers correctly.
    HashSet<JSCell*> m_sweepFinalizerCells WTF_GUARDED_BY_LOCK(m_lock);

    // Must hold the JSLock (creates the first-waiter Strong, registers the
    // teardown finalizer).
    Ref<PropertyWaiterList> findOrCreateList(VM& vm, JSObject* object, UniquedStringImpl* uid)
    {
        bool registerSweepFinalizer = false;
        RefPtr<PropertyWaiterList> list;
        {
            Locker locker { m_lock };
            auto result = m_lists.ensure(PropertyWaiterKey { object, uid }, [&] {
                return PropertyWaiterList::create();
            });
            list = result.iterator->value.copyRef();
            if (result.isNewEntry) {
                list->cellProtect.set(vm, object);
                list->uidProtect = uid;
            }
            registerSweepFinalizer = m_sweepFinalizerCells.add(object).isNewEntry;
        }
        // Round-4 fix (recorded next to D5 in docs/threads/INTEGRATE-api.md):
        // a never-notified INFINITE-timeout waitAsync has no other clearing
        // point for its Strongs — the notify path and the D5 finite-timeout
        // timer are the only consumers, DWT VM-shutdown cancelPendingWork
        // cancels the underlying ticket but never touches its
        // Strong<JSPromise>, and this table is a process-global singleton.
        // cellProtect roots the cell for the waiters' lifetime (5.10
        // liveness, by design), so the cell can die EARLY only after
        // removeListIfEmpty (normal GC; the sweep is then a no-op that
        // unregisters the address) and otherwise dies exactly at VM teardown
        // via lastChanceToFinalize — where this finalizer clears every
        // surviving Strong under the JSLock, before the VM's HandleSet is
        // destroyed (the 5.10 / VM-UAF class ~AsyncTicket RELEASE_ASSERTs
        // against). Public Heap API only, same pattern as
        // registerThreadStateFinalizer (ThreadObject.cpp); registered
        // outside m_lock so the rank-2 table lock never nests heap-internal
        // locks.
        if (registerSweepFinalizer) {
            vm.heap.addFinalizer(object, +[](JSCell* cell) {
                PropertyWaiterTable::singleton().sweepCellAtFinalization(cell);
            });
        }
        return list.releaseNonNull();
    }

    // Runs under the JSLock (GC finalization / lastChanceToFinalize), only
    // when the waited-on cell is dead. Removes every list keyed on the cell
    // (any uid) and clears all Strongs owned by the lists and their abandoned
    // async waiters' tickets. Sync waiters are left enqueued on the (now
    // unreachable) list: a sync waiter parked across VM teardown is an
    // embedder protocol violation, and its thread owns its own dequeue
    // (atomicsWaitOnProperty step 5) — the dequeued <=> flipped invariant
    // stays intact because async waiters are flipped under the list lock
    // here, in the same critical section that dequeues them.
    void sweepCellAtFinalization(JSCell* cell)
    {
        Vector<Ref<PropertyWaiterList>> sweptLists;
        {
            Locker locker { m_lock };
            m_sweepFinalizerCells.remove(cell);
            m_lists.removeIf([&](auto& entry) {
                if (entry.key.first != cell)
                    return false;
                sweptLists.append(entry.value.copyRef());
                return true;
            });
        }
        for (auto& list : sweptLists) {
            Vector<Ref<PropertyWaiter>> abandonedAsync;
            {
                Locker listLocker { list->listLock };
                Deque<Ref<PropertyWaiter>> kept;
                while (!list->waiters.isEmpty()) {
                    Ref<PropertyWaiter> waiter = list->waiters.takeFirst();
                    if (waiter->kind == PropertyWaiter::Kind::Async) {
                        waiter->state.store(PropertyWaiter::TimedOut, std::memory_order_release);
                        if (waiter->ticket)
                            waiter->ticket->state.store(PropertyWaiter::TimedOut, std::memory_order_release);
                        abandonedAsync.append(WTF::move(waiter));
                    } else
                        kept.append(WTF::move(waiter));
                }
                list->waiters = WTF::move(kept);
            }
            // Settle would be a no-op (the ticket is cancelled at DWT
            // shutdown, or about to be); clear the promise Strong here,
            // under the JSLock — mirroring the D5 timer-task bailout.
            for (auto& waiter : abandonedAsync) {
                if (waiter->ticket)
                    waiter->ticket->promise().clear();
            }
            list->cellProtect.clear();
            list->uidProtect = nullptr;
        }
    }

    RefPtr<PropertyWaiterList> findList(JSCell* cell, UniquedStringImpl* uid)
    {
        Locker locker { m_lock };
        auto it = m_lists.find(PropertyWaiterKey { cell, uid });
        if (it == m_lists.end())
            return nullptr;
        return RefPtr<PropertyWaiterList> { it->value.copyRef() };
    }

    // Must hold the JSLock (clears the Strong). Re-checks emptiness in rank
    // order (table lock, then list lock).
    void removeListIfEmpty(JSCell* cell, UniquedStringImpl* uid)
    {
        Locker locker { m_lock };
        auto it = m_lists.find(PropertyWaiterKey { cell, uid });
        if (it == m_lists.end())
            return;
        Ref<PropertyWaiterList> list = it->value;
        {
            Locker listLocker { list->listLock };
            if (!list->waiters.isEmpty())
                return;
        }
        list->cellProtect.clear();
        list->uidProtect = nullptr;
        m_lists.remove(it);
    }

    PropertyWaiterTable() = default; // for LazyNeverDestroyed only; use singleton()
};

static Seconds parseAtomicsTimeout(JSGlobalObject* globalObject, JSValue timeoutValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    double timeoutInMilliseconds = timeoutValue.toNumber(globalObject);
    RETURN_IF_EXCEPTION(scope, Seconds::infinity());
    Seconds timeout = Seconds::infinity();
    if (!std::isnan(timeoutInMilliseconds))
        timeout = std::max(Seconds::fromMilliseconds(timeoutInMilliseconds), 0_s);
    return timeout;
}

JSValue atomicsWaitOnProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, JSValue expected, JSValue timeoutValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    Seconds timeout = parseAtomicsTimeout(globalObject, timeoutValue);
    RETURN_IF_EXCEPTION(scope, { });

    // Step 1 (F4): validate + read under the JSLock; no re-read below.
    JSValue current = atomicsLoadOnProperty(globalObject, object, propertyName);
    RETURN_IF_EXCEPTION(scope, { });
    bool equal = sameValueZeroForAtomics(globalObject, current, expected);
    RETURN_IF_EXCEPTION(scope, { }); // String comparison can resolve ropes (OOM).
    if (!equal)
        return vm.smallStrings.notEqualString();

    // 4.5: the G11 gate guards the *block*, not the call — "!SVZ=>'not-equal';
    // else block (G11-gated)". A not-equal value returns "not-equal" even on a
    // thread that may not block, matching lock.hold which gates only on
    // contention (I18). Still under the JSLock; no side effects yet.
    if (!jsThreadsCanBlockOnCurrentThread(vm))
        return throwTypeError(globalObject, scope, "Atomics.wait cannot be called from the current thread."_s), JSValue();

    UniquedStringImpl* uid = propertyName.uid();
    if (!uid)
        return throwTypeError(globalObject, scope, "Atomics.wait: invalid property name"_s), JSValue();

    MonotonicTime deadline = MonotonicTime::timePointFromNow(timeout);

    // Step 2 (F4): still under the JSLock — table lock (rank 2), find-or-create
    // + first-waiter Strongs, drop; list lock (rank 3), enqueue Waiting, drop.
    // JSLock held from the step-1 read through the enqueue closes the lost
    // store+notify window (I10); no list lock is held across the GIL drop.
    auto& table = PropertyWaiterTable::singleton();
    Ref<PropertyWaiterList> list = table.findOrCreateList(vm, object, uid);
    Ref<PropertyWaiter> waiter = PropertyWaiter::create(PropertyWaiter::Kind::Sync);
    {
        Locker listLocker { list->listLock };
        list->waiters.append(waiter.copyRef());
    }

    uint8_t finalState;
    bool listNowEmpty = false;
    {
        // Steps 3-6: park with the GIL dropped; 10ms quantum to poll for
        // termination requests (VMTraps cannot wake property waiters).
        // Depth-free drop (GILDroppedSection, LockObject.h): timed waiters
        // wake in arbitrary order, which DropAllLocks' strict-LIFO unwind
        // protocol livelocks on.
        GILDroppedSection droppedSection(vm);
        Locker listLocker { list->listLock };
        while (waiter->state.load(std::memory_order_acquire) == PropertyWaiter::Waiting
            && !vm.hasTerminationRequest()
            && MonotonicTime::now() < deadline) {
            MonotonicTime quantum = MonotonicTime::now() + Seconds::fromMilliseconds(10);
            waiter->condition.waitUntil(list->listLock, std::min(deadline, quantum).approximate<WallTime>());
        }
        if (waiter->state.load(std::memory_order_acquire) == PropertyWaiter::Notified)
            finalState = PropertyWaiter::Notified;
        else {
            bool removed = list->waiters.removeFirstMatching([&](auto& entry) {
                return entry.ptr() == waiter.ptr();
            });
            ASSERT_UNUSED(removed, removed);
            finalState = vm.hasTerminationRequest() ? PropertyWaiter::Terminated : PropertyWaiter::TimedOut;
            waiter->state.store(finalState, std::memory_order_release);
        }
        listNowEmpty = list->waiters.isEmpty();
    }

    // Step 7: back under the JSLock.
    if (listNowEmpty)
        table.removeListIfEmpty(object, uid);
    switch (finalState) {
    case PropertyWaiter::Notified:
        return vm.smallStrings.okString();
    case PropertyWaiter::TimedOut:
        return vm.smallStrings.timedOutString();
    case PropertyWaiter::Terminated:
        vm.throwTerminationException();
        return { };
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return { };
    }
}

JSValue atomicsWaitAsyncOnProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, JSValue expected, JSValue timeoutValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    Seconds timeout = parseAtomicsTimeout(globalObject, timeoutValue);
    RETURN_IF_EXCEPTION(scope, { });

    JSValue current = atomicsLoadOnProperty(globalObject, object, propertyName);
    RETURN_IF_EXCEPTION(scope, { });

    JSObject* resultObject = constructEmptyObject(globalObject);
    bool isAsync = false;
    JSValue value;

    bool equal = sameValueZeroForAtomics(globalObject, current, expected);
    RETURN_IF_EXCEPTION(scope, { }); // String comparison can resolve ropes (OOM).
    if (!equal)
        value = vm.smallStrings.notEqualString();
    else if (!timeout)
        value = vm.smallStrings.timedOutString();
    else {
        UniquedStringImpl* uid = propertyName.uid();
        if (!uid)
            return throwTypeError(globalObject, scope, "Atomics.waitAsync: invalid property name"_s), JSValue();

        isAsync = true;
        JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
        Ref<AsyncTicket> ticket = AsyncTicket::create(globalObject, promise, { object });

        auto& table = PropertyWaiterTable::singleton();
        Ref<PropertyWaiterList> list = table.findOrCreateList(vm, object, uid);
        Ref<PropertyWaiter> waiter = PropertyWaiter::create(PropertyWaiter::Kind::Async);
        waiter->ticket = ticket.ptr();
        {
            Locker listLocker { list->listLock };
            list->waiters.append(waiter.copyRef());
        }

        if (timeout != Seconds::infinity()) {
            // Arm the timeout on the VM's run loop (SPEC-api 5.6 / G28).
            // The lambda holds Ref<VM>: WTF::RunLoop is independently
            // ref-counted and outlives the VM, so a dispatched task can run
            // after embedder VM teardown — nothing else guarantees the VM
            // outlives this timer (same rationale as constructThread's
            // protectedVM capture, ThreadObject.cpp). If DWT shutdown
            // already cancelled the ticket, bail out before touching VM
            // state beyond the lock (the JSLockHolder also keeps the
            // ticket's Strong-clearing destructor path lock-correct).
            JSCell* cell = object;
            vm.runLoop().dispatchAfter(timeout, [protectedVM = Ref { vm }, list = WTF::move(list), waiter = WTF::move(waiter), ticket = ticket.copyRef(), cell, uid = RefPtr<UniquedStringImpl> { uid }] {
                VM& timerVM = protectedVM.get();
                JSLockHolder locker(timerVM);
                if (ticket->isCancelled()) {
                    // DWT VM-shutdown cancelPendingWork ran: settle() would
                    // be a no-op, so clear the ticket's promise Strong HERE,
                    // under the lock — this lambda may hold the last Ref and
                    // a still-set Strong must never be destroyed off-lock
                    // (SPEC-api 5.10; ~AsyncTicket asserts it).
                    ticket->promise().clear();
                    return;
                }
                bool wasWaiting = false;
                {
                    Locker listLocker { list->listLock };
                    if (waiter->state.load(std::memory_order_acquire) == PropertyWaiter::Waiting) {
                        wasWaiting = true;
                        // 5.6 timer task: Waiting => findAndRemove (must succeed:
                        // dequeued <=> flipped, both under the list lock), then
                        // TimedOut; ticket state mirrors 5.5 Waiting->TimedOut.
                        bool removed = list->waiters.removeFirstMatching([&](auto& entry) {
                            return entry.ptr() == waiter.ptr();
                        });
                        ASSERT_UNUSED(removed, removed);
                        waiter->state.store(PropertyWaiter::TimedOut, std::memory_order_release);
                        ticket->state.store(PropertyWaiter::TimedOut, std::memory_order_release);
                    }
                }
                if (!wasWaiting)
                    return;
                PropertyWaiterTable::singleton().removeListIfEmpty(cell, uid.get());
                ticket->settle([](DeferredWorkTimer::Ticket dwtTicket) {
                    JSPromise* promise = uncheckedDowncast<JSPromise>(dwtTicket->target());
                    JSGlobalObject* lexicalGlobalObject = promise->realm();
                    VM& innerVM = lexicalGlobalObject->vm();
                    promise->resolve(lexicalGlobalObject, innerVM, innerVM.smallStrings.timedOutString());
                });
            });
        }
        value = promise;
    }

    resultObject->putDirect(vm, vm.propertyNames->async, jsBoolean(isAsync));
    resultObject->putDirect(vm, vm.propertyNames->value, value);
    return resultObject;
}

JSValue atomicsNotifyOnProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, JSValue countValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    double count = std::numeric_limits<double>::infinity();
    if (!countValue.isUndefined()) {
        count = countValue.toIntegerOrInfinity(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (count < 0)
            count = 0;
    }

    UniquedStringImpl* uid = propertyName.uid();
    unsigned woken = 0;
    if (uid) {
        auto& table = PropertyWaiterTable::singleton();
        if (RefPtr<PropertyWaiterList> list = table.findList(object, uid)) {
            Vector<Ref<AsyncTicket>> asyncWoken;
            bool listNowEmpty = false;
            {
                Locker listLocker { list->listLock };
                while (woken < count && !list->waiters.isEmpty()) {
                    Ref<PropertyWaiter> waiter = list->waiters.takeFirst();
                    waiter->state.store(PropertyWaiter::Notified, std::memory_order_release);
                    if (waiter->kind == PropertyWaiter::Kind::Sync)
                        waiter->condition.notifyOne();
                    else if (waiter->ticket) {
                        // F4 notify: flip the ticket Notified under the list
                        // lock (5.5 Waiting->Notified); collect, settle later.
                        waiter->ticket->state.store(PropertyWaiter::Notified, std::memory_order_release);
                        asyncWoken.append(*waiter->ticket);
                    }
                    woken++;
                }
                listNowEmpty = list->waiters.isEmpty();
            }
            for (auto& ticket : asyncWoken) {
                ticket->settle([](DeferredWorkTimer::Ticket dwtTicket) {
                    JSPromise* promise = uncheckedDowncast<JSPromise>(dwtTicket->target());
                    JSGlobalObject* lexicalGlobalObject = promise->realm();
                    VM& innerVM = lexicalGlobalObject->vm();
                    promise->resolve(lexicalGlobalObject, innerVM, innerVM.smallStrings.okString());
                });
            }
            if (listNowEmpty)
                table.removeListIfEmpty(object, uid);
        }
    }
    return jsNumber(woken);
}

} // namespace JSC
