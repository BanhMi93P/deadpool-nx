#pragma once

#include <string>

#include <rex/ui/imgui_dialog.h>

class FPSManager;

namespace renut {

class GameMenuDialog : public rex::ui::ImGuiDialog {
 public:
  GameMenuDialog(rex::ui::ImGuiDrawer* drawer, FPSManager* fps_manager);
  ~GameMenuDialog() override;

  bool WantsFrame() const override { return visible_; }
  void OnDraw(ImGuiIO& io) override;

 private:
  FPSManager* fps_manager_ = nullptr;
  bool visible_ = false;
  bool grab_focus_ = false;
  std::string last_diag_status_;
};

}  // namespace renut
