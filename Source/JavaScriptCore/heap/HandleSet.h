/*
 * Copyright (C) 2011-2021 Apple Inc. All rights reserved.
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

#include "Handle.h"
#include "HandleBlock.h"
#include "HeapCell.h"
#include <type_traits>
#include <wtf/DoublyLinkedList.h>
#include <wtf/HashCountedSet.h>

namespace JSC {

class HandleSet;
class VM;
class JSValue;

class HandleNode final {
public:
    HandleNode() = default;
    
    HandleSlot slot();
    HandleSet* handleSet();

    static HandleNode* toHandleNode(HandleSlot slot)
    {
        return std::bit_cast<HandleNode*>(std::bit_cast<uintptr_t>(slot) - OBJECT_OFFSETOF(HandleNode, m_value));
    }
    
    // For free list management
    HandleNode* next() const { return m_next; }
    void setNext(HandleNode* next) { m_next = next; }

private:
    union {
        JSValue m_value { };
        HandleNode* m_next;
    };
};

static_assert(std::is_standard_layout_v<HandleNode>, "HandleNode must be standard layout for OBJECT_OFFSETOF to work correctly");

/*
 * Manages strong handles for the garbage collector.
 *
 * This class allocates handles in large, contiguous HandleBlocks. Active handles
 * within each block are tracked using a bit-vector (m_usedSlots), which provides
 * fast, cache-friendly iteration during GC root scanning. This design replaces a
 * previous linked-list-based approach to improve performance and reduce per-handle
 * memory overhead.
 *
 * Key design decisions:
 * - Each HandleBlock contains a fixed number of HandleNode slots
 * - A FastBitVector tracks which slots are in use within each block
 * - GC scanning iterates through blocks and their bit-vectors for optimal cache locality
 * - Allocation searches for the first clear bit, with a hint for faster subsequent allocations
 * - No per-handle list pointers, saving 16 bytes per handle
 */
class HandleSet {
    friend class HandleBlock;
public:
    static HandleSet* heapFor(HandleSlot);

    HandleSet(VM&);
    ~HandleSet();

    VM& vm();

    JS_EXPORT_PRIVATE HandleSlot allocate();
    JS_EXPORT_PRIVATE void deallocate(HandleSlot);
    
    JS_EXPORT_PRIVATE void writeBarrier(HandleSlot, JSValue);

    template<typename Visitor> void visitStrongHandles(Visitor&);

    unsigned protectedGlobalObjectCount();

    template<typename Functor> void forEachStrongHandle(const Functor&, const HashCountedSet<JSCell*>& skipSet);

private:
    typedef HandleNode Node;

    JS_EXPORT_PRIVATE void grow();
    
#if ENABLE(GC_VALIDATION) || ASSERT_ENABLED
    JS_EXPORT_PRIVATE bool isLiveNode(Node*);
#endif

    VM& m_vm;
    DoublyLinkedList<HandleBlock> m_blockList;
    HandleBlock* m_freeBlockList { nullptr };
};

inline HandleSet* HandleSet::heapFor(HandleSlot handle)
{
    return HandleNode::toHandleNode(handle)->handleSet();
}

inline VM& HandleSet::vm()
{
    return m_vm;
}

inline HandleSlot HandleNode::slot()
{
    return &m_value;
}

inline HandleSet* HandleNode::handleSet()
{
    return HandleBlock::blockFor(this)->handleSet();
}

} // namespace JSC
