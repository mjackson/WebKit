/*
 * (C) 1999-2003 Lars Knoll (knoll@kde.org)
 * Copyright (C) 2004-2023 Apple Inc. All rights reserved.
 * Copyright (C) 2007 Alexey Proskuryakov <ap@webkit.org>
 * Copyright (C) 2026 Samuel Weinig <sam@webkit.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#pragma once

#include <WebCore/CSSCalcValue.h>
#include <WebCore/CSSPrimitiveNumericUnits.h>
#include <WebCore/CSSValue.h>
#include <WebCore/CSSValueKeywords.h>
#include <WebCore/LayoutUnit.h>
#include <utility>
#include <wtf/Forward.h>
#include <wtf/MathExtras.h>

namespace WebCore {

class CSSToLengthConversionData;
class FontCascade;
class RenderStyle;
class RenderView;

template<typename> class ExceptionOr;

class CSSPrimitiveValue final : public CSSValue {
public:
    // FIXME: Some of these use primitiveUnitType() and some use NODELETE primitiveType(). Many that use primitiveUnitType() are likely broken with calc().
    bool isAngle() const { return unitCategory(primitiveType()) == CSSUnitCategory::Angle; }
    bool isFontIndependentLength() const { return isFontIndependentLength(primitiveUnitType()); }
    bool isFontRelativeLength() const { return isFontRelativeLength(primitiveUnitType()); }
    bool isParentFontRelativeLength() const { return isPercentage() || (isFontRelativeLength() && !isRootFontRelativeLength()); }
    bool isRootFontRelativeLength() const { return isRootFontRelativeLength(primitiveUnitType()); }
    bool isLength() const { return isLength(static_cast<CSSUnitType>(primitiveType())); }
    bool isNumber() const { return primitiveType() == CSSUnitType::CSS_NUMBER; }
    bool isInteger() const { return primitiveType() == CSSUnitType::CSS_INTEGER; }
    bool isNumberOrInteger() const { return isNumber() || isInteger(); }
    bool isPercentage() const { return primitiveType() == CSSUnitType::CSS_PERCENTAGE; }
    bool isPx() const { return primitiveType() == CSSUnitType::CSS_PX; }
    bool isCalculated() const { return primitiveUnitType() == CSSUnitType::CSS_CALC; }
    bool isCalculatedPercentageWithLength() const { return primitiveType() == CSSUnitType::CSS_CALC_PERCENTAGE_WITH_LENGTH; }
    bool isFlex() const { return primitiveType() == CSSUnitType::CSS_FR; }

    bool NODELETE conversionToCanonicalUnitRequiresConversionData() const;

    static Ref<CSSPrimitiveValue> create(double);
    static Ref<CSSPrimitiveValue> create(double, CSSUnitType);
    static Ref<CSSPrimitiveValue> NODELETE createInteger(double);
    static Ref<CSSPrimitiveValue> create(Ref<CSSCalc::Value>);

    ~CSSPrimitiveValue();

    WEBCORE_EXPORT CSSUnitType primitiveType() const;

    // Exposed for DeprecatedCSSOMPrimitiveValue. Throws if conversion to `targetUnit` is not allowed.
    ExceptionOr<float> getFloatValueDeprecated(CSSUnitType targetUnit) const;

    // MARK: Integer (requires `isInteger() == true`)
    template<typename T = int> T resolveAsInteger(const CSSToLengthConversionData&) const;
    template<typename T = int> T resolveAsIntegerNoConversionDataRequired() const;
    template<typename T = int> T resolveAsIntegerDeprecated() const;
    template<typename T = int> std::optional<T> resolveAsIntegerIfNotCalculated() const;

    // MARK: Number (requires `isNumberOrInteger() == true`)
    template<typename T = double> T resolveAsNumber(const CSSToLengthConversionData&) const;
    template<typename T = double> T resolveAsNumberNoConversionDataRequired() const;
    template<typename T = double> T resolveAsNumberDeprecated() const;
    template<typename T = double> std::optional<T> resolveAsNumberIfNotCalculated() const;

    // MARK: Percentage (requires `isPercentage() == true`)
    template<typename T = double> T resolveAsPercentage(const CSSToLengthConversionData&) const;
    template<typename T = double> T resolveAsPercentageNoConversionDataRequired() const;
    template<typename T = double> T resolveAsPercentageDeprecated() const;
    template<typename T = double> std::optional<T> resolveAsPercentageIfNotCalculated() const;

    // MARK: Length (requires `isLength() == true`)
    template<typename T = double> T resolveAsLength(const CSSToLengthConversionData&) const;
    template<typename T = double> T resolveAsLengthNoConversionDataRequired() const;
    template<typename T = double> T resolveAsLengthDeprecated() const;

    // MARK: Non-converting
    template<typename T = double> T value(const CSSToLengthConversionData& conversionData) const { return clampTo<T>(doubleValue(conversionData)); }
    template<typename T = double> T valueNoConversionDataRequired() const { return clampTo<T>(doubleValueNoConversionDataRequired()); }
    template<typename T = double> std::optional<T> valueIfNotCalculated() const;

    using Calc = CSSCalc::Value;
    struct Raw {
        CSSUnitType unit;
        double value;
    };
    template<typename... F> decltype(auto) switchOn(F&&... f) const
    {
        auto visitor = WTF::makeVisitor(std::forward<F>(f)...);
        if (isCalculated())
            return visitor(protect(*m_value.calc));
        return visitor(Raw { primitiveUnitType(), m_value.number });
    }

    // MARK: Divides value by 100 if percentage.
    template<typename T = double> T valueDividingBy100IfPercentage(const CSSToLengthConversionData& conversionData) const { return clampTo<T>(doubleValueDividingBy100IfPercentage(conversionData)); }
    template<typename T = double> T valueDividingBy100IfPercentageNoConversionDataRequired() const { return clampTo<T>(doubleValueDividingBy100IfPercentageNoConversionDataRequired()); }
    template<typename T = double> T valueDividingBy100IfPercentageDeprecated() const { return clampTo<T>(doubleValueDividingBy100IfPercentageDeprecated()); }

    // These return nullopt for calc, for which range checking is not done at parse time: <https://www.w3.org/TR/css3-values/#calc-range>.
    std::optional<bool> NODELETE isZero() const;
    std::optional<bool> NODELETE isOne() const;
    std::optional<bool> NODELETE isPositive() const;
    std::optional<bool> NODELETE isNegative() const;

    WEBCORE_EXPORT String stringValue() const;
    const CSSCalc::Value* cssCalcValue() const { return isCalculated() ? m_value.calc : nullptr; }

    String customCSSText(const CSS::SerializationContext&) const;

    bool equals(const CSSPrimitiveValue&) const;

    static ASCIILiteral unitTypeString(CSSUnitType);

    void collectComputedStyleDependencies(ComputedStyleDependencies&) const;

private:
    friend class CSSValuePool;
    friend class StaticCSSValuePool;
    friend LazyNeverDestroyed<CSSPrimitiveValue>;
    friend bool CSSValue::addHash(Hasher&) const;

    CSSPrimitiveValue(const String&, CSSUnitType);
    CSSPrimitiveValue(double, CSSUnitType);
    explicit CSSPrimitiveValue(Ref<CSSCalc::Value>);

    CSSPrimitiveValue(StaticCSSValueTag, double, CSSUnitType);

    CSSUnitType primitiveUnitType() const { return static_cast<CSSUnitType>(m_primitiveUnitType); }
    void setPrimitiveUnitType(CSSUnitType type) { m_primitiveUnitType = std::to_underlying(type); }

    // MARK: Length converting
    double resolveAsLengthDouble(const CSSToLengthConversionData&) const;

    // MARK: Arbitrarily converting
    double doubleValue(CSSUnitType targetUnit, const CSSToLengthConversionData&) const;
    double doubleValueNoConversionDataRequired(CSSUnitType targetUnit) const;
    double doubleValueDeprecated(CSSUnitType targetUnit) const;

    template<typename T = double> inline T value(CSSUnitType targetUnit, const CSSToLengthConversionData& conversionData) const { return clampTo<T>(doubleValue(targetUnit, conversionData)); }
    template<typename T = double> inline T valueNoConversionDataRequired(CSSUnitType targetUnit) const { return clampTo<T>(doubleValueNoConversionDataRequired(targetUnit)); }
    template<typename T = double> inline T valueDeprecated(CSSUnitType targetUnit) const { return clampTo<T>(doubleValueDeprecated(targetUnit)); }

    // MARK: Non-converting
    double doubleValue(const CSSToLengthConversionData&) const;
    double doubleValueNoConversionDataRequired() const
    {
        ASSERT(!isCalculated());
        return m_value.number;
    }
    double doubleValueDeprecated() const;
    double doubleValueDividingBy100IfPercentage(const CSSToLengthConversionData&) const;
    double NODELETE doubleValueDividingBy100IfPercentageNoConversionDataRequired() const;
    double doubleValueDividingBy100IfPercentageDeprecated() const;
    template<typename T = double> inline T valueDeprecated() const { return clampTo<T>(doubleValueDeprecated()); }

    static std::optional<double> NODELETE conversionToCanonicalUnitsScaleFactor(CSSUnitType);

    std::optional<double> doubleValueInternal(CSSUnitType targetUnit, const CSSToLengthConversionData&) const;
    std::optional<double> doubleValueInternalDeprecated(CSSUnitType targetUnit) const;

    bool NODELETE addDerivedHash(Hasher&) const;

    ALWAYS_INLINE String serializeInternal(const CSS::SerializationContext&) const;
    NEVER_INLINE String formatNumberValue(ASCIILiteral suffix) const;
    NEVER_INLINE String formatIntegerValue(ASCIILiteral suffix) const;

    static constexpr bool isLength(CSSUnitType);
    static constexpr bool isFontIndependentLength(CSSUnitType);
    static constexpr bool isFontRelativeLength(CSSUnitType);
    static constexpr bool isRootFontRelativeLength(CSSUnitType);
    static constexpr bool isContainerPercentageLength(CSSUnitType);
    static constexpr bool isViewportPercentageLength(CSSUnitType);

    union {
        double number;
        const CSSCalc::Value* calc;
    } m_value;
};

constexpr bool CSSPrimitiveValue::isFontIndependentLength(CSSUnitType type)
{
    return type == CSSUnitType::CSS_PX
        || type == CSSUnitType::CSS_CM
        || type == CSSUnitType::CSS_MM
        || type == CSSUnitType::CSS_IN
        || type == CSSUnitType::CSS_PT
        || type == CSSUnitType::CSS_PC;
}

constexpr bool CSSPrimitiveValue::isRootFontRelativeLength(CSSUnitType type)
{
    return type == CSSUnitType::CSS_RCAP
        || type == CSSUnitType::CSS_RCH
        || type == CSSUnitType::CSS_REM
        || type == CSSUnitType::CSS_REX
        || type == CSSUnitType::CSS_RIC
        || type == CSSUnitType::CSS_RLH;
}

constexpr bool CSSPrimitiveValue::isFontRelativeLength(CSSUnitType type)
{
    return type == CSSUnitType::CSS_EM
        || type == CSSUnitType::CSS_EX
        || type == CSSUnitType::CSS_LH
        || type == CSSUnitType::CSS_CAP
        || type == CSSUnitType::CSS_CH
        || type == CSSUnitType::CSS_IC
        || type == CSSUnitType::CSS_QUIRKY_EM
        || isRootFontRelativeLength(type);
}

constexpr bool CSSPrimitiveValue::isContainerPercentageLength(CSSUnitType type)
{
    return type == CSSUnitType::CSS_CQW
        || type == CSSUnitType::CSS_CQH
        || type == CSSUnitType::CSS_CQI
        || type == CSSUnitType::CSS_CQB
        || type == CSSUnitType::CSS_CQMIN
        || type == CSSUnitType::CSS_CQMAX;
}

constexpr bool CSSPrimitiveValue::isLength(CSSUnitType type)
{
    return type == CSSUnitType::CSS_EM
        || type == CSSUnitType::CSS_EX
        || type == CSSUnitType::CSS_PX
        || type == CSSUnitType::CSS_CM
        || type == CSSUnitType::CSS_MM
        || type == CSSUnitType::CSS_IN
        || type == CSSUnitType::CSS_PT
        || type == CSSUnitType::CSS_PC
        || type == CSSUnitType::CSS_Q
        || isFontRelativeLength(type)
        || isViewportPercentageLength(type)
        || isContainerPercentageLength(type)
        || type == CSSUnitType::CSS_QUIRKY_EM;
}

constexpr bool CSSPrimitiveValue::isViewportPercentageLength(CSSUnitType type)
{
    return type >= CSSUnitType::FirstViewportCSSUnitType && type <= CSSUnitType::LastViewportCSSUnitType;
}

template<typename T> std::optional<T> CSSPrimitiveValue::valueIfNotCalculated() const
{
    if (isCalculated())
        return std::nullopt;
    return m_value.number;
}

// MARK: Integer

template<typename T> T CSSPrimitiveValue::resolveAsInteger(const CSSToLengthConversionData& conversionData) const
{
    ASSERT(isInteger());
    return value<T>(conversionData);
}

template<typename T> T CSSPrimitiveValue::resolveAsIntegerNoConversionDataRequired() const
{
    ASSERT(isInteger());
    return valueNoConversionDataRequired<T>();
}

template<typename T> T CSSPrimitiveValue::resolveAsIntegerDeprecated() const
{
    ASSERT(isInteger());
    return valueDeprecated<T>();
}

template<typename T> std::optional<T> CSSPrimitiveValue::resolveAsIntegerIfNotCalculated() const
{
    ASSERT(isInteger());
    return valueIfNotCalculated<T>();
}

// MARK: Number

template<typename T> T CSSPrimitiveValue::resolveAsNumber(const CSSToLengthConversionData& conversionData) const
{
    ASSERT(isNumberOrInteger());
    return value<T>(CSSUnitType::CSS_NUMBER, conversionData);
}

template<typename T> T CSSPrimitiveValue::resolveAsNumberNoConversionDataRequired() const
{
    ASSERT(isNumberOrInteger());
    return valueNoConversionDataRequired<T>(CSSUnitType::CSS_NUMBER);
}

template<typename T> T CSSPrimitiveValue::resolveAsNumberDeprecated() const
{
    ASSERT(isNumberOrInteger());
    return valueDeprecated<T>(CSSUnitType::CSS_NUMBER);
}

template<typename T> std::optional<T> CSSPrimitiveValue::resolveAsNumberIfNotCalculated() const
{
    ASSERT(isNumberOrInteger());
    return valueIfNotCalculated<T>();
}

// MARK: Percentage

template<typename T> T CSSPrimitiveValue::resolveAsPercentage(const CSSToLengthConversionData& conversionData) const
{
    ASSERT(isPercentage());
    return value<T>(conversionData);
}

template<typename T> T CSSPrimitiveValue::resolveAsPercentageNoConversionDataRequired() const
{
    ASSERT(isPercentage());
    return valueNoConversionDataRequired<T>();
}

template<typename T> T CSSPrimitiveValue::resolveAsPercentageDeprecated() const
{
    ASSERT(isPercentage());
    return valueDeprecated<T>();
}

template<typename T> std::optional<T> CSSPrimitiveValue::resolveAsPercentageIfNotCalculated() const
{
    ASSERT(isPercentage());
    return valueIfNotCalculated<T>();
}

// MARK: Length

template<typename T> T CSSPrimitiveValue::resolveAsLengthNoConversionDataRequired() const
{
    ASSERT(isLength());
    return valueNoConversionDataRequired<T>(CSSUnitType::CSS_PX);
}

template<typename T> T CSSPrimitiveValue::resolveAsLengthDeprecated() const
{
    ASSERT(isLength());
    return valueDeprecated<T>(CSSUnitType::CSS_PX);
}

inline bool CSSValue::isInteger() const
{
    auto* value = dynamicDowncast<CSSPrimitiveValue>(*this);
    return value && value->isInteger();
}

inline int CSSValue::integer(const CSSToLengthConversionData& conversionData) const
{
    ASSERT(isInteger());
    return downcast<CSSPrimitiveValue>(*this).resolveAsInteger(conversionData);
}

inline int CSSValue::integerDeprecated() const
{
    ASSERT(isInteger());
    return downcast<CSSPrimitiveValue>(*this).resolveAsIntegerDeprecated();
}

void add(Hasher&, const CSSPrimitiveValue&);

} // namespace WebCore

SPECIALIZE_TYPE_TRAITS_CSS_VALUE(CSSPrimitiveValue, isPrimitiveValue())
