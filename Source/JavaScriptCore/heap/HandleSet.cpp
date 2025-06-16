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

#include "config.h"
#include "HandleSet.h"

#include "HandleBlock.h"
#include "HandleBlockInlines.h"
#include "JSCJSValueInlines.h"

namespace JSC {

HandleSet::HandleSet(VM& vm)
    : m_vm(vm)
{
    grow();
}

HandleSet::~HandleSet()
{
    while (!m_blockList.isEmpty())
        HandleBlock::destroy(m_blockList.removeHead());
}

void HandleSet::grow()
{
    HandleBlock* newBlock = HandleBlock::create(this);
    m_blockList.append(newBlock);
    newBlock->m_usedSlots.resize(newBlock->nodeCapacity());
    newBlock->m_cellSlots.resize(newBlock->nodeCapacity());

    HandleNode* listHead = nullptr;
    for (int i = newBlock->nodeCapacity() - 1; i >= 0; --i) {
        HandleNode* node = newBlock->nodeAtIndex(i);
        node->setNext(listHead);
        listHead = node;
    }
    newBlock->setFreeListHead(listHead);

    newBlock->setNextInFreeList(m_freeBlockList);
    m_freeBlockList = newBlock;
}

HandleSlot HandleSet::allocate()
{
    if (!m_freeBlockList)
        grow(); // Creates a new block and puts it on the free list.

    HandleBlock* block = m_freeBlockList;
    HandleNode* node = block->freeListHead();
    ASSERT(node); // The block wouldn't be on the list if it had no free nodes.

    block->setFreeListHead(node->next());

    if (!block->freeListHead()) {
        // This was the last free node in the block. Remove the block from the free list.
        m_freeBlockList = block->nextInFreeList();
        block->setNextInFreeList(nullptr);
    }

    unsigned index = node - block->nodes();
    ASSERT(!block->isUsed(index));
    block->setUsed(index, true);
    block->setCell(index, false); // New handles start empty (not pointing to cells)

    new (node) HandleNode();
    return node->slot();
}

void HandleSet::deallocate(HandleSlot handle)
{
    HandleNode* node = HandleNode::toHandleNode(handle);
    HandleBlock* block = HandleBlock::blockFor(node);
    unsigned index = node - block->nodes();

    ASSERT(block->isUsed(index));
    block->setUsed(index, false);
    block->setCell(index, false);

    bool wasFull = !block->freeListHead();

    node->setNext(block->freeListHead());
    block->setFreeListHead(node);

    if (wasFull) {
        block->setNextInFreeList(m_freeBlockList);
        m_freeBlockList = block;
    }
}

void HandleSet::writeBarrier(HandleSlot slot, JSValue value)
{
    HandleNode* node = HandleNode::toHandleNode(slot);
    HandleBlock* block = HandleBlock::blockFor(node);
    unsigned index = node - block->nodes();
    
    // Update the cell bit based on whether the new value is a cell
    block->setCell(index, value && value.isCell());
}

template<typename Visitor>
void HandleSet::visitStrongHandles(Visitor& visitor)
{
    for (HandleBlock* block = m_blockList.head(); block; block = block->next()) {
        // Visit only slots that are both used AND contain cells
        auto cellHandles = block->m_usedSlots & block->m_cellSlots;
        cellHandles.forEachSetBit([&] (unsigned i) {
            visitor.appendUnbarriered(*block->nodeAtIndex(i)->slot());
        });
    }
}

template void HandleSet::visitStrongHandles(AbstractSlotVisitor&);
template void HandleSet::visitStrongHandles(SlotVisitor&);

unsigned HandleSet::protectedGlobalObjectCount()
{
    unsigned count = 0;
    for (HandleBlock* block = m_blockList.head(); block; block = block->next()) {
        // Only check slots that are both used AND contain cells
        auto cellHandles = block->m_usedSlots & block->m_cellSlots;
        cellHandles.forEachSetBit([&] (unsigned i) {
            JSValue value = *block->nodeAtIndex(i)->slot();
            if (value.isObject() && asObject(value.asCell())->isGlobalObject())
                count++;
        });
    }
    return count;
}

#if ENABLE(GC_VALIDATION) || ASSERT_ENABLED
bool HandleSet::isLiveNode(Node* node)
{
    HandleBlock* block = HandleBlock::blockFor(node);
    // Verify the block is part of this HandleSet. A full search is slow but fine for validation.
    bool blockFound = false;
    for (HandleBlock* b = m_blockList.head(); b; b = b->next()) {
        if (b == block) {
            blockFound = true;
            break;
        }
    }
    if (!blockFound)
        return false;

    // A node is live if its used bit is set.
    // Note: We don't check m_cellSlots here because a handle containing a primitive
    // is still a "live" handle - it's just not visited during GC.
    unsigned index = node - block->nodes();
    return block->isUsed(index);
}
#endif // ENABLE(GC_VALIDATION) || ASSERT_ENABLED

} // namespace JSC
