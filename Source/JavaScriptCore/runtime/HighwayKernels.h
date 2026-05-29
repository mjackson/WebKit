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

#pragma once

#include <cstddef>
#include <cstdint>

// Runtime-dispatched SIMD kernels backing the JSC lexer, JSON parser/
// stringifier, and Uint8Array hex codec. Implementations live in
// HighwayKernels.cpp under Google Highway's foreach_target multi-versioning.
//
// All find* functions return the index of the first matching element, or
// `length` if no element matches.

namespace JSC { namespace Highway {

// Lexer<T>::parseIdentifier — first char NOT in [A-Za-z0-9_$].
JS_EXPORT_PRIVATE size_t findIdentifierEnd8(const uint8_t*, size_t length);
JS_EXPORT_PRIVATE size_t findIdentifierEnd16(const uint16_t*, size_t length);

// Lexer<T>::parseString — first quote / '\\' / control < 0x0E / (optionally) > 0xFF.
JS_EXPORT_PRIVATE size_t findStringEnd8(const uint8_t*, size_t length, uint8_t quote);
JS_EXPORT_PRIVATE size_t findStringEnd16(const uint16_t*, size_t length, uint16_t quote, bool checkNonLatin1);

// Lexer single-line comment — first '\n' / '\r' / (16-bit only) U+2028 / U+2029.
JS_EXPORT_PRIVATE size_t findLineTerminator8(const uint8_t*, size_t length);
JS_EXPORT_PRIVATE size_t findLineTerminator16(const uint16_t*, size_t length);

// parseCommentDirectiveValue — first whitespace / line-terminator / '"' / '\''.
JS_EXPORT_PRIVATE size_t findCommentDirectiveEnd8(const uint8_t*, size_t length);

// LiteralParser::lexString — first '"' / '\\' / control < 0x20 (strict),
// or first quote / '\\' / control < 0x20 except '\t' (sloppy).
JS_EXPORT_PRIVATE size_t findJSONStringEnd8(const uint8_t*, size_t length);
JS_EXPORT_PRIVATE size_t findJSONStringEnd16(const uint16_t*, size_t length);
JS_EXPORT_PRIVATE size_t findJSONStringEndSloppy8(const uint8_t*, size_t length, uint8_t quote);
JS_EXPORT_PRIVATE size_t findJSONStringEndSloppy16(const uint16_t*, size_t length, uint16_t quote);

// FastStringifier::stringCopy* — copy `length` chars to `dst`, return true if
// any char needs JSON escaping ('"' / '\\' / < 0x20 / surrogate). Caller must
// have ensured `dst` has room for `length` elements.
JS_EXPORT_PRIVATE bool stringCopySameType8(const uint8_t* src, uint8_t* dst, size_t length);
JS_EXPORT_PRIVATE bool stringCopySameType16(const uint16_t* src, uint16_t* dst, size_t length);
JS_EXPORT_PRIVATE bool stringCopyUpconvert(const uint8_t* src, uint16_t* dst, size_t length);

// Uint8Array.fromHex — decode `2*outLength` hex chars to `outLength` bytes.
// Returns SIZE_MAX on success, else the index of the first invalid char.
JS_EXPORT_PRIVATE size_t decodeHex8(const uint8_t* src, uint8_t* dst, size_t outLength);
JS_EXPORT_PRIVATE size_t decodeHex16(const uint16_t* src, uint8_t* dst, size_t outLength);

// Uint8Array.toHex — encode `length` bytes to `2*length` lowercase hex chars.
JS_EXPORT_PRIVATE void encodeHex(const uint8_t* src, uint8_t* dst, size_t length);

}} // namespace JSC::Highway
