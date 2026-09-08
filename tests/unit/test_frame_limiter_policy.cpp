/**
 * @file tests/unit/test_frame_limiter_policy.cpp
 */
#include <gtest/gtest.h>

#include <src/frame_limiter_policy.h>

#include <algorithm>
#include <string>

namespace {

  using frame_limiter::availability_t;
  using frame_limiter::provider_e;

  availability_t all_available() {
    availability_t available;
    available.mangohud = true;
    available.libstrangle = true;
    available.gamescope = true;
    available.mangohud_library = "/usr/lib/mangohud/libMangoHud.so";
    available.libstrangle_library = "/usr/lib/libstrangle.so";
    return available;
  }

  framegen::stream_start_policy_t policy_with_fps(int fps) {
    framegen::stream_start_policy_t policy;
    policy.fps = fps;
    return policy;
  }

  std::string env_value(const frame_limiter::plan_t &plan, std::string_view name) {
    const auto it = std::find_if(plan.env.begin(), plan.env.end(), [name](const auto &entry) {
      return entry.name == name;
    });
    return it == plan.env.end() ? std::string {} : it->value;
  }

  bool has_env(const frame_limiter::plan_t &plan, std::string_view name) {
    return std::any_of(plan.env.begin(), plan.env.end(), [name](const auto &entry) {
      return entry.name == name;
    });
  }

  // --- parse_provider -----------------------------------------------------

  TEST(FrameLimiterProvider, ParsesKnownNames) {
    EXPECT_EQ(frame_limiter::parse_provider("mangohud"), provider_e::mangohud);
    EXPECT_EQ(frame_limiter::parse_provider("libstrangle"), provider_e::libstrangle);
    EXPECT_EQ(frame_limiter::parse_provider("strangle"), provider_e::libstrangle);
    EXPECT_EQ(frame_limiter::parse_provider("gamescope"), provider_e::gamescope);
    EXPECT_EQ(frame_limiter::parse_provider("none"), provider_e::none);
    EXPECT_EQ(frame_limiter::parse_provider("off"), provider_e::none);
    EXPECT_EQ(frame_limiter::parse_provider("disabled"), provider_e::none);
  }

  TEST(FrameLimiterProvider, IgnoresCaseAndPunctuation) {
    EXPECT_EQ(frame_limiter::parse_provider("MangoHud"), provider_e::mangohud);
    EXPECT_EQ(frame_limiter::parse_provider("Mango-Hud"), provider_e::mangohud);
    EXPECT_EQ(frame_limiter::parse_provider("  GameScope "), provider_e::gamescope);
    EXPECT_EQ(frame_limiter::parse_provider("lib_strangle"), provider_e::libstrangle);
  }

  TEST(FrameLimiterProvider, EmptyAndUnknownFallBackToAutomatic) {
    EXPECT_EQ(frame_limiter::parse_provider(""), provider_e::automatic);
    EXPECT_EQ(frame_limiter::parse_provider("auto"), provider_e::automatic);
    EXPECT_EQ(frame_limiter::parse_provider("something-else"), provider_e::automatic);
    // The Windows-only providers must not be mistaken for a Linux one.
    EXPECT_EQ(frame_limiter::parse_provider("rtss"), provider_e::automatic);
    EXPECT_EQ(frame_limiter::parse_provider("nvidia-control-panel"), provider_e::automatic);
  }

  TEST(FrameLimiterProvider, NamesRoundTrip) {
    for (const auto provider : {provider_e::none, provider_e::mangohud, provider_e::libstrangle, provider_e::gamescope}) {
      EXPECT_EQ(frame_limiter::parse_provider(frame_limiter::provider_name(provider)), provider);
    }
  }

  // --- select_provider ----------------------------------------------------

  TEST(FrameLimiterSelect, NoneStaysNoneEvenWhenEverythingIsInstalled) {
    EXPECT_EQ(frame_limiter::select_provider(provider_e::none, all_available()), provider_e::none);
  }

  TEST(FrameLimiterSelect, ExplicitProviderIsHonouredWhenAvailable) {
    EXPECT_EQ(frame_limiter::select_provider(provider_e::mangohud, all_available()), provider_e::mangohud);
    EXPECT_EQ(frame_limiter::select_provider(provider_e::libstrangle, all_available()), provider_e::libstrangle);
    EXPECT_EQ(frame_limiter::select_provider(provider_e::gamescope, all_available()), provider_e::gamescope);
  }

  TEST(FrameLimiterSelect, ExplicitProviderFallsToNoneWhenMissing) {
    availability_t none_installed;
    EXPECT_EQ(frame_limiter::select_provider(provider_e::mangohud, none_installed), provider_e::none);
    EXPECT_EQ(frame_limiter::select_provider(provider_e::libstrangle, none_installed), provider_e::none);
    EXPECT_EQ(frame_limiter::select_provider(provider_e::gamescope, none_installed), provider_e::none);
  }

  TEST(FrameLimiterSelect, ExplicitProviderDoesNotSilentlySubstituteAnother) {
    availability_t only_mangohud;
    only_mangohud.mangohud = true;
    EXPECT_EQ(frame_limiter::select_provider(provider_e::gamescope, only_mangohud), provider_e::none);
  }

  TEST(FrameLimiterSelect, AutomaticPrefersGamescopeThenMangoHudThenStrangle) {
    EXPECT_EQ(frame_limiter::select_provider(provider_e::automatic, all_available()), provider_e::gamescope);

    availability_t without_gamescope = all_available();
    without_gamescope.gamescope = false;
    EXPECT_EQ(frame_limiter::select_provider(provider_e::automatic, without_gamescope), provider_e::mangohud);

    availability_t only_strangle;
    only_strangle.libstrangle = true;
    EXPECT_EQ(frame_limiter::select_provider(provider_e::automatic, only_strangle), provider_e::libstrangle);
  }

  TEST(FrameLimiterSelect, AutomaticWithNothingInstalledIsNone) {
    EXPECT_EQ(frame_limiter::select_provider(provider_e::automatic, availability_t {}), provider_e::none);
  }

  // --- effective_limit_millihz --------------------------------------------

  TEST(FrameLimiterLimit, PlainFpsIsConvertedToMillihertz) {
    EXPECT_EQ(frame_limiter::effective_limit_millihz(policy_with_fps(60), 0), 60000u);
    EXPECT_EQ(frame_limiter::effective_limit_millihz(policy_with_fps(120), 0), 120000u);
  }

  TEST(FrameLimiterLimit, DisplayModeRateWinsOverStreamCadenceAndFps) {
    auto policy = policy_with_fps(60);
    policy.fps_scaled = 90000;
    policy.frame_limit_millihz = 59940;
    EXPECT_EQ(frame_limiter::effective_limit_millihz(policy, 0), 59940u);
  }

  TEST(FrameLimiterLimit, StreamCadenceWinsOverPlainFps) {
    auto policy = policy_with_fps(60);
    policy.fps_scaled = 59940;
    EXPECT_EQ(frame_limiter::effective_limit_millihz(policy, 0), 59940u);
  }

  TEST(FrameLimiterLimit, LosslessLimitOverridesTheStreamRate) {
    auto policy = policy_with_fps(120);
    policy.frame_limit_millihz = 120000;
    policy.lossless_rtss_limit = 40;
    EXPECT_EQ(frame_limiter::effective_limit_millihz(policy, 0), 40000u);
  }

  TEST(FrameLimiterLimit, ConfigOverrideBeatsEverything) {
    auto policy = policy_with_fps(120);
    policy.frame_limit_millihz = 120000;
    policy.lossless_rtss_limit = 40;
    EXPECT_EQ(frame_limiter::effective_limit_millihz(policy, 75000), 75000u);
  }

  TEST(FrameLimiterLimit, NothingRequestedIsUncapped) {
    EXPECT_EQ(frame_limiter::effective_limit_millihz(framegen::stream_start_policy_t {}, 0), 0u);
    EXPECT_EQ(frame_limiter::effective_limit_millihz(policy_with_fps(0), 0), 0u);
  }

  TEST(FrameLimiterLimit, NonPositiveLosslessLimitIsIgnored) {
    auto policy = policy_with_fps(60);
    policy.lossless_rtss_limit = 0;
    EXPECT_EQ(frame_limiter::effective_limit_millihz(policy, 0), 60000u);
  }

  // --- format_rate --------------------------------------------------------

  TEST(FrameLimiterFormat, WholeRatesHaveNoDecimalPoint) {
    EXPECT_EQ(frame_limiter::format_rate(60000), "60");
    EXPECT_EQ(frame_limiter::format_rate(144000), "144");
    EXPECT_EQ(frame_limiter::format_rate(0), "0");
  }

  TEST(FrameLimiterFormat, FractionalRatesKeepSignificantDigitsOnly) {
    EXPECT_EQ(frame_limiter::format_rate(59940), "59.94");
    EXPECT_EQ(frame_limiter::format_rate(23976), "23.976");
    EXPECT_EQ(frame_limiter::format_rate(29970), "29.97");
    EXPECT_EQ(frame_limiter::format_rate(100), "0.1");
    EXPECT_EQ(frame_limiter::format_rate(60500), "60.5");
  }

  TEST(FrameLimiterFormat, RoundedFpsMatchesTheRate) {
    frame_limiter::plan_t plan;
    plan.limit_millihz = 59940;
    EXPECT_EQ(plan.fps(), 60);
    plan.limit_millihz = 23976;
    EXPECT_EQ(plan.fps(), 24);
    plan.limit_millihz = 120000;
    EXPECT_EQ(plan.fps(), 120);
  }

  // --- build_plan ---------------------------------------------------------

  TEST(FrameLimiterPlan, MangoHudGetsLimiterOnlyConfiguration) {
    availability_t available;
    available.mangohud = true;
    available.mangohud_library = "/usr/lib/mangohud/libMangoHud.so";

    const auto plan = frame_limiter::build_plan(
      provider_e::mangohud, available, policy_with_fps(120), 0, ""
    );

    ASSERT_TRUE(plan.active());
    EXPECT_EQ(plan.provider, provider_e::mangohud);
    EXPECT_EQ(env_value(plan, "MANGOHUD"), "1");
    EXPECT_EQ(env_value(plan, "MANGOHUD_CONFIG"), "fps_limit=120,no_display");
    EXPECT_EQ(env_value(plan, "LD_PRELOAD"), "/usr/lib/mangohud/libMangoHud.so");
  }

  TEST(FrameLimiterPlan, MangoHudKeepsFractionalRates) {
    availability_t available;
    available.mangohud = true;
    auto policy = policy_with_fps(60);
    policy.frame_limit_millihz = 59940;

    const auto plan = frame_limiter::build_plan(provider_e::mangohud, available, policy, 0, "");
    EXPECT_EQ(env_value(plan, "MANGOHUD_CONFIG"), "fps_limit=59.94,no_display");
  }

  TEST(FrameLimiterPlan, MangoHudWithoutALibraryStillSetsTheVulkanLayer) {
    availability_t available;
    available.mangohud = true;  // library path unknown

    const auto plan = frame_limiter::build_plan(provider_e::mangohud, available, policy_with_fps(60), 0, "");
    EXPECT_EQ(env_value(plan, "MANGOHUD"), "1");
    EXPECT_FALSE(has_env(plan, "LD_PRELOAD"));
  }

  TEST(FrameLimiterPlan, ExistingPreloadIsPreserved) {
    availability_t available;
    available.mangohud = true;
    available.mangohud_library = "libMangoHud.so";

    const auto plan = frame_limiter::build_plan(
      provider_e::mangohud, available, policy_with_fps(60), 0, "/opt/other.so:/opt/second.so"
    );
    EXPECT_EQ(env_value(plan, "LD_PRELOAD"), "/opt/other.so:/opt/second.so:libMangoHud.so");
  }

  TEST(FrameLimiterPlan, PreloadIsNotDuplicated) {
    availability_t available;
    available.mangohud = true;
    available.mangohud_library = "libMangoHud.so";

    const auto plan = frame_limiter::build_plan(
      provider_e::mangohud, available, policy_with_fps(60), 0, "libMangoHud.so:/opt/other.so"
    );
    EXPECT_EQ(env_value(plan, "LD_PRELOAD"), "libMangoHud.so:/opt/other.so");
  }

  TEST(FrameLimiterPlan, StrangleUsesWholeFramesAndAppendsItsLibrary) {
    availability_t available;
    available.libstrangle = true;
    available.libstrangle_library = "/usr/lib/libstrangle.so";
    auto policy = policy_with_fps(60);
    policy.frame_limit_millihz = 59940;

    const auto plan = frame_limiter::build_plan(provider_e::libstrangle, available, policy, 0, "/opt/a.so");

    ASSERT_TRUE(plan.active());
    EXPECT_EQ(env_value(plan, "FPS"), "60");
    EXPECT_EQ(env_value(plan, "LD_PRELOAD"), "/opt/a.so:/usr/lib/libstrangle.so");
  }

  TEST(FrameLimiterPlan, StrangleFallsBackToTheSonameWhenNoPathIsKnown) {
    availability_t available;
    available.libstrangle = true;

    const auto plan = frame_limiter::build_plan(provider_e::libstrangle, available, policy_with_fps(60), 0, "");
    EXPECT_EQ(env_value(plan, "LD_PRELOAD"), "libstrangle.so");
  }

  TEST(FrameLimiterPlan, GamescopeTouchesNothingAtLaunch) {
    availability_t available;
    available.gamescope = true;

    const auto plan = frame_limiter::build_plan(provider_e::gamescope, available, policy_with_fps(120), 0, "");

    ASSERT_TRUE(plan.active());
    EXPECT_EQ(plan.provider, provider_e::gamescope);
    EXPECT_EQ(plan.limit_millihz, 120000u);
    EXPECT_TRUE(plan.env.empty());
  }

  TEST(FrameLimiterPlan, NoLimitRequestedProducesAnInactivePlan) {
    const auto plan = frame_limiter::build_plan(
      provider_e::mangohud, all_available(), framegen::stream_start_policy_t {}, 0, ""
    );
    EXPECT_FALSE(plan.active());
    EXPECT_EQ(plan.provider, provider_e::none);
    EXPECT_TRUE(plan.env.empty());
  }

  TEST(FrameLimiterPlan, MissingProviderProducesAnInactivePlanAndNoEnvironment) {
    const auto plan = frame_limiter::build_plan(
      provider_e::mangohud, availability_t {}, policy_with_fps(60), 0, "/opt/a.so"
    );
    EXPECT_FALSE(plan.active());
    EXPECT_TRUE(plan.env.empty());
  }

  TEST(FrameLimiterPlan, ConfigOverrideReachesTheProviderEnvironment) {
    availability_t available;
    available.mangohud = true;

    const auto plan = frame_limiter::build_plan(
      provider_e::mangohud, available, policy_with_fps(120), 30000, ""
    );
    EXPECT_EQ(env_value(plan, "MANGOHUD_CONFIG"), "fps_limit=30,no_display");
  }

}  // namespace
