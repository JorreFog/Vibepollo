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
  /**
   * @brief Turn off every enabled output except one, returning what was turned off.
   *
   * Used to make the virtual display the only output for the duration of a
   * stream, so the desktop does not stay mirrored across physical monitors that
   * nobody is looking at. The returned names are exactly what
   * restore_outputs() should be given afterwards - the set is captured before
   * anything changes rather than reconstructed later.
   *
   * @param display Connected Wayland display.
   * @param keep_output_name The output to leave enabled, as KWin names it.
   * @param timeout How long to wait for KWin to acknowledge the configuration.
   * @return The outputs that were disabled; empty if nothing was, which is also
   *         what is returned on failure - there is then nothing to restore.
   */
  [[nodiscard]] std::vector<std::string> disable_other_outputs(
    struct wl_display *display,
    const std::string &keep_output_name,
    std::chrono::milliseconds timeout
  );

  /**
   * @brief Re-enable outputs previously turned off by disable_other_outputs().
   * @return true if KWin accepted the configuration.
   */
  bool restore_outputs(
    struct wl_display *display,
    const std::vector<std::string> &output_names,
    std::chrono::milliseconds timeout
  );

  [[nodiscard]] bool apply_custom_mode(
    wl_display *display,
    const std::string &output_name,
    int width,
    int height,
    int refresh_mhz,
    std::chrono::milliseconds timeout = std::chrono::milliseconds {3000}
  );

}  // namespace kwin::vdisplay
