#pragma once
#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include "imgui.h"
#include "renut_engine/overlays/app_imgui.h"  // renut-ultimate (port 66)
#include "renut_engine/perf/frame_stats.h"
#include <chrono>
#include <memory>
#include <vector>
#include <string>

inline double cpuMS;
inline double gpuMS;
inline int flock;

class FPSCounter {
public:
    std::string name;
    void Tick();
    int AverageCount = 100;
    std::vector<float> frameTimes;
    float averageFps = 0.0f;
    float averageMs = 0.0f;
    std::chrono::steady_clock::time_point lastTick = std::chrono::steady_clock::now();
};

class FPSManager {
public:
    std::vector<std::unique_ptr<FPSCounter>> counters;
    bool showFPS = true;

    FPSCounter* GetCreateCounter(const std::string& name) {
        for (auto& counter : counters) {
            if (counter->name == name) return counter.get();
        }
        auto newCounter = std::make_unique<FPSCounter>();
        newCounter->name = name;
        FPSCounter* ptr = newCounter.get();
        counters.push_back(std::move(newCounter));
        return ptr;
    }
};

class FpsOverlayDialog : public rex::ui::ImGuiDialog {
public:
    explicit FpsOverlayDialog(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {
        rex::ui::RegisterBind("bind_fps_overlay", "F1", "Toggle FPS overlay", [this] {
            if (fpsManager) fpsManager->showFPS = !fpsManager->showFPS;
            });
    }

    ~FpsOverlayDialog() {
        rex::ui::UnregisterBind("bind_fps_overlay");
    }

    FPSManager* fpsManager = nullptr;

    bool WantsFrame() const override {
        return fpsManager && fpsManager->showFPS;
    }

    void OnDraw(ImGuiIO& io) override {

        //Limit draw to every 10 frames
        if(flock > 120){ flock = 0;}
        else{ flock++; }

        if (!fpsManager || !fpsManager->showFPS) return;
        if (!AppImGuiReady()) return;  // renut-ultimate (port 66)

        float hostFps = static_cast<float>(rex::cvar::Query<double>("present_host_fps"));
        float generatedFps = static_cast<float>(rex::cvar::Query<double>("present_fsr3_generated_fps"));
        const auto snap = renut::frame_stats::Get();
        if (fpsManager->counters.empty() && snap.fps <= 0.0 && hostFps <= 0.5f && generatedFps <= 0.5f) return;

        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoFocusOnAppearing;

        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::Begin("##FPSOverlay", nullptr, flags);

        auto color_for = [](float fps) {
            if (fps < 30.0f) return ImVec4(1.0f, 0.15f, 0.15f, 1.0f);
            if (fps < 60.0f) return ImVec4(1.0f, 0.9f, 0.0f, 1.0f);
            if (fps <= 70.0f) return ImVec4(0.2f, 1.0f, 0.2f, 1.0f);
            return ImVec4(0.2f, 0.6f, 1.0f, 1.0f);
        };

        if (!fpsManager->counters.empty()) {
            for (auto& counter : fpsManager->counters) {
                float fps = counter->averageFps;
                ImVec4 color = color_for(fps);
                ImGui::TextColored(color, "%.0f FPS", fps);
                ImGui::TextColored(color,"cpu: %.1fms", cpuMS);
                ImGui::TextColored(color,"gpu: %.1fms", gpuMS);
            }
        } else if (snap.fps > 0.0) {
            ImVec4 color = color_for(static_cast<float>(snap.fps));
            ImGui::TextColored(color, "%.1f FPS (%.1f ms)", snap.fps, snap.frame_ms);
            ImGui::TextColored(color, "cpu: %.1f ms", snap.cpu_ms);
            if (snap.render_thread_known) {
                ImGui::TextColored(color, "rt:  %.1f ms", snap.render_thread_ms);
            }
            if (snap.gpu_known) {
                ImGui::TextColored(color, "gpu: %.1f ms", snap.gpu_ms);
            }
        }

        if (hostFps > 0.5f) {
            ImGui::TextColored(ImVec4(0.65f, 0.4f, 1.0f, 1.0f), "%.0f FPS (host)", hostFps);
        }

        if (generatedFps > 0.5f) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.85f, 1.0f), "%.0f FPS (generated)",
                               generatedFps);
        }

        ImGui::End();
    }
};