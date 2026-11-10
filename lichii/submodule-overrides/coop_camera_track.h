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
        color.format != VideoCore::Surface::PixelFormat::B10G11R11_FLOAT) {
        return;
    }
    const bool better = Better(frame, width, height);
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
                                               const auto& binding, u32 bound_size,
                                               const auto* host, u32 host_offset) {
        // The scene camera comes in two forms: the full DLSS camera
        // (>= 0x130 bytes, extent/projection at 0x120/0xa8) and the
        // per-shader scene camera (176 bytes) with the world-to-clip
        // matrix at +0x30. Both use stage 0/2, index 3.
        constexpr std::size_t kWorldToClip = 0x30;
        constexpr std::size_t kMatrixEnd = kWorldToClip + 16 * sizeof(f32);
        if (storage || index != 3 || bound_size < kMatrixEnd ||
            buffer_cache.IsRegionGpuModified(binding.device_addr, bound_size)) {
            return;
        }
        std::array<f32, 0x130 / sizeof(f32)> camera{};
        const std::size_t read_size = (std::min)(static_cast<std::size_t>(bound_size),
                                                sizeof(camera));
        device_memory.ReadBlockUnsafe(binding.device_addr, camera.data(), read_size);
        if (bound_size >= sizeof(camera)) {
            constexpr size_t extent = 0x120 / sizeof(f32);
            constexpr size_t projection_wz = 0xa8 / sizeof(f32);
            if (camera[extent] != width || camera[extent + 1] != height ||
                camera[projection_wz] != -1.0f || camera[projection_wz + 1] != 0.0f) {
                return;
            }
            if (better) {
                Publish(frame, camera.data() + kWorldToClip / sizeof(f32), width, height,
                        down > 0.0f);
                SetCameraAddress(binding.device_addr);
                if (host != nullptr) {
                    SetCameraCpuAddress(host->CpuAddr() + host_offset);
                }
            }
        } else {
            // A per-shader scene camera: its projection row (the last four
            // floats of the world-to-clip matrix) must match the primary.
            const auto primary = PrimaryMatrix();
            if (!primary) {
                return;
            }
            const std::size_t base = kWorldToClip / sizeof(f32);
            for (std::size_t i = 12; i < 16; ++i) {
                if (std::fabs(camera[base + i] - (*primary)[i]) > 1e-3f) {
                    return;
                }
            }
        }
        AddCameraAddress(binding.device_addr,
                         host != nullptr ? host->CpuAddr() + host_offset : 0);
    });
}

// Called at the start of a draw, before the buffer cache uploads the camera
// uniform. Writing the guest camera buffer here makes the upload read the
// second view.
template <class BufferCache, class DeviceMemory>
void ApplySecondBeforeUpload(BufferCache& buffer_cache, DeviceMemory& device_memory) {
    const bool second = Split() ? CurrentView() != 0 : UseSecond();
    const unsigned count = CameraAddressCount();
    const auto matrix = SecondMatrix();
    static unsigned probe = 0;
    if (probe < 5000 && (probe++ % 1000) == 0) {
        std::fprintf(stderr, "[splitcam] probe wanted=%d second=%d cameras=%u matrix=%d\n",
                     Wanted() ? 1 : 0, second ? 1 : 0, count, matrix ? 1 : 0);
    }
    if (!Wanted() || !second || count == 0 || !matrix) {
        return;
    }
    static int logs = 0;
    for (unsigned i = 0; i < count; ++i) {
        const auto [gpu_address, cpu_address] = CameraAddressAt(i);
        if (gpu_address < 0x1000) {
            continue;
        }
        // The 176-byte scene camera has the 3x4 view at +0x0, the
        // view-projection at +0x30 and the projection at +0x70. Move the view
        // translation by the same world delta and replace the VP.
        if (const auto delta = SecondDelta()) {
            std::array<f32, 12> view{};
            device_memory.ReadBlockUnsafe(gpu_address, view.data(), sizeof(view));
            for (std::size_t row = 0; row < 3; ++row) {
                view[row * 4 + 3] += view[row * 4 + 0] * (*delta)[0] +
                                     view[row * 4 + 1] * (*delta)[1] +
                                     view[row * 4 + 2] * (*delta)[2];
            }
            device_memory.WriteBlockUnsafe(gpu_address, view.data(), sizeof(view));
        }
        device_memory.WriteBlockUnsafe(gpu_address + 0x30, matrix->data(), 16 * sizeof(f32));
        if (cpu_address != 0) {
            // Bypassing the memory manager leaves the buffer cache with the old
            // GPU copy. Mark the view and VP dirty so Configure uploads them.
            buffer_cache.WriteMemory(cpu_address, 0xB0, false);
        }
        if (logs < 3) {
            std::array<f32, 16> check{};
            device_memory.ReadBlockUnsafe(gpu_address + 0x30, check.data(), sizeof(check));
            std::fprintf(stderr,
                         "[splitcam] pre[%u] addr=%#llx wrote t=(%.2f %.2f %.2f) read t=(%.2f %.2f %.2f)\n",
                         i, (unsigned long long)gpu_address, (*matrix)[12], (*matrix)[13],
                         (*matrix)[14], check[12], check[13], check[14]);
            ++logs;
        }
    }
}

} // namespace CoopCamera
