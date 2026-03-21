// Copyright 2015 The Chromium Authors. All rights reserved.
// Copyright (C) 2016-2021 Apple Inc. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//    * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//    * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//    * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#pragma once

#include <WebCore/CSSPropertyNames.h>
#include <WebCore/CSSValue.h>
#include <WebCore/CSSValueKeywords.h>
#include <WebCore/CSSVariableData.h>
#include <wtf/text/WTFString.h>

namespace WebCore {

class CSSParserToken;
class CSSParserTokenRange;
struct CSSParserContext;

namespace Style {
class SubstitutionResolver;
}

class CSSVariableReferenceValue final : public CSSValue {
public:
    static Ref<CSSVariableReferenceValue> create(const CSSParserTokenRange&, const CSSParserContext&);
    static Ref<CSSVariableReferenceValue> create(Ref<CSSVariableData>&&);

    bool equals(const CSSVariableReferenceValue&) const;
    String customCSSText(const CSS::SerializationContext&) const;

    const CSSParserContext& NODELETE context() const;

    const CSSVariableData& data() const { return m_data.get(); }

private:
    friend class Style::SubstitutionResolver;

    explicit CSSVariableReferenceValue(Ref<CSSVariableData>&&);

    void cacheSimpleReference();

    const Ref<CSSVariableData> m_data;
    mutable String m_stringValue;

    // For quickly resolving simple var(--foo) values.
    struct SimpleReference {
        AtomString name;
        CSSValueID functionId;
    };
    std::optional<SimpleReference> m_simpleReference;

    struct Cache {
        RefPtr<CSSVariableData> dependencyData;
        RefPtr<CSSValue> value;
#if ASSERT_ENABLED
        CSSPropertyID propertyID { CSSPropertyInvalid };
#endif
    };
    mutable Cache m_cache;
};

} // namespace WebCore

SPECIALIZE_TYPE_TRAITS_CSS_VALUE(CSSVariableReferenceValue, isVariableReferenceValue())
