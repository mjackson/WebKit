/*
 * Copyright (C) 2013-2024 Apple Inc. All rights reserved.
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
#include "CryptoDigest.h"

#include "PALSwift.h"
#include <CommonCrypto/CommonCrypto.h>
#include <optional>
#include <span>
#include <wtf/TZoneMallocInlines.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#include "PALSwift-Generated.h"
#pragma clang diagnostic pop

namespace PAL {

struct CryptoDigestContext {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(CryptoDigestContext);

    using CCContext = Variant<
        std::unique_ptr<pal::Digest>,
        std::unique_ptr<CC_SHA1_CTX>,
        std::unique_ptr<CC_SHA256_CTX>,
        std::unique_ptr<CC_SHA512_CTX>
    >;

    CryptoDigest::Algorithm algorithm;
    CCContext ccContext;
};

WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(CryptoDigestContext);

inline CC_SHA1_CTX* toSHA1Context(CryptoDigestContext* context)
{
    ASSERT(context->algorithm == CryptoDigest::Algorithm::SHA_1);
    return static_cast<CC_SHA1_CTX*>(std::get<std::unique_ptr<CC_SHA1_CTX>>(context->ccContext).get());
}
inline CC_SHA256_CTX* toSHA256Context(CryptoDigestContext* context)
{
    ASSERT(context->algorithm == CryptoDigest::Algorithm::SHA_256);
    return static_cast<CC_SHA256_CTX*>(std::get<std::unique_ptr<CC_SHA256_CTX>>(context->ccContext).get());
}
inline CC_SHA512_CTX* toSHA384Context(CryptoDigestContext* context)
{
    ASSERT(context->algorithm == CryptoDigest::Algorithm::SHA_384);
    return static_cast<CC_SHA512_CTX*>(std::get<std::unique_ptr<CC_SHA512_CTX>>(context->ccContext).get());
}
inline CC_SHA512_CTX* toSHA512Context(CryptoDigestContext* context)
{
    ASSERT(context->algorithm == CryptoDigest::Algorithm::SHA_512);
    return static_cast<CC_SHA512_CTX*>(std::get<std::unique_ptr<CC_SHA512_CTX>>(context->ccContext).get());
}

CryptoDigest::CryptoDigest()
    : m_context(WTF::makeUnique<CryptoDigestContext>())
{
}

CryptoDigest::~CryptoDigest()
{
}

static CryptoDigestContext::CCContext createCryptoDigest(CryptoDigest::Algorithm algorithm)
{
    switch (algorithm) {
    case CryptoDigest::Algorithm::SHA_1: {
        return WTF::makeUniqueWithoutFastMallocCheck<pal::Digest>(pal::Digest::sha1Init());
    }
    case CryptoDigest::Algorithm::SHA_256: {
        return WTF::makeUniqueWithoutFastMallocCheck<pal::Digest>(pal::Digest::sha256Init());
    }
    case CryptoDigest::Algorithm::SHA_384: {
        return WTF::makeUniqueWithoutFastMallocCheck<pal::Digest>(pal::Digest::sha384Init());
    }
    case CryptoDigest::Algorithm::SHA_512: {
        return WTF::makeUniqueWithoutFastMallocCheck<pal::Digest>(pal::Digest::sha512Init());
    }
    case CryptoDigest::Algorithm::DEPRECATED_SHA_224: {
        RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("SHA224 is not supported.");
        std::unique_ptr<CC_SHA256_CTX> context = WTF::makeUniqueWithoutFastMallocCheck<CC_SHA256_CTX>();
        CC_SHA256_Init(context.get());
        return context;
    }
    }
}

std::unique_ptr<CryptoDigest> CryptoDigest::create(CryptoDigest::Algorithm algorithm)
{
    std::unique_ptr<CryptoDigest> digest = WTF::makeUnique<CryptoDigest>();
    ASSERT(digest->m_context);
    digest->m_context->algorithm = algorithm;
    digest->m_context->ccContext = createCryptoDigest(algorithm);
    return digest;
}

IGNORE_CLANG_WARNINGS_BEGIN("missing-noreturn")

void CryptoDigest::addBytes(std::span<const uint8_t> input)
{
    switch (m_context->algorithm) {
    case CryptoDigest::Algorithm::SHA_1:
    case CryptoDigest::Algorithm::SHA_256:
    case CryptoDigest::Algorithm::SHA_384:
    case CryptoDigest::Algorithm::SHA_512:
        std::get<std::unique_ptr<pal::Digest>>(m_context->ccContext)->update(input);
        return;
    case CryptoDigest::Algorithm::DEPRECATED_SHA_224:
        RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("SHA224 is not supported.");
        CC_SHA256_Update(toSHA256Context(m_context.get()), static_cast<const void*>(input.data()), input.size());
        return;
    }
}

IGNORE_CLANG_WARNINGS_END

Vector<uint8_t> CryptoDigest::computeHash()
{
    Vector<uint8_t> result;
    switch (m_context->algorithm) {
    case CryptoDigest::Algorithm::SHA_1:
    case CryptoDigest::Algorithm::SHA_256:
    case CryptoDigest::Algorithm::SHA_384:
    case CryptoDigest::Algorithm::SHA_512:
        return std::get<std::unique_ptr<pal::Digest>>(m_context->ccContext)->finalize();
    case CryptoDigest::Algorithm::DEPRECATED_SHA_224:
        RELEASE_ASSERT_NOT_REACHED_WITH_MESSAGE("SHA224 is not supported.");
        break;
    }
    return result;
}

} // namespace PAL
