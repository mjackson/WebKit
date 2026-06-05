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
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>

namespace JSC {

// ---------------- own-data-property helpers ----------------

enum class OwnPropertyKind : uint8_t { Missing, Data, Accessor };

static OwnPropertyKind getOwnPropertyForAtomics(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, JSValue& value, unsigned& attributes)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertySlot slot(object, PropertySlot::InternalMethodType::GetOwnProperty);
    bool hasProperty = object->methodTable()->getOwnPropertySlot(object, globalObject, propertyName, slot);
    RETURN_IF_EXCEPTION(scope, OwnPropertyKind::Missing);
    if (!hasProperty)
        return OwnPropertyKind::Missing;
    attributes = slot.attributes();
    if (!slot.isValue())
        return OwnPropertyKind::Accessor;
    value = slot.getValue(globalObject, propertyName);
    RETURN_IF_EXCEPTION(scope, OwnPropertyKind::Missing);
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
    object->putDirectMayBeIndex(globalObject, propertyName, value);
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
    if (sameValueZeroForAtomics(globalObject, current, expected)) {
        object->putDirectMayBeIndex(globalObject, propertyName, replacement);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return current;
}

JSValue atomicsRMWOnProperty(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, AtomicsRMWOp op, JSValue operand)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (op == AtomicsRMWOp::Exchange) {
        JSValue current;
        unsigned attributes = 0;
        auto kind = getOwnPropertyForAtomics(globalObject, object, propertyName, current, attributes);
        RETURN_IF_EXCEPTION(scope, { });
        if (kind != OwnPropertyKind::Data) {
            throwTypeError(globalObject, scope, "Atomics.exchange: object has no own data property"_s);
            return { };
        }
        object->putDirectMayBeIndex(globalObject, propertyName, operand);
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
    object->putDirectMayBeIndex(globalObject, propertyName, newValue);
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

    // Must hold the JSLock (creates the first-waiter Strong).
    Ref<PropertyWaiterList> findOrCreateList(VM& vm, JSObject* object, UniquedStringImpl* uid)
    {
        Locker locker { m_lock };
        auto result = m_lists.ensure(PropertyWaiterKey { object, uid }, [&] {
            return PropertyWaiterList::create();
        });
        Ref<PropertyWaiterList> list = result.iterator->value;
        if (result.isNewEntry) {
            list->cellProtect.set(vm, object);
            list->uidProtect = uid;
        }
        return list;
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

    if (!jsThreadsCanBlockOnCurrentThread(vm))
        return throwTypeError(globalObject, scope, "Atomics.wait cannot be called from the current thread."_s), JSValue();

    // Step 1: validate + read under the JSLock; no re-read below.
    JSValue current = atomicsLoadOnProperty(globalObject, object, propertyName);
    RETURN_IF_EXCEPTION(scope, { });
    if (!sameValueZeroForAtomics(globalObject, current, expected))
        return vm.smallStrings.notEqualString();

    UniquedStringImpl* uid = propertyName.uid();
    if (!uid)
        return throwTypeError(globalObject, scope, "Atomics.wait: invalid property name"_s), JSValue();

    auto& table = PropertyWaiterTable::singleton();
    Ref<PropertyWaiterList> list = table.findOrCreateList(vm, object, uid);
    Ref<PropertyWaiter> waiter = PropertyWaiter::create(PropertyWaiter::Kind::Sync);
    {
        Locker listLocker { list->listLock };
        list->waiters.append(waiter.copyRef());
    }

    MonotonicTime deadline = MonotonicTime::timePointFromNow(timeout);
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

    if (!sameValueZeroForAtomics(globalObject, current, expected))
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
            JSCell* cell = object;
            vm.runLoop().dispatchAfter(timeout, [&vm, list = WTF::move(list), waiter = WTF::move(waiter), ticket = ticket.copyRef(), cell, uid = RefPtr<UniquedStringImpl> { uid }] {
                JSLockHolder locker(vm);
                bool wasWaiting = false;
                {
                    Locker listLocker { list->listLock };
                    if (waiter->state.load(std::memory_order_acquire) == PropertyWaiter::Waiting) {
                        wasWaiting = true;
                        waiter->state.store(PropertyWaiter::TimedOut, std::memory_order_release);
                        list->waiters.removeFirstMatching([&](auto& entry) {
                            return entry.ptr() == waiter.ptr();
                        });
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
                    else if (waiter->ticket)
                        asyncWoken.append(*waiter->ticket);
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
