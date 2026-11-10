// Finds the game's camera in a draw, for co-op markers. Included only by the
// patched src/video_core/renderer_vulkan/vk_rasterizer.cpp, which calls Track
// once per draw after the pipeline is configured.
//
// The same buffer the emulator's DLSS path reads (TrackOcarinaCamera), but
// without needing DLSS: a draw into a scene-sized B10G11R11 colour target
// with depth, whose vertex-stage uniform buffer 3 holds the target size at
// +0x120 and (-1, 0) at +0xa8, where the emulator checks a projection's w row.
// The world-to-clip matrix is the 16 floats at +0x30, row-major
// (the emulator's own motion shader reads it as row_major).
#pragma once

#include <array>
#include <cstdio>

#include "coop_camera.h"

namespace CoopCamera {

template <class TextureCache, class BufferCache, class DeviceMemory, class Regs>
void Track(TextureCache& texture_cache, BufferCache& buffer_cache, DeviceMemory& device_memory,
           const Regs& regs, u64 frame) {
    if (!Wanted()) {
        return;
    }
    const bool split = Split();
    const bool second = split ? CurrentView() != 0 : UseSecond();
    const auto& targets = texture_cache.GetRenderTargets();
    if (!targets.color_buffer_ids[0] || !targets.depth_buffer_id) {
        return;
    }
    const auto& color = texture_cache.GetImageView(targets.color_buffer_ids[0]);
    const auto width = static_cast<float>(color.size.width);
    const auto height = static_cast<float>(color.size.height);
    if (width < 640 || height < 360 ||
        color.format != VideoCore::Surface::PixelFormat::B10G11R11_FLOAT ||
        !Better(frame, width, height)) {
        return;
    }
    // Which way clip y runs on the picture: Maxwell maps y to
    // translate_y + scale_y * y (negated first by a NegativeY swizzle),
    // counted from the bottom when the window origin is lower-left.
    const auto& viewport = regs.viewport_transform[0];
    float down = viewport.scale_y;
    if (viewport.swizzle.y == Regs::ViewportSwizzle::NegativeY) {
        down = -down;
    }
    if (regs.window_origin.mode != Regs::WindowOrigin::Mode::UpperLeft) {
        down = -down;
    }
    buffer_cache.ForEachBoundBuffer(false, [&](u32 stage, bool storage, u32 index, bool,
                                               const auto& binding, u32 bound_size, const auto*,
                                               u32) {
        std::array<f32, 0x130 / sizeof(f32)> camera;
        if (stage != 0 || storage || index != 3 || bound_size < sizeof(camera) ||
            buffer_cache.IsRegionGpuModified(binding.device_addr, sizeof(camera))) {
            return;
        }
        device_memory.ReadBlockUnsafe(binding.device_addr, camera.data(), sizeof(camera));
        constexpr size_t extent = 0x120 / sizeof(f32);
        constexpr size_t projection_wz = 0xa8 / sizeof(f32);
        if (camera[extent] != width || camera[extent + 1] != height ||
            camera[projection_wz] != -1.0f || camera[projection_wz + 1] != 0.0f) {
            return;
        }
        Publish(frame, camera.data() + 0x30 / sizeof(f32), width, height, down > 0.0f);
        SetCameraAddress(binding.device_addr);
    });
}

// Called at the start of a draw, before the buffer cache uploads the camera
// uniform. Writing the guest camera buffer here makes the upload read the
// second view.
template <class DeviceMemory>
void ApplySecondBeforeUpload(DeviceMemory& device_memory) {
    const bool second = Split() ? CurrentView() != 0 : UseSecond();
    const std::uint64_t address = CameraAddress();
    const auto matrix = SecondMatrix();
    static unsigned probe = 0;
    if (probe < 2000 && (probe++ % 500) == 0) {
        std::fprintf(stderr,
                     "[splitcam] probe wanted=%d second=%d addr=%#llx matrix=%d\n",
                     Wanted() ? 1 : 0, second ? 1 : 0, (unsigned long long)address,
                     matrix ? 1 : 0);
    }
    if (!Wanted() || !second || address < 0x1000 || !matrix) {
        return;
    }
    device_memory.WriteBlockUnsafe(address + 0x30, matrix->data(), 16 * sizeof(f32));
    static int logs = 0;
    if (logs < 3) {
        std::array<f32, 16> check{};
        device_memory.ReadBlockUnsafe(address + 0x30, check.data(), sizeof(check));
        std::fprintf(stderr,
                     "[splitcam] pre addr=%#llx wrote t=(%.2f %.2f %.2f) read t=(%.2f %.2f %.2f)\n",
                     (unsigned long long)address, (*matrix)[12], (*matrix)[13], (*matrix)[14],
                     check[12], check[13], check[14]);
        ++logs;
    }
}

} // namespace CoopCamera
