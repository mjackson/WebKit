/*
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

#include "config.h"
#include "HighwayKernels.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
IGNORE_WARNINGS_BEGIN("error=undef")
IGNORE_WARNINGS_BEGIN("undef")

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "runtime/HighwayKernels.cpp"
#include <hwy/foreach_target.h>
#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();
namespace JSC { namespace Highway {
namespace HWY_NAMESPACE {

namespace hn = hwy::HWY_NAMESPACE;

// ───────────────────────────────────────────────────────────────────────────
// Generic find driver: returns index of first lane where `predicate` is true,
// or `length` if none.
// ───────────────────────────────────────────────────────────────────────────

template<typename T, typename Predicate>
static HWY_INLINE size_t Find(const T* p, size_t length, Predicate predicate)
{
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);

    size_t i = 0;
    for (; i + N <= length; i += N) {
        const auto v = hn::LoadU(d, p + i);
        const intptr_t pos = hn::FindFirstTrue(d, predicate(d, v));
        if (HWY_UNLIKELY(pos >= 0))
            return i + static_cast<size_t>(pos);
    }
    if (i < length) {
        const size_t remaining = length - i;
        const auto v = hn::LoadN(d, p + i, remaining);
        const auto m = hn::And(predicate(d, v), hn::FirstN(d, remaining));
        const intptr_t pos = hn::FindFirstTrue(d, m);
        if (pos >= 0)
            return i + static_cast<size_t>(pos);
    }
    return length;
}

// ───────────────────────────────────────────────────────────────────────────
// Lexer parseIdentifier: NOT in [A-Za-z0-9_$]
// ───────────────────────────────────────────────────────────────────────────

template<typename T>
static HWY_INLINE auto IdentifierContinue(const hn::ScalableTag<T> d, hn::Vec<hn::ScalableTag<T>> v)
{
    const auto folded = hn::Or(v, hn::Set(d, 0x20));
    const auto isAlpha = hn::And(hn::Ge(folded, hn::Set(d, 'a')), hn::Le(folded, hn::Set(d, 'z')));
    const auto isDigit = hn::And(hn::Ge(v, hn::Set(d, '0')), hn::Le(v, hn::Set(d, '9')));
    const auto isDollar = hn::Eq(v, hn::Set(d, '$'));
    const auto isUnderscore = hn::Eq(v, hn::Set(d, '_'));
    return hn::Or(hn::Or(isAlpha, isDigit), hn::Or(isDollar, isUnderscore));
}

static size_t FindIdentifierEnd8(const uint8_t* p, size_t length)
{
    return Find<uint8_t>(p, length, [](auto d, auto v) { return hn::Not(IdentifierContinue<uint8_t>(d, v)); });
}

static size_t FindIdentifierEnd16(const uint16_t* p, size_t length)
{
    return Find<uint16_t>(p, length, [](auto d, auto v) { return hn::Not(IdentifierContinue<uint16_t>(d, v)); });
}

// ───────────────────────────────────────────────────────────────────────────
// Lexer parseString
// ───────────────────────────────────────────────────────────────────────────

static size_t FindStringEnd8(const uint8_t* p, size_t length, uint8_t quote)
{
    return Find<uint8_t>(p, length, [quote](auto d, auto v) {
        const auto quotes = hn::Eq(v, hn::Set(d, quote));
        const auto escapes = hn::Eq(v, hn::Set(d, static_cast<uint8_t>('\\')));
        const auto controls = hn::Lt(v, hn::Set(d, static_cast<uint8_t>(0x0E)));
        return hn::Or(hn::Or(quotes, escapes), controls);
    });
}

static size_t FindStringEnd16(const uint16_t* p, size_t length, uint16_t quote, bool checkNonLatin1)
{
    if (checkNonLatin1) {
        return Find<uint16_t>(p, length, [quote](auto d, auto v) {
            const auto quotes = hn::Eq(v, hn::Set(d, quote));
            const auto escapes = hn::Eq(v, hn::Set(d, static_cast<uint16_t>('\\')));
            const auto controls = hn::Lt(v, hn::Set(d, static_cast<uint16_t>(0x0E)));
            const auto nonLatin1 = hn::Gt(v, hn::Set(d, static_cast<uint16_t>(0xFF)));
            return hn::Or(hn::Or(quotes, escapes), hn::Or(controls, nonLatin1));
        });
    }
    return Find<uint16_t>(p, length, [quote](auto d, auto v) {
        const auto quotes = hn::Eq(v, hn::Set(d, quote));
        const auto escapes = hn::Eq(v, hn::Set(d, static_cast<uint16_t>('\\')));
        const auto controls = hn::Lt(v, hn::Set(d, static_cast<uint16_t>(0x0E)));
        return hn::Or(hn::Or(quotes, escapes), controls);
    });
}

// ───────────────────────────────────────────────────────────────────────────
// Lexer single-line comment: '\n' / '\r' / U+2028 / U+2029
// ───────────────────────────────────────────────────────────────────────────

static size_t FindLineTerminator8(const uint8_t* p, size_t length)
{
    return Find<uint8_t>(p, length, [](auto d, auto v) {
        return hn::Or(hn::Eq(v, hn::Set(d, static_cast<uint8_t>('\n'))), hn::Eq(v, hn::Set(d, static_cast<uint8_t>('\r'))));
    });
}

static size_t FindLineTerminator16(const uint16_t* p, size_t length)
{
    return Find<uint16_t>(p, length, [](auto d, auto v) {
        const auto lf = hn::Eq(v, hn::Set(d, static_cast<uint16_t>('\n')));
        const auto cr = hn::Eq(v, hn::Set(d, static_cast<uint16_t>('\r')));
        const auto ls = hn::Eq(v, hn::Set(d, static_cast<uint16_t>(0x2028)));
        const auto ps = hn::Eq(v, hn::Set(d, static_cast<uint16_t>(0x2029)));
        return hn::Or(hn::Or(lf, cr), hn::Or(ls, ps));
    });
}

// ───────────────────────────────────────────────────────────────────────────
// parseCommentDirectiveValue: 0x09..0x0D / ' ' / '"' / '\'' / NBSP
// ───────────────────────────────────────────────────────────────────────────

static size_t FindCommentDirectiveEnd8(const uint8_t* p, size_t length)
{
    return Find<uint8_t>(p, length, [](auto d, auto v) {
        const auto controls = hn::And(hn::Ge(v, hn::Set(d, static_cast<uint8_t>(0x09))), hn::Le(v, hn::Set(d, static_cast<uint8_t>(0x0D))));
        const auto space = hn::Eq(v, hn::Set(d, static_cast<uint8_t>(0x20)));
        const auto dquote = hn::Eq(v, hn::Set(d, static_cast<uint8_t>(0x22)));
        const auto squote = hn::Eq(v, hn::Set(d, static_cast<uint8_t>(0x27)));
        const auto nbsp = hn::Eq(v, hn::Set(d, static_cast<uint8_t>(0xA0)));
        return hn::Or(hn::Or(hn::Or(controls, space), hn::Or(dquote, squote)), nbsp);
    });
}

// ───────────────────────────────────────────────────────────────────────────
// LiteralParser (JSON) lexString
// ───────────────────────────────────────────────────────────────────────────

template<typename T>
static HWY_INLINE auto JSONUnsafeStrict(const hn::ScalableTag<T> d, hn::Vec<hn::ScalableTag<T>> v)
{
    const auto quotes = hn::Eq(v, hn::Set(d, static_cast<T>('"')));
    const auto escapes = hn::Eq(v, hn::Set(d, static_cast<T>('\\')));
    const auto controls = hn::Lt(v, hn::Set(d, static_cast<T>(' ')));
    return hn::Or(hn::Or(quotes, escapes), controls);
}

static size_t FindJSONStringEnd8(const uint8_t* p, size_t length)
{
    return Find<uint8_t>(p, length, [](auto d, auto v) { return JSONUnsafeStrict<uint8_t>(d, v); });
}

static size_t FindJSONStringEnd16(const uint16_t* p, size_t length)
{
    return Find<uint16_t>(p, length, [](auto d, auto v) { return JSONUnsafeStrict<uint16_t>(d, v); });
}

template<typename T>
static HWY_INLINE size_t FindJSONStringEndSloppy(const T* p, size_t length, T quote)
{
    return Find<T>(p, length, [quote](auto d, auto v) {
        const auto quotes = hn::Eq(v, hn::Set(d, quote));
        const auto escapes = hn::Eq(v, hn::Set(d, static_cast<T>('\\')));
        const auto controls = hn::Lt(v, hn::Set(d, static_cast<T>(' ')));
        const auto tabs = hn::Eq(v, hn::Set(d, static_cast<T>('\t')));
        return hn::Or(hn::Or(quotes, escapes), hn::AndNot(tabs, controls));
    });
}

static size_t FindJSONStringEndSloppy8(const uint8_t* p, size_t length, uint8_t quote)
{
    return FindJSONStringEndSloppy<uint8_t>(p, length, quote);
}

static size_t FindJSONStringEndSloppy16(const uint16_t* p, size_t length, uint16_t quote)
{
    return FindJSONStringEndSloppy<uint16_t>(p, length, quote);
}

// ───────────────────────────────────────────────────────────────────────────
// FastStringifier copy + escape detect
// ───────────────────────────────────────────────────────────────────────────

template<typename T, bool kCheckSurrogate>
static HWY_INLINE bool StringCopySameType(const T* src, T* dst, size_t length)
{
    const hn::ScalableTag<T> d;
    const size_t N = hn::Lanes(d);
    const auto vQuote = hn::Set(d, static_cast<T>('"'));
    const auto vEscape = hn::Set(d, static_cast<T>('\\'));
    const auto vSpace = hn::Set(d, static_cast<T>(' '));
    const auto vSurMask = hn::Set(d, static_cast<T>(0xF800));
    const auto vSurValue = hn::Set(d, static_cast<T>(0xD800));

    if (length >= N) {
        auto accumulated = hn::MaskFalse(d);
        size_t i = 0;
        for (; i + N <= length; i += N) {
            const auto v = hn::LoadU(d, src + i);
            hn::StoreU(v, d, dst + i);
            auto m = hn::Or(hn::Or(hn::Eq(v, vQuote), hn::Eq(v, vEscape)), hn::Lt(v, vSpace));
            if constexpr (kCheckSurrogate)
                m = hn::Or(m, hn::Eq(hn::And(v, vSurMask), vSurValue));
            accumulated = hn::Or(accumulated, m);
        }
        if (i < length) {
            const auto v = hn::LoadU(d, src + length - N);
            hn::StoreU(v, d, dst + length - N);
            auto m = hn::Or(hn::Or(hn::Eq(v, vQuote), hn::Eq(v, vEscape)), hn::Lt(v, vSpace));
            if constexpr (kCheckSurrogate)
                m = hn::Or(m, hn::Eq(hn::And(v, vSurMask), vSurValue));
            accumulated = hn::Or(accumulated, m);
        }
        return !hn::AllFalse(d, accumulated);
    }
    bool any = false;
    for (size_t i = 0; i < length; ++i) {
        T c = src[i];
        dst[i] = c;
        if constexpr (kCheckSurrogate) {
            if ((c & 0xF800) == 0xD800)
                any = true;
        }
        if (c == '"' || c == '\\' || c < ' ')
            any = true;
    }
    return any;
}

static bool StringCopySameType8(const uint8_t* src, uint8_t* dst, size_t length)
{
    return StringCopySameType<uint8_t, false>(src, dst, length);
}

static bool StringCopySameType16(const uint16_t* src, uint16_t* dst, size_t length)
{
    return StringCopySameType<uint16_t, true>(src, dst, length);
}

static bool StringCopyUpconvert(const uint8_t* src, uint16_t* dst, size_t length)
{
    const hn::ScalableTag<uint8_t> d8;
    const hn::ScalableTag<uint16_t> d16;
    const size_t N = hn::Lanes(d8);
    const auto vQuote = hn::Set(d8, '"');
    const auto vEscape = hn::Set(d8, '\\');
    const auto vSpace = hn::Set(d8, ' ');

    if (length >= N) {
        auto accumulated = hn::MaskFalse(d8);
        size_t i = 0;
        for (; i + N <= length; i += N) {
            const auto v = hn::LoadU(d8, src + i);
            hn::StoreU(hn::PromoteLowerTo(d16, v), d16, dst + i);
            hn::StoreU(hn::PromoteUpperTo(d16, v), d16, dst + i + hn::Lanes(d16));
            accumulated = hn::Or(accumulated, hn::Or(hn::Or(hn::Eq(v, vQuote), hn::Eq(v, vEscape)), hn::Lt(v, vSpace)));
        }
        if (i < length) {
            const size_t off = length - N;
            const auto v = hn::LoadU(d8, src + off);
            hn::StoreU(hn::PromoteLowerTo(d16, v), d16, dst + off);
            hn::StoreU(hn::PromoteUpperTo(d16, v), d16, dst + off + hn::Lanes(d16));
            accumulated = hn::Or(accumulated, hn::Or(hn::Or(hn::Eq(v, vQuote), hn::Eq(v, vEscape)), hn::Lt(v, vSpace)));
        }
        return !hn::AllFalse(d8, accumulated);
    }
    bool any = false;
    for (size_t i = 0; i < length; ++i) {
        uint8_t c = src[i];
        dst[i] = c;
        if (c == '"' || c == '\\' || c < ' ')
            any = true;
    }
    return any;
}

// ───────────────────────────────────────────────────────────────────────────
// Uint8Array.fromHex / toHex
//
// Kept at 16-byte vectors: the published nibble-decode and TableLookupBytes
// algorithms are 128-bit-shaped (16-entry table, u16↔u8 narrowing). Highway
// still gives runtime dispatch across SSSE3/SSE4/NEON; widening these to
// AVX2/512 is a follow-up.
// ───────────────────────────────────────────────────────────────────────────

using D8x16 = hn::CappedTag<uint8_t, 16>;
using D16x8 = hn::CappedTag<uint16_t, 8>;
using D8x8 = hn::Half<D8x16>;

static HWY_INLINE bool VectorDecodeHex8(D8x16 d8, hn::Vec<D8x16> v, uint8_t* out)
{
    const auto t1 = hn::Add(v, hn::Set(d8, static_cast<uint8_t>(0xFF - '9')));
    const auto t2 = hn::SaturatedSub(t1, hn::Set(d8, static_cast<uint8_t>(6)));
    const auto t3 = hn::Sub(t2, hn::Set(d8, static_cast<uint8_t>(0xF0)));
    const auto t4 = hn::And(v, hn::Set(d8, static_cast<uint8_t>(0xDF)));
    const auto t5 = hn::Sub(t4, hn::Set(d8, static_cast<uint8_t>('A')));
    const auto t6 = hn::SaturatedAdd(t5, hn::Set(d8, static_cast<uint8_t>(10)));
    const auto t7 = hn::Min(t3, t6);
    if (HWY_UNLIKELY(!hn::AllFalse(d8, hn::Gt(t7, hn::Set(d8, static_cast<uint8_t>(15))))))
        return false;

    D16x8 d16;
    D8x8 d8h;
    const auto nibbles16 = hn::BitCast(d16, t7);
    const auto low = hn::ShiftRight<8>(nibbles16);
    const auto high = hn::ShiftLeft<4>(nibbles16);
    const auto packed = hn::TruncateTo(d8h, hn::Or(low, high));
    hn::StoreU(packed, d8h, out);
    return true;
}

static size_t DecodeHex8(const uint8_t* src, uint8_t* dst, size_t outLength)
{
    D8x16 d8;
    constexpr size_t stride = 16;
    const size_t inLength = outLength * 2;
    size_t i = 0;
    if (inLength >= stride) {
        for (; i + stride <= inLength; i += stride) {
            if (!VectorDecodeHex8(d8, hn::LoadU(d8, src + i), dst + (i / 2)))
                break;
        }
        if (i + stride > inLength && i < inLength && i > 0 && (i == inLength - (inLength - i))) {
            // Overlapping tail handled below by scalar to keep error-index exact.
        }
    }
    for (; i < inLength; i += 2) {
        auto hex = [](unsigned c) -> int {
            if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
            c |= 0x20;
            if (c >= 'a' && c <= 'f') return static_cast<int>(c - 'a' + 10);
            return -1;
        };
        int hi = hex(src[i]);
        if (hi < 0) return i;
        int lo = hex(src[i + 1]);
        if (lo < 0) return i + 1;
        dst[i / 2] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return SIZE_MAX;
}

static size_t DecodeHex16(const uint16_t* src, uint8_t* dst, size_t outLength)
{
    D8x16 d8;
    constexpr size_t stride = 16;
    const size_t inLength = outLength * 2;
    size_t i = 0;
    if (inLength >= stride) {
        for (; i + stride <= inLength; i += stride) {
            hn::Vec<D8x16> low, high;
            hn::LoadInterleaved2(d8, reinterpret_cast<const uint8_t*>(src + i), low, high);
            if (!hn::AllFalse(d8, hn::Ne(high, hn::Zero(d8))))
                break;
            if (!VectorDecodeHex8(d8, low, dst + (i / 2)))
                break;
        }
    }
    for (; i < inLength; i += 2) {
        auto hex = [](unsigned c) -> int {
            if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
            if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') return static_cast<int>((c | 0x20) - 'a' + 10);
            return -1;
        };
        int hi = hex(src[i]);
        if (hi < 0) return i;
        int lo = hex(src[i + 1]);
        if (lo < 0) return i + 1;
        dst[i / 2] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return SIZE_MAX;
}

static void EncodeHex(const uint8_t* src, uint8_t* dst, size_t length)
{
    D8x16 d8;
    D8x8 d8h;
    D16x8 d16;
    constexpr size_t stride = 8;
    static constexpr uint8_t kTable[16] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };
    const auto table = hn::LoadDup128(d8, kTable);
    const auto vMask = hn::Set(d8, static_cast<uint8_t>(0x0F));

    size_t i = 0;
    if (length >= stride) {
        for (; i + stride <= length; i += stride) {
            const auto widen = hn::PromoteTo(d16, hn::LoadU(d8h, src + i));
            const auto masked = hn::And(hn::BitCast(d8, hn::Or(hn::ShiftLeft<8>(widen), hn::ShiftRight<4>(widen))), vMask);
            hn::StoreU(hn::TableLookupBytes(table, masked), d8, dst + i * 2);
        }
        if (i < length) {
            const size_t off = length - stride;
            const auto widen = hn::PromoteTo(d16, hn::LoadU(d8h, src + off));
            const auto masked = hn::And(hn::BitCast(d8, hn::Or(hn::ShiftLeft<8>(widen), hn::ShiftRight<4>(widen))), vMask);
            hn::StoreU(hn::TableLookupBytes(table, masked), d8, dst + off * 2);
        }
        return;
    }
    for (; i < length; ++i) {
        dst[i * 2] = kTable[src[i] >> 4];
        dst[i * 2 + 1] = kTable[src[i] & 0x0F];
    }
}

} // namespace HWY_NAMESPACE
}} // namespace JSC::Highway
HWY_AFTER_NAMESPACE();

#if HWY_ONCE

namespace JSC { namespace Highway {

HWY_EXPORT(FindIdentifierEnd8);
HWY_EXPORT(FindIdentifierEnd16);
HWY_EXPORT(FindStringEnd8);
HWY_EXPORT(FindStringEnd16);
HWY_EXPORT(FindLineTerminator8);
HWY_EXPORT(FindLineTerminator16);
HWY_EXPORT(FindCommentDirectiveEnd8);
HWY_EXPORT(FindJSONStringEnd8);
HWY_EXPORT(FindJSONStringEnd16);
HWY_EXPORT(FindJSONStringEndSloppy8);
HWY_EXPORT(FindJSONStringEndSloppy16);
HWY_EXPORT(StringCopySameType8);
HWY_EXPORT(StringCopySameType16);
HWY_EXPORT(StringCopyUpconvert);
HWY_EXPORT(DecodeHex8);
HWY_EXPORT(DecodeHex16);
HWY_EXPORT(EncodeHex);

size_t findIdentifierEnd8(const uint8_t* p, size_t n) { return HWY_DYNAMIC_DISPATCH(FindIdentifierEnd8)(p, n); }
size_t findIdentifierEnd16(const uint16_t* p, size_t n) { return HWY_DYNAMIC_DISPATCH(FindIdentifierEnd16)(p, n); }
size_t findStringEnd8(const uint8_t* p, size_t n, uint8_t q) { return HWY_DYNAMIC_DISPATCH(FindStringEnd8)(p, n, q); }
size_t findStringEnd16(const uint16_t* p, size_t n, uint16_t q, bool c) { return HWY_DYNAMIC_DISPATCH(FindStringEnd16)(p, n, q, c); }
size_t findLineTerminator8(const uint8_t* p, size_t n) { return HWY_DYNAMIC_DISPATCH(FindLineTerminator8)(p, n); }
size_t findLineTerminator16(const uint16_t* p, size_t n) { return HWY_DYNAMIC_DISPATCH(FindLineTerminator16)(p, n); }
size_t findCommentDirectiveEnd8(const uint8_t* p, size_t n) { return HWY_DYNAMIC_DISPATCH(FindCommentDirectiveEnd8)(p, n); }
size_t findJSONStringEnd8(const uint8_t* p, size_t n) { return HWY_DYNAMIC_DISPATCH(FindJSONStringEnd8)(p, n); }
size_t findJSONStringEnd16(const uint16_t* p, size_t n) { return HWY_DYNAMIC_DISPATCH(FindJSONStringEnd16)(p, n); }
size_t findJSONStringEndSloppy8(const uint8_t* p, size_t n, uint8_t q) { return HWY_DYNAMIC_DISPATCH(FindJSONStringEndSloppy8)(p, n, q); }
size_t findJSONStringEndSloppy16(const uint16_t* p, size_t n, uint16_t q) { return HWY_DYNAMIC_DISPATCH(FindJSONStringEndSloppy16)(p, n, q); }
bool stringCopySameType8(const uint8_t* s, uint8_t* d, size_t n) { return HWY_DYNAMIC_DISPATCH(StringCopySameType8)(s, d, n); }
bool stringCopySameType16(const uint16_t* s, uint16_t* d, size_t n) { return HWY_DYNAMIC_DISPATCH(StringCopySameType16)(s, d, n); }
bool stringCopyUpconvert(const uint8_t* s, uint16_t* d, size_t n) { return HWY_DYNAMIC_DISPATCH(StringCopyUpconvert)(s, d, n); }
size_t decodeHex8(const uint8_t* s, uint8_t* d, size_t n) { return HWY_DYNAMIC_DISPATCH(DecodeHex8)(s, d, n); }
size_t decodeHex16(const uint16_t* s, uint8_t* d, size_t n) { return HWY_DYNAMIC_DISPATCH(DecodeHex16)(s, d, n); }
void encodeHex(const uint8_t* s, uint8_t* d, size_t n) { HWY_DYNAMIC_DISPATCH(EncodeHex)(s, d, n); }

}} // namespace JSC::Highway

#endif // HWY_ONCE

IGNORE_WARNINGS_END
IGNORE_WARNINGS_END
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
