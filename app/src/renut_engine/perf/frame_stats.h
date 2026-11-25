#pragma once
#include <cstdint>
#include <string>  // blur-nx (2026-11-13): WaitReport
// Frame statistics for the FPS overlay. A game frame is one D3DDevice_Swap:
// the frames the game actually finishes, whatever its update loop does.

namespace renut::frame_stats {

struct Snapshot {
    static constexpr int kHistory = 180;

    double fps = 0.0;        // game frames per second over the last second
    double frame_ms = 0.0;   // their average length
    double worst_ms = 0.0;   // the longest of them
    // Averages over the same frames. CPU is the game thread's update plus
    // render (appMainTickPreDraw + appMainDraw, wall time) less the time it
    // waited for the render thread (OnGameThreadWait); GPU is the time the
    // GPU spent executing each frame's command lists (timestamp queries).
    double cpu_ms = 0.0;
    double gpu_ms = 0.0;
    bool gpu_known = false;  // false until the renderer has reported a GPU time
    // The render thread's busy time per frame (it records the GPU work the
    // game thread queues), less its waits for the GPU and the display
    // (render_thread::WaitScope), averaged the same way.
    double render_thread_ms = 0.0;
    bool render_thread_known = false;
    double update_ms = 0.0;  // appMainTickPreDraw, last frame
    double render_ms = 0.0;  // appMainDraw, last frame (CPU time issuing the frame)
    float history[kHistory] = {};  // frame lengths in ms, oldest first
    int history_count = 0;
};

struct PacingSample {
    float frame_ms = 0.0f;
    float cpu_active_ms = 0.0f;
    float wait_fence_ms = 0.0f;
    float wait_swap_ahead_ms = 0.0f;
    float wait_display_ms = 0.0f;
    float wait_limiter_ms = 0.0f;
    float render_thread_ms = 0.0f;
    float gpu_ms = 0.0f;
};

struct PacingSnapshot {
    static constexpr int kRingSize = 256;

    int count = 0;
    double best_ms = 0.0;
    double worst_ms = 0.0;
    double median_ms = 0.0;
    double avg_ms = 0.0;
    double avg_fps = 0.0;
    double one_percent_low_fps = 0.0;
    int stutters = 0;          // > 2x median
    int hitches = 0;           // > 45 ms (sampled window)
    uint64_t total_hitches = 0;
    uint32_t vblanks[5] = {};  // 1, 2, 3, 4, 5+ (16.6667 ms steps)

    double cur_cpu_active_ms = 0.0, avg_cpu_active_ms = 0.0;
    double cur_wait_fence_ms = 0.0, avg_wait_fence_ms = 0.0;
    double cur_wait_swap_ahead_ms = 0.0, avg_wait_swap_ahead_ms = 0.0;
    double cur_wait_display_ms = 0.0, avg_wait_display_ms = 0.0;
    double cur_wait_limiter_ms = 0.0, avg_wait_limiter_ms = 0.0;
    double cur_render_thread_ms = 0.0, avg_render_thread_ms = 0.0;
    double cur_gpu_ms = 0.0, avg_gpu_ms = 0.0;
    bool gpu_known = false;
    bool render_thread_known = false;
};

// Called by the renderer at each guest D3DDevice_Swap.
void OnGameFrame();

// Called by the renderer when a frame's GPU time is known (a frame or two
// after it was submitted).
void OnGpuFrame(double ms);

// Called by the render thread at the end of each frame's work.
void OnRenderThreadFrame(double busy_ms);

// Called by the renderer when the game thread has waited for the render
// thread (Swap's frame pacing, fence waits), for the CPU time to leave out.
// 'source': 0 = fence (BlockOnFence), 1 = Swap's frames-ahead limit, 2 = display refresh, 3 = frame limiter.
void OnGameThreadWait(double ms, int source = 0);
// The 10 s report's line of those waits (count and ms a second by source), and the window started again.
std::string WaitReport();
// Non-mutating peek of WaitReport() for one-shot diagnostic captures.
std::string WaitReportPeek();

Snapshot Get();
PacingSnapshot GetPacingSnapshot();

} // namespace renut::frame_stats
