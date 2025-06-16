/*
 * Copyright (C) 2013-2019 Apple Inc. All rights reserved.
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

#include <wtf/DoublyLinkedList.h>
#include <wtf/FastBitVector.h>

namespace JSC {

class HandleSet;
class HandleNode;

class HandleBlock : public DoublyLinkedListNode<HandleBlock> {
    friend class WTF::DoublyLinkedListNode<HandleBlock>;
public:
    static HandleBlock* create(HandleSet*);
    static void destroy(HandleBlock*);
    static HandleBlock* blockFor(HandleNode*);

    static constexpr size_t blockSize = 4 * KB;

    HandleSet* handleSet();

    inline HandleNode* nodes();
    inline HandleNode* nodeAtIndex(unsigned);
    inline unsigned nodeCapacity() const;
    
    bool isUsed(unsigned index) const { return m_usedSlots[index]; }
    void setUsed(unsigned index, bool value) { m_usedSlots[index] = value; }
    
    bool isCell(unsigned index) const { return m_cellSlots[index]; }
    void setCell(unsigned index, bool value) { m_cellSlots[index] = value; }
    
    HandleNode* freeListHead() { return m_freeListHead; }
    void setFreeListHead(HandleNode* head) { m_freeListHead = head; }
    
    HandleBlock* nextInFreeList() { return m_nextInFreeList; }
    void setNextInFreeList(HandleBlock* next) { m_nextInFreeList = next; }
    
    WTF::FastBitVector m_usedSlots;
    WTF::FastBitVector m_cellSlots;
    HandleNode* m_freeListHead { nullptr };
    HandleBlock* m_nextInFreeList { nullptr };

private:
    HandleBlock(HandleSet*);

    char* payload();
    char* payloadEnd();
    const char* payload() const;
    const char* payloadEnd() const;

    static constexpr size_t s_blockMask = ~(blockSize - 1);

    HandleBlock* m_prev;
    HandleBlock* m_next;
    HandleSet* m_handleSet;
};

inline HandleBlock* HandleBlock::blockFor(HandleNode* node)
{
    return reinterpret_cast<HandleBlock*>(reinterpret_cast<size_t>(node) & s_blockMask);
}

inline HandleSet* HandleBlock::handleSet()
{
    return m_handleSet;
}

} // namespace JSC
