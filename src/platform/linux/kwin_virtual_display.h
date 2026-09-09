/**
 * @file src/platform/linux/kwin_virtual_display.h
 * @brief Runtime virtual display creation for KWin (KDE Plasma) Wayland sessions.
 *
 * KWin can materialise an extra output at runtime that behaves like a real
 * monitor: the desktop extends onto it, windows can be moved to it and it is
 * composited normally. It is requested through zkde_screencast_unstable_v1's
 * `stream_virtual_output_with_description`, which both creates the output and
 * returns a PipeWire node carrying its contents, so capture comes for free.
 *
 * The output KWin creates always runs at 60 Hz. To stream above that the mode
 * has to be replaced with a custom one, which is what kde_output_management_v2
 * (>= v18, `set_custom_modes`) is used for here.
 *
 * This is the Linux counterpart of the IddCx virtual display driver Vibepollo
 * uses on Windows, but needs no kernel driver: KWin already owns the necessary
 * machinery (DrmBackend::createVirtualOutput).
 */
#pragma once

// standard includes
#include <chrono>
#include <string>

struct wl_display;

namespace kwin::vdisplay {

  /**
   * @brief Whether this session can create a virtual output at all.
   *
   * Virtual outputs come from zkde_screencast_unstable_v1's
   * stream_virtual_output_with_description, which exists from version 4. A
   * session without that interface - anything that is not KWin, or a Plasma too
   * old - cannot make one, and clients that ask about it up front should be told
   * so rather than be allowed to start a launch that then fails.
   *
   * The answer cannot change while the compositor is running, so it is probed
   * once on first call and cached.
   */
  bool supported();


  /**
   * @brief Name KWin gives to a virtual output created with the supplied name.
   *
   * KWin prefixes virtual outputs with "Virtual-" (see DrmVirtualOutput), so the
   * name a client passes to stream_virtual_output_with_description is not the
   * name the output is later addressed by.
   */
  [[nodiscard]] std::string kwin_output_name(const std::string &requested_name);

  /**
   * @brief Add a custom mode to a KWin output and switch the output to it.
   *
   * Applied in two steps because a mode can only be selected once it exists:
   * the custom mode list is committed first, then the resulting mode is made
   * current.
   *
   * @param display Connected Wayland display. Not taken ownership of.
   * @param output_name KWin output name, e.g. "Virtual-Vibepollo".
   * @param width Mode width in hardware pixels.
   * @param height Mode height in hardware pixels.
   * @param refresh_mhz Refresh rate in mHz (60000 == 60 Hz).
   * @param timeout How long to wait for the output to show up and for KWin to
   *                acknowledge the configuration.
   * @return true if the output is running at the requested mode.
   */
  [[nodiscard]] bool apply_custom_mode(
    wl_display *display,
    const std::string &output_name,
    int width,
    int height,
    int refresh_mhz,
    std::chrono::milliseconds timeout = std::chrono::milliseconds {3000}
  );

}  // namespace kwin::vdisplay
