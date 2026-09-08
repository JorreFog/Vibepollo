/**
 * @file src/platform/linux/frame_limiter.cpp
 * @brief Frame limiter integration for Linux hosts.
 */
// standard includes
#include <atomic>
#include <mutex>
#include <optional>
#include <string>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/linux/frame_limiter.h"
#include "src/platform/linux/frame_limiter_detect.h"
#include "src/platform/linux/frame_limiter_gamescope.h"

using namespace std::literals;

namespace platf {

  namespace {
    std::mutex g_availability_mutex;
    std::optional<frame_limiter::availability_t> g_cached_libraries;
    std::atomic<frame_limiter::provider_e> g_selected_provider {frame_limiter::provider_e::none};
    /// Whether we wrote gamescope's property, and what it held beforehand, so
    /// a limit the user set themselves survives a stream.
    std::atomic<bool> g_runtime_limit_applied {false};
    std::atomic<int> g_runtime_previous_limit {0};

    /// Library lookup is expensive enough to cache; the gamescope check is not.
    frame_limiter::availability_t probe_libraries() {
      std::lock_guard lock {g_availability_mutex};
      if (!g_cached_libraries) {
        g_cached_libraries = frame_limiter_detect::probe(
          frame_limiter_detect::default_library_directories(),
          frame_limiter_detect::default_vulkan_layer_directories(),
          false
        );
      }
      return *g_cached_libraries;
    }
  }  // namespace

  frame_limiter::availability_t frame_limiter_availability() {
    auto available = probe_libraries();
    available.gamescope = gamescope::present();
    return available;
  }

  void frame_limiter_reset_availability_cache() {
    std::lock_guard lock {g_availability_mutex};
    g_cached_libraries.reset();
  }

  frame_limiter::provider_e frame_limiter_selected_provider() {
    return g_selected_provider.load(std::memory_order_acquire);
  }

  std::vector<frame_limiter::env_var_t> frame_limiter_launch_env(
    const framegen::stream_start_policy_t &policy,
    const std::string_view inherited_ld_preload
  ) {
    if (!config::frame_limiter.enable) {
      return {};
    }

    const auto configured = frame_limiter::parse_provider(config::frame_limiter.provider);
    const auto available = frame_limiter_availability();
    const auto plan = frame_limiter::build_plan(
      configured,
      available,
      policy,
      config::frame_limiter.fps_limit_millihz,
      inherited_ld_preload
    );
    g_selected_provider.store(plan.provider, std::memory_order_release);

    if (!plan.active()) {
      BOOST_LOG(info) << "[frame_limiter] no limit applied at launch (configured="sv
                      << frame_limiter::provider_name(configured)
                      << " mangohud="sv << available.mangohud
                      << " libstrangle="sv << available.libstrangle
                      << " gamescope="sv << available.gamescope
                      << " limit="sv << frame_limiter::format_rate(frame_limiter::effective_limit_millihz(policy, config::frame_limiter.fps_limit_millihz))
                      << ")"sv;
      return {};
    }

    if (plan.provider == frame_limiter::provider_e::gamescope) {
      // Nothing to inject; the cap is written to gamescope at stream start.
      return {};
    }

    BOOST_LOG(info) << "[frame_limiter] launching with "sv << frame_limiter::provider_name(plan.provider)
                    << " capped to "sv << frame_limiter::format_rate(plan.limit_millihz) << " FPS"sv;
    return plan.env;
  }

  bool frame_limiter_apply_runtime(const framegen::stream_start_policy_t &policy) {
    if (!config::frame_limiter.enable) {
      return false;
    }

    const auto configured = frame_limiter::parse_provider(config::frame_limiter.provider);
    const auto available = frame_limiter_availability();
    const auto plan = frame_limiter::build_plan(
      configured,
      available,
      policy,
      config::frame_limiter.fps_limit_millihz,
      {}
    );

    if (!plan.active() || plan.provider != frame_limiter::provider_e::gamescope) {
      return false;
    }

    // Remember what was there so the stream does not permanently change it.
    if (!g_runtime_limit_applied.load(std::memory_order_acquire)) {
      g_runtime_previous_limit.store(gamescope::get_fps_limit().value_or(0), std::memory_order_release);
    }

    // gamescope's property is a whole number of frames per second.
    if (!gamescope::set_fps_limit(plan.fps())) {
      BOOST_LOG(warning) << "[frame_limiter] gamescope was detected but its "sv
                         << gamescope::fps_limit_property << " property could not be written"sv;
      return false;
    }

    g_selected_provider.store(frame_limiter::provider_e::gamescope, std::memory_order_release);
    g_runtime_limit_applied.store(true, std::memory_order_release);
    BOOST_LOG(info) << "[frame_limiter] gamescope capped to "sv << plan.fps() << " FPS"sv;
    return true;
  }

  void frame_limiter_clear_runtime() {
    // Only undo a limit this process actually applied.
    if (!g_runtime_limit_applied.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    const auto restored = g_runtime_previous_limit.load(std::memory_order_acquire);
    if (gamescope::set_fps_limit(restored)) {
      BOOST_LOG(info) << "[frame_limiter] gamescope frame cap restored to "sv
                      << (restored > 0 ? std::to_string(restored) : std::string {"unlimited"});
    }
  }

}  // namespace platf
