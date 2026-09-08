/**
 * @file src/platform/linux/frame_limiter_detect.h
 * @brief Filesystem probing for the installed frame limiter providers.
 *
 * Kept apart from the rest of the platform layer so the search can be pointed
 * at a scratch directory in tests instead of the real system paths.
 */
#pragma once

// standard includes
#include <filesystem>
#include <string>
#include <vector>

// local includes
#include "src/frame_limiter_policy.h"

namespace platf::frame_limiter_detect {

  /// Directories normally holding libMangoHud.so / libstrangle.so.
  [[nodiscard]] std::vector<std::filesystem::path> default_library_directories();

  /// Directories holding Vulkan implicit layer manifests.
  [[nodiscard]] std::vector<std::filesystem::path> default_vulkan_layer_directories();

  /**
   * @brief Look for the provider libraries under the given directories.
   *
   * MangoHud counts as usable when either its library or its implicit Vulkan
   * layer manifest is present: the layer alone is enough to limit a Vulkan
   * game, the library is only needed to reach OpenGL ones.
   *
   * @param gamescope_session Whether the session is running under gamescope,
   *                          which this function cannot determine itself.
   */
  [[nodiscard]] frame_limiter::availability_t probe(
    const std::vector<std::filesystem::path> &library_directories,
    const std::vector<std::filesystem::path> &vulkan_layer_directories,
    bool gamescope_session
  );

}  // namespace platf::frame_limiter_detect
