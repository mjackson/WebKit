/*
 * Copyright (C) 2023-2024, 2026 Apple Inc. All rights reserved.
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

#ifdef __cplusplus

#include "BPlatform.h"

#if BUSE(TZONE)

#if !BUSE(LIBPAS)
#error TZONE implementation requires LIBPAS
#endif

#include "Algorithm.h"
#include "BInline.h"
#include "CompactAllocationMode.h"

#define BUSE_TZONE_SPEC_NAME_ARG 0
#if BUSE_TZONE_SPEC_NAME_ARG
#define TZONE_SPEC_NAME_ARG(x)  x, __FILE__, __LINE__,
#else
#define TZONE_SPEC_NAME_ARG(x)
#endif

namespace bmalloc {

namespace TZone {

static constexpr size_t sizeClassFor(size_t size)
{
    constexpr size_t tzoneSmallSizeThreshold = 512;
    constexpr double tzoneMidSizeGrowthRate = 1.05;
    constexpr size_t tzoneMidSizeThreshold = 7872;
    constexpr double tzoneLargeSizeGrowthRate = 1.3;

    if (size <= tzoneSmallSizeThreshold)
        return roundUpToMultipleOf<16>(size);
    double nextSize = tzoneSmallSizeThreshold;
    size_t previousRoundedNextSize = 0;
    size_t roundedNextSize = roundUpToMultipleOf<16>(tzoneSmallSizeThreshold);
    do {
        previousRoundedNextSize = roundedNextSize;
        nextSize = nextSize * tzoneMidSizeGrowthRate;
        roundedNextSize = roundUpToMultipleOf<16>(nextSize);
        if (size < previousRoundedNextSize)
            continue;
        if (size <= roundedNextSize)
            return roundedNextSize;
    } while (roundedNextSize < tzoneMidSizeThreshold);
    do {
        previousRoundedNextSize = roundedNextSize;
        nextSize = nextSize * tzoneLargeSizeGrowthRate;
        roundedNextSize = roundUpToMultipleOf<16>(nextSize);
        if (size < previousRoundedNextSize)
            continue;
    } while (size > roundedNextSize);
    return roundedNextSize;
}

template<typename T>
static constexpr size_t sizeClass()
{
    return sizeClassFor(sizeof(T));
}

template<typename T>
static constexpr size_t alignment()
{
    return roundUpToMultipleOf<16>(alignof(T));
}

template<typename T>
inline constexpr unsigned inheritedSizeClass()
{
    if constexpr (requires { std::remove_pointer_t<T>::inheritedSizeClass; })
        return std::remove_pointer_t<T>::inheritedSizeClass();
    return 0;
}

template<typename T>
inline constexpr unsigned inheritedAlignment()
{
    if constexpr (requires { std::remove_pointer_t<T>::inheritedAlignment; })
        return std::remove_pointer_t<T>::inheritedAlignment();
    return 0;
}

template<typename T>
inline constexpr bool usesTZoneHeap()
{
    if constexpr (requires { std::remove_pointer_t<T>::usesTZoneHeap; })
        return std::remove_pointer_t<T>::usesTZoneHeap();
    return false;
}

} // namespace TZone

namespace api {

enum class TZoneMallocFallback : uint8_t {
    Undecided,
    ForceDebugMalloc,
    DoNotFallBack
};

extern BEXPORT TZoneMallocFallback tzoneMallocFallback;

using HeapRef = void*;
using TZoneDescriptor = uint64_t;

struct TZoneDescriptorHashTrait {
    static constexpr unsigned long hash(TZoneDescriptor descriptor)
    {
        return (descriptor >> 32) ^ descriptor;
    }
};

struct TZoneSpecification {

    constexpr unsigned sizeClass() const { return TZone::sizeClassFor(size); }

    template<typename T>
    static constexpr unsigned encodeSize()
    {
        // This tracks the actual size (not sizeClass) of the TZone type.
        constexpr size_t size = sizeof(T);
        static_assert(size <= UINT32_MAX);
        return size;
    }

    template<typename T>
    static constexpr uint16_t encodeAlignment()
    {
        constexpr size_t alignment = TZone::alignment<T>();
        static_assert(alignment <= UINT16_MAX);
        static_assert(isPowerOfTwo(alignment));
        return alignment;
    }

    template<typename T>
    static constexpr TZoneDescriptor encodeDescriptor()
    {
        size_t sizeClass = TZone::sizeClass<T>();
        size_t alignment = TZone::alignment<T>();
        return encodeDescriptor(sizeClass, alignment);
    }

    static constexpr TZoneDescriptor encodeDescriptor(unsigned sizeClass, uint16_t alignment)
    {
        return (static_cast<uint64_t>(alignment) << 32) | sizeClass;
    }

    HeapRef* addressOfHeapRef;
    unsigned size;
    uint16_t alignment;
    CompactAllocationMode allocationMode;
    TZoneDescriptor descriptor;
#if BUSE_TZONE_SPEC_NAME_ARG
    const char* name;
    const char* file;
    unsigned line;
#endif

    friend class TZoneDescriptorDecoder;
};

template<typename T>
inline constexpr CompactAllocationMode compactAllocationMode()
{
    if constexpr (requires { std::remove_pointer_t<T>::allowCompactPointers; })
        return std::remove_pointer_t<T>::allowCompactPointers ? CompactAllocationMode::Compact : CompactAllocationMode::NonCompact;
    return CompactAllocationMode::NonCompact;
}

BEXPORT void determineTZoneMallocFallback();

BEXPORT void* tzoneAllocateCompact(HeapRef);
BEXPORT void* tzoneAllocateNonCompact(HeapRef);
BEXPORT void* tzoneAllocateCompactSlow(size_t requestedSize, const TZoneSpecification&);
BEXPORT void* tzoneAllocateNonCompactSlow(size_t requestedSize, const TZoneSpecification&);

BEXPORT void tzoneFree(void*);

#define MAKE_BTZONE_MALLOCED_COMMON(_type, _compactMode, _exportMacro) \
public: \
    using HeapRef = ::bmalloc::api::HeapRef; \
    using TZoneDescriptor = ::bmalloc::api::TZoneDescriptor; \
    using TZoneMallocFallback = ::bmalloc::api::TZoneMallocFallback; \
    using CompactAllocationMode = ::bmalloc::CompactAllocationMode; \
private: \
    static _exportMacro HeapRef s_heapRef; \
    static _exportMacro const TZoneSpecification s_heapSpec; \
    \
public: \
    static constexpr bool usesTZoneHeap() { return true; } \
    static constexpr unsigned inheritedSizeClass() { return ::bmalloc::TZone::sizeClass<_type>(); } \
    static constexpr unsigned inheritedAlignment() { return ::bmalloc::TZone::alignment<_type>(); } \
    \
    BINLINE void* operator new(size_t, void* p) { return p; } \
    BINLINE void* operator new[](size_t, void* p) { return p; } \
    \
    void* operator new[](size_t size) = delete; \
    void operator delete[](void* p) = delete; \
    \
    BINLINE void* operator new(size_t, NotNullTag, void* location) \
    { \
        ASSERT(location); \
        return location; \
    } \
    \
    void* operator new(size_t size) \
    { \
        if (!s_heapRef || size != sizeof(_type)) [[unlikely]] \
            BMUST_TAIL_CALL return operatorNewSlow(size); \
        BASSERT(::bmalloc::api::tzoneMallocFallback > TZoneMallocFallback::ForceDebugMalloc); \
        if constexpr (::bmalloc::api::compactAllocationMode<_type>() == CompactAllocationMode::Compact) \
            return ::bmalloc::api::tzoneAllocateCompact(s_heapRef); \
        return ::bmalloc::api::tzoneAllocate ## _compactMode(s_heapRef); \
    } \
    \
    BINLINE void operator delete(void* p) \
    { \
        ::bmalloc::api::tzoneFree(p); \
    } \
    \
    BINLINE static void freeAfterDestruction(void* p) \
    { \
        ::bmalloc::api::tzoneFree(p); \
    } \
    \
    using WTFIsFastMallocAllocated = int;

#define MAKE_BTZONE_MALLOCED_COMMON_NON_TEMPLATE(_type, _compactMode, _exportMacro) \
private: \
    static _exportMacro BNO_INLINE void* operatorNewSlow(size_t);

#define MAKE_BTZONE_MALLOCED_COMMON_TEMPLATE(_type, _compactMode, _exportMacro) \
private: \
    static _exportMacro BNO_INLINE void* operatorNewSlow(size_t size) \
    { \
        static const TZoneSpecification s_heapSpec = { \
            &s_heapRef, \
            TZoneSpecification::encodeSize<_type>(), \
            TZoneSpecification::encodeAlignment<_type>(), \
            ::bmalloc::api::compactAllocationMode<_type>(), \
            TZoneSpecification::encodeDescriptor<_type>(), \
            TZONE_SPEC_NAME_ARG(#_type) \
        }; \
        if constexpr (::bmalloc::api::compactAllocationMode<_type>() == CompactAllocationMode::Compact) \
            return ::bmalloc::api::tzoneAllocateCompactSlow(size, s_heapSpec); \
        return ::bmalloc::api::tzoneAllocate ## _compactMode ## Slow(size, s_heapSpec); \
    }

#define MAKE_BTZONE_MALLOCED(_type, _compactMode, _exportMacro) \
    MAKE_BTZONE_MALLOCED_COMMON(_type, _compactMode, _exportMacro) \
    MAKE_BTZONE_MALLOCED_COMMON_NON_TEMPLATE(_type, _compactMode, _exportMacro) \
private: \
    using __makeTZoneMallocedMacroSemicolonifier BUNUSED_TYPE_ALIAS = int

#define MAKE_STRUCT_BTZONE_MALLOCED(_type, _compactMode, _exportMacro) \
    MAKE_BTZONE_MALLOCED_COMMON(_type, _compactMode, _exportMacro) \
    MAKE_BTZONE_MALLOCED_COMMON_NON_TEMPLATE(_type, _compactMode, _exportMacro) \
public: \
    using __makeTZoneMallocedMacroSemicolonifier BUNUSED_TYPE_ALIAS = int

#define MAKE_BTZONE_MALLOCED_TEMPLATE(_type, _compactMode, _exportMacro) \
    MAKE_BTZONE_MALLOCED_COMMON(_type, _compactMode, _exportMacro) \
    MAKE_BTZONE_MALLOCED_COMMON_TEMPLATE(_type, _compactMode, _exportMacro) \
private: \
    using __makeTZoneMallocedMacroSemicolonifier BUNUSED_TYPE_ALIAS = int


#define MAKE_BTZONE_MALLOCED_TEMPLATE_IMPL(_templateParameters, _type) \
    _templateParameters ::bmalloc::api::HeapRef _type::s_heapRef

// The following requires these 3 macros to be defined:
// TZONE_TEMPLATE_PARAMS, TZONE_TYPE
#define MAKE_BTZONE_MALLOCED_TEMPLATE_IMPL_WITH_MULTIPLE_PARAMETERS() \
    TZONE_TEMPLATE_PARAMS \
    ::bmalloc::api::HeapRef TZONE_TYPE::s_heapRef

} } // namespace bmalloc::api

using TZoneSpecification = ::bmalloc::api::TZoneSpecification;

#endif // BUSE(TZONE)

#endif // __cplusplus
