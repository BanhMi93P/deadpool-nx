// katamari-nx (2026-10-30): the keys of banjo-nx files this port does not build, because their code is written for
// N&B's addresses (cpu/banjo_reverb_native.cpp, cpu/banjo_xaudio_native.cpp, cpu/banjo_hot_guest.cpp,
// renut_engine/hooks.cpp, renut_engine/frameHooks.cpp). banjo-nx's enhancement table (cpu/banjo_enhancements.cpp) and
// Renut's renderer still read them by name, so they are defined here, off: nothing in this build uses their code.
// Each comes back with its own file once it has been moved to Beautiful Katamari's functions (docs/katamari-notes.md).

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>

#include "profiler/profiler.h"
#include "renut_engine/display_sync.h"

#if defined(__SWITCH__)
#include <switch.h>
#endif

// N&B's XAudio natives (its reverb and sample-rate converter at N&B's addresses).
REXCVAR_DEFINE_BOOL(banjo_native_reverb, false, "Katamari/Not built", "N&B's native XAudio reverb (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_native_reverb_verify, false, "Katamari/Not built", "Verify mode of the above (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_native_xaudio_src, false, "Katamari/Not built", "N&B's native XAudio SRC (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_native_xaudio_src_verify, false, "Katamari/Not built", "Verify mode of the above (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The hand-written natives' keys and banjo_hot_count are cpu/banjo_hot_guest.cpp's (Katamari's natives: memset and the
// matrix routines, cpu/hot/banjo_hand_natives.h).

// renut_engine/hooks.cpp's MSAA switch: N&B's own key, read only by its mid-asm hooks at N&B's render-target setup
// (hooks.cpp, which this port does not build). Renut's renderer reads msaa_level (1/2/4) instead (vivapinata-nx's note,
// 2026-11-12; docs/katamari-notes.md, run emu3_nomsaa). Kept so the tomls that name it still load.
REXCVAR_DEFINE_BOOL(disable_msaa, false, "Katamari/Graphics",
                    "Not read in this port (N&B's key): msaa_level sets the samples (4, 2 or 1)");

// The frame cap (renut_engine/frameHooks.cpp's limiter, not built here because of its N&B hooks).
REXCVAR_DEFINE_STRING(frame_cap, "Off", "Katamari/Performance",
                      "Frame rate cap: Off (the game's own, uncapped as on the Xbox 360) or a number of frames a second")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// katamari-nx (2026-11-02, the user's request: a 30 fps cap in katamari.toml, off by default): renutFrameLimit of
// renut_engine/frameHooks.cpp, called at the end of the game's Swap (d3d/hooks/device.cpp). With the display's refresh
// known (the Switch) a frame is released at a refresh plus the measured phase (renut_engine/display_sync.h), else on its
// own timer; a late frame goes at once. Off, it returns at the first test and starts no thread.
void renutFrameLimit() {
  using clock = std::chrono::steady_clock;
  static const int fps = [] {
    const std::string &v = REXCVAR_GET(frame_cap);
    return v.empty() || v == "Off" ? 0 : std::atoi(v.c_str());
  }();
  // pacman-nx (2026-11-03, pacman_vblank_display_sync, futari-nx's): with no frame cap the display's refresh starts the
  // game's next frame (display_sync.h). The game's speed follows its frames.
  if (fps <= 0) {
    static const bool pacing = banjo::display_sync::FramePacingOn();
    if (pacing)
      banjo::display_sync::WaitForFrameStart();
    return;
  }
  // sa2-nx (2026-10-28, sa2_renut_queue_drain): this release a refresh later, once, when the display thread asks.
  static const bool drain = banjo::display_sync::QueueDrainOn();
  const bool hold = drain && banjo::display_sync::TakeHold();
  constexpr uint64_t kRefreshNs = 16'666'667ull;
  banjo::display_sync::Start();
  static clock::time_point next = clock::now();
  static uint64_t previous_release = 0;
  const clock::time_point wait_start = clock::now();
  clock::time_point now = wait_start;
  const auto frame = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / fps));
  constexpr auto kTail = std::chrono::microseconds(400);
  const auto wait_until = [&](clock::time_point target) {
    for (;;) {
      now = clock::now();
      if (now >= target)
        break;
      if (target - now > kTail)
        std::this_thread::sleep_for(target - now - kTail);
      else
        std::this_thread::yield();
    }
  };
  bool anchored = false;
  uint64_t release = banjo::display_sync::NextRelease(
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count()), previous_release,
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(frame).count()), &anchored);
  if (release && hold)
    release += kRefreshNs;
  if (release) {
    previous_release = release;
    next = clock::time_point(std::chrono::duration_cast<clock::duration>(std::chrono::nanoseconds(release)));
    wait_until(next);
    banjo::display_sync::NoteRelease(release, anchored);
  } else {
    previous_release = 0;
    next += frame;
    if (next <= now)
      next = now;  // a slow frame: from the present, no burst to catch up
    else
      wait_until(next);
  }
  banjo::profiler::RecordLimiterWait(std::chrono::duration<double, std::milli>(clock::now() - wait_start).count());
}

// banjo-nx's PM4 renderer's D3D-level fast paths (me/banjo_d3d_clear.h, me/banjo_d3d_native.h, defined there because
// banjo_me_hooks.cpp includes them): its hooks are N&B's, so off here (masseffect_renderer_native stays false anyway).
REXCVAR_DEFINE_BOOL(banjo_d3d_api_clear, false, "Katamari/Not built", "banjo-nx's PM4 renderer: Clear markers (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_d3d_api_resolve, false, "Katamari/Not built", "banjo-nx's PM4 renderer: Resolve markers (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_d3d_native_draw_indexed, false, "Katamari/Not built", "banjo-nx's PM4 renderer: native DrawIndexed (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_d3d_native_draw_vertices, false, "Katamari/Not built", "banjo-nx's PM4 renderer: native DrawVertices (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Game frames presented (banjo-nx counted them in banjo_me_hooks.cpp's Swap hook): Renut's Present hook counts them
// here (d3d/hooks/draw.cpp); the captures' video waits on it.
std::atomic<uint64_t> g_banjo_game_frames{0};

REXCVAR_DEFINE_BOOL(deadpool_perf_log, false, "Katamari/Debug",
                    "Enable comprehensive CPU, GPU, memory, stack-sampling, and workload performance logging")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(
    language, "english", "Kernel",
    "Game language: auto (Switch system language), english, german, french, spanish, or italian");

extern "C" const char* RexAppBuildStamp(void) {
  return "deadpool-nx (" __DATE__ " " __TIME__ ")";
}

namespace deadpool {

namespace {

uint32_t DetectSwitchLanguageId() {
#if defined(__SWITCH__)
  Result rc = setInitialize();
  if (R_FAILED(rc)) {
    REXLOG_WARN("Deadpool language: setInitialize failed (0x{:08X}), falling back to English", rc);
    return 1;
  }
  u64 lang_code = 0;
  SetLanguage sys_lang = SetLanguage_ENUS;
  rc = setGetSystemLanguage(&lang_code);
  if (R_SUCCEEDED(rc)) {
    rc = setMakeLanguage(lang_code, &sys_lang);
  }
  setExit();
  if (R_FAILED(rc)) {
    REXLOG_WARN("Deadpool language: Switch system language query failed (0x{:08X}), falling back to English", rc);
    return 1;
  }
  switch (sys_lang) {
    case SetLanguage_DE:
      return 3;  // DEU
    case SetLanguage_FR:
    case SetLanguage_FRCA:
      return 4;  // FRA
    case SetLanguage_ES:
    case SetLanguage_ES419:
      return 5;  // ESN
    case SetLanguage_IT:
      return 6;  // ITA
    default:
      return 1;  // INT (English)
  }
#else
  return 1;
#endif
}

std::string NormalizeLanguageValue(std::string value) {
  auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

uint32_t ResolveLanguageId(std::string_view raw, bool* valid = nullptr) {
  const std::string key = NormalizeLanguageValue(std::string(raw));
  if (valid) *valid = true;
  if (key == "auto" || key == "system" || key == "0") {
    return DetectSwitchLanguageId();
  }
  if (key == "english" || key == "en" || key == "int" || key == "1") {
    return 1;
  }
  if (key == "german" || key == "de" || key == "deu" || key == "deutsch" || key == "3") {
    return 3;
  }
  if (key == "french" || key == "fr" || key == "fra" || key == "francais" || key == "fran\xc3\xa7" "ais" || key == "4") {
    return 4;
  }
  if (key == "spanish" || key == "es" || key == "esn" || key == "espanol" || key == "espa\xc3\xb1" "ol" || key == "5") {
    return 5;
  }
  if (key == "italian" || key == "it" || key == "ita" || key == "italiano" || key == "6") {
    return 6;
  }
  if (valid) *valid = false;
  return 1;
}

const char* LanguageCodeName(uint32_t id) {
  switch (id) {
    case 3: return "DEU (German)";
    case 4: return "FRA (French)";
    case 5: return "ESN (Spanish)";
    case 6: return "ITA (Italian)";
    default: return "INT (English)";
  }
}

}  // namespace

void ApplyLanguageConfig() {
  const auto lang_src = rex::cvar::GetFlagSource("language");
  const auto user_lang_src = rex::cvar::GetFlagSource("user_language");
  if (user_lang_src >= rex::cvar::Source::kConfig && user_lang_src > lang_src) {
    return;
  }
  if (lang_src < rex::cvar::Source::kConfig && user_lang_src >= rex::cvar::Source::kConfig) {
    return;
  }

  const std::string raw = REXCVAR_GET(language);
  bool ok = false;
  const uint32_t resolved = ResolveLanguageId(raw, &ok);
  if (!ok) {
    REXLOG_WARN("Deadpool language '{}' is unsupported (use auto, english, german, french, spanish, italian); using English",
                raw);
  }
  rex::cvar::SetFlagByName("user_language", std::to_string(resolved));
  REXLOG_INFO("Deadpool language: '{}' -> {} (XGetLanguage={})", raw, LanguageCodeName(resolved), resolved);
}

}  // namespace deadpool


// g_banjo_store_cards (whether the generated code marks the store cards) is cpu/banjo_hot_guest.cpp's again.

// renut_engine/cvar_menu.cpp's toml writer (N&B's in-game settings menu, not built): the keyboard controls dialog
// calls it after a rebind. Not written here: the toml a test or the user wrote stays as it is.
void RenutSaveConfig() {}
