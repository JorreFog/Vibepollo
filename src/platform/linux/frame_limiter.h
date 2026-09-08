/**
 * @file src/platform/linux/frame_limiter.h
 * @brief Frame limiter integration for Linux hosts.
 *
 * The Windows limiter drives RTSS over its shared memory interface. Linux has
 * no single equivalent, so two shapes are supported:
 *
 *   - providers configured through the environment a game is launched with
 *     (MangoHud, libstrangle), which is where most of the work happens;
 *   - gamescope, which watches a property on its own X root window and can
 *     therefore be re-limited while a stream is running.
 */
#pragma once

#ifdef __linux__

  // standard includes
  #include <string>
  #include <string_view>
  #include <vector>

  // local includes
  #include "src/frame_limiter_policy.h"

namespace platf {

  /**
   * @brief What this host can limit with.
   *
   * Library lookup is cached; whether gamescope is reachable is re-checked on
   * every call because a session can appear after Vibepollo has started.
   */
  [[nodiscard]] frame_limiter::availability_t frame_limiter_availability();

  /// Drop the cached library lookup, forcing the next probe to run again.
  void frame_limiter_reset_availability_cache();

  /**
   * @brief Environment to merge into a launched game.
   * @param inherited_ld_preload LD_PRELOAD already destined for the child.
   * @return Empty when the limiter is disabled or nothing can apply a limit.
   */
  [[nodiscard]] std::vector<frame_limiter::env_var_t> frame_limiter_launch_env(
    const framegen::stream_start_policy_t &policy,
    std::string_view inherited_ld_preload
  );

  /**
   * @brief Apply a limit that can be changed while the stream runs.
   *
   * Only gamescope supports this; the environment-based providers are already
   * fixed by the time the game is running.
   *
   * @return true when a limit was actually applied.
   */
  bool frame_limiter_apply_runtime(const framegen::stream_start_policy_t &policy);

  /// Lift a runtime limit previously applied by frame_limiter_apply_runtime.
  void frame_limiter_clear_runtime();

  /// Provider chosen for the most recent decision, for logging and the API.
  [[nodiscard]] frame_limiter::provider_e frame_limiter_selected_provider();

}  // namespace platf

#endif  // __linux__
