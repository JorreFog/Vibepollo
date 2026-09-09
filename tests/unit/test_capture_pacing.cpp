/**
 * @file tests/unit/test_capture_pacing.cpp
 *
 * The arrival pacer decides which pushed frames reach the encoder. It is the
 * core capture loop, so the cases that matter are the rate ratios a compositor
 * actually produces, plus the two things that make a naive implementation
 * misbehave: arrival jitter, and falling behind.
 */
#include <gtest/gtest.h>

#include <src/platform/linux/capture_pacing.h>

#include <chrono>
#include <vector>

namespace {

  using namespace std::chrono_literals;
  namespace pacing = platf::capture_pacing;
  using clock = pacing::arrival_pacer_t::clock;

  /// Feed frames arriving every `source` interval, return which were kept.
  std::vector<bool> run(clock::duration target, clock::duration source, int frames, clock::duration jitter = 0ns) {
    pacing::arrival_pacer_t pacer {target};
    std::vector<bool> kept;
    auto now = clock::time_point {};
    for (int i = 0; i < frames; ++i) {
      now += source + (i % 2 ? jitter : -jitter);
      kept.push_back(pacer.should_emit(now));
    }
    return kept;
  }

  int count_kept(const std::vector<bool> &kept) {
    int n = 0;
    for (const bool k : kept) {
      n += k ? 1 : 0;
    }
    return n;
  }

  TEST(CapturePacing, SourceEqualsTargetKeepsEveryFrame) {
    // The common case: compositor and stream both at 120. Dropping anything here
    // would be a visible stutter for no reason.
    const auto kept = run(8333us, 8333us, 200);
    EXPECT_EQ(count_kept(kept), 200);
  }

  TEST(CapturePacing, SourceTwiceTargetKeepsEveryOther) {
    const auto kept = run(16666us, 8333us, 200);
    EXPECT_NEAR(count_kept(kept), 100, 2);
  }

  TEST(CapturePacing, SourceSlowerThanTargetKeepsEverything) {
    // 60 Hz source, 120 Hz target: there is nothing to drop.
    const auto kept = run(8333us, 16666us, 100);
    EXPECT_EQ(count_kept(kept), 100);
  }

  TEST(CapturePacing, ToleranceAbsorbsJitterInsteadOfDroppingAFrame) {
    // Without tolerance a frame arriving a hair early is dropped, and the next
    // one is a full two intervals later - a far worse artefact than the jitter.
    const auto kept = run(8333us, 8333us, 200, 200us);
    EXPECT_EQ(count_kept(kept), 200);
  }

  TEST(CapturePacing, ReAnchorsAfterFallingBehind) {
    pacing::arrival_pacer_t pacer {10ms};
    auto now = clock::time_point {};
    EXPECT_TRUE(pacer.should_emit(now));

    // A long stall: deadlines must not pile up and then burst through.
    now += 1s;
    EXPECT_TRUE(pacer.should_emit(now));
    now += 1ms;
    EXPECT_FALSE(pacer.should_emit(now)) << "deadline did not re-anchor to the late arrival";
    now += 10ms;
    EXPECT_TRUE(pacer.should_emit(now));
  }

  TEST(CapturePacing, ZeroIntervalNeverDrops) {
    const auto kept = run(0ns, 1ms, 50);
    EXPECT_EQ(count_kept(kept), 50);
  }

  TEST(CapturePacing, ResetForgetsTheCadence) {
    pacing::arrival_pacer_t pacer {10ms};
    auto now = clock::time_point {};
    EXPECT_TRUE(pacer.should_emit(now));
    now += 1ms;
    EXPECT_FALSE(pacer.should_emit(now));
    pacer.reset();
    EXPECT_TRUE(pacer.should_emit(now)) << "reset should accept the next frame immediately";
  }

  TEST(CapturePacing, ModeNamesRoundTrip) {
    EXPECT_EQ(pacing::parse_mode("arrival"), pacing::mode_e::arrival);
    EXPECT_EQ(pacing::parse_mode("interval"), pacing::mode_e::interval);
    EXPECT_EQ(pacing::mode_name(pacing::mode_e::arrival), "arrival");
    EXPECT_EQ(pacing::mode_name(pacing::mode_e::interval), "interval");
  }

}  // namespace
