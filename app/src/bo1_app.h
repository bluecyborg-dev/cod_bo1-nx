// Call of Duty: Black Ops (Xbox 360) for Nintendo Switch: the ReXGlue application object.
//
// First bring-up version: paths, GPU plugin choice and clocks only. The game is drawn by ReXGlue's Xenos GPU
// emulation (slow on the Switch: the point of this build is to see the game boot, not to play it).

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <memory>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>

#if REX_PLATFORM_SWITCH
#include <rex/ui/switch_apm.h>
#endif

#include "native/bo1_native_system.h"

REXCVAR_DECLARE(std::string, bo1_debug_hash_files);

// bo1_engine.cpp: MINUS + D-pad Right/Left cycles the shadow presets on the Switch.
void Bo1RegisterShadowCombo();
// bo1_engine.cpp: sends the console profiler's reports to the log (bo1_profile_log).
void Bo1StartProfileLog();

class Bo1App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Bo1App>(new Bo1App(ctx, "bo1", PPCImageConfig));
  }

 protected:
  void OnPostInitLogging() override {
    Bo1RegisterShadowCombo();
    Bo1StartProfileLog();
    // The runtime defaults to no GPU plugin: select ReXGlue's Xenos emulation, keeping an explicit choice made
    // in bo1.toml.
    if (!rex::cvar::HasNonDefaultValue("gpu_plugin")) {
      rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    }
  }

  // The Switch has no command line: when --game_data_root is not given, use the game_root/ folder next to the
  // executable (sdmc:/switch/bo1-nx/game_root on the SD card), which holds the files of the disc.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) {
      return;
    }
    std::error_code ec;
    const auto folder = rex::filesystem::GetExecutableFolder();
    const auto extracted = folder / "game_root";
    if (!folder.empty() && std::filesystem::is_directory(extracted, ec)) {
      paths.game_data_root = extracted;
#if REX_PLATFORM_SWITCH
    } else if (std::filesystem::is_directory("sdmc:/switch/bo1-nx/game_root", ec)) {
      // Started by the Homebrew Menu's netloader (tools/nxlink_run.sh), argv[0] is not the usual NRO path.
      paths.game_data_root = "sdmc:/switch/bo1-nx/game_root";
#endif
    } else {
      REXLOG_ERROR("bo1: no game_root folder next to the executable ({})", rex::path_to_utf8(extracted));
    }
  }

  // Bring-up diagnostics: how the game's main thread ended. Its exit status and the last guest registers say whether
  // it returned from its entry point (lr) or called ExTerminateThread, and with which code.
  void OnGuestThreadExit(rex::system::XThread* thread) override {
    if (!thread) {
      return;
    }
    auto* kthread = thread->guest_object<rex::system::X_KTHREAD>();
    const auto* state = thread->thread_state();
    const auto* ctx = state ? state->context() : nullptr;
    REXLOG_WARN("bo1: guest main thread '{}' (id {}) ended: exit status {:#x}, r3 {:#x}, lr {:#010x}",
                thread->name(), thread->thread_id(), kthread ? uint32_t(kthread->exit_status) : 0u,
                ctx ? ctx->r3.u32 : 0u, ctx ? uint32_t(ctx->lr) : 0u);
  }

  // Debug: hashes game files as this machine reads them, per MiB (FNV-1a 64), to compare with the disc image
  // (bo1_debug_hash_files = "common_zombie.ff,zombie_theater.ff"; tools/hash_game_files.py makes the reference).
  // Used to tell bad bytes from the SD card apart from a crash in the code.
  void OnPostSetup() override {
    std::string list = REXCVAR_GET(bo1_debug_hash_files);
    if (list.empty() || !runtime()) return;
    const std::filesystem::path root = runtime()->game_data_root();
    size_t start = 0;
    while (start <= list.size()) {
      const size_t comma = list.find(',', start);
      const std::string name = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
      start = comma == std::string::npos ? list.size() + 1 : comma + 1;
      if (name.empty()) continue;
      const std::filesystem::path path = root / name;
      std::FILE* f = std::fopen(rex::path_to_utf8(path).c_str(), "rb");
      if (!f) {
        REXLOG_WARN("bo1: hash {}: cannot open", name);
        continue;
      }
      std::vector<uint8_t> chunk(1 << 20);
      uint64_t whole = 1469598103934665603ull;
      for (size_t index = 0;; ++index) {
        const size_t n = std::fread(chunk.data(), 1, chunk.size(), f);
        if (n == 0) break;
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < n; ++i) {
          h = (h ^ chunk[i]) * 1099511628211ull;
          whole = (whole ^ chunk[i]) * 1099511628211ull;
        }
        REXLOG_INFO("bo1: hash {} MiB {} {:016x}", name, index, h);
      }
      std::fclose(f);
      REXLOG_INFO("bo1: hash {} whole {:016x}", name, whole);
    }
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // bo1_renderer = "native": the native renderer (src/native) replaces the SDK's emulated GPU entirely.
    if (bo1::native::Enabled()) {
      config.graphics = bo1::native::CreateGraphicsSystem();
    }
#if REX_PLATFORM_SWITCH
    // Official Nintendo 460.8 MHz handheld GPU profile (not an overclock; docked keeps the system's 768 MHz).
    RexSwitchApmRequestGpuMhz(460);
    RexSwitchApmApply();
#endif
  }
};
