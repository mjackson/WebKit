/*
 * Copyright (C) 2025 Apple Inc. All rights reserved.
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
#include "JSPromiseAllGlobalContext.h"

#include "JSCInlines.h"

namespace JSC {

const ClassInfo JSPromiseAllGlobalContext::s_info = { "PromiseAllGlobalContext"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(JSPromiseAllGlobalContext) };

JSPromiseAllGlobalContext* JSPromiseAllGlobalContext::create(VM& vm, JSValue promise, JSValue values, JSValue remainingElementsCount)
{
    auto* structure = vm.promiseAllGlobalContextStructure.get();
    JSPromiseAllGlobalContext* result = new (NotNull, allocateCell<JSPromiseAllGlobalContext>(vm)) JSPromiseAllGlobalContext(vm, structure, promise, values, remainingElementsCount);
    result->finishCreation(vm);
    return result;
}

Structure* JSPromiseAllGlobalContext::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(JSPromiseAllGlobalContextType, StructureFlags), info());
}

template<typename Visitor>
void JSPromiseAllGlobalContext::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = jsCast<JSPromiseAllGlobalContext*>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_promise);
    visitor.append(thisObject->m_values);
    visitor.append(thisObject->m_remainingElementsCount);
}

DEFINE_VISIT_CHILDREN(JSPromiseAllGlobalContext);

} // namespace JSC
