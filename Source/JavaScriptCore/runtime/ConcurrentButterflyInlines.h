/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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

// ConcurrentButterflyInlines.h — T2-segmented-accessors-inline.
//
// Hot §9.3 spine/fragment accessors. These were JS_EXPORT_PRIVATE
// out-of-line definitions in ConcurrentButterfly.cpp; perf self% (W=2 bench,
// SCALEBENCH §25) showed segmentedPublicLength 3.25% +
// segmentedIndexedSlotIfWithinVectorLength 0.94% +
// segmentedIndexedSlotIfReadable 0.23% etc. behind a PLT stub + frame for
// every segmented indexed read/write. The spine→fragment→slot arithmetic is
// shift+mask+two loads (Butterfly.h ButterflySpine layout) — fully inlinable.
//
// Bodies live here (not in ConcurrentButterfly.h) because they need the full
// ButterflySpine definition from Butterfly.h, which ConcurrentButterfly.h only
// forward-declares. This header is included from JSObject.h (which already
// includes both Butterfly.h and ConcurrentButterfly.h), so every hot caller —
// the JSObject.h inline length()/quickly-family dispatch, JSObject.cpp,
// JSArray.cpp, and ConcurrentButterfly.cpp's own §2 dispatch loops — sees the
// inline body. The cold §9.3 helpers (spine*Fragment, segmentedOutOfLineSlot,
// setSegmentedPublicLength) stay JS_EXPORT_PRIVATE in ConcurrentButterfly.cpp
// for the JIT operation TUs that reference them by name.
//
// Flag-off (I22): every body below is reached only behind
// Options::useJSThreads() callers; this header is pure reorganisation and
// adds no flag-off code path.

#include "Butterfly.h"
#include "ConcurrentButterfly.h"
#include <algorithm>

namespace JSC {

ALWAYS_INLINE WriteBarrierBase<Unknown>* segmentedIndexedSlot(ButterflySpine* spine, unsigned index)
{
    // Precondition (C4): index < spine->vectorLength; the member ASSERTs it.
    spine->tsanConsume(); // V7
    return spine->indexedSlot(index);
}

ALWAYS_INLINE uint32_t segmentedPublicLength(ButterflySpine* spine)
{
    spine->tsanConsume(); // V7
    return spine->publicLength(); // RELEASE_ASSERTs indexedFragmentCount (C2).
}

// Bounds-checked consumer variants (Task 2 additions; nullptr = the caller
// loaded a stale spine, or the access is out of the C4/I33 bound, and must
// acquire-re-load the tagged word and re-dispatch).

ALWAYS_INLINE WriteBarrierBase<Unknown>* segmentedOutOfLineSlotIfWithinBounds(ButterflySpine* spine, PropertyOffset offset)
{
    ASSERT(isOutOfLineOffset(offset));
    spine->tsanConsume(); // V7
    uint64_t outOfLineIndex = outOfLineButterflyIndex(offset);
    if (outOfLineIndex >= static_cast<uint64_t>(butterflyFragmentSlots) * spine->outOfLineFragmentCountConcurrent())
        return nullptr; // I33: stale spine.
    return spine->outOfLineSlot(static_cast<unsigned>(outOfLineIndex));
}

ALWAYS_INLINE WriteBarrierBase<Unknown>* segmentedIndexedSlotIfReadable(ButterflySpine* spine, unsigned index)
{
    spine->tsanConsume(); // V7
    if (!spine->indexedFragmentCountConcurrent())
        return nullptr; // C2: header-less spine has no indexed storage.
    // C4: bound by min(publicLength, the SAME loaded spine's vectorLength);
    // [vectorLength, publicLength) reads as holes (SAB-granularity staleness).
    // publicLength() is already a relaxed atomic load of the shared header
    // slot (review amendment C: the one spine-reachable word that is mutable
    // post-publication is annotated on both sides — setPublicLength /
    // bumpPublicLengthToAtLeast are its paired atomic writers).
    if (index >= std::min(spine->publicLength(), spine->vectorLengthConcurrent()))
        return nullptr;
    return spine->indexedSlot(index);
}

ALWAYS_INLINE WriteBarrierBase<Unknown>* segmentedIndexedSlotIfWithinVectorLength(ButterflySpine* spine, unsigned index)
{
    spine->tsanConsume(); // V7
    if (!spine->indexedFragmentCountConcurrent())
        return nullptr; // C2
    if (index >= spine->vectorLengthConcurrent())
        return nullptr; // C4 precondition for writes that may bump publicLength.
    return spine->indexedSlot(index);
}

ALWAYS_INLINE uint32_t segmentedVectorLength(ButterflySpine* spine)
{
    spine->tsanConsume(); // V7
    return spine->vectorLengthConcurrent();
}

} // namespace JSC
