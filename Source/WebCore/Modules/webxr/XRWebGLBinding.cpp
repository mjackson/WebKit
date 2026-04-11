/*
 * Copyright (C) 2024 Apple, Inc. All rights reserved.
 * Copyright (C) 2026 Igalia, S.L. All rights reserved.
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
#include "XRWebGLBinding.h"

#if ENABLE(WEBXR_LAYERS)

#include "ExceptionOr.h"
#include "WebGL2RenderingContext.h"
#include "WebGLOpaqueTexture.h"
#include "WebGLRenderingContext.h"
#include "WebGLRenderingContextBase.h"
#include "WebXRSession.h"
#include "WebXRView.h"
#include "WebXRViewport.h"
#include "XRProjectionLayer.h"
#include "XRProjectionLayerInit.h"
#include "XRWebGLProjectionLayerBacking.h"
#include "XRWebGLSubImage.h"
#include <wtf/Ref.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

using GL = GraphicsContextGL;

WTF_MAKE_TZONE_ALLOCATED_IMPL(XRWebGLBinding);

ExceptionOr<Ref<XRWebGLBinding>> XRWebGLBinding::create(Ref<WebXRSession>&& session, WebXRWebGLRenderingContext&& context)
{
    if (session->ended())
        return Exception { ExceptionCode::InvalidStateError, "Cannot create an XRWebGLBinding with an XRSession that has ended."_s };

    return WTF::switchOn(WTF::move(context),
        [&](auto&& context) -> ExceptionOr<Ref<XRWebGLBinding>> {
            if (context->isContextLost())
                return Exception { ExceptionCode::InvalidStateError, "Cannot create an XRWebGLBinding with a lost WebGL context."_s };

            if (!isImmersive(session->mode()))
                return Exception { ExceptionCode::InvalidStateError, "Cannot create an XRWebGLBinding for non immersive sessions."_s };

            if (!context->isXRCompatible())
                return Exception { ExceptionCode::InvalidStateError, "Cannot create an XRWebGLBinding with a non XR compatible WebGL context."_s };

            return adoptRef(*new XRWebGLBinding(WTF::move(session), WTF::move(context)));
        }
    );
}

XRWebGLBinding::XRWebGLBinding(Ref<WebXRSession>&& session, WebXRWebGLRenderingContext&& context)
    : m_session(WTF::move(session))
    , m_context(WTF::move(context))
{
}

// https://immersive-web.github.io/layers/#initialize-a-composition-layer
void XRWebGLBinding::initializeCompositionLayer(XRCompositionLayer& layer)
{
    layer.setBlendTextureSourceAlpha(true);
    layer.setOpacity(1.0);
}

// https://immersive-web.github.io/layers/#determine-the-layout-attribute
ExceptionOr<XRLayerLayout> XRWebGLBinding::determineLayout(XRTextureType textureType, XRLayerLayout layout)
{
    if (layout == XRLayerLayout::Mono)
        return layout;

    auto determineLayoutInternal = [&]() {
        if (layout == XRLayerLayout::Default) {
            if (m_session->views().size() == 1)
                return XRLayerLayout::Mono;
            if (textureType == XRTextureType::TextureArray)
                return layout;
        }
        if (layout == XRLayerLayout::Default || layout == XRLayerLayout::Stereo)
            return XRLayerLayout::StereoLeftRight;
        return layout;
    };
    return WTF::switchOn(m_context,
        [&](const Ref<WebGL2RenderingContext>&) -> ExceptionOr<XRLayerLayout> {
            return determineLayoutInternal();
        },
        [&](const Ref<WebGLRenderingContext>&) -> ExceptionOr<XRLayerLayout> {
            if (textureType == XRTextureType::TextureArray)
                return Exception { ExceptionCode::TypeError };
            return determineLayoutInternal();
        },
        [](std::monostate) {
            ASSERT_NOT_REACHED();
            return XRLayerLayout::Mono;
        });
}

// https://immersive-web.github.io/layers/#list-of-color-formats-for-projection-layers
bool XRWebGLBinding::colorFormatIsSupportedForProjectionLayer(GCGLenum textureFormat) const
{
    Vector<GCGLenum> supportedFormats = { GL::RGBA, GL::RGB };
    return WTF::switchOn(m_context,
        [&](const Ref<WebGL2RenderingContext>&) -> bool {
            const Vector<GCGLenum> supportedWebGL2Formats = { GL::RGBA8, GL::RGB8, GL::SRGB8, GL::SRGB8_ALPHA8 };
            return supportedFormats.contains(textureFormat) || supportedWebGL2Formats.contains(textureFormat);
        },
        [&](const Ref<WebGLRenderingContext>& context) -> bool {
            const Vector<GCGLenum> additionalWebGLFormats = { GL::SRGB_EXT, GL::SRGB_ALPHA_EXT };
            return supportedFormats.contains(textureFormat) || (additionalWebGLFormats.contains(textureFormat) && context->extensionIsEnabled("EXT_sRGB"_s));
        },
        [](std::monostate) {
            ASSERT_NOT_REACHED();
            return false;
        });
}

// https://immersive-web.github.io/layers/#list-of-depth-formats-for-projection-layers
bool XRWebGLBinding::depthFormatIsSupportedForProjectionLayer(GCGLenum textureFormat) const
{
    Vector<GCGLenum> supportedFormats = { GL::DEPTH_COMPONENT, GL::DEPTH_STENCIL };
    return WTF::switchOn(m_context,
        [&](const Ref<WebGL2RenderingContext>&) -> bool {
            const Vector<GCGLenum> supportedWebGL2Formats = { GL::DEPTH_COMPONENT24, GL::DEPTH24_STENCIL8 };
            return supportedFormats.contains(textureFormat) || supportedWebGL2Formats.contains(textureFormat);
        },
        [&](const Ref<WebGLRenderingContext>& context) -> bool {
            return supportedFormats.contains(textureFormat) && context->extensionIsEnabled("WEBGL_depth_texture"_s);
        },
        [](std::monostate) {
            ASSERT_NOT_REACHED();
            return false;
        });
}


ExceptionOr<Ref<XRProjectionLayer>> XRWebGLBinding::createProjectionLayer(ScriptExecutionContext& scriptExecutionContext, const XRProjectionLayerInit& init)
{
    if (m_session->ended())
        return Exception { ExceptionCode::InvalidStateError, "Cannot create an projection layer with an XRSession that has ended."_s };

    return WTF::switchOn(m_context,
        [&](const Ref<WebGLRenderingContextBase>& baseContext) -> ExceptionOr<Ref<XRProjectionLayer>> {
            if (baseContext->isContextLost())
                return Exception { ExceptionCode::InvalidStateError, "Cannot create a projection layer with a lost WebGL context"_s };

            // The following two checks are really part of the allocate textures algorithm, but we need to fail early for the projection layer case
            // as the allocation happens lazily when getViewSubImage() is called.
            if (!colorFormatIsSupportedForProjectionLayer(init.colorFormat))
                return Exception { ExceptionCode::NotSupportedError, "Unsupported texture format."_s };

            if (init.depthFormat && !depthFormatIsSupportedForProjectionLayer(init.depthFormat))
                return Exception { ExceptionCode::NotSupportedError, "Unsupported depth texture format."_s };

            auto createBackingResult = XRWebGLProjectionLayerBacking::create(m_session, baseContext, init);
            if (createBackingResult.hasException())
                return createBackingResult.releaseException();
            auto backing = createBackingResult.releaseReturnValue();

            auto layer = XRProjectionLayer::create(scriptExecutionContext, m_session, WTF::move(backing), init);
            initializeCompositionLayer(layer.get());
            layer->setIsStatic(false);
            layer->setIgnoreDepthValues(!!init.depthFormat);
            layer->setFixedFoveation(0);

            auto layoutResult = determineLayout(init.textureType, XRLayerLayout::Default);
            if (layoutResult.hasException())
                return layoutResult.releaseException();
            auto layout = layoutResult.releaseReturnValue();
            layer->setLayout(layout);
            layer->setNeedsRedraw(true);

            return layer;
        },
        [](std::monostate) {
            ASSERT_NOT_REACHED();
            return Exception { ExceptionCode::OperationError, "Could not get a WebGL rendering context."_s };
        }
    );
}

bool XRWebGLBinding::validateXRWebGLSubImageCreation(const XRProjectionLayer& layer, const WebXRFrame& frame) const
{

    if (&frame.session() != layer.session())
        return false;

    if (!frame.isActive())
        return false;

    if (!frame.isAnimationFrame())
        return false;

    if (&m_session.get() != layer.session())
        return false;

    if (layer.colorTextures().isEmpty())
        return false;

    if (layer.isStatic() && !layer.needsRedraw())
        return false;

    return true;
}

IntRect XRWebGLBinding::rectForView(const XRProjectionLayer& layer, const WebXRView& view) const
{
    // If the layer is not side-by-side return the full texture size adjusted by the viewport scale.
    if (layer.textureArrayLength() > 1)
        return { 0, 0, static_cast<int>(layer.textureWidth() * view.requestedViewportScale()), static_cast<int>(layer.textureHeight() * view.requestedViewportScale()) };

    // Otherwise the layer is side-by-side, so the viewports should be distributed across the texture width.
    int viewportWidth = layer.textureWidth() / m_session->views().size();
    int viewportOffset = viewportWidth * (view.eye() == XREye::Left ? 0 : 1);
    return { viewportOffset, 0, static_cast<int>(viewportWidth * view.requestedViewportScale()), static_cast<int>(layer.textureHeight() * view.requestedViewportScale()) };
}

ExceptionOr<Vector<RefPtr<WebGLOpaqueTexture>>> XRWebGLBinding::allocateColorTexturesForProjectionLayer(XRProjectionLayer& layer, XRTextureType textureType, GCGLenum textureFormat, double scaleFactor)
{
    return WTF::switchOn(m_context,
        [&](const Ref<WebGLRenderingContextBase>& baseContext) -> ExceptionOr<Vector<RefPtr<WebGLOpaqueTexture>>> {
            if (baseContext->isContextLost())
                return Exception { ExceptionCode::InvalidStateError, "Cannot create a projection layer with a lost WebGL context"_s };

            Vector<RefPtr<WebGLOpaqueTexture>> textures;
            switch (layer.layout()) {
            case XRLayerLayout::Default:
            case XRLayerLayout::Mono:
                return Exception { ExceptionCode::NotSupportedError, "Mono layout not implemented."_s };
            case XRLayerLayout::Stereo:
                if (textureType == XRTextureType::TextureArray)
                    return Exception { ExceptionCode::NotSupportedError, "Texture arrays not implemented."_s };
                for (auto& view : m_session->views()) {
                    if (!view.active)
                        continue;
                    textures.append(static_cast<XRWebGLLayerBacking&>(layer.backing()).currentColorTexture());
                }
                break;
            case XRLayerLayout::StereoLeftRight: {
                if (textureType == XRTextureType::TextureArray)
                    return Exception { ExceptionCode::NotSupportedError, "Texture arrays not implemented."_s };
                textures.append(static_cast<XRWebGLLayerBacking&>(layer.backing()).currentColorTexture());
                break;
            }
            case XRLayerLayout::StereoTopBottom:
                return Exception { ExceptionCode::NotSupportedError, "Stereo top bottom not implemented."_s };
            };

            UNUSED_PARAM(textureFormat);
            UNUSED_PARAM(scaleFactor);

            if (textures.isEmpty())
                return Exception { ExceptionCode::OperationError };
            return textures;
        },
        [](std::monostate) {
            ASSERT_NOT_REACHED();
            return Exception { ExceptionCode::OperationError, "Could not get a WebGL rendering context."_s };
        }
    );
}

ExceptionOr<Vector<RefPtr<WebGLOpaqueTexture>>> XRWebGLBinding::allocateDepthTexturesForProjectionLayer(XRProjectionLayer& layer, XRTextureType textureType, GCGLenum textureFormat, double scaleFactor)
{
    return WTF::switchOn(m_context,
        [&](const Ref<WebGLRenderingContextBase>& baseContext) -> ExceptionOr<Vector<RefPtr<WebGLOpaqueTexture>>> {
            if (baseContext->isContextLost())
                return Exception { ExceptionCode::InvalidStateError, "Cannot create a projection layer with a lost WebGL context"_s };

            Vector<RefPtr<WebGLOpaqueTexture>> textures;
            if (!textureFormat)
                return textures;

            if (baseContext->isWebGL1() && !baseContext->extensionIsEnabled("WEBGL_depth_texture"_s))
                return textures;

            switch (layer.layout()) {
            case XRLayerLayout::Default:
            case XRLayerLayout::Mono:
                return Exception { ExceptionCode::NotSupportedError, "Mono layout not implemented."_s };
            case XRLayerLayout::Stereo:
                if (textureType == XRTextureType::TextureArray)
                    return Exception { ExceptionCode::NotSupportedError, "Texture arrays not implemented."_s };
                for (auto& view : m_session->views()) {
                    if (!view.active)
                        continue;
                    textures.append(static_cast<XRWebGLLayerBacking&>(layer.backing()).currentDepthTexture());
                }
                break;
            case XRLayerLayout::StereoLeftRight: {
                if (textureType == XRTextureType::TextureArray)
                    return Exception { ExceptionCode::NotSupportedError, "Texture arrays not implemented."_s };
                textures.append(static_cast<XRWebGLLayerBacking&>(layer.backing()).currentDepthTexture());
                break;
            }
            case XRLayerLayout::StereoTopBottom:
                return Exception { ExceptionCode::NotSupportedError, "Stereo top bottom not implemented."_s };
            };

            UNUSED_PARAM(textureFormat);
            UNUSED_PARAM(scaleFactor);

            if (textures.isEmpty())
                return Exception { ExceptionCode::OperationError };
            return textures;
        },
        [](std::monostate) {
            ASSERT_NOT_REACHED();
            return Exception { ExceptionCode::OperationError, "Could not get a WebGL rendering context."_s };
        }
    );
}

ExceptionOr<Ref<XRWebGLSubImage>> XRWebGLBinding::getViewSubImage(XRProjectionLayer& layer, const WebXRView& view)
{
    // In the specs this is part of the createProjectionLayer algorithm. However by that time the platform textures are not available yet.
    auto colorTexturesResult = allocateColorTexturesForProjectionLayer(layer, layer.init().textureType, layer.init().colorFormat, layer.init().scaleFactor);
    if (colorTexturesResult.hasException())
        return colorTexturesResult.releaseException();
    layer.setColorTextures(colorTexturesResult.releaseReturnValue());

    auto depthTexturesResult = allocateDepthTexturesForProjectionLayer(layer, layer.init().textureType, layer.init().depthFormat, layer.init().scaleFactor);
    if (depthTexturesResult.hasException())
        return depthTexturesResult.releaseException();
    layer.setDepthStencilTextures(depthTexturesResult.releaseReturnValue());

    auto& frame = view.frame();
    if (!validateXRWebGLSubImageCreation(layer, frame))
        return Exception { ExceptionCode::InvalidStateError, "Cannot get view subimage."_s };

    // FIXME: if getViewSubImage() was called previously with the same binding, layer and view, the UA MAY return the same XRWebGLSubImage object as was returned previously.
    Ref viewport = WebXRViewport::create(rectForView(layer, view));
    return XRWebGLSubImage::create(WTF::move(viewport), layer);
}

} // namespace WebCore

#endif // ENABLE(WEBXR_LAYERS)
