// bbport: shadPS4 settings used by the video core, read once from GOW3R_* environment
// variables (defaults match shadPS4 except the pipeline cache, which is on).
#pragma once
#include <algorithm>
#include <cstdlib>
#include <string>
#include "common/types.h"

u32 BbDisplayRefreshHz(); // bbgpu.cpp: primary display refresh rate, 60 when unknown

enum GpuReadbacksMode : int { Disabled, Relaxed, Precise };

class EmulatorSettingsImpl {
public:
    static EmulatorSettingsImpl* GetInstance() { static EmulatorSettingsImpl s; return &s; }
    static bool Flag(const char* name, bool fallback) {
        const char* v = std::getenv(name);
        return v ? (v[0] == '1' || v[0] == 'y' || v[0] == 't') : fallback;
    }
    static long Number(const char* name, long fallback) {
        const char* v = std::getenv(name);
        return v ? std::strtol(v, nullptr, 10) : fallback;
    }
    s32 GetGpuId() { static const auto value = s32(Number("GOW3R_GPU_ID", -1)); return value; }
    u32 GetInternalScreenWidth() { static const auto value = u32(Number("GOW3R_INTERNAL_WIDTH", 1920)); return value; }
    u32 GetInternalScreenHeight() { static const auto value = u32(Number("GOW3R_INTERNAL_HEIGHT", 1080)); return value; }
    std::string GetPresentMode() { const char* v = std::getenv("GOW3R_PRESENT_MODE"); return v ? v : "Mailbox"; }
    int GetRcasAttenuation() { static const auto value = int(Number("GOW3R_RCAS_ATTENUATION", 250)); return value; }
    // GoW3R fast path: general read-fault tracking is disabled. Linear images are
    // downloaded asynchronously below, including the tiny exposure targets used by GoW3R.
    u32 GetReadbacksMode() { static const auto value = u32(Number("GOW3R_READBACKS", GpuReadbacksMode::Disabled)); return value; }
    // bbport: GOW3R_VBLANK_HZ=0 (uncapped presets): vblank runs at 480 Hz so a finished frame is
    // shown within ~2 ms instead of waiting for the next display-rate vblank (below the display
    // rate frames alternated 10/20 ms at 100 Hz: judder), and GetFrameLimit() caps the rate at
    // the display refresh, at most 120 (above ~120 FPS movement timing breaks: running slows).
    // An explicit GOW3R_VBLANK_HZ is used as given, without a limit (measurements).
    u32 GetVblankFrequency() {
        static const u32 value = [] {
            const long hz = Number("GOW3R_VBLANK_HZ", 60);
            return hz > 0 ? u32(hz) : 480u;
        }();
        return value;
    }
    /// Frames per second the present thread lets through; 0 = no limit. GOW3R_FPS_LIMIT overrides.
    u32 GetFrameLimit() {
        static const u32 value = [] {
            const long limit = Number("GOW3R_FPS_LIMIT", -1);
            if (limit >= 0) return u32(limit);
            return Number("GOW3R_VBLANK_HZ", 60) > 0 ? 0u : std::min<u32>(BbDisplayRefreshHz(), 120);
        }();
        return value;
    }
    bool IsCopyGpuBuffers() { static const auto value = Flag("GOW3R_COPY_GPU_BUFFERS", false); return value; }
    bool IsGow3AliasSyncEnabled() { static const auto value = Flag("GOW3R_ALIAS_SYNC", true); return value; }
    bool IsDirectMemoryAccessEnabled() { static const auto value = Flag("GOW3R_DIRECT_MEMORY_ACCESS", false); return value; }
    bool IsDumpShaders() { static const auto value = Flag("GOW3R_DUMP_SHADERS", false); return value; }
    bool IsFsrEnabled() { static const auto value = Flag("GOW3R_FSR1", false); return value; }
    bool IsHdrAllowed() { static const auto value = Flag("GOW3R_HDR", false); return value; }
    bool IsNullGPU() { static const auto value = Flag("GOW3R_NULL_GPU", false); return value; }
    bool IsPatchShaders() { return false; }
    bool IsPipelineCacheArchived() { return false; }
    bool IsPipelineCacheEnabled() { static const auto value = Flag("GOW3R_PIPELINE_CACHE", true); return value; }
    bool IsRcasEnabled() { static const auto value = Flag("GOW3R_RCAS", false); return value; }
    bool IsReadbackLinearImagesEnabled() { static const auto value = Flag("GOW3R_READBACK_LINEAR", true); return value; }
    bool IsRenderdocEnabled() { return false; }
    bool IsShaderCollect() { return false; }
    bool IsUserfaultfdTracking() { return false; }
    bool IsVkCrashDiagnosticEnabled() { return false; }
    bool IsVkGuestMarkersEnabled() { static const auto value = Flag("GOW3R_VK_MARKERS", false); return value; }
    bool IsVkHostMarkersEnabled() { static const auto value = Flag("GOW3R_VK_MARKERS", false); return value; }
    bool IsVkValidationCoreEnabled() { return true; }
    bool IsVkValidationEnabled() { static const auto value = Flag("GOW3R_VK_VALIDATION", false); return value; }
    bool IsVkValidationGpuEnabled() { return false; }
    bool IsVkValidationSyncEnabled() { static const auto value = Flag("GOW3R_VK_VALIDATION_SYNC", false); return value; }
};
#define EmulatorSettings (*EmulatorSettingsImpl::GetInstance())
