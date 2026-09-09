/**
 * @file src/platform/linux/capture_pacing.cpp
 * @brief When a pushed capture frame should be handed to the encoder.
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <string>

// local includes
#include "src/platform/linux/capture_pacing.h"

namespace platf::capture_pacing {

  namespace {
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
  }  // namespace

  mode_e parse_mode(std::string_view value) {
    const auto squashed = squash(value);
    if (squashed == "interval" || squashed == "grid" || squashed == "legacy") {
      return mode_e::interval;
    }
    // Anything else, including empty, gets the lower-latency default.
    return mode_e::arrival;
  }

  std::string_view mode_name(const mode_e mode) {
    switch (mode) {
      case mode_e::interval:
        return "interval";
      case mode_e::arrival:
        return "arrival";
    }
    return "arrival";
  }

  arrival_pacer_t::clock::duration arrival_pacer_t::default_tolerance(const clock::duration min_interval) {
    return min_interval > clock::duration::zero() ? min_interval / 4 : clock::duration::zero();
  }

  arrival_pacer_t::arrival_pacer_t(const clock::duration min_interval):
      arrival_pacer_t(min_interval, default_tolerance(min_interval)) {}

  arrival_pacer_t::arrival_pacer_t(const clock::duration min_interval, const clock::duration tolerance):
      min_interval_ {std::max(min_interval, clock::duration::zero())},
      tolerance_ {std::clamp(tolerance, clock::duration::zero(), std::max(min_interval, clock::duration::zero()))} {}

  bool arrival_pacer_t::should_emit(const clock::time_point arrival) {
    if (min_interval_ == clock::duration::zero()) {
      return true;  // Uncapped: the source rate is the target rate.
    }

    if (!started_) {
      started_ = true;
      next_deadline_ = arrival + min_interval_;
      return true;
    }

    if (arrival + tolerance_ < next_deadline_) {
      return false;  // Too soon; keeping it would exceed the requested rate.
    }

    next_deadline_ += min_interval_;
    if (next_deadline_ <= arrival) {
      // The source fell behind far enough that the cadence is meaningless;
      // start counting again from this frame rather than firing a burst.
      next_deadline_ = arrival + min_interval_;
    }
    return true;
  }

  void arrival_pacer_t::reset() {
    started_ = false;
    next_deadline_ = {};
  }

}  // namespace platf::capture_pacing
