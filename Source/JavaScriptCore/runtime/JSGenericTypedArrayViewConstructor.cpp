/*
 * Copyright (C) 2024 Devin Rousso <webkit@devinrousso.com>. All rights reserved.
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
#include "HighwayKernels.h"
#include "JSGenericTypedArrayView.h"
#include "JSGenericTypedArrayViewConstructor.h"
#include "JSGenericTypedArrayViewConstructorInlines.h"

#include "JSGenericTypedArrayView.h"
#include "JSGlobalObjectInlines.h"
#include "ParseInt.h"
#include <wtf/text/Base64.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

JSC_DEFINE_HOST_FUNCTION(uint8ArrayConstructorFromBase64, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSString* jsString = dynamicDowncast<JSString>(callFrame->argument(0));
    if (!jsString) [[unlikely]]
        return throwVMTypeError(globalObject, scope, "Uint8Array.fromBase64 requires a string"_s);

    auto alphabet = WTF::Alphabet::Base64;
    auto lastChunkHandling = WTF::LastChunkHandling::Loose;

    JSValue optionsValue = callFrame->argument(1);
    if (!optionsValue.isUndefined()) {
        JSObject* optionsObject = dynamicDowncast<JSObject>(optionsValue);
        if (!optionsValue.isObject()) [[unlikely]]
            return throwVMTypeError(globalObject, scope, "Uint8Array.fromBase64 requires that options be an object"_s);

        JSValue alphabetValue = optionsObject->get(globalObject, vm.propertyNames->alphabet);
        RETURN_IF_EXCEPTION(scope, { });
        if (!alphabetValue.isUndefined()) {
            JSString* alphabetString = dynamicDowncast<JSString>(alphabetValue);
            if (!alphabetString) [[unlikely]]
                return throwVMTypeError(globalObject, scope, "Uint8Array.fromBase64 requires that alphabet be \"base64\" or \"base64url\""_s);

            auto alphabetStringView = alphabetString->view(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            if (alphabetStringView == "base64url"_s)
                alphabet = WTF::Alphabet::Base64URL;
            else if (alphabetStringView != "base64"_s)
                return throwVMTypeError(globalObject, scope, "Uint8Array.fromBase64 requires that alphabet be \"base64\" or \"base64url\""_s);
        }

        JSValue lastChunkHandlingValue = optionsObject->get(globalObject, vm.propertyNames->lastChunkHandling);
        RETURN_IF_EXCEPTION(scope, { });
        if (!lastChunkHandlingValue.isUndefined()) {
            JSString* lastChunkHandlingString = dynamicDowncast<JSString>(lastChunkHandlingValue);
            if (!lastChunkHandlingString) [[unlikely]]
                return throwVMTypeError(globalObject, scope, "Uint8Array.fromBase64 requires that lastChunkHandling be \"loose\", \"strict\", or \"stop-before-partial\""_s);

            auto lastChunkHandlingStringView = lastChunkHandlingString->view(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
            if (lastChunkHandlingStringView == "strict"_s)
                lastChunkHandling = WTF::LastChunkHandling::Strict;
            else if (lastChunkHandlingStringView == "stop-before-partial"_s)
                lastChunkHandling = WTF::LastChunkHandling::StopBeforePartial;
            else if (lastChunkHandlingStringView != "loose"_s)
                return throwVMTypeError(globalObject, scope, "Uint8Array.fromBase64 requires that lastChunkHandling be \"loose\", \"strict\", or \"stop-before-partial\""_s);
        }
    }

    auto gcOwnedData = jsString->view(globalObject);
    StringView view = gcOwnedData;
    RETURN_IF_EXCEPTION(scope, { });

    Vector<uint8_t, 128> output;
    output.grow(maxLengthFromBase64(view));
    auto [shouldThrowError, readLength, writeLength] = fromBase64(view, output.mutableSpan(), alphabet, lastChunkHandling);
    if (shouldThrowError == WTF::FromBase64ShouldThrowError::Yes) [[unlikely]]
        return JSValue::encode(throwSyntaxError(globalObject, scope, "Uint8Array.fromBase64 requires a valid base64 string"_s));

    ASSERT(readLength <= view.length());
    JSUint8Array* uint8Array = JSUint8Array::createUninitialized(globalObject, globalObject->typedArrayStructure(TypeUint8, false), writeLength);
    RETURN_IF_EXCEPTION(scope, { });

    memcpySpan(uint8Array->typedSpan(), output.span().first(writeLength));
    return JSValue::encode(uint8Array);
}

template<typename CharacterType>
[[nodiscard]] inline static size_t decodeHexImpl(std::span<CharacterType> span, std::span<uint8_t> result)
{
    ASSERT(span.size() == result.size() * 2);
    size_t r;
    if constexpr (sizeof(CharacterType) == 1)
        r = Highway::decodeHex8(std::bit_cast<const uint8_t*>(span.data()), result.data(), result.size());
    else
        r = Highway::decodeHex16(std::bit_cast<const uint16_t*>(span.data()), result.data(), result.size());
    return r == SIZE_MAX ? WTF::notFound : r;
}

[[nodiscard]] size_t decodeHex(std::span<const Latin1Character> span, std::span<uint8_t> result)
{
    return decodeHexImpl(span, result);
}

[[nodiscard]] size_t decodeHex(std::span<const char16_t> span, std::span<uint8_t> result)
{
    return decodeHexImpl(span, result);
}

JSC_DEFINE_HOST_FUNCTION(uint8ArrayConstructorFromHex, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSString* jsString = dynamicDowncast<JSString>(callFrame->argument(0));
    if (!jsString) [[unlikely]]
        return throwVMTypeError(globalObject, scope, "Uint8Array.fromHex requires a string"_s);
    if (jsString->length() % 2) [[unlikely]]
        return JSValue::encode(throwSyntaxError(globalObject, scope, "Uint8Array.fromHex requires a string of even length"_s));

    auto gcOwnedData = jsString->view(globalObject);
    StringView view = gcOwnedData;
    RETURN_IF_EXCEPTION(scope, { });

    size_t count = static_cast<size_t>(view.length() / 2);
    JSUint8Array* uint8Array = JSUint8Array::createUninitialized(globalObject, globalObject->typedArrayStructure(TypeUint8, false), count);
    RETURN_IF_EXCEPTION(scope, { });

    uint8_t* data = uint8Array->typedVector();
    auto result = std::span { data, data + count };

    bool success = false;
    if (view.is8Bit())
        success = decodeHex(view.span8(), result) == WTF::notFound;
    else
        success = decodeHex(view.span16(), result) == WTF::notFound;

    if (!success) [[unlikely]]
        return JSValue::encode(throwSyntaxError(globalObject, scope, "Uint8Array.prototype.fromHex requires a string containing only \"0123456789abcdefABCDEF\""_s));

    return JSValue::encode(uint8Array);
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
