/**
 * Copyright (C) 2000 Lars Knoll (knoll@kde.org)
 * Copyright (C) 2000 Antti Koivisto (koivisto@kde.org)
 * Copyright (C) 2000 Dirk Mueller (mueller@kde.org)
 * Copyright (C) 2003-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2006 Graham Dennis (graham.dennis@gmail.com)
 * Copyright (C) 2014-2021 Google Inc. All rights reserved.
 * Copyright (C) 2025 Samuel Weinig <sam@webkit.org>
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

#include <WebCore/RenderStyle.h>

#define RENDER_STYLE_PROPERTIES_INITIAL_INLINES_INCLUDE_TRAP 1
#include <WebCore/RenderStyleProperties+InitialInlines.h>
#undef RENDER_STYLE_PROPERTIES_INITIAL_INLINES_INCLUDE_TRAP

namespace WebCore {

// MARK: - Non-property initial values

constexpr Style::ZIndex RenderStyle::initialUsedZIndex()
{
    return Style::ComputedStyle::initialUsedZIndex();
}

inline Style::PageSize RenderStyle::initialPageSize()
{
    return Style::ComputedStyle::initialPageSize();
}

#if ENABLE(TEXT_AUTOSIZING)

inline Style::LineHeight RenderStyle::initialSpecifiedLineHeight()
{
    return Style::ComputedStyle::initialSpecifiedLineHeight();
}

#endif

} // namespace WebCore
