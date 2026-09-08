/**
 * @file src/platform/linux/frame_limiter_detect.cpp
 * @brief Filesystem probing for the installed frame limiter providers.
 */
// standard includes
#include <array>
#include <system_error>

// local includes
#include "src/platform/linux/frame_limiter_detect.h"

namespace platf::frame_limiter_detect {

  namespace {
    /// Distributions disagree on where these end up; MangoHud additionally
    /// keeps its library in a subdirectory of its own.
    constexpr std::array library_roots {
      "/usr/lib",
      "/usr/lib64",
      "/usr/lib/x86_64-linux-gnu",
      "/usr/local/lib",
      "/usr/local/lib64",
    };

    bool regular_file_exists(const std::filesystem::path &path) {
      std::error_code error;
      return std::filesystem::is_regular_file(path, error);
    }

    /// First existing `<directory>/<name>` or `<directory>/<subdir>/<name>`.
    std::string find_library(
      const std::vector<std::filesystem::path> &directories,
      const std::string &name,
      const std::string &subdirectory
    ) {
      for (const auto &directory : directories) {
        if (!subdirectory.empty()) {
          const auto nested = directory / subdirectory / name;
          if (regular_file_exists(nested)) {
            return nested.string();
          }
        }
        const auto direct = directory / name;
        if (regular_file_exists(direct)) {
          return direct.string();
        }
      }
      return {};
    }

    bool has_mangohud_vulkan_layer(const std::vector<std::filesystem::path> &directories) {
      // The architecture suffix varies, so match on the stem instead.
      for (const auto &directory : directories) {
        std::error_code error;
        std::filesystem::directory_iterator entries {directory, error};
        if (error) {
          continue;
        }
        for (const auto &entry : entries) {
          const auto filename = entry.path().filename().string();
          if (filename.starts_with("MangoHud") && entry.path().extension() == ".json") {
            return true;
          }
        }
      }
      return false;
    }
  }  // namespace

  std::vector<std::filesystem::path> default_library_directories() {
    std::vector<std::filesystem::path> directories;
    directories.reserve(library_roots.size());
    for (const auto *root : library_roots) {
      directories.emplace_back(root);
    }
    return directories;
  }

  std::vector<std::filesystem::path> default_vulkan_layer_directories() {
    return {
      "/usr/share/vulkan/implicit_layer.d",
      "/usr/local/share/vulkan/implicit_layer.d",
      "/etc/vulkan/implicit_layer.d",
    };
  }

  frame_limiter::availability_t probe(
    const std::vector<std::filesystem::path> &library_directories,
    const std::vector<std::filesystem::path> &vulkan_layer_directories,
    const bool gamescope_session
  ) {
    frame_limiter::availability_t available;

    available.mangohud_library = find_library(library_directories, "libMangoHud.so", "mangohud");
    available.libstrangle_library = find_library(library_directories, "libstrangle.so", "strangle");

    // A Vulkan-only install still limits Vulkan games through MANGOHUD=1.
    available.mangohud = !available.mangohud_library.empty() ||
                         has_mangohud_vulkan_layer(vulkan_layer_directories);
    available.libstrangle = !available.libstrangle_library.empty();
    available.gamescope = gamescope_session;

    return available;
  }

}  // namespace platf::frame_limiter_detect
