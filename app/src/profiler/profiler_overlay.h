// banjo-nx: the on-screen profiler, ported from UnleashedRecomp's DrawProfiler() (gpu/video.cpp).
//
// Same layout: a "Frame Time" graph of every series over the last 256 UI frames, then the current and
// average value of each, then renderer/device facts. Unleashed plots with ImPlot; the SDK's ImGui
// (1.92) is newer than any ImPlot release Unleashed carries, so the graph is drawn with the window's
// draw list instead. F5 on PC (F1 is reNut's FPS overlay); banjo_show_profiler everywhere.
#pragma once

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include "imgui.h"
#include "renut_engine/overlays/app_imgui.h"  // renut-ultimate (port 66)
#include "renut_engine/perf/frame_stats.h"
#include "profiler/profiler.h"

REXCVAR_DECLARE(bool, banjo_show_profiler);
extern "C" bool RexSwitchPerfStacksActive(void);

class ProfilerOverlayDialog : public rex::ui::ImGuiDialog {
 public:
  explicit ProfilerOverlayDialog(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {
    rex::ui::RegisterBind("bind_profiler", "F5", "Toggle CPU/GPU profiler", [] {
      rex::cvar::SetFlagByName("banjo_show_profiler", REXCVAR_GET(banjo_show_profiler) ? "false" : "true");
    });
  }
  ~ProfilerOverlayDialog() override { rex::ui::UnregisterBind("bind_profiler"); }

  bool WantsFrame() const override { return REXCVAR_GET(banjo_show_profiler); }

  void OnDraw(ImGuiIO& io) override {
    if (!REXCVAR_GET(banjo_show_profiler)) return;
    if (!AppImGuiReady()) return;  // renut-ultimate (port 66)

    using banjo::profiler::Series;
    constexpr size_t kSeries = static_cast<size_t>(Series::kCount);
    constexpr size_t kHistory = 256;

    // Sample every series once per UI frame when visible.
    for (size_t s = 0; s < kSeries; ++s) {
      const auto series = static_cast<Series>(s);
      history_[s][index_] = banjo::profiler::HasValue(series)
                                ? static_cast<float>(banjo::profiler::Current(series))
                                : 0.0f;
    }
    index_ = (index_ + 1) % kHistory;

    const ImGuiWindowFlags hud_flags = ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoFocusOnAppearing;

    // Window 1: top-right Frame Time graph + series breakdown.
    ImGui::SetNextWindowSize(ImVec2(540.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(std::max(20.0f, io.DisplaySize.x - 560.0f), 20.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.78f);
    bool open = true;
    if (ImGui::Begin("Profiler", &open, hud_flags)) {
      DrawGraph();

      ImGui::Separator();
      for (size_t s = 0; s < kSeries; ++s) {
        const auto series = static_cast<Series>(s);
        if (!banjo::profiler::HasValue(series)) {
          ImGui::TextDisabled("%-28s n/a", banjo::profiler::SeriesName(series));
          continue;
        }
        const double cur = banjo::profiler::Current(series);
        const double avg = Average(s);
        ImGui::TextColored(Color(s), "%-28s", banjo::profiler::SeriesName(series));
        ImGui::SameLine();
        if (series == Series::kLimiterWait || series == Series::kRenderThread) {
          ImGui::Text("%7.2f ms  (avg %7.2f ms)", cur, avg);
        } else {
          ImGui::Text("%7.2f ms  (avg %7.2f ms, %5.1f FPS)", cur, avg, avg > 0.0 ? 1000.0 / avg : 0.0);
        }
      }

      ImGui::Separator();
      ImGui::Text("Renderer: %s", banjo::profiler::RendererApi().c_str());
      const std::string device = banjo::profiler::RendererDevice();
      if (!device.empty()) ImGui::Text("Device: %s", device.c_str());

      if (io.DeltaTime > 0.0f) threads_age_ += io.DeltaTime;
      if (threads_age_ >= 1.0f) {
        threads_age_ = 0.0f;
        const std::string threads = banjo::profiler::ThreadCpuUsage();
        if (!threads.empty()) last_threads_ = threads;
      }
      if (!last_threads_.empty() && ImGui::TreeNode("Thread CPU use")) {
        ImGui::TextWrapped("%s", last_threads_.c_str());
        ImGui::TreePop();
      }
    }
    ImGui::End();

    // Window 2: bottom-left Frame Pacing / Vblanks / Process CPU% / Wait Breakdown.
    const auto pacing = renut::frame_stats::GetPacingSnapshot();
    const auto proc_cpu = banjo::profiler::QueryProcessCpuUsage();
    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(20.0f, std::max(20.0f, io.DisplaySize.y - 340.0f)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.78f);
    bool open_pacing = true;
    if (ImGui::Begin("Profiler (Deadpool)", &open_pacing, hud_flags)) {
      ImGui::Text("Frame pacing (last %d game frames)", pacing.count);
      if (pacing.count > 0) {
        ImGui::Text("best %.2f ms  |  median %.2f ms  |  worst %.2f ms",
                    pacing.best_ms, pacing.median_ms, pacing.worst_ms);
        ImGui::Text("average %.1f FPS  |  1%% low %.1f FPS  |  stutters (>2x med): %d",
                    pacing.avg_fps, pacing.one_percent_low_fps, pacing.stutters);
        ImGui::Text("vblanks per frame (16.7 ms): 1:%u  2:%u  3:%u  4:%u  5+:%u",
                    pacing.vblanks[0], pacing.vblanks[1], pacing.vblanks[2],
                    pacing.vblanks[3], pacing.vblanks[4]);
      } else {
        ImGui::TextDisabled("waiting for game frames...");
      }

      ImGui::Separator();
      if (proc_cpu.valid) {
        ImGui::Text("Process CPU (all threads): %.1f%% of one core, %.1f%% of the four",
                    proc_cpu.pct_one_core, proc_cpu.pct_four_cores);
      } else {
        ImGui::TextDisabled("Process CPU (all threads): n/a");
      }

      if (pacing.count > 0) {
        ImGui::Text("Game thread CPU (active):    %6.2f ms  (avg %6.2f ms)",
                    pacing.cur_cpu_active_ms, pacing.avg_cpu_active_ms);
        ImGui::Text("Game waiting for fences:     %6.2f ms  (avg %6.2f ms)",
                    pacing.cur_wait_fence_ms, pacing.avg_wait_fence_ms);
        ImGui::Text("Game waiting for Swap:       %6.2f ms  (avg %6.2f ms)",
                    pacing.cur_wait_swap_ahead_ms, pacing.avg_wait_swap_ahead_ms);
        ImGui::Text("Game waiting for display:    %6.2f ms  (avg %6.2f ms)",
                    pacing.cur_wait_display_ms, pacing.avg_wait_display_ms);
        ImGui::Text("Game waiting for limiter:    %6.2f ms  (avg %6.2f ms)",
                    pacing.cur_wait_limiter_ms, pacing.avg_wait_limiter_ms);
        if (pacing.render_thread_known) {
          ImGui::Text("Render thread:               %6.2f ms  (avg %6.2f ms)",
                      pacing.cur_render_thread_ms, pacing.avg_render_thread_ms);
        } else {
          ImGui::TextDisabled("Render thread:               n/a");
        }
        if (pacing.gpu_known) {
          ImGui::Text("GPU Frame:                   %6.2f ms  (avg %6.2f ms)",
                      pacing.cur_gpu_ms, pacing.avg_gpu_ms);
        } else {
          ImGui::TextDisabled("GPU Frame:                   n/a");
        }
      }

      ImGui::Separator();
      ImGui::Text("Hitches (> 45 ms): %d in ring  (%llu total)",
                  pacing.hitches, static_cast<unsigned long long>(pacing.total_hitches));
      const bool stacks_on = RexSwitchPerfStacksActive();
      ImGui::Text("Stack profiler (perfil_pilas.flag):");
      ImGui::SameLine();
      ImGui::TextColored(stacks_on ? ImVec4(0.35f, 1.0f, 0.35f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                         "%s", stacks_on ? "ON" : "off");
    }
    ImGui::End();

    if (!open || !open_pacing) rex::cvar::SetFlagByName("banjo_show_profiler", "false");
  }

 private:
  static constexpr size_t kSeriesCount = static_cast<size_t>(banjo::profiler::Series::kCount);
  static constexpr size_t kHistoryCount = 256;

  static ImVec4 Color(size_t s) {
    static const ImVec4 kColors[] = {
        ImVec4(0.95f, 0.95f, 0.95f, 1.0f),  // Application
        ImVec4(0.35f, 0.75f, 1.00f, 1.0f),  // Update
        ImVec4(1.00f, 0.65f, 0.25f, 1.0f),  // Draw
        ImVec4(0.55f, 0.55f, 0.55f, 1.0f),  // Limiter
        ImVec4(0.40f, 1.00f, 0.40f, 1.0f),  // GPU
        ImVec4(1.00f, 0.40f, 0.75f, 1.0f),  // Present
        ImVec4(1.00f, 0.90f, 0.30f, 1.0f),  // Render thread (Renut's renderer)
    };
    return kColors[s % (sizeof(kColors) / sizeof(kColors[0]))];
  }

  double Average(size_t s) const {
    double sum = 0.0;
    size_t n = 0;
    for (float v : history_[s]) {
      if (v > 0.0f) {
        sum += v;
        ++n;
      }
    }
    return n ? sum / double(n) : 0.0;
  }

  void DrawGraph() {
    ImGui::TextUnformatted("Frame Time (ms)");
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 160.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(20, 20, 24, 220));

    // Unleashed fixes the axis at 0-20 ms for 60 FPS; Banjo runs at 30, so grow to fit (at least 40 ms).
    float top = 40.0f;
    for (size_t s = 0; s < kSeriesCount; ++s)
      for (float v : history_[s]) top = std::max(top, std::min(v, 250.0f));
    for (float ms : {16.67f, 33.33f, 50.0f, 100.0f, 200.0f}) {
      if (ms >= top) break;
      const float y = p1.y - (ms / top) * size.y;
      dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), IM_COL32(80, 80, 90, 160));
      char label[16];
      if (ms < 40.0f) {
        std::snprintf(label, sizeof(label), "%.1f", ms);
      } else {
        std::snprintf(label, sizeof(label), "%.0f", ms);
      }
      dl->AddText(ImVec2(p0.x + 2.0f, y - ImGui::GetTextLineHeight()), IM_COL32(150, 150, 160, 255), label);
    }

    for (size_t s = 0; s < kSeriesCount; ++s) {
      if (!banjo::profiler::HasValue(static_cast<banjo::profiler::Series>(s))) continue;
      const ImU32 col = ImGui::ColorConvertFloat4ToU32(Color(s));
      ImVec2 prev{};
      for (size_t i = 0; i < kHistoryCount; ++i) {
        const float v = std::min(history_[s][(index_ + i) % kHistoryCount], top);
        const ImVec2 pt(p0.x + size.x * float(i) / float(kHistoryCount - 1), p1.y - (v / top) * size.y);
        if (i) dl->AddLine(prev, pt, col, 1.5f);
        prev = pt;
      }
    }
    ImGui::Dummy(size);
  }

  std::array<std::array<float, kHistoryCount>, kSeriesCount> history_{};
  size_t index_ = 0;
  std::string last_threads_;
  float threads_age_ = 0.0f;
};
