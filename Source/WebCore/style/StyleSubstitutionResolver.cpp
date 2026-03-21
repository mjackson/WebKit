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

#include "config.h"
#include "StyleSubstitutionResolver.h"

#include "CSSCustomPropertyValue.h"
#include "CSSPendingSubstitutionValue.h"
#include "CSSPropertyNames.h"
#include "CSSPropertyParser.h"
#include "CSSPropertyParserConsumer+Primitives.h"
#include "CSSRegisteredCustomProperty.h"
#include "CSSValueKeywords.h"
#include "CSSVariableData.h"
#include "CSSVariableReferenceValue.h"
#include "ConstantPropertyMap.h"
#include "CustomFunctionRegistry.h"
#include "Document.h"
#include "Element.h"
#include "RenderStyle+GettersInlines.h"
#include "RenderStyle+SettersInlines.h"
#include "StyleBuilder.h"
#include "StyleCustomProperty.h"
#include "StyleCustomPropertyRegistry.h"
#include "StyleResolver.h"
#include "StyleScope.h"

namespace WebCore {
namespace Style {

SubstitutionResolver::SubstitutionResolver(Builder& builder)
    : m_styleBuilder(builder)
{
}

auto SubstitutionResolver::substituteVariableFallback(const AtomString& variableName, CSSParserTokenRange range, CSSValueID functionId, const CSSParserContext& context) const -> std::pair<FallbackResult, Vector<CSSParserToken>> {
    ASSERT(range.atEnd() || range.peek().type() == CommaToken);

    if (range.atEnd())
        return { FallbackResult::None, { } };

    range.consumeIncludingWhitespace();

    auto tokens = substituteTokenRange(range, context);

    if (functionId == CSSValueVar) {
        auto* registered = m_styleBuilder.state().document().customPropertyRegistry().get(variableName);
        if (registered && !registered->syntax.isUniversal()) {
            // https://drafts.css-houdini.org/css-properties-values-api/#fallbacks-in-var-references
            // The fallback value must match the syntax definition of the custom property being referenced,
            // otherwise the declaration is invalid at computed-value time
            if (!tokens || !CSSPropertyParser::isValidCustomPropertyValueForSyntax(registered->syntax, *tokens, context))
                return { FallbackResult::Invalid, { } };

            return { FallbackResult::Valid, WTF::move(*tokens) };
        }
    }

    if (!tokens)
        return { FallbackResult::None, { } };

    return { FallbackResult::Valid, WTF::move(*tokens) };
}

RefPtr<const CustomProperty> SubstitutionResolver::propertyValueForVariableName(const AtomString& variableName, CSSValueID functionId) const
{
    if (functionId == CSSValueEnv)
        return m_styleBuilder.state().document().constantProperties().values().get(variableName);

    // Apply this variable first, in case it is still unresolved
    m_styleBuilder.applyCustomProperty(variableName);

    return protect(m_styleBuilder.state().style())->customPropertyValue(variableName);
}

bool SubstitutionResolver::substituteVariableFunction(CSSParserTokenRange range, CSSValueID functionId, Vector<CSSParserToken>& tokens, const CSSParserContext& context) const
{
    // The maximum number of tokens that may be produced by a substitution function reference or fallback value.
    // https://drafts.csswg.org/css-variables/#long-variables
    static constexpr size_t maxSubstitutionTokens = 65536;

    ASSERT(functionId == CSSValueVar || functionId == CSSValueEnv);

    range.consumeWhitespace();
    ASSERT(range.peek().type() == IdentToken);
    auto variableName = range.consumeIncludingWhitespace().value().toAtomString();

    // Fallback has to be resolved even when not used to detect cycles and invalid syntax.
    auto [fallbackResult, fallbackTokens] = substituteVariableFallback(variableName, range, functionId, context);
    if (fallbackResult == FallbackResult::Invalid)
        return false;

    RefPtr property = propertyValueForVariableName(variableName, functionId);

    if (!property || property->isGuaranteedInvalid()) {
        if (fallbackTokens.size() > maxSubstitutionTokens)
            return false;

        if (fallbackResult == FallbackResult::Valid) {
            tokens.appendVector(fallbackTokens);
            return true;
        }
        return false;
    }

    if (property->tokens().size() > maxSubstitutionTokens)
        return false;

    tokens.appendVector(property->tokens());
    return true;
}

bool SubstitutionResolver::substituteDashedFunction(StringView functionName, CSSParserTokenRange, Vector<CSSParserToken>& tokens) const
{
    // https://drafts.csswg.org/css-mixins/#evaluating-custom-functions

    if (!m_styleBuilder.state().element())
        return false;

    auto scopedFunctionName = ScopedName { functionName.toAtomString(), m_styleBuilder.state().styleScopeOrdinal() };

    CheckedPtr element = m_styleBuilder.state().element();
    auto customFunction = Scope::resolveTreeScopedReference(*element, scopedFunctionName, [](const Scope& scope, const AtomString& name) -> CheckedPtr<const CustomFunction> {
        RefPtr resolver = scope.resolverIfExists();
        CheckedPtr registry = resolver ? resolver->customFunctionRegistry() : nullptr;
        return registry ? registry->functionForName(name) : nullptr;
    });

    if (!customFunction)
        return false;

    // FIXME: Evaluate the function instead of just substituting.

    auto properties = customFunction->properties;
    auto resultValue = dynamicDowncast<CSSCustomPropertyValue>(properties->getPropertyCSSValue(CSSPropertyResult));
    if (!resultValue)
        return false;

    auto data = resultValue->asVariableData();
    tokens.appendVector(data->tokens());
    return true;
}

bool SubstitutionResolver::substituteAttrFunction(CSSParserTokenRange range, Vector<CSSParserToken>& tokens, const CSSParserContext& context) const
{
    // https://drafts.csswg.org/css-values-5/#funcdef-attr
    // attr( <attr-name> <attr-type>? , <declaration-value>?)

    range.consumeWhitespace();

    if (range.peek().type() != IdentToken)
        return false;

    auto attributeName = range.consumeIncludingWhitespace().value().toAtomString();

    // FIXME: Support <attr-type> for non-string types.

    m_styleBuilder.state().registerContentAttribute(attributeName);
    m_styleBuilder.state().style().setHasAttrContent();

    CheckedPtr element = m_styleBuilder.state().element();
    if (!element)
        return false;

    auto& attributeValue = element->getAttribute(QualifiedName { nullAtom(), attributeName, nullAtom() });

    if (attributeValue.isNull()) {
        // Use fallback if available.
        if (range.atEnd())
            return false;

        if (!CSSPropertyParserHelpers::consumeCommaIncludingWhitespace(range))
            return false;

        auto fallbackTokens = substituteTokenRange(range, context);
        if (!fallbackTokens)
            return false;

        tokens.appendVector(*fallbackTokens);
        return true;
    }

    // Default type is string: produce a CSS string token with the attribute value.
    tokens.append(CSSParserToken(StringToken, attributeValue));
    return true;
}

std::optional<Vector<CSSParserToken>> SubstitutionResolver::substituteTokenRange(CSSParserTokenRange range, const CSSParserContext& context) const
{
    Vector<CSSParserToken> tokens;
    bool success = true;
    while (!range.atEnd()) {
        auto token = range.peek();
        if (token.type() == FunctionToken) {
            auto functionId = token.functionId();
            if (functionId == CSSValueVar || functionId == CSSValueEnv) {
                if (!substituteVariableFunction(range.consumeBlock(), functionId, tokens, context))
                    success = false;
                continue;
            }
            if (functionId == CSSValueAttr) {
                if (!substituteAttrFunction(range.consumeBlock(), tokens, context))
                    success = false;
                continue;
            }
            if (isCustomPropertyName(token.value())) {
                // <dashed-function>
                if (!substituteDashedFunction(token.value(), range.consumeBlock(), tokens))
                    success = false;
                continue;
            }
        }
        tokens.append(range.consume());
    }
    if (!success)
        return { };

    return tokens;
}

RefPtr<CSSVariableData> SubstitutionResolver::trySimpleSubstitution(const CSSVariableReferenceValue& value) const
{
    if (!value.m_simpleReference)
        return nullptr;

    // Shortcut for the simple common case of property:var(--foo)

    RefPtr property = propertyValueForVariableName(value.m_simpleReference->name, value.m_simpleReference->functionId);
    if (!property || !std::holds_alternative<Ref<CSSVariableData>>(property->value()))
        return nullptr;

    return std::get<Ref<CSSVariableData>>(property->value()).ptr();
}

RefPtr<CSSVariableData> SubstitutionResolver::substitute(const CSSVariableReferenceValue& value) const
{
    if (auto data = trySimpleSubstitution(value))
        return data;

    auto& context = value.context();
    auto substitutedTokens = substituteTokenRange(value.m_data->tokenRange(), context);
    if (!substitutedTokens)
        return nullptr;

    return CSSVariableData::create(*substitutedTokens, context);
}

RefPtr<CSSValue> SubstitutionResolver::substituteAndParse(const CSSVariableReferenceValue& variableRef, CSSPropertyID propertyID) const
{
    auto data = substitute(variableRef);
    if (!data)
        return nullptr;

    if (!arePointingToEqualData(variableRef.m_cache.dependencyData, data)) {
        variableRef.m_cache.value = CSSPropertyParser::parseStylePropertyLonghand(propertyID, data->tokens(), variableRef.context());
#if ASSERT_ENABLED
        variableRef.m_cache.propertyID = propertyID;
#endif
    }
    variableRef.m_cache.dependencyData = WTF::move(data);

    ASSERT(variableRef.m_cache.propertyID == propertyID);
    return variableRef.m_cache.value;
}

RefPtr<CSSValue> SubstitutionResolver::substituteAndParseShorthand(const CSSPendingSubstitutionValue& substitution, CSSPropertyID propertyID) const
{
    auto& variableRef = substitution.shorthandValue();

    auto data = substitute(variableRef);
    if (!data)
        return nullptr;

    if (!arePointingToEqualData(variableRef.m_cache.dependencyData, data)) {
        ParsedPropertyVector parsedProperties;
        if (!CSSPropertyParser::parseValue(substitution.m_shorthandPropertyId, IsImportant::No, data->tokens(), data->context(), parsedProperties, StyleRuleType::Style))
            substitution.m_cachedPropertyValues = { };
        else
            substitution.m_cachedPropertyValues = parsedProperties;
    }
    variableRef.m_cache.dependencyData = WTF::move(data);

    for (auto& property : substitution.m_cachedPropertyValues) {
        if (property.id() == propertyID)
            return property.value();
    }

    return nullptr;
}

} // namespace Style
} // namespace WebCore
