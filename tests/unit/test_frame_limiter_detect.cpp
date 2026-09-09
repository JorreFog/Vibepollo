/**
 * @file tests/unit/test_frame_limiter_detect.cpp
 */
#include <gtest/gtest.h>

#include <src/platform/linux/frame_limiter_detect.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

  namespace detect = platf::frame_limiter_detect;

  /// Scratch tree standing in for the system library directories.
  class FrameLimiterDetect: public ::testing::Test {
  protected:
    void SetUp() override {
      root_ = std::filesystem::temp_directory_path() /
              ("frame_limiter_detect_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
               "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
      std::filesystem::remove_all(root_);
      std::filesystem::create_directories(lib_dir());
      std::filesystem::create_directories(layer_dir());
    }

    void TearDown() override {
      std::error_code error;
      std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] std::filesystem::path lib_dir() const {
      return root_ / "lib";
    }

    [[nodiscard]] std::filesystem::path layer_dir() const {
      return root_ / "vulkan";
    }

    static void touch(const std::filesystem::path &path) {
      std::filesystem::create_directories(path.parent_path());
      std::ofstream stream(path);
      stream << "placeholder";
    }

    [[nodiscard]] frame_limiter::availability_t probe(bool gamescope = false) const {
      return detect::probe({lib_dir()}, {layer_dir()}, gamescope);
    }

    std::filesystem::path root_;
  };

  TEST_F(FrameLimiterDetect, EmptySystemOffersNothing) {
    const auto available = probe();
    EXPECT_FALSE(available.mangohud);
    EXPECT_FALSE(available.libstrangle);
    EXPECT_FALSE(available.gamescope);
    EXPECT_TRUE(available.mangohud_library.empty());
    EXPECT_TRUE(available.libstrangle_library.empty());
  }

  TEST_F(FrameLimiterDetect, FindsMangoHudInASubdirectory) {
    // The usual Arch layout.
    touch(lib_dir() / "mangohud" / "libMangoHud.so");
    const auto available = probe();
    EXPECT_TRUE(available.mangohud);
    EXPECT_EQ(available.mangohud_library, (lib_dir() / "mangohud" / "libMangoHud.so").string());
  }

  TEST_F(FrameLimiterDetect, FindsMangoHudDirectlyInTheLibraryDirectory) {
    touch(lib_dir() / "libMangoHud.so");
    const auto available = probe();
    EXPECT_TRUE(available.mangohud);
    EXPECT_EQ(available.mangohud_library, (lib_dir() / "libMangoHud.so").string());
  }

  TEST_F(FrameLimiterDetect, PrefersTheShimOverTheLegacyLibrary) {
    // MangoHud >= 0.8 ships both. Only the shim hooks OpenGL; preloading
    // libMangoHud.so there attaches nothing and silently applies no limit.
    touch(lib_dir() / "mangohud" / "libMangoHud.so");
    touch(lib_dir() / "mangohud" / "libMangoHud_shim.so");
    const auto available = probe();
    EXPECT_TRUE(available.mangohud);
    EXPECT_EQ(available.mangohud_library, (lib_dir() / "mangohud" / "libMangoHud_shim.so").string());
  }

  TEST_F(FrameLimiterDetect, FallsBackToTheLegacyLibraryWhenNoShimExists) {
    // Pre-0.8 installs only ship libMangoHud.so, which does hook OpenGL.
    touch(lib_dir() / "mangohud" / "libMangoHud.so");
    const auto available = probe();
    EXPECT_TRUE(available.mangohud);
    EXPECT_EQ(available.mangohud_library, (lib_dir() / "mangohud" / "libMangoHud.so").string());
  }

  TEST_F(FrameLimiterDetect, VulkanLayerAloneMakesMangoHudUsable) {
    // Vulkan titles only need MANGOHUD=1, so no library is required.
    touch(layer_dir() / "MangoHud.x86_64.json");
    const auto available = probe();
    EXPECT_TRUE(available.mangohud);
    EXPECT_TRUE(available.mangohud_library.empty());
  }

  TEST_F(FrameLimiterDetect, IgnoresUnrelatedVulkanLayers) {
    touch(layer_dir() / "VkLayer_something_else.json");
    EXPECT_FALSE(probe().mangohud);
  }

  TEST_F(FrameLimiterDetect, IgnoresAMangoHudManifestThatIsNotJson) {
    touch(layer_dir() / "MangoHud.notes.txt");
    EXPECT_FALSE(probe().mangohud);
  }

  TEST_F(FrameLimiterDetect, FindsLibstrangle) {
    touch(lib_dir() / "libstrangle.so");
    const auto available = probe();
    EXPECT_TRUE(available.libstrangle);
    EXPECT_EQ(available.libstrangle_library, (lib_dir() / "libstrangle.so").string());
  }

  TEST_F(FrameLimiterDetect, FindsLibstrangleInASubdirectory) {
    touch(lib_dir() / "strangle" / "libstrangle.so");
    EXPECT_TRUE(probe().libstrangle);
  }

  TEST_F(FrameLimiterDetect, DirectoryNamedLikeALibraryIsNotAccepted) {
    std::filesystem::create_directories(lib_dir() / "libstrangle.so");
    EXPECT_FALSE(probe().libstrangle);
  }

  TEST_F(FrameLimiterDetect, MissingDirectoriesAreNotAnError) {
    const auto available = detect::probe({root_ / "absent"}, {root_ / "absent-too"}, false);
    EXPECT_FALSE(available.mangohud);
    EXPECT_FALSE(available.libstrangle);
  }

  TEST_F(FrameLimiterDetect, GamescopeFlagIsPassedStraightThrough) {
    EXPECT_TRUE(probe(true).gamescope);
    EXPECT_FALSE(probe(false).gamescope);
  }

  TEST_F(FrameLimiterDetect, BothProvidersAreReportedTogether) {
    touch(lib_dir() / "mangohud" / "libMangoHud.so");
    touch(lib_dir() / "libstrangle.so");
    const auto available = probe(true);
    EXPECT_TRUE(available.mangohud);
    EXPECT_TRUE(available.libstrangle);
    EXPECT_TRUE(available.gamescope);
  }

  TEST_F(FrameLimiterDetect, DefaultSearchPathsAreNotEmpty) {
    // Guards against the arrays being emptied by accident.
    EXPECT_FALSE(detect::default_library_directories().empty());
    EXPECT_FALSE(detect::default_vulkan_layer_directories().empty());
  }

}  // namespace
