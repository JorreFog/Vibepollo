/**
 * @file tests/unit/test_frame_limiter_gamescope.cpp
 *
 * Exercised against a bare X server (Xvfb is enough). The test interns
 * GAMESCOPE_FPS_LIMIT itself, which is what gamescope does on startup, then
 * checks the property really carries the values written to it.
 */
#include <gtest/gtest.h>

#include <src/platform/linux/frame_limiter_gamescope.h>

#include <cstdlib>

#include <X11/Xlib.h>

namespace {

  namespace gamescope = platf::gamescope;

  class FrameLimiterGamescope: public ::testing::Test {
  protected:
    void SetUp() override {
      if (const char *display_name = std::getenv("DISPLAY"); !display_name || !*display_name) {
        GTEST_SKIP() << "no X display available";
      }
      display_ = XOpenDisplay(nullptr);
      if (!display_) {
        GTEST_SKIP() << "could not open the X display";
      }
      // Stand in for gamescope: interning the atom is what makes a display
      // identifiable as gamescope's.
      XInternAtom(display_, gamescope::fps_limit_property, False);
      XSync(display_, False);
    }

    void TearDown() override {
      if (display_) {
        XCloseDisplay(display_);
      }
    }

    Display *display_ = nullptr;
  };

  TEST_F(FrameLimiterGamescope, DisplayWithThePropertyIsRecognised) {
    EXPECT_TRUE(gamescope::present());
  }

  TEST_F(FrameLimiterGamescope, WrittenLimitReadsBack) {
    ASSERT_TRUE(gamescope::set_fps_limit(120));
    EXPECT_EQ(gamescope::get_fps_limit(), 120);
  }

  TEST_F(FrameLimiterGamescope, LimitCanBeChangedWhileRunning) {
    // The point of the gamescope provider: re-limiting without a relaunch.
    for (const int fps : {60, 144, 30, 240, 90}) {
      ASSERT_TRUE(gamescope::set_fps_limit(fps)) << "failed at " << fps;
      EXPECT_EQ(gamescope::get_fps_limit(), fps);
    }
  }

  TEST_F(FrameLimiterGamescope, ZeroClearsTheLimit) {
    ASSERT_TRUE(gamescope::set_fps_limit(120));
    ASSERT_TRUE(gamescope::set_fps_limit(0));
    EXPECT_EQ(gamescope::get_fps_limit(), 0);
  }

  TEST_F(FrameLimiterGamescope, NegativeRatesAreTreatedAsUnlimited) {
    ASSERT_TRUE(gamescope::set_fps_limit(-5));
    EXPECT_EQ(gamescope::get_fps_limit(), 0);
  }

  TEST_F(FrameLimiterGamescope, PreExistingLimitCanBeReadAndPutBack) {
    // A stream saves whatever the user had set and restores it afterwards.
    ASSERT_TRUE(gamescope::set_fps_limit(75));
    const auto before = gamescope::get_fps_limit();
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(*before, 75);

    ASSERT_TRUE(gamescope::set_fps_limit(144));
    EXPECT_EQ(gamescope::get_fps_limit(), 144);

    ASSERT_TRUE(gamescope::set_fps_limit(*before));
    EXPECT_EQ(gamescope::get_fps_limit(), 75);
  }

  TEST_F(FrameLimiterGamescope, HighRefreshRatesSurviveTheRoundTrip) {
    ASSERT_TRUE(gamescope::set_fps_limit(500));
    EXPECT_EQ(gamescope::get_fps_limit(), 500);
  }

}  // namespace
