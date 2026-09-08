/**
 * @file src/frame_limiter_policy.cpp
 * @brief Provider selection and launch environment for the frame limiter.
 */
// standard includes
#include <algorithm>
#include <array>
#include <cctype>

// local includes
#include "src/frame_limiter_policy.h"

namespace frame_limiter {

  namespace {
    constexpr std::uint32_t millihz_per_hertz = 1000;

    /// Lower-case and strip everything that is not a letter or digit.
    std::string squash(std::string_view value) {
      std::string squashed;
      squashed.reserve(value.size());
      for (const char ch : value) {
        const auto uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch)) {
          squashed.push_back(static_cast<char>(std::tolower(uch)));
        }
      }
      return squashed;
    }

    std::uint32_t integer_fps_to_millihz(int fps) {
      if (fps <= 0) {
        return 0;
      }
      constexpr int max_fps = static_cast<int>(UINT32_MAX / millihz_per_hertz);
      return static_cast<std::uint32_t>(std::min(fps, max_fps)) * millihz_per_hertz;
    }

    /// Append an entry to a colon-separated LD_PRELOAD, skipping duplicates.
    std::string append_preload(std::string_view inherited, std::string_view library) {
      if (library.empty()) {
        return std::string {inherited};
      }
      // libstrangle documents being loaded last, and MangoHud's own launcher
      // appends too, so extend rather than prepend.
      for (std::size_t start = 0; start <= inherited.size();) {
        const auto end = std::min(inherited.find(':', start), inherited.size());
        if (inherited.substr(start, end - start) == library) {
          return std::string {inherited};
        }
        start = end + 1;
      }
      if (inherited.empty()) {
        return std::string {library};
      }
      return std::string {inherited} + ":" + std::string {library};
    }
  }  // namespace

  provider_e parse_provider(std::string_view value) {
    const auto squashed = squash(value);
    if (squashed.empty() || squashed == "auto" || squashed == "automatic") {
      return provider_e::automatic;
    }
    if (squashed == "none" || squashed == "off" || squashed == "disabled") {
      return provider_e::none;
    }
    if (squashed == "mangohud") {
      return provider_e::mangohud;
    }
    if (squashed == "libstrangle" || squashed == "strangle") {
      return provider_e::libstrangle;
    }
    if (squashed == "gamescope") {
      return provider_e::gamescope;
    }
    // Unknown, including the Windows-only names, behaves as automatic.
    return provider_e::automatic;
  }

  std::string_view provider_name(provider_e provider) {
    switch (provider) {
      case provider_e::none:
        return "none";
      case provider_e::automatic:
        return "auto";
      case provider_e::mangohud:
        return "mangohud";
      case provider_e::libstrangle:
        return "libstrangle";
      case provider_e::gamescope:
        return "gamescope";
    }
    return "none";
  }

  provider_e select_provider(provider_e configured, const availability_t &available) {
    const auto is_available = [&available](provider_e provider) {
      switch (provider) {
        case provider_e::mangohud:
          return available.mangohud;
        case provider_e::libstrangle:
          return available.libstrangle;
        case provider_e::gamescope:
          return available.gamescope;
        default:
          return false;
      }
    };

    if (configured == provider_e::none) {
      return provider_e::none;
    }
    if (configured != provider_e::automatic) {
      return is_available(configured) ? configured : provider_e::none;
    }
    // gamescope first: it is the only one that can be re-limited mid-stream,
    // and if the session is already running under it the limit is applied
    // without touching how the game is launched.
    for (const auto candidate : {provider_e::gamescope, provider_e::mangohud, provider_e::libstrangle}) {
      if (is_available(candidate)) {
        return candidate;
      }
    }
    return provider_e::none;
  }

  std::uint32_t effective_limit_millihz(
    const framegen::stream_start_policy_t &policy,
    const std::uint32_t config_override_millihz
  ) {
    std::uint32_t millihz = policy.frame_limit_millihz > 0 ?
                              policy.frame_limit_millihz :
                              (policy.fps_scaled > 0 ?
                                 static_cast<std::uint32_t>(policy.fps_scaled) :
                                 framegen::normalize_refresh_millihz(policy.fps));
    if (policy.lossless_rtss_limit && *policy.lossless_rtss_limit > 0) {
      millihz = integer_fps_to_millihz(*policy.lossless_rtss_limit);
    }
    if (config_override_millihz > 0) {
      millihz = config_override_millihz;
    }
    return millihz;
  }

  int plan_t::fps() const {
    return framegen::rounded_fps_from_millihz(limit_millihz);
  }

  std::string format_rate(const std::uint32_t millihz) {
    const auto whole = millihz / millihz_per_hertz;
    const auto fraction = millihz % millihz_per_hertz;
    auto rendered = std::to_string(whole);
    if (fraction == 0) {
      return rendered;
    }
    auto digits = std::to_string(fraction);
    digits.insert(0, 3 - digits.size(), '0');
    while (!digits.empty() && digits.back() == '0') {
      digits.pop_back();
    }
    return rendered + "." + digits;
  }

  plan_t build_plan(
    const provider_e configured,
    const availability_t &available,
    const framegen::stream_start_policy_t &policy,
    const std::uint32_t config_override_millihz,
    const std::string_view inherited_ld_preload
  ) {
    plan_t plan;
    plan.limit_millihz = effective_limit_millihz(policy, config_override_millihz);
    plan.provider = select_provider(configured, available);

    if (!plan.active()) {
      // Either nothing to limit to, or nothing able to do the limiting.
      plan.provider = provider_e::none;
      plan.limit_millihz = 0;
      return plan;
    }

    switch (plan.provider) {
      case provider_e::mangohud: {
        plan.env.push_back({"MANGOHUD", "1"});
        // no_display keeps it a limiter: an overlay would be captured into the
        // stream. MangoHud accepts fractional limits, so 59.94 survives.
        plan.env.push_back({"MANGOHUD_CONFIG", "fps_limit=" + format_rate(plan.limit_millihz) + ",no_display"});
        if (!available.mangohud_library.empty()) {
          // Only needed for OpenGL; Vulkan is covered by MANGOHUD=1 alone.
          plan.env.push_back({"LD_PRELOAD", append_preload(inherited_ld_preload, available.mangohud_library)});
        }
        break;
      }
      case provider_e::libstrangle: {
        // libstrangle reads a whole number of frames per second.
        plan.env.push_back({"FPS", std::to_string(plan.fps())});
        plan.env.push_back({"LD_PRELOAD", append_preload(inherited_ld_preload, available.libstrangle_library.empty() ? "libstrangle.so" : available.libstrangle_library)});
        break;
      }
      case provider_e::gamescope:
        // Applied at runtime through the GAMESCOPE_FPS_LIMIT property; the
        // launch environment is left alone.
        break;
      case provider_e::none:
      case provider_e::automatic:
        break;
    }
    return plan;
  }

}  // namespace frame_limiter
