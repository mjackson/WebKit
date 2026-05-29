/*
 * Copyright (C) 2022 Apple Inc. All rights reserved.
 * Copyright (C) 2025 Oven Authors. All rights reserved.
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

// Highway implementations of the out-of-line StringCommon scan kernels.
//
// This TU is re-included once per target ISA via hwy/foreach_target.h, so the
// kernel bodies must live inside HWY_NAMESPACE. Everything that should compile
// once (the WTF::*AlignedImpl entry points) lives under #if HWY_ONCE.

#include "config.h"
#include <wtf/text/StringCommon.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
IGNORE_WARNINGS_BEGIN("error=undef")
IGNORE_WARNINGS_BEGIN("undef")

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "text/StringCommonHighway.cpp"
#include <hwy/foreach_target.h>
#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();
namespace WTF {
namespace HWY_NAMESPACE {

namespace hn = hwy::HWY_NAMESPACE;

// Callers (StringCommon.h) align to 16 bytes and round up to a 16-byte multiple
// before calling, so the contract is the same as the simde versions: pointer is
// 16-aligned, length is nonzero, the final iteration may read past `length` up
// to the next 16-byte boundary. Highway vectors may be wider than 16, so we use
// LoadU and a length-aware tail (LoadN) instead of relying on that overread.

static const float* FindFloatAligned(const float* pointer, float target, size_t length)
{
    const hn::ScalableTag<float> d;
    const size_t N = hn::Lanes(d);
    const auto vTarget = hn::Set(d, target);

    size_t i = 0;
    for (; i + N <= length; i += N) {
        const auto v = hn::LoadU(d, pointer + i);
        const auto eq = hn::Eq(v, vTarget);
        const intptr_t pos = hn::FindFirstTrue(d, eq);
        if (HWY_UNLIKELY(pos >= 0))
            return pointer + i + static_cast<size_t>(pos);
    }
    if (i < length) {
        const size_t remaining = length - i;
        const auto v = hn::LoadN(d, pointer + i, remaining);
        const auto eq = hn::And(hn::Eq(v, vTarget), hn::FirstN(d, remaining));
        const intptr_t pos = hn::FindFirstTrue(d, eq);
        if (pos >= 0)
            return pointer + i + static_cast<size_t>(pos);
    }
    return nullptr;
}

static const double* FindDoubleAligned(const double* pointer, double target, size_t length)
{
    const hn::ScalableTag<double> d;
    const size_t N = hn::Lanes(d);
    const auto vTarget = hn::Set(d, target);

    size_t i = 0;
    for (; i + N <= length; i += N) {
        const auto v = hn::LoadU(d, pointer + i);
        const auto eq = hn::Eq(v, vTarget);
        const intptr_t pos = hn::FindFirstTrue(d, eq);
        if (HWY_UNLIKELY(pos >= 0))
            return pointer + i + static_cast<size_t>(pos);
    }
    if (i < length) {
        const size_t remaining = length - i;
        const auto v = hn::LoadN(d, pointer + i, remaining);
        const auto eq = hn::And(hn::Eq(v, vTarget), hn::FirstN(d, remaining));
        const intptr_t pos = hn::FindFirstTrue(d, eq);
        if (pos >= 0)
            return pointer + i + static_cast<size_t>(pos);
    }
    return nullptr;
}

static const uint8_t* Find8NonASCIIAligned(const uint8_t* pointer, size_t length)
{
    const hn::ScalableTag<uint8_t> d;
    const size_t N = hn::Lanes(d);
    const auto threshold = hn::Set(d, 0x80);

    size_t i = 0;
    for (; i + N <= length; i += N) {
        const auto v = hn::LoadU(d, pointer + i);
        const auto ge = hn::Ge(v, threshold);
        const intptr_t pos = hn::FindFirstTrue(d, ge);
        if (HWY_UNLIKELY(pos >= 0))
            return pointer + i + static_cast<size_t>(pos);
    }
    if (i < length) {
        const size_t remaining = length - i;
        const auto v = hn::LoadN(d, pointer + i, remaining);
        const auto ge = hn::And(hn::Ge(v, threshold), hn::FirstN(d, remaining));
        const intptr_t pos = hn::FindFirstTrue(d, ge);
        if (pos >= 0)
            return pointer + i + static_cast<size_t>(pos);
    }
    return nullptr;
}

static const uint16_t* Find16NonASCIIAligned(const uint16_t* pointer, size_t length)
{
    const hn::ScalableTag<uint16_t> d;
    const size_t N = hn::Lanes(d);
    const auto threshold = hn::Set(d, 0x80);

    size_t i = 0;
    for (; i + N <= length; i += N) {
        const auto v = hn::LoadU(d, pointer + i);
        const auto ge = hn::Ge(v, threshold);
        const intptr_t pos = hn::FindFirstTrue(d, ge);
        if (HWY_UNLIKELY(pos >= 0))
            return pointer + i + static_cast<size_t>(pos);
    }
    if (i < length) {
        const size_t remaining = length - i;
        const auto v = hn::LoadN(d, pointer + i, remaining);
        const auto ge = hn::And(hn::Ge(v, threshold), hn::FirstN(d, remaining));
        const intptr_t pos = hn::FindFirstTrue(d, ge);
        if (pos >= 0)
            return pointer + i + static_cast<size_t>(pos);
    }
    return nullptr;
}

} // namespace HWY_NAMESPACE
} // namespace WTF
HWY_AFTER_NAMESPACE();

#if HWY_ONCE

namespace WTF {

HWY_EXPORT(FindFloatAligned);
HWY_EXPORT(FindDoubleAligned);
HWY_EXPORT(Find8NonASCIIAligned);
HWY_EXPORT(Find16NonASCIIAligned);

SUPPRESS_NODELETE SUPPRESS_ASAN
const float* findFloatAlignedImpl(const float* pointer, float target, size_t length)
{
    ASSERT(length);
    ASSERT(!(reinterpret_cast<uintptr_t>(pointer) & 0xf));
    return HWY_DYNAMIC_DISPATCH(FindFloatAligned)(pointer, target, length);
}

SUPPRESS_NODELETE SUPPRESS_ASAN
const double* findDoubleAlignedImpl(const double* pointer, double target, size_t length)
{
    ASSERT(length);
    ASSERT(!(reinterpret_cast<uintptr_t>(pointer) & 0xf));
    return HWY_DYNAMIC_DISPATCH(FindDoubleAligned)(pointer, target, length);
}

SUPPRESS_NODELETE SUPPRESS_ASAN
const Latin1Character* find8NonASCIIAlignedImpl(std::span<const Latin1Character> data)
{
    ASSERT(data.size());
    ASSERT(!(reinterpret_cast<uintptr_t>(data.data()) & 0xf));
    auto* result = HWY_DYNAMIC_DISPATCH(Find8NonASCIIAligned)(std::bit_cast<const uint8_t*>(data.data()), data.size());
    return std::bit_cast<const Latin1Character*>(result);
}

SUPPRESS_NODELETE SUPPRESS_ASAN
const char16_t* find16NonASCIIAlignedImpl(std::span<const char16_t> data)
{
    ASSERT(data.size());
    ASSERT(!(reinterpret_cast<uintptr_t>(data.data()) & 0xf));
    auto* result = HWY_DYNAMIC_DISPATCH(Find16NonASCIIAligned)(std::bit_cast<const uint16_t*>(data.data()), data.size());
    return std::bit_cast<const char16_t*>(result);
}

} // namespace WTF

#endif // HWY_ONCE

IGNORE_WARNINGS_END
IGNORE_WARNINGS_END
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
