/**
 * @file tests/unit/test_frame_limiter_gamescope.cpp
 *
 * Exercised against a bare X server (Xvfb is enough). The test publishes
 * GAMESCOPE_FPS_LIMIT on the root window itself, which is what gamescope does
 * on startup, then checks the property really carries the values written to it.
 */
#include <gtest/gtest.h>

#include <src/platform/linux/frame_limiter_gamescope.h>

#include <cstdlib>

#include <X11/Xatom.h>
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
      // Stand in for gamescope, which publishes the property on its root
      // window. Interning the atom alone is deliberately not enough: atom
      // names are global and permanent for the server's lifetime, so that
      // would also be true on any display where the name was ever mentioned.
      property_ = XInternAtom(display_, gamescope::fps_limit_property, False);
      publish(0);
    }

    void TearDown() override {
      if (display_) {
        // Leave no property behind: these tests run against whatever DISPLAY
        // is set, and a leftover property would make the next run - and any
        // other detection on this machine - believe gamescope is present.
        unpublish();
        XCloseDisplay(display_);
      }
    }

    void publish(const unsigned long value) const {
      XChangeProperty(
        display_,
        DefaultRootWindow(display_),
        property_,
        XA_CARDINAL,
        32,
        PropModeReplace,
        reinterpret_cast<const unsigned char *>(&value),
        1
      );
      XSync(display_, False);
    }

    void unpublish() const {
      XDeleteProperty(display_, DefaultRootWindow(display_), property_);
      XSync(display_, False);
    }

    Atom property_ = None;

    Display *display_ = nullptr;
  };

  TEST_F(FrameLimiterGamescope, DisplayWithThePropertyIsRecognised) {
    EXPECT_TRUE(gamescope::present());
  }

  TEST_F(FrameLimiterGamescope, AnInternedAtomWithoutAPropertyIsNotGamescope) {
    // The regression this guards: X atom names are global to the server and
    // never go away, so any client that once named GAMESCOPE_FPS_LIMIT used to
    // make every later detection report gamescope. On a plain desktop that
    // selected a provider nothing honours - the limit was written, success was
    // logged, and the frame rate was never capped.
    unpublish();
    EXPECT_FALSE(gamescope::present());
    publish(0);
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
