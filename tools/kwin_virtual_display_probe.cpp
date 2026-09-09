/**
 * @file tools/kwin_virtual_display_probe.cpp
 * @brief Standalone check that KWin can create a virtual display on this system.
 *
 * Creates a virtual output through zkde_screencast_unstable_v1, sets it to the
 * requested mode and holds it open until interrupted, printing the PipeWire node
 * that carries its contents. Useful for confirming that a Plasma install has
 * everything Vibepollo's Linux virtual display needs, without rebuilding
 * Vibepollo itself.
 *
 * Build:
 *   see docs/linux_virtual_display.md
 * Run:
 *   kwin-virtual-display-probe 3840 2160 120
 */
// standard includes
#include <atomic>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

// lib includes
#include <poll.h>
#include <unistd.h>
#include <wayland-client.h>

// generated protocol headers
#include <zkde-screencast-unstable-v1.h>

// local includes
#include "src/platform/linux/kwin_virtual_display.h"

namespace {
  /// stream_virtual_output_with_description was added in version 4.
  constexpr uint32_t screencast_version_for_virtual_output = 4;
  constexpr uint32_t screencast_version_max = 6;

  std::atomic_bool interrupted {false};

  void on_signal(int) {
    interrupted = true;
  }

  struct state_t {
    struct zkde_screencast_unstable_v1 *screencast = nullptr;
    uint32_t screencast_version = 0;
    bool created = false;
    bool failed = false;
    uint32_t node = 0;
    std::string error;
  };

  void on_stream_created(void *data, struct zkde_screencast_stream_unstable_v1 *, uint32_t node) {
    auto *state = static_cast<state_t *>(data);
    state->node = node;
    state->created = true;
  }

  void on_stream_failed(void *data, struct zkde_screencast_stream_unstable_v1 *, const char *error) {
    auto *state = static_cast<state_t *>(data);
    state->error = error ? error : "unknown error";
    state->failed = true;
  }

  void on_stream_closed(void *data, struct zkde_screencast_stream_unstable_v1 *) {
    static_cast<state_t *>(data)->failed = true;
  }

  void on_stream_serial(void *, struct zkde_screencast_stream_unstable_v1 *, uint32_t, uint32_t) {
    // Only needed for re-use-safe PipeWire object lookup, not for this probe.
  }

  constexpr struct zkde_screencast_stream_unstable_v1_listener stream_listener = {
    .closed = on_stream_closed,
    .created = on_stream_created,
    .failed = on_stream_failed,
    .serial = on_stream_serial,
  };

  void on_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    auto *state = static_cast<state_t *>(data);
    if (!std::strcmp(interface, zkde_screencast_unstable_v1_interface.name)) {
      state->screencast_version = std::min(version, screencast_version_max);
      state->screencast = static_cast<struct zkde_screencast_unstable_v1 *>(
        wl_registry_bind(registry, name, &zkde_screencast_unstable_v1_interface, state->screencast_version)
      );
    }
  }

  void on_global_remove(void *, struct wl_registry *, uint32_t) {}

  constexpr struct wl_registry_listener registry_listener = {
    .global = on_global,
    .global_remove = on_global_remove,
  };

  /**
   * KWin only hands zkde_screencast_unstable_v1 to executables listed in a
   * .desktop file that declares the interface. Drop one in the user's
   * applications directory so this probe is allowed to bind it.
   */
  void install_permission_file() {
    if (const char *disabled = std::getenv("KWIN_WAYLAND_NO_PERMISSION_CHECKS"); disabled && !std::strcmp(disabled, "1")) {
      return;
    }
    std::string executable(PATH_MAX, '\0');
    const auto length = readlink("/proc/self/exe", executable.data(), executable.size() - 1);
    if (length <= 0) {
      return;
    }
    executable.resize(static_cast<size_t>(length));

    const char *home = std::getenv("HOME");
    const char *data_home = std::getenv("XDG_DATA_HOME");
    std::filesystem::path directory = data_home && *data_home
                                        ? std::filesystem::path(data_home) / "applications"
                                        : std::filesystem::path(home ? home : ".") / ".local/share/applications";

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const auto file = directory / "kwin-virtual-display-probe.desktop";

    std::ofstream stream(file);
    if (!stream) {
      std::cerr << "warning: could not write " << file << "\n";
      return;
    }
    stream << "[Desktop Entry]\n"
           << "Exec=" << executable << "\n"
           << "X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1\n"
           << "Type=Application\n"
           << "Name=kwin-virtual-display-probe\n"
           << "NoDisplay=true\n";
    stream.close();
    std::cout << "installed permission file " << file << ", waiting for KWin to notice it\n";
    std::this_thread::sleep_for(std::chrono::seconds(3));
  }
}  // namespace

int main(int argc, char **argv) {
  const int width = argc > 1 ? std::atoi(argv[1]) : 1920;
  const int height = argc > 2 ? std::atoi(argv[2]) : 1080;
  // Fractional, because the interesting rates are not whole numbers: a panel
  // calibrated to 120.064 Hz and a stream at 120.000 beat against each other
  // once every few seconds, which is visible.
  const double refresh_hz = argc > 3 ? std::atof(argv[3]) : 60.0;
  const int refresh_mhz = static_cast<int>(refresh_hz * 1000.0 + 0.5);

  if (width <= 0 || height <= 0 || refresh_mhz <= 0) {
    std::cerr << "usage: " << argv[0] << " [width] [height] [refresh_hz]\n";
    return 2;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  const char *display_name = std::getenv("WAYLAND_DISPLAY");
  if (!display_name) {
    std::cerr << "WAYLAND_DISPLAY is not set: this must run inside the Wayland session\n";
    return 1;
  }

  install_permission_file();

  auto *display = wl_display_connect(display_name);
  if (!display) {
    std::cerr << "cannot connect to Wayland display " << display_name << "\n";
    return 1;
  }

  state_t state;
  auto *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, &state);
  wl_display_roundtrip(display);

  if (!state.screencast) {
    std::cerr << "zkde_screencast_unstable_v1 is not available. This session is either not KWin,\n"
                 "or KWin refused the interface. Retry with KWIN_WAYLAND_NO_PERMISSION_CHECKS=1.\n";
    return 1;
  }
  if (state.screencast_version < screencast_version_for_virtual_output) {
    std::cerr << "KWin offers zkde_screencast_unstable_v1 v" << state.screencast_version
              << " but virtual outputs need v" << screencast_version_for_virtual_output << "\n";
    return 1;
  }

  const std::string name = "VibepolloProbe";
  auto *stream = zkde_screencast_unstable_v1_stream_virtual_output_with_description(
    state.screencast,
    name.c_str(),
    "Vibepollo virtual display probe",
    width,
    height,
    wl_fixed_from_double(1.0),
    ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED
  );
  zkde_screencast_stream_unstable_v1_add_listener(stream, &stream_listener, &state);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!state.created && !state.failed && std::chrono::steady_clock::now() < deadline) {
    if (wl_display_dispatch(display) < 0) {
      break;
    }
  }

  if (!state.created) {
    std::cerr << "KWin did not create the virtual output"
              << (state.error.empty() ? "" : ": " + state.error) << "\n";
    return 1;
  }

  const auto output_name = kwin::vdisplay::kwin_output_name(name);
  std::cout << "virtual output " << output_name << " created, PipeWire node " << state.node << "\n";

  if (!kwin::vdisplay::apply_custom_mode(display, output_name, width, height, refresh_mhz)) {
    std::cerr << "could not set " << width << "x" << height << "@" << refresh_hz
              << "Hz; the output stays at its default 60 Hz mode\n";
  }

  std::cout << "holding the display open, press Ctrl+C to remove it\n";
  while (!interrupted && !state.failed) {
    wl_display_flush(display);
    struct pollfd pfd {};
    pfd.fd = wl_display_get_fd(display);
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 200) > 0 && (pfd.revents & POLLIN) && wl_display_dispatch(display) < 0) {
      break;
    }
  }

  zkde_screencast_stream_unstable_v1_close(stream);
  wl_display_roundtrip(display);
  wl_display_disconnect(display);
  std::cout << "virtual output removed\n";
  return 0;
}
