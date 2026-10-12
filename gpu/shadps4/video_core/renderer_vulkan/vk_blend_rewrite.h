// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/amdgpu/pixel_format.h"
#include "video_core/amdgpu/regs_color.h"

namespace Vulkan {

struct BlendEquation {
    AmdGpu::BlendControl::BlendFactor src_factor;
    AmdGpu::BlendControl::BlendFunc function;
    AmdGpu::BlendControl::BlendFactor dst_factor;
};

enum class BlendChannel { Color, Alpha };

enum class BlendRewriteResult {
    Native,
    Exact,
    Squared,
    Unsupported,
};

[[nodiscard]] constexpr bool IsUnsignedNormalizedBlendTarget(const AmdGpu::NumberFormat format) {
    using Format = AmdGpu::NumberFormat;
    return format == Format::Unorm || format == Format::Srgb;
}

[[nodiscard]] constexpr bool IsSelfScaledBlend(const BlendEquation& equation,
                                               const BlendChannel channel) {
    using Factor = AmdGpu::BlendControl::BlendFactor;
    if (channel == BlendChannel::Color) {
        return equation.src_factor == Factor::SrcColor && equation.dst_factor == Factor::DstColor;
    }
    return (equation.src_factor == Factor::SrcColor || equation.src_factor == Factor::SrcAlpha) &&
           (equation.dst_factor == Factor::DstColor || equation.dst_factor == Factor::DstAlpha);
}

/**
 * GNM scales MIN/MAX operands while Vulkan ignores their factors. Zero operands can be rewritten
 * exactly on unsigned normalized targets. Self-scaled operands can use native MIN/MAX followed by
 * a second draw that squares the selected destination value.
 */
[[nodiscard]] constexpr BlendRewriteResult RewriteScaledMinMaxBlend(
    BlendEquation& equation, const AmdGpu::NumberFormat target_format, const BlendChannel channel) {
    using Factor = AmdGpu::BlendControl::BlendFactor;
    using Function = AmdGpu::BlendControl::BlendFunc;

    if (equation.function != Function::Min && equation.function != Function::Max) {
        return BlendRewriteResult::Native;
    }
    if (equation.src_factor == Factor::One && equation.dst_factor == Factor::One) {
        return BlendRewriteResult::Native;
    }
    if (!IsUnsignedNormalizedBlendTarget(target_format)) {
        return BlendRewriteResult::Unsupported;
    }
    if (IsSelfScaledBlend(equation, channel)) {
        return BlendRewriteResult::Squared;
    }
    if (equation.src_factor != Factor::Zero && equation.dst_factor != Factor::Zero) {
        return BlendRewriteResult::Unsupported;
    }
    if (equation.function == Function::Min) {
        equation.src_factor = Factor::Zero;
        equation.dst_factor = Factor::Zero;
    }
    equation.function = Function::Add;
    return BlendRewriteResult::Exact;
}

} // namespace Vulkan
