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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "Debugger.h"
#include "JSCellInlines.h"
#include "JSMicrotaskDispatcher.h"
#include "MicrotaskQueue.h"
#include "TopExceptionScope.h"

namespace JSC {

inline JSCell* QueuedTask::dispatcher() const
{
    return m_dispatcher.pointer();
}

inline JSGlobalObject* QueuedTask::globalObject() const
{
    auto* dispatcher = this->dispatcher();
    if (dispatcher->type() == JSMicrotaskDispatcherType) [[unlikely]]
        return jsCast<JSMicrotaskDispatcher*>(dispatcher)->globalObject();
    return jsCast<JSGlobalObject*>(dispatcher);
}

inline JSMicrotaskDispatcher* QueuedTask::jsMicrotaskDispatcher() const
{
    auto* dispatcher = this->dispatcher();
    if (dispatcher->type() == JSMicrotaskDispatcherType) [[unlikely]]
        return jsCast<JSMicrotaskDispatcher*>(dispatcher);
    return nullptr;
}

inline std::optional<MicrotaskIdentifier> QueuedTask::identifier() const
{
    auto* dispatcher = jsMicrotaskDispatcher();
    if (!dispatcher)
        return std::nullopt;
    return MicrotaskIdentifier { std::bit_cast<uintptr_t>(dispatcher) };
}

#if USE(BUN_JSC_ADDITIONS)
inline void MarkedMicrotaskDeque::clearForGlobalObject(JSGlobalObject* targetGlobalObject)
{
    if (!targetGlobalObject)
        return;
    Deque<QueuedTask> remaining;
    while (!m_queue.isEmpty()) {
        QueuedTask task = m_queue.takeFirst();
        if (task.globalObject() != targetGlobalObject)
            remaining.append(WTF::move(task));
    }
    m_queue.swap(remaining);
    m_markedBefore = 0;
}

inline void MicrotaskQueue::clearForGlobalObject(JSGlobalObject* targetGlobalObject)
{
    if (!targetGlobalObject)
        return;
    m_queue.clearForGlobalObject(targetGlobalObject);
    m_toKeep.clearForGlobalObject(targetGlobalObject);
}
#endif

template<bool useCallOnEachMicrotask>
inline void MicrotaskQueue::performMicrotaskCheckpoint(VM& vm, NOESCAPE const Invocable<QueuedTask::Result(QueuedTask&)> auto& functor)
{
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (vm.executionForbidden()) [[unlikely]]
        clear();
    else {
        if (vm.disallowVMEntryCount) [[unlikely]] {
            VM::checkVMEntryPermission();
            return;
        }

        while (!m_queue.isEmpty()) {
            auto task = m_queue.dequeue();
            auto result = functor(task);
            if (!catchScope.clearExceptionExceptTermination()) [[unlikely]] {
                clear();
                break;
            }

            if constexpr (useCallOnEachMicrotask) {
                vm.callOnEachMicrotaskTick();
                if (!catchScope.clearExceptionExceptTermination()) [[unlikely]] {
                    clear();
                    break;
                }
            }

            switch (result) {
            case QueuedTask::Result::Executed:
                break;
            case QueuedTask::Result::Discard:
                // Let this task go away.
                break;
            case QueuedTask::Result::Suspended: {
                m_toKeep.enqueue(WTF::move(task));
                break;
            }
            }
        }
        vm.didEnterVM = true;
    }
    m_queue.swap(m_toKeep);
}


} // namespace JSC
