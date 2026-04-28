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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include <wtf/UnbarrieredMonotonicTime.h>

#include <wtf/ContinuousTime.h>
#include <wtf/MonotonicTime.h>
#include <wtf/PrintStream.h>
#include <wtf/WallTime.h>

namespace WTF {

#if USE(HARDWARE_UNBARRIERED_MONOTONIC_TIME)
UnbarrieredMonotonicTime::Calibration UnbarrieredMonotonicTime::s_calibration;

void UnbarrieredMonotonicTime::calibrate()
{
    constexpr double nanosecondsPerSecond = 1000'000'000;
    if (!s_calibration.nanosecondsPerTick) [[unlikely]] {
        auto readCounterFrequency = [] -> uint64_t {
            uint64_t val;
            __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(val) : : "memory");
            return val;
        };

        auto currentTime = MonotonicTime::now();
        s_calibration.startCntpct = readCounter();
        s_calibration.startNanoseconds = currentTime.secondsSinceEpoch().nanoseconds();
        uint64_t ticksPerSecond = readCounterFrequency();
        s_calibration.nanosecondsPerTick = nanosecondsPerSecond / ticksPerSecond;
    }
}
#endif

WallTime UnbarrieredMonotonicTime::approximateWallTime() const
{
    if (isInfinity())
        return WallTime::fromRawSeconds(m_value);
    return *this - now() + WallTime::now();
}

MonotonicTime UnbarrieredMonotonicTime::approximateMonotonicTime() const
{
    if (isInfinity())
        return MonotonicTime::fromRawSeconds(m_value);
    return *this - now() + MonotonicTime::now();

}

ContinuousTime UnbarrieredMonotonicTime::approximateContinuousTime() const
{
    if (isInfinity())
        return ContinuousTime::fromRawSeconds(m_value);
    return *this - now() + ContinuousTime::now();
}

void UnbarrieredMonotonicTime::dump(PrintStream& out) const
{
    out.print("UnbarrieredMonotonic(", m_value, " sec)");
}

} // namespace WTF
