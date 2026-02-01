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
#include "StreamTransferUtilities.h"

#include "JSDOMException.h"
#include "MessagePort.h"
#include "ReadableStream.h"
#include "ReadableStreamSource.h"
#include "StructuredSerializeOptions.h"
#include <JavaScriptCore/ObjectConstructor.h>

namespace WebCore {

// https://streams.spec.whatwg.org/#abstract-opdef-packandpostmessage
static ExceptionOr<void> packAndPostMessage(JSDOMGlobalObject& globalObject, MessagePort& port, const String& type, JSC::JSValue value)
{
    Ref vm = globalObject.vm();
    Locker locker(vm->apiLock());
    auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

    auto* data = JSC::constructEmptyObject(&globalObject);
    if (catchScope.exception()) [[unlikely]]
        return Exception { ExceptionCode::InvalidStateError, "Unable to post message"_s };
    JSC::Strong<JSC::JSObject> strongData(vm, data);

    auto* jsType = JSC::jsNontrivialString(vm.get(), type);
    if (catchScope.exception()) [[unlikely]]
        return Exception { ExceptionCode::InvalidStateError, "Unable to post message"_s };
    JSC::Strong<JSC::JSString> strongType(vm.get(), jsType);

    data->putDirect(vm.get(), vm->propertyNames->type, jsType);
    if (catchScope.exception()) [[unlikely]]
        return Exception { ExceptionCode::InvalidStateError, "Unable to set value"_s };

    data->putDirect(vm.get(), vm->propertyNames->value, value);
    if (catchScope.exception()) [[unlikely]]
        return Exception { ExceptionCode::InvalidStateError, "Unable to set value"_s };

    return port.postMessage(globalObject, data, StructuredSerializeOptions { });
}

// https://streams.spec.whatwg.org/#abstract-opdef-crossrealmtransformsenderror
static void crossRealmTransformSendError(JSDOMGlobalObject& globalObject, MessagePort& port, JSC::JSValue error)
{
    packAndPostMessage(globalObject, port, "error"_s, error);
}

static ExceptionOr<void> packAndPostMessageHandlingError(JSDOMGlobalObject& globalObject, MessagePort& port, const String& type, JSC::JSValue value)
{
    auto result = packAndPostMessage(globalObject, port, type, value);
    if (result.hasException())
        crossRealmTransformSendError(globalObject, port, toJS(&globalObject, &globalObject, DOMException::create(result.exception()).get()));
    return result;
}

class CrossRealmReadableStreamSource final : public ReadableStreamSource, public RefCountedAndCanMakeWeakPtr<CrossRealmReadableStreamSource> {
public:
    static Ref<CrossRealmReadableStreamSource> create(Ref<MessagePort>&& port)
    {
        Ref source = adoptRef(*new CrossRealmReadableStreamSource(WTF::move(port)));
        source->m_port->setMessageHandler([weakSource = WeakPtr { source }](auto& globalObject, auto& value) {
            RefPtr protectedSource = weakSource.get();
            if (!protectedSource)
                return;

            auto& vm = globalObject.vm();
            Locker locker(vm.apiLock());
            auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

            bool didFail = false;
            auto deserialized = value.deserialize(globalObject, &globalObject, { }, SerializationErrorMode::NonThrowing, &didFail);
            bool isSuccess = [&] {
                if (catchScope.exception() || didFail) [[unlikely]]
                    return false;

                auto* object = deserialized.getObject();
                if (!object)
                    return false;

                JSC::Strong<JSC::JSObject> strongObject(vm, object);
                return protectedSource->handleMessage(globalObject, *object);
            }();

            if (!isSuccess)
                protectedSource->handleMessageError(globalObject);
        });
        return source;
    }

    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

private:
    explicit CrossRealmReadableStreamSource(Ref<MessagePort>&& port)
        : m_port(WTF::move(port))
    {
    }

    bool handleMessage(JSDOMGlobalObject& globalObject, JSC::JSObject& object)
    {
        Ref vm = globalObject.vm();
        auto catchScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

        auto typeValue = object.get(&globalObject, vm->propertyNames->type);
        if (catchScope.exception()) [[unlikely]]
            return false;
        auto value = object.get(&globalObject, vm->propertyNames->value);
        if (catchScope.exception()) [[unlikely]]
            return false;

        if (!typeValue.isString())
            return false;

        String type = JSC::asString(typeValue)->tryGetValue();
        if (type == "chunk"_s) {
            if (controller().enqueue(value))
                pullFinished();
            return true;
        }
        if (type == "close"_s) {
            controller().close();
            return true;
        }
        if (type == "error"_s) {
            errorStream(globalObject, value);
            return true;
        }

        errorStream(globalObject, createDOMException(&globalObject, ExceptionCode::TypeError, "Unexpected value"_s));
        return true;
    }

    void handleMessageError(JSDOMGlobalObject& globalObject)
    {
        errorStream(globalObject, createDOMException(&globalObject, ExceptionCode::DataCloneError, "Failed to deserialize value"_s));
    }

    void errorStream(JSDOMGlobalObject& globalObject, JSC::JSValue error)
    {
        Ref vm = globalObject.vm();
        JSC::Strong<JSC::Unknown> strongError(vm.get(), error);
        crossRealmTransformSendError(globalObject, m_port, error);
        this->error(globalObject, error);
        m_port->close();
    }

    // ReadableStreamSource
    void setActive() final { };
    void setInactive() final { };

    void doStart() final { startFinished(); }
    void doPull() final
    {
        RefPtr context = m_port->scriptExecutionContext();
        auto* globalObject = context ? context->globalObject() : nullptr;
        if (!globalObject)
            return;

        packAndPostMessage(*JSC::jsCast<JSDOMGlobalObject*>(globalObject), m_port.get(), "pull"_s, JSC::jsUndefined());
    }
    void doCancel(JSC::JSValue reason) final
    {
        // FIXME: Reject cancel promise in case of error.
        RefPtr context = m_port->scriptExecutionContext();
        auto* globalObject = context ? context->globalObject() : nullptr;
        if (!globalObject)
            return;

        packAndPostMessageHandlingError(*JSC::jsCast<JSDOMGlobalObject*>(globalObject), m_port.get(), "error"_s, reason);
        m_port->close();
    }

    const Ref<MessagePort> m_port;
};

ExceptionOr<Ref<ReadableStream>> setupCrossRealmTransformReadable(JSDOMGlobalObject& globalObject, MessagePort& port)
{
    return ReadableStream::create(globalObject, CrossRealmReadableStreamSource::create(port));
}

} // namespace WebCore
