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
#include <cmath>
#include <cstdio>
#include <utility>

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
        constexpr std::size_t kCameraSize = 0xB0;  // view + VP + projection
        if (storage || index != 3 || bound_size < kCameraSize ||
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
                static int published = 0;
                if (published < 5) {
                    ++published;
                    const f32* clip = camera.data() + kWorldToClip / sizeof(f32);
                    std::fprintf(stderr,
                                 "[splitcam] publish col3=(%.3f %.3f %.3f %.3f)\n",
                                 clip[3], clip[7], clip[11], clip[15]);
                }
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
//
// The game's matrices are row-major with column vectors, so moving the camera
// by d = link2 - link1 is view2 = view1 * T(-d) = view1 * T(delta) with
// delta = link1 - link2. Only the translation column (column 3) changes.
// The view-projection is rebuilt from the view and the projection at +0x70,
// so repeated draws in the same frame never accumulate the translation.
inline std::array<f32, 16> MultiplyMatrices(const std::array<f32, 16>& a,
                                            const std::array<f32, 16>& b) {
    std::array<f32, 16> out{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t col = 0; col < 4; ++col) {
            f32 sum = 0.0f;
            for (std::size_t k = 0; k < 4; ++k) {
                sum += a[row * 4 + k] * b[k * 4 + col];
            }
            out[row * 4 + col] = sum;
        }
    }
    return out;
}

inline bool InvertMatrix(const std::array<f32, 16>& m, std::array<f32, 16>& out) {
    std::array<std::array<f32, 8>, 4> a{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t col = 0; col < 4; ++col) {
            a[row][col] = m[row * 4 + col];
            a[row][4 + col] = row == col ? 1.0f : 0.0f;
        }
    }
    for (std::size_t col = 0; col < 4; ++col) {
        std::size_t pivot = col;
        for (std::size_t row = col + 1; row < 4; ++row) {
            if (std::fabs(a[row][col]) > std::fabs(a[pivot][col])) {
                pivot = row;
            }
        }
        if (std::fabs(a[pivot][col]) < 1e-8f) {
            return false;
        }
        if (pivot != col) {
            std::swap(a[pivot], a[col]);
        }
        const f32 scale = 1.0f / a[col][col];
        for (f32& value : a[col]) {
            value *= scale;
        }
        for (std::size_t row = 0; row < 4; ++row) {
            if (row == col) {
                continue;
            }
            const f32 factor = a[row][col];
            if (factor == 0.0f) {
                continue;
            }
            for (std::size_t k = 0; k < 8; ++k) {
                a[row][k] -= factor * a[col][k];
            }
        }
    }
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t col = 0; col < 4; ++col) {
            out[row * 4 + col] = a[row][4 + col];
        }
    }
    return true;
}

template <class BufferCache, class DeviceMemory>
void ApplySecondBeforeUpload(BufferCache& buffer_cache, DeviceMemory& device_memory, u64 frame) {
    const bool second = Split() ? CurrentView() != 0 : UseSecond();
    const unsigned count = CameraAddressCount();
    const auto delta = SecondDelta();
    static unsigned probe = 0;
    if (probe < 5000 && (probe++ % 1000) == 0) {
        std::fprintf(stderr, "[splitcam] probe wanted=%d second=%d cameras=%u delta=%d\n",
                     Wanted() ? 1 : 0, second ? 1 : 0, count, delta ? 1 : 0);
    }
    if (!Wanted() || !second || count == 0 || !delta) {
        return;
    }
    static int logs = 0;
    for (unsigned i = 0; i < count; ++i) {
        const auto [gpu_address, cpu_address] = CameraAddressAt(i);
        if (gpu_address < 0x1000) {
            continue;
        }
        // Read the camera as the game left it. The first write of each frame
        // records this primary; later draws reuse it, so the world translation
        // is applied exactly once per frame.
        std::array<f32, 16> current{};
        device_memory.ReadBlockUnsafe(gpu_address + 0x30, current.data(), sizeof(current));
        const auto base = CameraPrimaryMatrix(i, frame, current);
        std::array<f32, 16> moved = base;
        for (std::size_t row = 0; row < 4; ++row) {
            moved[row * 4 + 3] += base[row * 4 + 0] * (*delta)[0] +
                                  base[row * 4 + 1] * (*delta)[1] +
                                  base[row * 4 + 2] * (*delta)[2];
        }
        // View2 = projection^-1 * VP2, stored as the 3x4 view at +0x0.
        std::array<f32, 16> projection{};
        device_memory.ReadBlockUnsafe(gpu_address + 0x70, projection.data(),
                                      sizeof(projection));
        std::array<f32, 16> inverse{};
        if (InvertMatrix(projection, inverse)) {
            const auto view = MultiplyMatrices(inverse, moved);
            device_memory.WriteBlockUnsafe(gpu_address, view.data(), 12 * sizeof(f32));
        }
        device_memory.WriteBlockUnsafe(gpu_address + 0x30, moved.data(), sizeof(moved));
        if (cpu_address != 0) {
            // Bypassing the memory manager leaves the buffer cache with the old
            // GPU copy. Mark the view and VP dirty so Configure uploads them.
            buffer_cache.WriteMemory(cpu_address, 0xB0, false);
        }
        if (logs < 3) {
            std::array<f32, 16> check{};
            device_memory.ReadBlockUnsafe(gpu_address + 0x30, check.data(), sizeof(check));
            std::fprintf(stderr,
                         "[splitcam] pre[%u] addr=%#llx base=(%.3f %.3f %.3f %.3f) "
                         "col3=(%.3f %.3f %.3f %.3f) read=(%.3f %.3f %.3f %.3f)\n",
                         i, (unsigned long long)gpu_address, base[3], base[7], base[11],
                         base[15], moved[3], moved[7], moved[11], moved[15], check[3],
                         check[7], check[11], check[15]);
            ++logs;
        }
    }
}

} // namespace CoopCamera
