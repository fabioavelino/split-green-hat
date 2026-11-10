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
std::mutex g_primary_mutex;
std::optional<std::array<float, 16>> g_primary;
std::mutex g_delta_mutex;
std::optional<std::array<float, 3>> g_delta;
std::mutex g_rig_mutex;
std::optional<SecondRig> g_rig;

std::atomic<bool> g_split{false};
std::atomic<bool> g_split_vertical{true};
std::atomic<std::uint32_t> g_current_view{0};
std::atomic<std::uint64_t> g_camera_address{0};
std::atomic<std::uint64_t> g_camera_cpu_address{0};
constexpr unsigned kMaxCameraAddresses = 256;
struct CameraBinding {
    std::uint64_t gpu = 0;
    std::uint64_t cpu = 0;
    std::uint64_t frame = 0;
    std::uint64_t seen = 0;
    std::array<float, 16> primary{};
};
std::mutex g_camera_mutex;
std::array<CameraBinding, kMaxCameraAddresses> g_cameras{};
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
    SetPrimaryMatrix(f.clip_from_world);
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

void SetSecondDelta(const std::array<float, 3>& delta) {
    std::lock_guard lock(g_delta_mutex);
    g_delta = delta;
}

std::optional<std::array<float, 3>> SecondDelta() {
    std::lock_guard lock(g_delta_mutex);
    return g_delta;
}

void SetSecondRig(const SecondRig& rig) {
    std::lock_guard lock(g_rig_mutex);
    g_rig = rig;
}

std::optional<SecondRig> SecondRigState() {
    std::lock_guard lock(g_rig_mutex);
    return g_rig;
}

void SetPrimaryMatrix(const std::array<float, 16>& clip_from_world) {
    std::lock_guard lock(g_primary_mutex);
    g_primary = clip_from_world;
}

std::optional<std::array<float, 16>> PrimaryMatrix() {
    std::lock_guard lock(g_primary_mutex);
    return g_primary;
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

void AddCameraAddress(std::uint64_t gpu_address, std::uint64_t cpu_address,
                      std::uint64_t frame) {
    if (gpu_address < 0x1000) {
        return;
    }
    std::lock_guard lock(g_camera_mutex);
    for (unsigned i = 0; i < g_camera_count; ++i) {
        if (g_cameras[i].gpu == gpu_address) {
            g_cameras[i].cpu = cpu_address;
            g_cameras[i].seen = frame;
            return;
        }
    }
    if (g_camera_count < kMaxCameraAddresses) {
        g_cameras[g_camera_count].gpu = gpu_address;
        g_cameras[g_camera_count].cpu = cpu_address;
        g_cameras[g_camera_count].seen = frame;
        ++g_camera_count;
    }
}

std::uint64_t CameraLastSeen(unsigned index) {
    std::lock_guard lock(g_camera_mutex);
    return index < g_camera_count ? g_cameras[index].seen : 0;
}

unsigned CameraAddressCount() {
    std::lock_guard lock(g_camera_mutex);
    return g_camera_count;
}

std::pair<std::uint64_t, std::uint64_t> CameraAddressAt(unsigned index) {
    std::lock_guard lock(g_camera_mutex);
    return index < g_camera_count ? std::pair{g_cameras[index].gpu, g_cameras[index].cpu}
                                  : std::pair<std::uint64_t, std::uint64_t>{0, 0};
}

std::array<float, 16> CameraPrimaryMatrix(unsigned index, std::uint64_t frame,
                                          const std::array<float, 16>& current) {
    std::lock_guard lock(g_camera_mutex);
    if (index >= g_camera_count) {
        return current;
    }
    CameraBinding& camera = g_cameras[index];
    if (camera.frame != frame) {
        camera.frame = frame;
        camera.primary = current;
    }
    return camera.primary;
}

} // namespace CoopCamera
