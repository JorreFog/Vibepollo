/**
 * @file src/platform/linux/capture_pacing.h
 * @brief When a pushed capture frame should be handed to the encoder.
 *
 * Poll-based backends (X11, KMS, wlroots) grab the screen at the instant they
 * wake, so sleeping to a fixed grid costs nothing: the frame is fresh. A
 * push-based backend is different. PipeWire delivers frames on the
 * compositor's schedule, and a frame that arrives just after a grid point sits
 * finished in memory until the next one. With source and target at the same
 * rate the phase offset between the two is fixed for the whole session, so
 * that wait is a constant addition to end-to-end latency of anywhere up to a
 * full frame interval.
 *
 * Arrival pacing removes the wait: a frame is forwarded as soon as it lands,
 * and the frame rate is held down by dropping frames that come too early
 * rather than by delaying the ones that are kept.
 */
#pragma once

#ifdef __linux__

  // standard includes
  #include <chrono>
  #include <cstdint>
  #include <string_view>

namespace platf::capture_pacing {

  enum class mode_e : std::uint8_t {
    arrival,  ///< Forward each frame as it arrives, dropping to hold the rate.
    interval,  ///< Wake on a fixed grid and take whatever is newest.
  };

  [[nodiscard]] mode_e parse_mode(std::string_view value);
  [[nodiscard]] std::string_view mode_name(mode_e mode);

  /**
   * @brief Rate cap for arrival-paced capture.
   *
   * Deadlines advance by exactly one interval each time a frame is kept, so
   * the cadence stays anchored instead of drifting with arrival jitter. A
   * frame arriving slightly ahead of its deadline is still accepted: without
   * that tolerance, jitter of a fraction of a millisecond would drop a frame
   * and leave a visible gap of two intervals.
   */
  class arrival_pacer_t {
  public:
    using clock = std::chrono::steady_clock;

    /// @param min_interval Shortest gap between kept frames. Zero never drops.
    explicit arrival_pacer_t(clock::duration min_interval);
    arrival_pacer_t(clock::duration min_interval, clock::duration tolerance);

    /// How early a frame may arrive and still be kept. A quarter interval.
    [[nodiscard]] static clock::duration default_tolerance(clock::duration min_interval);

    /**
     * @brief Decide what to do with a frame that just became available.
     * @return true to encode it, false to discard it and wait for the next.
     */
    [[nodiscard]] bool should_emit(clock::time_point arrival);

    /// Forget the cadence, e.g. after a stream reset.
    void reset();

    [[nodiscard]] clock::duration min_interval() const {
      return min_interval_;
    }

    [[nodiscard]] clock::duration tolerance() const {
      return tolerance_;
    }

  private:
    clock::duration min_interval_;
    clock::duration tolerance_;
    bool started_ = false;
    clock::time_point next_deadline_ {};
  };

}  // namespace platf::capture_pacing

#endif  // __linux__
