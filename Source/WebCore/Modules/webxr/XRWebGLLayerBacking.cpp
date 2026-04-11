/*
 * Copyright (C) 2026 Igalia S.L. All rights reserved.
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
#include "XRWebGLLayerBacking.h"

#if ENABLE(WEBXR_LAYERS)

#include "GraphicsContextGL.h"
#include "WebGLOpaqueTexture.h"
#include "WebGLRenderingContextBase.h"
#include "WebXROpaqueFramebuffer.h"
#include "WebXRWebGLSwapchain.h"

#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(XRWebGLLayerBacking);

using GL = GraphicsContextGL;

XRWebGLLayerBacking::XRWebGLLayerBacking(PlatformXR::LayerHandle handle, std::unique_ptr<WebXRWebGLSwapchain>&& colorSwapchain, std::unique_ptr<WebXRWebGLSwapchain>&& depthSwapchain)
    : m_colorSwapchain(WTF::move(colorSwapchain))
    , m_depthSwapchain(WTF::move(depthSwapchain))
{
    setHandle(handle);
}

uint32_t XRWebGLLayerBacking::colorTextureWidth() const
{
    return m_colorSwapchain->size().width();
};

uint32_t XRWebGLLayerBacking::colorTextureHeight() const
{
    return m_colorSwapchain->size().height();
};

uint32_t XRWebGLLayerBacking::colorTextureArrayLength() const
{
    // FIXME: Support texture arrays for multiview.
    return 1;
};

std::optional<uint32_t> XRWebGLLayerBacking::depthTextureWidth() const
{
    return m_depthSwapchain ? std::make_optional(m_depthSwapchain->size().width()) : std::nullopt;
}

std::optional<uint32_t> XRWebGLLayerBacking::depthTextureHeight() const
{
    return m_depthSwapchain ? std::make_optional(m_depthSwapchain->size().height()) : std::nullopt;
}

#if PLATFORM(COCOA)
void XRWebGLLayerBacking::startFrame(size_t, MachSendRight&&, MachSendRight&&, MachSendRight&&, size_t, PlatformXR::RateMapDescription&&)
{
}

void XRWebGLLayerBacking::endFrame()
{
}

#else
void XRWebGLLayerBacking::startFrame(PlatformXR::FrameData& data)
{
    ASSERT(m_colorSwapchain);

    auto it = data.layers.find(handle());
    if (it == data.layers.end())
        return;

    m_colorSwapchain->startFrame(it->value);
    if (m_depthSwapchain)
        m_depthSwapchain->startFrame(it->value);
}

void XRWebGLLayerBacking::endFrame(PlatformXR::DeviceLayer& layerData)
{
    ASSERT(m_colorSwapchain);
    m_colorSwapchain->endFrame(layerData);
    if (m_depthSwapchain)
        m_depthSwapchain->endFrame(layerData);
}
#endif

RefPtr<WebGLOpaqueTexture> XRWebGLLayerBacking::currentColorTexture() const
{
    return WebGLOpaqueTexture::create(*m_colorSwapchain->context(), m_colorSwapchain->currentTexture());
}

RefPtr<WebGLOpaqueTexture> XRWebGLLayerBacking::currentDepthTexture() const
{
    return WebGLOpaqueTexture::create(*m_depthSwapchain->context(), m_depthSwapchain->currentTexture());
}

// Based on https://source.chromium.org/chromium/chromium/src/+/main:third_party/blink/renderer/modules/xr/xr_webgl_binding.cc
XRWebGLLayerBacking::SwapchainFormats XRWebGLLayerBacking::swapchainFormatsForLayerFormat(GCGLenum layerFormat)
{
    switch (layerFormat) {
    case GL::RGBA:
    case GL::RGBA8:
        return { GL::RGBA, GL::RGBA8 };

    case GL::RGB:
    case GL::RGB8:
        return { GL::RGB, GL::RGB8 };

    case GL::SRGB_EXT:
    case GL::SRGB8:
        return { GL::SRGB_EXT, GL::SRGB8 };

    case GL::SRGB_ALPHA_EXT:
    case GL::SRGB8_ALPHA8:
        return { GL::SRGB_ALPHA_EXT, GL::SRGB8_ALPHA8 };

    case GL::DEPTH_COMPONENT:
    case GL::DEPTH_COMPONENT24:
        return { GL::DEPTH_COMPONENT, GL::DEPTH_COMPONENT24 };

    case GL::DEPTH_STENCIL:
    case GL::DEPTH24_STENCIL8:
        return { GL::DEPTH_STENCIL, GL::DEPTH24_STENCIL8 };

    default:
        ASSERT_NOT_REACHED();
        return { 0, 0 };
    };
}

bool XRWebGLLayerBacking::formatHasStencil(GCGLenum layerFormat)
{
    switch (layerFormat) {
    case GL::DEPTH_STENCIL:
    case GL::DEPTH24_STENCIL8:
        return true;
    default:
        return false;
    };
}

} // namespace WebCore


#endif // ENABLE(WEBXR_LAYERS)
