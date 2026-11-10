// The game's camera, handed from the renderer (GPU thread) to the co-op hook
// (present thread). Built into the emulator's core library; the patched
// vk_rasterizer.cpp fills it through coop_camera_track.h. No emulator code.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

namespace CoopCamera {

struct Frame {
    // World to clip, row-major as the game's shader reads it (clip = m * p).
    std::array<float, 16> clip_from_world{};
    float width = 0.0f;   // the scene target the matrix was drawn into, pixels
    float height = 0.0f;
    // Clip y grows down the picture (the opposite of OpenGL), from the
    // viewport and window origin of the draw.
    bool y_down = false;
    std::uint64_t frame = 0;  // the renderer's frame counter
    std::chrono::steady_clock::time_point at{};
};

// Turned on by the hook while Ocarina is running with co-op on, so the
// renderer does nothing otherwise. Any thread.
void SetWanted(bool wanted);
bool Wanted();

// GPU thread. True when a camera from a target of this size would replace
// what `frame` already has (the largest scene target of a frame wins, as in
// the emulator's own DLSS path: reflections and half-size passes are smaller).
bool Better(std::uint64_t frame, float width, float height);
void Publish(std::uint64_t frame, const float* clip_from_world, float width, float height,
             bool y_down);

// The camera of the last finished frame, if any. Any thread.
std::optional<Frame> Latest();

// M2: replace the game camera with a second world-to-clip matrix on the next
// draws. The split hook computes it from a player-2 chase/translation. Off by
// default; the renderer only writes it when asked.
void SetSecondMatrix(const std::array<float, 16>& clip_from_world);
void SetUseSecond(bool use);
bool UseSecond();
std::optional<std::array<float, 16>> SecondMatrix();

// Split-screen: frame N uses player 1's camera, frame N+1 player 2's. The
// renderer marks the view it drew and the presenter composites the two
// histories into the two halves.
void SetSplit(bool split);
bool Split();
void SetSplitLayout(bool vertical);
bool SplitLayoutVertical();
void SetCurrentView(std::uint32_t view);
std::uint32_t CurrentView();

} // namespace CoopCamera
