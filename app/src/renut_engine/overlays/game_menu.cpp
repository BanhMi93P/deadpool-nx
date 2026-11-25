#include "renut_engine/overlays/game_menu.h"

#include <string>

#include <rex/cvar.h>
#include <rex/ui/keybinds.h>

#include "imgui.h"
#include "renut_engine/Fps.h"
#include "renut_engine/gpu/backend/gpu_profile.h"
#include "renut_engine/overlays/app_imgui.h"

REXCVAR_DECLARE(bool, banjo_show_fps);
REXCVAR_DECLARE(bool, banjo_show_profiler);

namespace renut {

GameMenuDialog::GameMenuDialog(rex::ui::ImGuiDrawer* drawer, FPSManager* fps_manager)
    : rex::ui::ImGuiDialog(drawer), fps_manager_(fps_manager) {
  rex::ui::RegisterBind("bind_game_menu", "F6", "Toggle Deadpool quick menu", [this] {
    visible_ = !visible_;
    if (visible_) {
      grab_focus_ = true;
    }
  });
}

GameMenuDialog::~GameMenuDialog() {
  rex::ui::UnregisterBind("bind_game_menu");
}

void GameMenuDialog::OnDraw(ImGuiIO& io) {
  if (!visible_) return;
  if (!AppImGuiReady()) return;

  if (grab_focus_) {
    ImGui::SetNextWindowFocus();
    grab_focus_ = false;
  }

  ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowBgAlpha(0.88f);

  bool open = true;
  if (ImGui::Begin("Deadpool Menu (+ & -)##deadpool_game_menu", &open,
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    bool show_fps = fps_manager_ ? fps_manager_->showFPS : REXCVAR_GET(banjo_show_fps);
    if (ImGui::Checkbox("Show FPS", &show_fps)) {
      if (fps_manager_) {
        fps_manager_->showFPS = show_fps;
      }
      rex::cvar::SetFlagByName("banjo_show_fps", show_fps ? "true" : "false");
    }

    bool show_profiler = REXCVAR_GET(banjo_show_profiler);
    if (ImGui::Checkbox("Show Profiler", &show_profiler)) {
      rex::cvar::SetFlagByName("banjo_show_profiler", show_profiler ? "true" : "false");
    }

    ImGui::Spacing();
    if (ImGui::Button("Capture Frame Diag")) {
      renut::gpu::gpu_profile::RequestFrameDiagCapture();
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
      open = false;
    }

    const std::string diag_status = renut::gpu::gpu_profile::FrameDiagStatus();
    if (!diag_status.empty()) {
      ImGui::Spacing();
      ImGui::TextWrapped("%s", diag_status.c_str());
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
      open = false;
    }
  }
  ImGui::End();

  if (!open) {
    visible_ = false;
  }
}

}  // namespace renut
