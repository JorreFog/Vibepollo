/**
 * @file src/platform/linux/frame_limiter_gamescope.h
 * @brief gamescope's runtime frame cap, driven through its X root property.
 *
 * gamescope watches GAMESCOPE_FPS_LIMIT on the root window of the X display it
 * manages and applies whatever is written there, which makes it the only Linux
 * provider that can be re-limited without relaunching the game.
 *
 * Split out from the rest of the limiter so it can be exercised against a bare
 * X server in tests, with no configuration or logging pulled in.
 */
#pragma once

#ifdef __linux__

  // standard includes
  #include <optional>

namespace platf::gamescope {

  /// Name of the property gamescope polls.
  inline constexpr auto fps_limit_property = "GAMESCOPE_FPS_LIMIT";

  /**
   * @brief Whether the current X display looks like a gamescope session.
   *
   * The property is interned by gamescope itself, so an ordinary Xorg or
   * Xwayland display will not have it.
   */
  [[nodiscard]] bool present();

  /**
   * @brief Write the frame cap. Zero removes the cap.
   * @return false when there is no X display, or it is not gamescope's.
   */
  bool set_fps_limit(int fps);

  /**
   * @brief Read the cap back.
   * @return Empty when the property is absent or unreadable.
   */
  [[nodiscard]] std::optional<int> get_fps_limit();

}  // namespace platf::gamescope

#endif  // __linux__
