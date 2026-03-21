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

#include "CSSParserTokenRange.h"

namespace WebCore {

class CSSPendingSubstitutionValue;
class CSSValue;
class CSSVariableData;
class CSSVariableReferenceValue;
struct CSSParserContext;
enum CSSPropertyID : uint16_t;
enum CSSValueID : uint16_t;

namespace Style {

class Builder;
class CustomProperty;

// https://drafts.csswg.org/css-values-5/#arbitrary-substitution
class SubstitutionResolver {
public:
    explicit SubstitutionResolver(Builder&);

    RefPtr<CSSValue> substituteAndParse(const CSSVariableReferenceValue&, CSSPropertyID) const;
    RefPtr<CSSValue> substituteAndParseShorthand(const CSSPendingSubstitutionValue&, CSSPropertyID) const;
    RefPtr<CSSVariableData> substitute(const CSSVariableReferenceValue&) const;

private:
    std::optional<Vector<CSSParserToken>> substituteTokenRange(CSSParserTokenRange, const CSSParserContext&) const;
    bool substituteVariableFunction(CSSParserTokenRange, CSSValueID, Vector<CSSParserToken>&, const CSSParserContext&) const;
    bool substituteDashedFunction(StringView functionName, CSSParserTokenRange, Vector<CSSParserToken>&) const;
    bool substituteAttrFunction(CSSParserTokenRange, Vector<CSSParserToken>&, const CSSParserContext&) const;

    enum class FallbackResult : uint8_t { None, Valid, Invalid };
    std::pair<FallbackResult, Vector<CSSParserToken>> substituteVariableFallback(const AtomString& variableName, CSSParserTokenRange, CSSValueID functionId, const CSSParserContext&) const;

    RefPtr<const CustomProperty> propertyValueForVariableName(const AtomString&, CSSValueID) const;
    RefPtr<CSSVariableData> trySimpleSubstitution(const CSSVariableReferenceValue&) const;

    Builder& m_styleBuilder;
};

} // namespace Style
} // namespace WebCore
