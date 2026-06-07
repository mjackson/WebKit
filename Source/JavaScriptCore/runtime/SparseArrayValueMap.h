/*
 * Copyright (C) 2011-2023 Apple Inc. All rights reserved.
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

#include "JSCast.h"
#include "JSTypeInfo.h"
#include "PropertyDescriptor.h"
#include "PutDirectIndexMode.h"
#include "VM.h"
#include "WriteBarrier.h"
#include <wtf/HashMap.h>
#include <wtf/TZoneMalloc.h>
#include <optional>

namespace JSC {

class SparseArrayValueMap;

class SparseArrayEntry : private WriteBarrier<Unknown> {
    WTF_MAKE_TZONE_ALLOCATED(SparseArrayEntry);
public:
    using Base = WriteBarrier<Unknown>;

    SparseArrayEntry()
    {
        Base::setWithoutWriteBarrier(jsUndefined());
    }

    void NODELETE get(JSObject*, PropertySlot&) const;
    void get(PropertyDescriptor&) const;
    bool put(JSGlobalObject*, JSValue thisValue, SparseArrayValueMap*, JSValue, bool shouldThrow);
    JSValue NODELETE getNonSparseMode() const;
    JSValue getConcurrently() const;
    JSValue get() const;

    unsigned attributes() const { return m_attributes; }

    // Defined below SparseArrayValueMap: the bodies need its complete type.
    inline void forceSet(SparseArrayValueMap*, unsigned attributes);
    inline void forceSet(VM&, SparseArrayValueMap*, JSValue, unsigned attributes);

    WriteBarrier<Unknown>& asValue() { return *this; }

private:
    unsigned m_attributes { 0 };
};

class SparseArrayValueMap final : public JSCell {
public:
    typedef JSCell Base;
    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;
    
private:
    typedef UncheckedKeyHashMap<uint64_t, SparseArrayEntry, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> Map;

    enum Flags {
        Normal                             = 0,
        SparseMode                         = 1 << 0,
        LengthIsReadOnly                   = 1 << 1,
        HasAnyKindOfGetterSetterProperties = 1 << 2,
    };

    SparseArrayValueMap(VM&);
    
    DECLARE_DEFAULT_FINISH_CREATION;

public:
    DECLARE_EXPORT_INFO;
    
    typedef Map::iterator iterator;
    typedef Map::const_iterator const_iterator;
    typedef Map::AddResult AddResult;

    static SparseArrayValueMap* create(VM&);
    
    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return &vm.sparseArrayValueMapSpace();
    }
    
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    DECLARE_VISIT_CHILDREN;

    bool sparseMode()
    {
        return m_flags & SparseMode;
    }

    void setSparseMode()
    {
        m_flags = static_cast<Flags>(m_flags | SparseMode);
    }

    bool lengthIsReadOnly()
    {
        return m_flags & LengthIsReadOnly;
    }

    void setLengthIsReadOnly()
    {
        m_flags = static_cast<Flags>(m_flags | LengthIsReadOnly);
    }

    bool hasAnyKindOfGetterSetterProperties()
    {
        return m_flags & HasAnyKindOfGetterSetterProperties;
    }

    void setHasAnyKindOfGetterSetterProperties()
    {
        m_flags = static_cast<Flags>(m_flags | HasAnyKindOfGetterSetterProperties);
    }

    // These methods may mutate the contents of the map
    bool putEntry(JSGlobalObject*, JSObject*, unsigned, JSValue, bool shouldThrow);
    bool putDirect(JSGlobalObject*, JSObject*, unsigned, JSValue, unsigned attributes, PutDirectIndexMode);
    AddResult add(JSObject*, unsigned);
    // AB18-G: GIL-only. An unlocked probe races a locked mutator's rehash
    // (HashTable.h checkValidity assert / freed-table SEGV), and even a
    // locked probe cannot protect the caller's notFound() comparison or
    // it->value dereference after the lock drops. GIL-off callers must use
    // getEntry()/forEachEntry() instead.
    iterator find(unsigned i) { return m_map.find(i); }
    // This should ASSERT the remove is valid (check the result of the find).
    void remove(iterator it);
    void remove(unsigned i);

    JSValue getConcurrently(unsigned index);

    // AB18-G: GIL-off-safe read API. SparseArrayEntry is plain data
    // (WriteBarrier word + attribute word), so a by-value copy taken under
    // the cell lock is a coherent snapshot; the JSValue/GetterSetter cell it
    // names is GC-stable.
    std::optional<SparseArrayEntry> getEntry(unsigned i);
    template<typename Functor> void forEachEntry(const Functor& functor)
    {
        // Functor runs under the cell lock: it must not run JS, allocate GC
        // memory, or re-enter this map (regime-3 lock rules).
        Locker locker { cellLock() };
        for (auto& entry : m_map)
            functor(entry.key, entry.value);
    }

    // These methods do not mutate the contents of the map.
    // AB18-G: notFound() is GIL-only, like find() — the sentinel comparison
    // races a rehash.
    iterator notFound() { return m_map.end(); }
    bool isEmpty() const
    {
        if (Options::useJSThreads()) [[unlikely]] {
            // AB18-G: serialize the probe against a racing add()/remove() rehash.
            Locker locker { cellLock() };
            return m_map.isEmpty();
        }
        return m_map.isEmpty();
    }
    bool contains(unsigned i) const
    {
        if (Options::useJSThreads()) [[unlikely]] {
            Locker locker { cellLock() };
            return m_map.contains(i);
        }
        return m_map.contains(i);
    }
    size_t size() const
    {
        if (Options::useJSThreads()) [[unlikely]] {
            Locker locker { cellLock() };
            return m_map.size();
        }
        return m_map.size();
    }
    // Only allow const begin/end iteration.
    // AB18-G: GIL-only. Unlocked iteration races a mutator rehash; GIL-off
    // callers must use forEachEntry() instead.
    const_iterator begin() const { return m_map.begin(); }
    const_iterator end() const { return m_map.end(); }

private:
    Map m_map;
    Flags m_flags { Normal };
    size_t m_reportedCapacity { 0 };
};

inline void SparseArrayEntry::forceSet(SparseArrayValueMap* map, unsigned attributes)
{
    // FIXME: We can expand this for non x86 environments. Currently, loading ReadOnly | DontDelete property
    // from compiler thread is only supported in X86 architecture because of its TSO nature.
    // https://bugs.webkit.org/show_bug.cgi?id=134641
    if (isX86())
        WTF::storeStoreFence();

    if (attributes & PropertyAttribute::Accessor)
        map->setHasAnyKindOfGetterSetterProperties();
    m_attributes = attributes;
}

inline void SparseArrayEntry::forceSet(VM& vm, SparseArrayValueMap* map, JSValue value, unsigned attributes)
{
    Base::set(vm, map, value);
    forceSet(map, attributes);
}

} // namespace JSC
