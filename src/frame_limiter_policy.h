/**
 * @file src/frame_limiter_policy.h
 * @brief Provider selection and launch environment for the frame limiter.
 *
 * Pure decision logic, deliberately free of syscalls so it can be tested on
 * its own. The platform layer supplies what is actually installed and applies
 * the result.
 *
 * On Windows the limiter is RTSS or the NVIDIA control panel. Linux has no
 * single equivalent, so three interchangeable providers are supported:
 *
 *   - gamescope    already-running session, limit set at runtime via an X property
 *   - MangoHud     LD_PRELOAD/Vulkan layer, limit fixed for the process lifetime
 *   - libstrangle  LD_PRELOAD, limit fixed for the process lifetime
 *
 * Only gamescope can be re-limited mid-stream; the other two are configured
 * through the environment the game is launched with.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "src/framegen_policy.h"

namespace frame_limiter {

  enum class provider_e : std::uint8_t {
    none,  ///< Do not limit.
    automatic,  ///< Pick whatever is installed.
    mangohud,
    libstrangle,
    gamescope,
  };

  /**
   * @brief Parse a configured provider name.
   *
   * Unknown values fall back to automatic, matching how the Windows side
   * treats its own provider setting.
   */
  [[nodiscard]] provider_e parse_provider(std::string_view value);

  [[nodiscard]] std::string_view provider_name(provider_e provider);

  /**
   * @brief What this host can actually do, as probed by the platform layer.
   */
  struct availability_t {
    bool mangohud = false;
    bool libstrangle = false;
    /// True only when the session is already running under gamescope.
    bool gamescope = false;
    /// Absolute path to libMangoHud.so, empty when only the Vulkan layer is usable.
    std::string mangohud_library;
    /// Absolute path to libstrangle.so.
    std::string libstrangle_library;
  };

  /**
   * @brief Resolve the configured provider against what is installed.
   * @return The provider to use, or none when the request cannot be honoured.
   */
  [[nodiscard]] provider_e select_provider(provider_e configured, const availability_t &available);

  /**
   * @brief Frame cap for this stream, in millihertz. Zero leaves it uncapped.
   *
   * Mirrors the precedence the Windows limiter uses so a given stream is
   * capped the same way on either platform: the client's exact display-mode
   * rate wins, then the raw stream cadence, then the plain requested FPS; a
   * lossless-scaling limit overrides those, and an explicit configuration
   * override beats everything.
   */
  [[nodiscard]] std::uint32_t effective_limit_millihz(
    const framegen::stream_start_policy_t &policy,
    std::uint32_t config_override_millihz
  );

  struct env_var_t {
    std::string name;
    std::string value;
  };

  /**
   * @brief Everything the platform layer needs to apply a limit.
   */
  struct plan_t {
    provider_e provider = provider_e::none;
    std::uint32_t limit_millihz = 0;
    /// Variables to merge into the launched application's environment.
    std::vector<env_var_t> env;

    [[nodiscard]] bool active() const {
      return provider != provider_e::none && limit_millihz > 0;
    }

    /// Rounded whole-frame rate, for providers and logs that want an integer.
    [[nodiscard]] int fps() const;
  };

  /**
   * @brief Render a millihertz rate the way a provider expects to read it.
   *
   * Trailing zeros are trimmed, so 60000 becomes "60" and 59940 "59.94".
   */
  [[nodiscard]] std::string format_rate(std::uint32_t millihz);

  /**
   * @brief Build the launch plan for a stream.
   * @param inherited_ld_preload Existing LD_PRELOAD, preserved and extended.
   */
  [[nodiscard]] plan_t build_plan(
    provider_e configured,
    const availability_t &available,
    const framegen::stream_start_policy_t &policy,
    std::uint32_t config_override_millihz,
    std::string_view inherited_ld_preload
  );

}  // namespace frame_limiter
