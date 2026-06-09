/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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

#include "Identifier.h"
#include "JSString.h"
#include "KeyAtomStringCache.h"
#include "Options.h"
#include "SmallStrings.h"
#include "VM.h"
#include <atomic>
#include <stdlib.h>
#include <wtf/DataLog.h>

namespace JSC {

template<typename Buffer, typename Func>
ALWAYS_INLINE JSString* KeyAtomStringCache::make(VM& vm, Buffer& buffer, const Func& func)
{
    if (buffer.characters.empty())
        return jsEmptyString(vm);

    if (buffer.characters.size() == 1) {
        auto firstCharacter = buffer.characters[0];
        if (firstCharacter <= maxSingleCharacterString)
            return vm.smallStrings.singleCharacterString(firstCharacter);
    }

    ASSERT(buffer.characters.size() <= maxStringLengthForCache);
    auto& slot = m_cache[buffer.hash % capacity];
    // SNAPSHOT the slot once. The slot is shared by all lites under GIL-off
    // and ping-pongs between same-bucket atoms (e.g. "p8"/"p17" both hash to
    // bucket 357 of 512). We verify the snapshot and return the SAME
    // snapshot; re-reading the slot after verification can return a
    // different, also-valid atom and silently resolve the wrong property key
    // (UNGIL Race C; see KeyAtomStringCache.h).
    // BUGHUNT r3 ARM-B' instrumentation (REVERT BEFORE LANDING ANYTHING):
    // - one-shot provenance canary so every campaign log self-certifies the
    //   binary contains the snapshot-return fix;
    // - after a successful verification, widen the verify->return window with
    //   a spin (GIL-off, p-keys only), re-load the slot, and log KEYATOM-RACE
    //   when the reload differs from the verified snapshot;
    // - default arm returns the VERIFIED snapshot (shipped semantics);
    //   env KEYATOM_REGRESS=1 returns the RELOAD (pre-fix bug restored).
    static std::atomic<int> s_regressArm { -1 };
    if (Options::useJSThreads() && !Options::useThreadGIL()) {
        static std::atomic<unsigned> s_loggedActive;
        if (!s_loggedActive.exchange(1, std::memory_order_relaxed)) {
            s_regressArm.store(!!getenv("KEYATOM_REGRESS"), std::memory_order_relaxed);
            dataLogLn("KEYATOM-SNAPSHOT-FIX-ACTIVE regressArm=", !!getenv("KEYATOM_REGRESS"));
        }
    }
    if (JSString* cached = slot.load(std::memory_order_acquire)) {
        if (auto* impl = cached->tryGetValueImpl()) {
            if (impl->hash() == buffer.hash && equal(impl, buffer.characters)) {
                if (Options::useJSThreads() && !Options::useThreadGIL()
                    && buffer.characters.size() >= 2 && buffer.characters.size() <= 3 && buffer.characters[0] == 'p') {
                    for (volatile int spin = 0; spin < 30000; ++spin) { }
                    JSString* reloaded = slot.load(std::memory_order_acquire);
                    if (reloaded != cached) [[unlikely]] {
                        auto* rimpl = reloaded ? reloaded->tryGetValueImpl() : nullptr;
                        dataLogLn("KEYATOM-RACE idx=", buffer.hash % capacity,
                            " requested=", String(buffer.characters),
                            " verified=", String(impl),
                            " reloaded=", rimpl ? String(rimpl) : String("<null-or-rope>"_s));
                        if (s_regressArm.load(std::memory_order_relaxed) == 1)
                            return reloaded; // ARM-REGRESS: pre-fix `return slot;` reload semantics.
                    }
                }
                return cached;
            }
        }
    }

    JSString* result = func(vm, buffer);
    if (result) [[likely]]
        slot.store(result, std::memory_order_release);
    return result;
}

} // namespace JSC
