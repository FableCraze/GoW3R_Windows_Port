// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <cstdio>
#include <vector>

#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/tile_manager.h"

#include "video_core/host_shaders/tiling_macro_128_comp.h"
#include "video_core/host_shaders/tiling_macro_16_comp.h"
#include "video_core/host_shaders/tiling_macro_32_comp.h"
#include "video_core/host_shaders/tiling_macro_64_comp.h"
#include "video_core/host_shaders/tiling_macro_8_comp.h"
#include "video_core/host_shaders/tiling_macro_96_comp.h"
#include "video_core/host_shaders/tiling_micro_128_comp.h"
#include "video_core/host_shaders/tiling_micro_16_comp.h"
#include "video_core/host_shaders/tiling_micro_32_comp.h"
#include "video_core/host_shaders/tiling_micro_64_comp.h"
#include "video_core/host_shaders/tiling_micro_8_comp.h"
#include "video_core/host_shaders/tiling_micro_96_comp.h"

#include <magic_enum/magic_enum.hpp>
#include <vk_mem_alloc.h>

namespace VideoCore {

namespace {

// The device-local snapshot fixes the many small alpha/foliage uploads used by GoW3, but the
// 2048x2048 BC3 surfaces streamed with Gaia regress on the older buffer-cache backend vendored by
// this port (regular diagonal bands appear on her face).  Those large surfaces were stable on the
// direct input path before staging was introduced.  Keep the two paths deliberately separated
// until the rest of cuesta4's buffer-cache lifetime model can be imported as a unit.
bool StageLargeBc3Textures() {
    static const bool enabled = [] {
        const char* value = std::getenv("GOW3R_STAGE_LARGE_BC3");
        return value && value[0] != '0';
    }();
    return enabled;
}

bool IsLargeBc3Texture(const ImageInfo& info) {
    return info.props.is_block && info.num_bits == 128 && info.size.width >= 2048 &&
           info.size.height >= 2048;
}

} // Anonymous namespace

struct TilingInfo {
    u32 bank_swizzle;
    u32 num_slices;
    u32 num_mips;
    std::array<ImageInfo::MipInfo, 16> mips;
};

struct TilingRegion {
    u32 mip;
    u32 first_tile;
    u32 num_tiles;
    u32 tile_width;
    u32 tile_height;
};

static TilingInfo MakeTilingInfo(const ImageInfo& info) {
    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.num_slices = info.props.is_volume ? info.size.depth : info.resources.layers;
    params.num_mips = info.resources.levels;
    params.mips = info.mips_layout;
    if (info.props.is_block) {
        for (u32 mip = 0; mip < params.num_mips; ++mip) {
            params.mips[mip].pitch = std::max((params.mips[mip].pitch + 3) / 4, 1U);
            params.mips[mip].height = std::max((params.mips[mip].height + 3) / 4, 1U);
        }
    }
    return params;
}

using TilingRegions = std::vector<TilingRegion>;

// Build the same tile-shaped work regions used by cuesta4 commit 83352ea.  Merely switching
// the address function for a small mip is insufficient: the invocation-to-coordinate mapping
// must switch with it or padded macro resources are traversed as linear rows.
static TilingRegions MakeTilingRegions(const ImageInfo& info, const TilingInfo& params,
                                       std::span<const vk::BufferImageCopy> copies) {
    TilingRegions regions;
    regions.reserve(copies.size());
    const u32 bytes_per_pixel = info.num_bits / 8;
    const u32 thickness = AmdGpu::GetMicroTileThickness(info.array_mode);
    for (const auto& copy : copies) {
        const u32 mip = copy.imageSubresource.mipLevel;
        if (mip >= params.num_mips ||
            std::ranges::any_of(regions, [mip](const TilingRegion& region) {
                return region.mip == mip;
            })) {
            continue;
        }
        const auto& layout = params.mips[mip];
        if (info.num_samples != 1 || layout.pitch % 8 || layout.height % 8) {
            const u32 texels = layout.size / bytes_per_pixel;
            regions.push_back({mip, 0, (texels + 63U) / 64U, 0, 0});
            continue;
        }

        u32 tile_width = 8;
        u32 tile_height = 8;
        if (AmdGpu::IsMacroTiled(info.array_mode)) {
            const auto mode = AmdGpu::CalculateMacrotileMode(info.tile_mode, info.num_bits, 1);
            const u32 num_pipes = AmdGpu::GetPipeCount(AmdGpu::GetPipeConfig(info.tile_mode));
            const u32 num_banks = AmdGpu::GetNumBanks(mode);
            const u32 aspect = AmdGpu::GetMacrotileAspect(mode);
            const u32 macro_width = 8 * AmdGpu::GetBankWidth(mode) * num_pipes * aspect;
            const u32 macro_height = 8 * AmdGpu::GetBankHeight(mode) * num_banks / aspect;
            if (layout.pitch >= macro_width && layout.height >= macro_height) {
                tile_width = macro_width;
                tile_height = macro_height;
            }
        }

        const u32 tile_bytes = tile_width * tile_height * thickness * bytes_per_pixel;
        const u32 num_tiles = (layout.size + tile_bytes - 1U) / tile_bytes;
        if (num_tiles != 0) {
            regions.push_back({mip, 0, num_tiles, tile_width, tile_height});
        }
    }
    return regions;
}

TileManager::TileManager(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, StreamBuffer& stream_buffer_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, stream_buffer{stream_buffer_} {
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings = {{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 2,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    }};

    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    auto desc_layout_result = device.createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(desc_layout_result.result == vk::Result::eSuccess,
               "Failed to create descriptor set layout: {}",
               vk::to_string(desc_layout_result.result));
    desc_layout = std::move(desc_layout_result.value);

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PushConstantRange push_range{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(TilingRegion),
    };
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &push_range,
    };
    auto [layout_result, layout] = device.createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess, "Failed to create pipeline layout: {}",
               vk::to_string(layout_result));
    pl_layout = std::move(layout);
    std::printf("GPU: complete tiling regions enabled (aligned mips; coordinated micro tails)\n");
    std::printf("GPU: device-local texture staging enabled (explicit detile barriers)\n");
    std::printf("GPU: large BC3 input path: %s (GOW3R_STAGE_LARGE_BC3=%d)\n",
                StageLargeBc3Textures() ? "device-local staging" : "direct compatibility",
                StageLargeBc3Textures() ? 1 : 0);
}

TileManager::~TileManager() = default;

vk::Pipeline TileManager::GetTilingPipeline(const ImageInfo& info, bool is_tiler) {
    const TilingKey key{
        .tile_mode = info.tile_mode,
        .num_bits = info.num_bits,
        .num_samples = info.num_samples,
        .is_tiler = is_tiler,
    };
    if (const auto it = tiling_pipelines.find(key); it != tiling_pipelines.end()) {
        return *it->second;
    }

    const auto device = instance.GetDevice();
    const auto micro_tile_mode = AmdGpu::GetMicroTileMode(info.tile_mode);
    const bool is_macro = AmdGpu::IsMacroTiled(info.array_mode);
    std::array<u32, 12> spec_data{
        info.num_samples,
        u32(micro_tile_mode),
        AmdGpu::GetMicroTileThickness(info.array_mode),
        u32(is_tiler),
    };
    if (is_macro) {
        const auto macro_tile_mode =
            AmdGpu::CalculateMacrotileMode(info.tile_mode, info.num_bits, info.num_samples);
        spec_data[4] = u32(info.array_mode);
        spec_data[5] = u32(AmdGpu::GetPipeConfig(info.tile_mode));
        spec_data[6] = AmdGpu::GetBankWidth(macro_tile_mode);
        spec_data[7] = AmdGpu::GetBankHeight(macro_tile_mode);
        spec_data[8] = AmdGpu::GetNumBanks(macro_tile_mode);
        spec_data[9] = std::bit_width(spec_data[8]) - 1;
        spec_data[10] = AmdGpu::CalculateTileSplit(info.tile_mode, info.array_mode, micro_tile_mode,
                                                   info.num_bits);
        spec_data[11] = AmdGpu::GetMacrotileAspect(macro_tile_mode);
    }

    std::span<const u32> code;
    switch (info.num_bits) {
    case 8:
        code = is_macro ? std::span<const u32>{TILING_MACRO_8_COMP}
                        : std::span<const u32>{TILING_MICRO_8_COMP};
        break;
    case 16:
        code = is_macro ? std::span<const u32>{TILING_MACRO_16_COMP}
                        : std::span<const u32>{TILING_MICRO_16_COMP};
        break;
    case 32:
        code = is_macro ? std::span<const u32>{TILING_MACRO_32_COMP}
                        : std::span<const u32>{TILING_MICRO_32_COMP};
        break;
    case 64:
        code = is_macro ? std::span<const u32>{TILING_MACRO_64_COMP}
                        : std::span<const u32>{TILING_MICRO_64_COMP};
        break;
    case 96:
        code = is_macro ? std::span<const u32>{TILING_MACRO_96_COMP}
                        : std::span<const u32>{TILING_MICRO_96_COMP};
        break;
    case 128:
        code = is_macro ? std::span<const u32>{TILING_MACRO_128_COMP}
                        : std::span<const u32>{TILING_MICRO_128_COMP};
        break;
    default:
        UNREACHABLE_MSG("Unsupported tiling pixel width {}", info.num_bits);
    }

    const auto module = Vulkan::CompileSPV(code, device);
    static constexpr auto spec_entries = [] {
        std::array<vk::SpecializationMapEntry, 12> entries{};
        for (u32 i = 0; i < entries.size(); ++i) {
            entries[i] = vk::SpecializationMapEntry{i, i * u32(sizeof(u32)), sizeof(u32)};
        }
        return entries;
    }();

    const u32 spec_count = is_macro ? 12 : 4;
    const vk::SpecializationInfo specialization{
        .mapEntryCount = spec_count,
        .pMapEntries = spec_entries.data(),
        .dataSize = spec_count * sizeof(u32),
        .pData = spec_data.data(),
    };

    const auto module_name = fmt::format("{}_{} {}", magic_enum::enum_name(info.tile_mode),
                                         info.num_bits, is_tiler ? "tiler" : "detiler");
    LOG_INFO(Render_Vulkan, "Creating tiling pipeline {}", module_name);
    Vulkan::SetObjectName(device, module, module_name);

    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
        .pSpecializationInfo = &specialization,
    };
    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .stage = shader_ci,
        .layout = *pl_layout,
    };

    auto [result, pipeline] =
        device.createComputePipelineUnique(VK_NULL_HANDLE, compute_pipeline_ci);
    ASSERT_MSG(result == vk::Result::eSuccess, "Detiler pipeline creation failed {}",
               vk::to_string(result));
    const auto handle = *pipeline;
    tiling_pipelines.emplace(key, std::move(pipeline));
    device.destroyShaderModule(module);
    return handle;
}

void TileManager::RecordTilingDispatch(vk::Pipeline pipeline, const vk::DescriptorBufferInfo& tiled,
                                       const vk::DescriptorBufferInfo& linear,
                                       const vk::DescriptorBufferInfo& params, const ImageInfo& info,
                                       std::span<const vk::BufferImageCopy> buffer_copies) {
    const auto tiling_info = MakeTilingInfo(info);
    auto regions = MakeTilingRegions(info, tiling_info, buffer_copies);
    const u32 thickness = AmdGpu::GetMicroTileThickness(info.array_mode);
    scheduler.Record([pipeline, layout = *pl_layout, tiled, linear, params,
                      regions = std::move(regions), thickness](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        const std::array<vk::WriteDescriptorSet, 3> set_writes = {{
            {
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &tiled,
            },
            {
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &linear,
            },
            {
                .dstBinding = 2,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eUniformBuffer,
                .pBufferInfo = &params,
            },
        }};
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, set_writes);
        for (auto region : regions) {
            const u32 groups_per_tile = region.tile_width == 0
                                            ? 1
                                            : region.tile_width * region.tile_height * thickness / 64;
            const u32 max_tiles = std::max(65535U / groups_per_tile, 1U);
            while (region.num_tiles != 0) {
                const u32 count = std::min(region.num_tiles, max_tiles);
                cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(region),
                                     &region);
                cmdbuf.dispatch(count * groups_per_tile, 1, 1);
                region.first_tile += count;
                region.num_tiles -= count;
            }
        }
    });
}

std::pair<const Buffer*, u64> TileManager::DetileImage(const VideoCore::Buffer* in_buffer,
                                                       u64 in_offset, const ImageInfo& info,
                                                       std::span<const vk::BufferImageCopy> buffer_copies) {
    if (!info.props.is_tiled) {
        return {in_buffer, in_offset};
    }

    const auto params = MakeTilingInfo(info);

    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = stream_buffer.Copy(&params, sizeof(params), instance.UniformMinAlignment()),
        .range = sizeof(params),
    };

    // GoW3 streams most texture payloads through a host-visible upload buffer.  Reading that
    // allocation directly from the detiling compute shader was timing-sensitive on Windows:
    // slowing the GPU thread down with texture tracing made Gaia's textures substantially more
    // reliable.  The current cuesta4 path stages host memory into device-local memory before
    // detiling.  Besides avoiding slow device reads over the host heap, the copy gives the
    // upload one well-defined snapshot of all mip data.
    const bool stage_input = !in_buffer->mapped_data.empty() &&
                             (!IsLargeBc3Texture(info) || StageLargeBc3Textures());
    const auto device_input =
        stage_input
            ? runtime.GetStagingPool().Request(info.guest_size, MemoryType::DeviceLocal)
            : Vulkan::StagingBufferRef{};
    const auto staging = runtime.GetStagingPool().Request(info.guest_size, MemoryType::DeviceLocal);

    scheduler.EndRendering();
    runtime.FlushBarriers();

    if (stage_input) {
        const vk::BufferCopy copy{
            .srcOffset = in_offset,
            .dstOffset = device_input.offset,
            .size = info.guest_size,
        };
        scheduler.Record([src = in_buffer->Handle(), dst = device_input.buffer->Handle(), copy,
                          size = info.guest_size](vk::CommandBuffer cmdbuf) {
            cmdbuf.copyBuffer(src, dst, copy);
            const vk::BufferMemoryBarrier2 barrier{
                .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
                .buffer = dst,
                .offset = copy.dstOffset,
                .size = size,
            };
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .bufferMemoryBarrierCount = 1,
                .pBufferMemoryBarriers = &barrier,
            });
        });
        runtime.AccessBuffer(in_buffer, in_offset, info.guest_size,
                             vk::PipelineStageFlagBits2::eCopy,
                             vk::AccessFlagBits2::eTransferRead);
        runtime.AccessBuffer(device_input.buffer, device_input.offset, info.guest_size,
                             vk::PipelineStageFlagBits2::eCopy,
                             vk::AccessFlagBits2::eTransferWrite);
    }

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = stage_input ? device_input.buffer->Handle() : in_buffer->Handle(),
        .offset = stage_input ? device_input.offset : in_offset,
        .range = info.guest_size,
    };
    const vk::DescriptorBufferInfo linear_buffer_info{
        .buffer = staging.buffer->Handle(),
        .offset = staging.offset,
        .range = info.guest_size,
    };
    // bbport: recorded with copies of the descriptor infos (threaded recording).
    RecordTilingDispatch(GetTilingPipeline(info, false), tiled_buffer_info, linear_buffer_info,
                         params_buffer_info, info, buffer_copies);
    // Keep this dependency adjacent to the dispatch.  UploadImage also tracks the buffer, but
    // an explicit range barrier matches the reference implementation and cannot be reordered
    // away from the compute work by threaded command recording.
    scheduler.Record([buffer = staging.buffer->Handle(), offset = staging.offset,
                      size = info.guest_size](vk::CommandBuffer cmdbuf) {
        const vk::BufferMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .buffer = buffer,
            .offset = offset,
            .size = size,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &barrier,
        });
    });
    // bbport: the source may be the arena (BB_PREUPLOAD), which later uploads write: a copy into
    // it must wait for this read.
    runtime.AccessBuffer(stage_input ? device_input.buffer : in_buffer,
                         stage_input ? device_input.offset : in_offset, info.guest_size,
                         vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderRead);

    runtime.AccessBuffer(staging.buffer, staging.offset, info.guest_size,
                         vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderWrite);

    return {staging.buffer, staging.offset};
}

void TileManager::TileImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                            const VideoCore::Buffer* out_buffer, u64 out_offset) {
    const auto& info = in_image.info;
    if (!info.props.is_tiled) {
        for (auto& copy : buffer_copies) {
            copy.bufferOffset += out_offset;
        }
        runtime.DownloadImage(&in_image, out_buffer, buffer_copies);
        return;
    }

    const auto params = MakeTilingInfo(info);

    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = stream_buffer.Copy(&params, sizeof(params), instance.UniformMinAlignment()),
        .range = sizeof(params),
    };

    const auto staging = runtime.GetStagingPool().Request(info.guest_size, MemoryType::DeviceLocal);
    for (auto& copy : buffer_copies) {
        copy.bufferOffset += staging.offset;
    }

    runtime.DownloadImage(&in_image, staging.buffer, buffer_copies);
    runtime.FlushBarriers();

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = out_buffer->Handle(),
        .offset = out_offset,
        .range = info.guest_size,
    };
    const vk::DescriptorBufferInfo linear_buffer_info{
        .buffer = staging.buffer->Handle(),
        .offset = staging.offset,
        .range = info.guest_size,
    };
    // bbport: recorded with copies of the descriptor infos (threaded recording).
    RecordTilingDispatch(GetTilingPipeline(info, true), tiled_buffer_info, linear_buffer_info,
                         params_buffer_info, info, buffer_copies);

    runtime.AccessBuffer(out_buffer, out_offset, info.guest_size,
                         vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderWrite);
}

} // namespace VideoCore
