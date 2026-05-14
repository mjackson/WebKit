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

// JSC Temporal Core — Temporal enum types
// temporal_rs reference: src/options.rs

#include <cstdint>
#include <wtf/Int128.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringView.h>

namespace JSC {

// https://tc39.es/proposal-temporal/#sec-temporal-gettemporaldisambiguationoption
// temporal_rs: Disambiguation
enum class TemporalDisambiguation : uint8_t {
    Compatible,
    Earlier,
    Later,
    Reject,
};

// https://tc39.es/proposal-temporal/#sec-temporal-gettemporaloffsetoption
// temporal_rs: OffsetDisambiguation
enum class TemporalOffsetDisambiguation : uint8_t {
    Use,
    Prefer,
    Ignore,
    Reject,
};

// OffsetBehaviour encodes the offset source used by interpretISODateTimeOffset.
enum class OffsetBehaviour : uint8_t {
    Wall, // no inline offset in string — resolve via timezone + disambiguation
    Exact, // Z flag — UTC epoch directly
    Option, // +HH:MM present — use inlineOffsetNs
};

// https://tc39.es/proposal-temporal/#sec-temporal-totemporalcalendaridentifier
// temporal_rs: AnyCalendarKind (icu_calendar crate, imported by builtins/core/calendar.rs)
enum class CalendarKind : uint8_t {
    Iso8601 = 0, // must be 0 so zero-initialised objects default to iso8601
    Buddhist,
    Chinese,
    Coptic,
    Dangi,
    Ethiopic,
    EthioAA,
    Gregory,
    Hebrew,
    Indian,
    Islamic,
    IslamicCivil,
    IslamicRGSA,
    IslamicTBLA,
    IslamicUmmAlQura,
    Japanese,
    Persian,
    Roc,
    Bangla,
    Gujarati,
    Kannada,
    Marathi,
    Odia,
    Tamil,
    Telugu,
    Vikram,
};

inline ASCIILiteral toCalendarIdentifier(CalendarKind k)
{
    switch (k) {
    case CalendarKind::Iso8601:
        return "iso8601"_s;
    case CalendarKind::Buddhist:
        return "buddhist"_s;
    case CalendarKind::Chinese:
        return "chinese"_s;
    case CalendarKind::Coptic:
        return "coptic"_s;
    case CalendarKind::Dangi:
        return "dangi"_s;
    case CalendarKind::Ethiopic:
        return "ethiopic"_s;
    case CalendarKind::EthioAA:
        return "ethioaa"_s;
    case CalendarKind::Gregory:
        return "gregory"_s;
    case CalendarKind::Hebrew:
        return "hebrew"_s;
    case CalendarKind::Indian:
        return "indian"_s;
    case CalendarKind::Islamic:
        return "islamic"_s;
    case CalendarKind::IslamicCivil:
        return "islamic-civil"_s;
    case CalendarKind::IslamicRGSA:
        return "islamic-rgsa"_s;
    case CalendarKind::IslamicTBLA:
        return "islamic-tbla"_s;
    case CalendarKind::IslamicUmmAlQura:
        return "islamic-umalqura"_s;
    case CalendarKind::Japanese:
        return "japanese"_s;
    case CalendarKind::Persian:
        return "persian"_s;
    case CalendarKind::Roc:
        return "roc"_s;
    case CalendarKind::Bangla:
        return "bangla"_s;
    case CalendarKind::Gujarati:
        return "gujarati"_s;
    case CalendarKind::Kannada:
        return "kannada"_s;
    case CalendarKind::Marathi:
        return "marathi"_s;
    case CalendarKind::Odia:
        return "odia"_s;
    case CalendarKind::Tamil:
        return "tamil"_s;
    case CalendarKind::Telugu:
        return "telugu"_s;
    case CalendarKind::Vikram:
        return "vikram"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

inline CalendarKind toCalendarKind(WTF::StringView s)
{
    if (s.isEmpty() || s == "iso8601"_s)
        return CalendarKind::Iso8601;
    if (s == "buddhist"_s)
        return CalendarKind::Buddhist;
    if (s == "chinese"_s)
        return CalendarKind::Chinese;
    if (s == "coptic"_s)
        return CalendarKind::Coptic;
    if (s == "dangi"_s)
        return CalendarKind::Dangi;
    if (s == "ethiopic"_s)
        return CalendarKind::Ethiopic;
    if (s == "ethioaa"_s || s == "ethiopic-amete-alem"_s)
        return CalendarKind::EthioAA;
    if (s == "gregory"_s || s == "gregorian"_s)
        return CalendarKind::Gregory;
    if (s == "hebrew"_s)
        return CalendarKind::Hebrew;
    if (s == "indian"_s)
        return CalendarKind::Indian;
    if (s == "islamic"_s)
        return CalendarKind::Islamic;
    if (s == "islamic-civil"_s || s == "islamicc"_s)
        return CalendarKind::IslamicCivil;
    if (s == "islamic-rgsa"_s)
        return CalendarKind::IslamicRGSA;
    if (s == "islamic-tbla"_s)
        return CalendarKind::IslamicTBLA;
    if (s == "islamic-umalqura"_s)
        return CalendarKind::IslamicUmmAlQura;
    if (s == "japanese"_s)
        return CalendarKind::Japanese;
    if (s == "persian"_s)
        return CalendarKind::Persian;
    if (s == "roc"_s)
        return CalendarKind::Roc;
    if (s == "bangla"_s)
        return CalendarKind::Bangla;
    if (s == "gujarati"_s)
        return CalendarKind::Gujarati;
    if (s == "kannada"_s)
        return CalendarKind::Kannada;
    if (s == "marathi"_s)
        return CalendarKind::Marathi;
    if (s == "odia"_s)
        return CalendarKind::Odia;
    if (s == "tamil"_s)
        return CalendarKind::Tamil;
    if (s == "telugu"_s)
        return CalendarKind::Telugu;
    if (s == "vikram"_s)
        return CalendarKind::Vikram;
    return CalendarKind::Iso8601;
}

// -----------------------------------------------------------------------
// Temporal unit
// -----------------------------------------------------------------------

#define JSC_TEMPORAL_PLAIN_DATE_UNITS(macro) \
    macro(year, Year) \
    macro(month, Month) \
    macro(day, Day) \

#define JSC_TEMPORAL_PLAIN_MONTH_DAY_UNITS(macro) \
    macro(month, Month) \
    macro(day, Day)

#define JSC_TEMPORAL_PLAIN_YEAR_MONTH_UNITS(macro) \
    macro(year, Year) \
    macro(month, Month)

#define JSC_TEMPORAL_PLAIN_TIME_UNITS(macro) \
    macro(hour, Hour) \
    macro(minute, Minute) \
    macro(second, Second) \
    macro(millisecond, Millisecond) \
    macro(microsecond, Microsecond) \
    macro(nanosecond, Nanosecond) \

#define JSC_TEMPORAL_UNITS(macro) \
    macro(year, Year) \
    macro(month, Month) \
    macro(week, Week) \
    macro(day, Day) \
    JSC_TEMPORAL_PLAIN_TIME_UNITS(macro) \

// temporal_rs: Unit (src/options.rs)
// https://tc39.es/proposal-temporal/#table-temporal-units
enum class TemporalUnit : uint8_t {
#define JSC_DEFINE_TEMPORAL_UNIT_ENUM(name, capitalizedName) capitalizedName,
    JSC_TEMPORAL_UNITS(JSC_DEFINE_TEMPORAL_UNIT_ENUM)
#undef JSC_DEFINE_TEMPORAL_UNIT_ENUM
};
#define JSC_COUNT_TEMPORAL_UNITS(name, capitalizedName) + 1
static constexpr unsigned numberOfTemporalUnits = 0 JSC_TEMPORAL_UNITS(JSC_COUNT_TEMPORAL_UNITS);
static constexpr unsigned numberOfTemporalPlainDateUnits = 0 JSC_TEMPORAL_PLAIN_DATE_UNITS(JSC_COUNT_TEMPORAL_UNITS);
static constexpr unsigned numberOfTemporalPlainTimeUnits = 0 JSC_TEMPORAL_PLAIN_TIME_UNITS(JSC_COUNT_TEMPORAL_UNITS);
static constexpr unsigned numberOfTemporalPlainYearMonthUnits = 0 JSC_TEMPORAL_PLAIN_YEAR_MONTH_UNITS(JSC_COUNT_TEMPORAL_UNITS);
static constexpr unsigned numberOfTemporalPlainMonthDayUnits = 0 JSC_TEMPORAL_PLAIN_MONTH_DAY_UNITS(JSC_COUNT_TEMPORAL_UNITS);
#undef JSC_COUNT_TEMPORAL_UNITS

extern const TemporalUnit temporalUnitsInTableOrder[numberOfTemporalUnits];

// https://tc39.es/proposal-temporal/#table-temporal-units
constexpr Int128 lengthInNanoseconds(TemporalUnit unit)
{
    switch (unit) {
    case TemporalUnit::Nanosecond:
        return 1;
    case TemporalUnit::Microsecond:
        return 1000;
    case TemporalUnit::Millisecond:
        return 1000 * lengthInNanoseconds(TemporalUnit::Microsecond);
    case TemporalUnit::Second:
        return 1000 * lengthInNanoseconds(TemporalUnit::Millisecond);
    case TemporalUnit::Minute:
        return 60 * lengthInNanoseconds(TemporalUnit::Second);
    case TemporalUnit::Hour:
        return 60 * lengthInNanoseconds(TemporalUnit::Minute);
    case TemporalUnit::Day:
        return 24 * lengthInNanoseconds(TemporalUnit::Hour);
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// -----------------------------------------------------------------------
// Rounding enums
// -----------------------------------------------------------------------

// temporal_rs: RoundingMode (src/options.rs)
// https://tc39.es/proposal-temporal/#sec-temporal-totemporalroundingmode
enum class RoundingMode : uint8_t {
    Ceil,
    Floor,
    Expand,
    Trunc,
    HalfCeil,
    HalfFloor,
    HalfExpand,
    HalfTrunc,
    HalfEven
};

// temporal_rs: UnsignedRoundingMode (src/options.rs)
enum class UnsignedRoundingMode : uint8_t {
    Infinity,
    Zero,
    HalfInfinity,
    HalfZero,
    HalfEven
};

// https://tc39.es/proposal-temporal/#sec-getunsignedroundingmode
// temporal_rs: RoundingMode::get_unsigned_round_mode (src/options.rs)
constexpr UnsignedRoundingMode getUnsignedRoundingMode(RoundingMode roundingMode, bool isNegative)
{
    switch (roundingMode) {
    case RoundingMode::Ceil:
        return isNegative ? UnsignedRoundingMode::Zero : UnsignedRoundingMode::Infinity;
    case RoundingMode::Floor:
        return isNegative ? UnsignedRoundingMode::Infinity : UnsignedRoundingMode::Zero;
    case RoundingMode::Expand:
        return UnsignedRoundingMode::Infinity;
    case RoundingMode::Trunc:
        return UnsignedRoundingMode::Zero;
    case RoundingMode::HalfCeil:
        return isNegative ? UnsignedRoundingMode::HalfZero : UnsignedRoundingMode::HalfInfinity;
    case RoundingMode::HalfFloor:
        return isNegative ? UnsignedRoundingMode::HalfInfinity : UnsignedRoundingMode::HalfZero;
    case RoundingMode::HalfExpand:
        return UnsignedRoundingMode::HalfInfinity;
    case RoundingMode::HalfTrunc:
        return UnsignedRoundingMode::HalfZero;
    default:
        return UnsignedRoundingMode::HalfEven;
    }
}

// temporal_rs: no direct equivalent; used as parameter to ValidateTemporalRoundingIncrement.
enum class Inclusivity : bool {
    Inclusive,
    Exclusive
};

// -----------------------------------------------------------------------
// Arithmetic operation enums
// -----------------------------------------------------------------------

// temporal_rs: Overflow (src/options.rs)
// https://tc39.es/proposal-temporal/#sec-temporal-totemporaloverflow
enum class TemporalOverflow : bool {
    Constrain,
    Reject,
};

// temporal_rs: DifferenceOperation (src/options.rs)
enum class DifferenceOperation : bool {
    Since,
    Until
};

} // namespace JSC
