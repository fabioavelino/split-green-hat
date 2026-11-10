#include "coop_camera.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace CoopCamera {
namespace {

std::atomic<bool> g_wanted{false};
std::mutex g_mutex;
std::optional<Frame> g_pending;  // the frame the renderer is drawing
std::optional<Frame> g_latest;   // the last finished one

std::atomic<bool> g_use_second{false};
std::mutex g_second_mutex;
std::optional<std::array<float, 16>> g_second;

std::atomic<bool> g_split{false};
std::atomic<bool> g_split_vertical{true};
std::atomic<std::uint32_t> g_current_view{0};
std::atomic<std::uint64_t> g_camera_address{0};
std::atomic<std::uint64_t> g_camera_cpu_address{0};
constexpr unsigned kMaxCameraAddresses = 8;
std::mutex g_camera_mutex;
std::array<std::pair<std::uint64_t, std::uint64_t>, kMaxCameraAddresses> g_cameras{};
unsigned g_camera_count = 0;

} // namespace

void SetWanted(bool wanted) {
    if (g_wanted.exchange(wanted) && !wanted) {
        std::lock_guard lock(g_mutex);
        g_pending.reset();
        g_latest.reset();
    }
}

bool Wanted() {
    return g_wanted.load(std::memory_order_relaxed);
}

bool Better(std::uint64_t frame, float width, float height) {
    std::lock_guard lock(g_mutex);
    return !g_pending || g_pending->frame != frame ||
           width * height > g_pending->width * g_pending->height;
}

void Publish(std::uint64_t frame, const float* clip_from_world, float width, float height,
             bool y_down) {
    Frame f;
    std::copy_n(clip_from_world, 16, f.clip_from_world.begin());
    f.width = width;
    f.height = height;
    f.y_down = y_down;
    f.frame = frame;
    f.at = std::chrono::steady_clock::now();
    std::lock_guard lock(g_mutex);
    if (g_pending && g_pending->frame != frame) {
        g_latest = g_pending;  // a new frame began: the last one is finished
    }
    g_pending = f;
}

std::optional<Frame> Latest() {
    std::lock_guard lock(g_mutex);
    return g_latest;
}

void SetSecondMatrix(const std::array<float, 16>& clip_from_world) {
    std::lock_guard lock(g_second_mutex);
    g_second = clip_from_world;
}

void SetUseSecond(bool use) {
    g_use_second.store(use, std::memory_order_relaxed);
}

bool UseSecond() {
    return g_use_second.load(std::memory_order_relaxed);
}

std::optional<std::array<float, 16>> SecondMatrix() {
    std::lock_guard lock(g_second_mutex);
    return g_second;
}

void SetSplit(bool split) {
    g_split.store(split, std::memory_order_relaxed);
}

bool Split() {
    return g_split.load(std::memory_order_relaxed);
}

void SetSplitLayout(bool vertical) {
    g_split_vertical.store(vertical, std::memory_order_relaxed);
}

bool SplitLayoutVertical() {
    return g_split_vertical.load(std::memory_order_relaxed);
}

void SetCurrentView(std::uint32_t view) {
    g_current_view.store(view, std::memory_order_relaxed);
}

std::uint32_t CurrentView() {
    return g_current_view.load(std::memory_order_relaxed);
}

void SetCameraAddress(std::uint64_t address) {
    g_camera_address.store(address, std::memory_order_relaxed);
}

std::uint64_t CameraAddress() {
    return g_camera_address.load(std::memory_order_relaxed);
}

void SetCameraCpuAddress(std::uint64_t address) {
    g_camera_cpu_address.store(address, std::memory_order_relaxed);
}

std::uint64_t CameraCpuAddress() {
    return g_camera_cpu_address.load(std::memory_order_relaxed);
}

void AddCameraAddress(std::uint64_t gpu_address, std::uint64_t cpu_address) {
    if (gpu_address < 0x1000) {
        return;
    }
    std::lock_guard lock(g_camera_mutex);
    for (unsigned i = 0; i < g_camera_count; ++i) {
        if (g_cameras[i].first == gpu_address) {
            g_cameras[i].second = cpu_address;
            return;
        }
    }
    if (g_camera_count < kMaxCameraAddresses) {
        g_cameras[g_camera_count++] = {gpu_address, cpu_address};
    }
}

unsigned CameraAddressCount() {
    std::lock_guard lock(g_camera_mutex);
    return g_camera_count;
}

std::pair<std::uint64_t, std::uint64_t> CameraAddressAt(unsigned index) {
    std::lock_guard lock(g_camera_mutex);
    return index < g_camera_count ? g_cameras[index]
                                  : std::pair<std::uint64_t, std::uint64_t>{0, 0};
}

} // namespace CoopCamera
