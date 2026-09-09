/**
 * @file src/platform/linux/kwin_virtual_display.cpp
 * @brief Custom-mode configuration for KWin outputs, used to drive virtual displays.
 *
 * KWin hands out virtual outputs locked to 60 Hz (DrmVirtualOutput builds a
 * single 60000 mHz mode), so anything faster has to be added as a custom mode
 * through kde_output_management_v2 and then selected. Both steps are separate
 * commits: a mode cannot be made current before the compositor has generated it.
 *
 * Every event of kde_output_device_v2 needs a handler because libwayland aborts
 * the process when it dispatches an event whose listener slot is NULL. The
 * uninteresting ones are generated no-ops; they are exhaustive on purpose.
 */
// standard includes
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// lib includes
#include <poll.h>
#include <wayland-client.h>

// generated protocol headers
#include <kde-output-device-v2.h>
#include <kde-output-management-v2.h>
#include <zkde-screencast-unstable-v1.h>

// local includes
#include "kwin_virtual_display.h"

#ifdef KWIN_VDISPLAY_STANDALONE
  #include <iostream>
  #include <sstream>
#else
  #include "src/logging.h"
#endif

using namespace std::literals;

#ifdef KWIN_VDISPLAY_STANDALONE
namespace kwin::vdisplay {
  namespace detail {
    /// Minimal stand-in for BOOST_LOG so the probe tool can reuse this file.
    struct log_line_t {
      std::ostringstream buffer;

      ~log_line_t() {
        std::cerr << buffer.str() << std::endl;
      }

      template<class T>
      std::ostringstream &operator<<(const T &value) {
        buffer << value;
        return buffer;
      }
    };
  }  // namespace detail
}  // namespace kwin::vdisplay
  #define VD_LOG(level) ::kwin::vdisplay::detail::log_line_t {} << "[vdisplay] "
#else
  #define VD_LOG(level) BOOST_LOG(level) << "[vdisplay] "
#endif

namespace kwin::vdisplay {
  namespace {
    /// set_custom_modes was introduced in kde_output_management_v2 version 18.
    constexpr uint32_t management_version_for_custom_modes = 18;
    constexpr uint32_t management_version_max = 21;
    constexpr uint32_t device_version_max = 23;
    /// Binding kde_output_device_registry_v2 below 21 is a protocol error.
    constexpr uint32_t device_registry_version_min = 21;
    /// kde_output_device_v2 only gained its release request in version 21.
    constexpr uint32_t device_release_version_min = 21;
    /// Accept a mode whose refresh is within 1 Hz of what was asked for.
    constexpr int refresh_tolerance_mhz = 1000;

    class output_configurator_t;

    /**
     * A single mode advertised by an output.
     */
    struct mode_info_t {
      struct kde_output_device_mode_v2 *proxy = nullptr;
      int width = 0;
      int height = 0;
      int refresh_mhz = 0;
      bool removed = false;

      static void on_size(void *data, struct kde_output_device_mode_v2 *, int32_t width, int32_t height) {
        auto *self = static_cast<mode_info_t *>(data);
        self->width = width;
        self->height = height;
      }

      static void on_refresh(void *data, struct kde_output_device_mode_v2 *, int32_t refresh) {
        static_cast<mode_info_t *>(data)->refresh_mhz = refresh;
      }

      static void on_removed(void *data, struct kde_output_device_mode_v2 *) {
        static_cast<mode_info_t *>(data)->removed = true;
      }

      static void ignore_preferred(
        void *data [[maybe_unused]],
        struct kde_output_device_mode_v2 *kde_output_device_mode_v2 [[maybe_unused]]
      ) {}

      static void ignore_flags(
        void *data [[maybe_unused]],
        struct kde_output_device_mode_v2 *kde_output_device_mode_v2 [[maybe_unused]],
        uint32_t flags [[maybe_unused]]
      ) {}

      static constexpr struct kde_output_device_mode_v2_listener mode_listener = {
        .size = on_size,
        .refresh = on_refresh,
        .preferred = ignore_preferred,
        .removed = on_removed,
        .flags = ignore_flags,
      };
    };

    /**
     * An output KWin knows about, together with the modes it advertises.
     */
    struct device_info_t {
      output_configurator_t *owner = nullptr;
      struct kde_output_device_v2 *proxy = nullptr;
      std::string name;
      struct kde_output_device_mode_v2 *current_mode = nullptr;
      std::vector<mode_info_t *> modes;
      bool removed = false;
      bool enabled = false;

      static void on_name(void *data, struct kde_output_device_v2 *, const char *name) {
        static_cast<device_info_t *>(data)->name = name ? name : "";
      }

      static void on_current_mode(void *data, struct kde_output_device_v2 *, struct kde_output_device_mode_v2 *mode) {
        static_cast<device_info_t *>(data)->current_mode = mode;
      }

      static void on_enabled(void *data, struct kde_output_device_v2 *, int32_t enabled) {
        static_cast<device_info_t *>(data)->enabled = enabled != 0;
      }

      static void on_done(void *, struct kde_output_device_v2 *) {
        // Every property is applied as it arrives, so there is nothing to commit here.
      }

      static void on_removed(void *data, struct kde_output_device_v2 *) {
        static_cast<device_info_t *>(data)->removed = true;
      }

      /// Defined out of line: it has to allocate through the owning configurator.
      static void on_mode(void *data, struct kde_output_device_v2 *, struct kde_output_device_mode_v2 *mode);

      static void ignore_geometry(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        int32_t x [[maybe_unused]],
        int32_t y [[maybe_unused]],
        int32_t physical_width [[maybe_unused]],
        int32_t physical_height [[maybe_unused]],
        int32_t subpixel [[maybe_unused]],
        const char * make [[maybe_unused]],
        const char * model [[maybe_unused]],
        int32_t transform [[maybe_unused]]
      ) {}

      static void ignore_scale(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        wl_fixed_t factor [[maybe_unused]]
      ) {}

      static void ignore_edid(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        const char * raw [[maybe_unused]]
      ) {}

      static void ignore_enabled(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        int32_t enabled [[maybe_unused]]
      ) {}

      static void ignore_uuid(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        const char * uuid [[maybe_unused]]
      ) {}

      static void ignore_serial_number(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        const char * serialNumber [[maybe_unused]]
      ) {}

      static void ignore_eisa_id(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        const char * eisaId [[maybe_unused]]
      ) {}

      static void ignore_capabilities(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t flags [[maybe_unused]]
      ) {}

      static void ignore_overscan(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t overscan [[maybe_unused]]
      ) {}

      static void ignore_vrr_policy(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t vrr_policy [[maybe_unused]]
      ) {}

      static void ignore_rgb_range(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t rgb_range [[maybe_unused]]
      ) {}

      static void ignore_high_dynamic_range(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t hdr_enabled [[maybe_unused]]
      ) {}

      static void ignore_sdr_brightness(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t sdr_brightness [[maybe_unused]]
      ) {}

      static void ignore_wide_color_gamut(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t wcg_enabled [[maybe_unused]]
      ) {}

      static void ignore_auto_rotate_policy(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t policy [[maybe_unused]]
      ) {}

      static void ignore_icc_profile_path(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        const char * profile_path [[maybe_unused]]
      ) {}

      static void ignore_brightness_metadata(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t max_peak_brightness [[maybe_unused]],
        uint32_t max_frame_average_brightness [[maybe_unused]],
        uint32_t min_brightness [[maybe_unused]]
      ) {}

      static void ignore_brightness_overrides(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        int32_t max_peak_brightness [[maybe_unused]],
        int32_t max_average_brightness [[maybe_unused]],
        int32_t min_brightness [[maybe_unused]]
      ) {}

      static void ignore_sdr_gamut_wideness(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t gamut_wideness [[maybe_unused]]
      ) {}

      static void ignore_color_profile_source(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t source [[maybe_unused]]
      ) {}

      static void ignore_brightness(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t brightness [[maybe_unused]]
      ) {}

      static void ignore_color_power_tradeoff(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t preference [[maybe_unused]]
      ) {}

      static void ignore_dimming(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t multiplier [[maybe_unused]]
      ) {}

      static void ignore_replication_source(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        const char * source [[maybe_unused]]
      ) {}

      static void ignore_ddc_ci_allowed(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t allowed [[maybe_unused]]
      ) {}

      static void ignore_max_bits_per_color(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t max_bpc [[maybe_unused]]
      ) {}

      static void ignore_max_bits_per_color_range(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t min_value [[maybe_unused]],
        uint32_t max_value [[maybe_unused]]
      ) {}

      static void ignore_automatic_max_bits_per_color_limit(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t max_bpc_limit [[maybe_unused]]
      ) {}

      static void ignore_edr_policy(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t policy [[maybe_unused]]
      ) {}

      static void ignore_sharpness(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t sharpness [[maybe_unused]]
      ) {}

      static void ignore_priority(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t priority [[maybe_unused]]
      ) {}

      static void ignore_auto_brightness(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t enabled [[maybe_unused]]
      ) {}

      static void ignore_hdr_icc_profile_path(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        const char * profile_path [[maybe_unused]]
      ) {}

      static void ignore_hdr_color_profile_source(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t source [[maybe_unused]]
      ) {}

      static void ignore_abm_level(
        void *data [[maybe_unused]],
        struct kde_output_device_v2 *kde_output_device_v2 [[maybe_unused]],
        uint32_t level [[maybe_unused]]
      ) {}

      static constexpr struct kde_output_device_v2_listener device_listener = {
        .geometry = ignore_geometry,
        .current_mode = on_current_mode,
        .mode = on_mode,
        .done = on_done,
        .scale = ignore_scale,
        .edid = ignore_edid,
        .enabled = on_enabled,
        .uuid = ignore_uuid,
        .serial_number = ignore_serial_number,
        .eisa_id = ignore_eisa_id,
        .capabilities = ignore_capabilities,
        .overscan = ignore_overscan,
        .vrr_policy = ignore_vrr_policy,
        .rgb_range = ignore_rgb_range,
        .name = on_name,
        .high_dynamic_range = ignore_high_dynamic_range,
        .sdr_brightness = ignore_sdr_brightness,
        .wide_color_gamut = ignore_wide_color_gamut,
        .auto_rotate_policy = ignore_auto_rotate_policy,
        .icc_profile_path = ignore_icc_profile_path,
        .brightness_metadata = ignore_brightness_metadata,
        .brightness_overrides = ignore_brightness_overrides,
        .sdr_gamut_wideness = ignore_sdr_gamut_wideness,
        .color_profile_source = ignore_color_profile_source,
        .brightness = ignore_brightness,
        .color_power_tradeoff = ignore_color_power_tradeoff,
        .dimming = ignore_dimming,
        .replication_source = ignore_replication_source,
        .ddc_ci_allowed = ignore_ddc_ci_allowed,
        .max_bits_per_color = ignore_max_bits_per_color,
        .max_bits_per_color_range = ignore_max_bits_per_color_range,
        .automatic_max_bits_per_color_limit = ignore_automatic_max_bits_per_color_limit,
        .edr_policy = ignore_edr_policy,
        .sharpness = ignore_sharpness,
        .priority = ignore_priority,
        .auto_brightness = ignore_auto_brightness,
        .removed = on_removed,
        .hdr_icc_profile_path = ignore_hdr_icc_profile_path,
        .hdr_color_profile_source = ignore_hdr_color_profile_source,
        .abm_level = ignore_abm_level,
      };
    };

    /// Tracks one apply() round trip of a kde_output_configuration_v2.
    struct configuration_result_t {
      bool finished = false;
      bool applied = false;
      /// Populated by failure_reason, which KWin sends just before failed.
      std::string failure_reason;

      static void on_applied(void *data, struct kde_output_configuration_v2 *) {
        auto *self = static_cast<configuration_result_t *>(data);
        self->finished = true;
        self->applied = true;
      }

      static void on_failed(void *data, struct kde_output_configuration_v2 *) {
        auto *self = static_cast<configuration_result_t *>(data);
        self->finished = true;
        self->applied = false;
      }

      static void on_failure_reason(void *data, struct kde_output_configuration_v2 *, const char *reason) {
        static_cast<configuration_result_t *>(data)->failure_reason = reason ? reason : "";
      }

      static constexpr struct kde_output_configuration_v2_listener listener = {
        .applied = on_applied,
        .failed = on_failed,
        .failure_reason = on_failure_reason,
      };
    };

    /**
     * Binds the KDE output-management globals on an existing Wayland connection
     * and drives mode changes on a named output.
     *
     * The connection is borrowed, never closed, so this can share the display
     * kwingrab already uses for screencasting.
     */
    class output_configurator_t {
    public:
      output_configurator_t &operator=(output_configurator_t &&) = delete;

      explicit output_configurator_t(struct wl_display *display):
          display_ {display} {
        registry_ = wl_display_get_registry(display_);
        wl_registry_add_listener(registry_, &registry_listener, this);
        // First round trip binds the globals, the second collects the events
        // those globals emit on bind (output names, modes, ...).
        wl_display_roundtrip(display_);
        wl_display_roundtrip(display_);
      }

      ~output_configurator_t() {
        for (const auto &mode : modes_) {
          if (mode->proxy) {
            kde_output_device_mode_v2_destroy(mode->proxy);
          }
        }
        for (const auto &device : devices_) {
          if (!device->proxy) {
            continue;
          }
          // release is a destructor request added in version 21; below that the
          // proxy can only be dropped locally.
          if (wl_proxy_get_version(reinterpret_cast<struct wl_proxy *>(device->proxy)) >= device_release_version_min) {
            kde_output_device_v2_release(device->proxy);
          } else {
            kde_output_device_v2_destroy(device->proxy);
          }
        }
        if (device_registry_) {
          kde_output_device_registry_v2_destroy(device_registry_);
        }
        if (management_) {
          kde_output_management_v2_destroy(management_);
        }
        if (registry_) {
          wl_registry_destroy(registry_);
        }
      }

      /// Whether KWin exposes the output-management global at all.
      [[nodiscard]] bool available() const {
        return management_ != nullptr;
      }

      /// Whether the bound management version can generate custom modes.
      [[nodiscard]] bool supports_custom_modes() const {
        return management_ && management_version_ >= management_version_for_custom_modes;
      }

      mode_info_t *track_mode(struct kde_output_device_mode_v2 *proxy) {
        auto &slot = modes_.emplace_back(std::make_unique<mode_info_t>());
        slot->proxy = proxy;
        kde_output_device_mode_v2_add_listener(proxy, &mode_info_t::mode_listener, slot.get());
        return slot.get();
      }

      /**
       * @brief Add a custom mode to an output and switch the output to it.
       * @return true once the output reports the requested mode as current.
       */
      bool apply_mode(const std::string &output_name, int width, int height, int refresh_mhz, std::chrono::milliseconds timeout) {
        auto *device = wait_for_device(output_name, timeout);
        if (!device) {
          VD_LOG(error) << "output "sv << output_name << " never appeared"sv;
          return false;
        }

        if (!available()) {
          VD_LOG(error) << "kde_output_management_v2 is missing; cannot change modes"sv;
          return false;
        }

        if (find_mode(device, width, height, refresh_mhz)) {
          // KWin already advertises what we want, e.g. a plain 60 Hz request.
          return select_mode(device, width, height, refresh_mhz, timeout);
        }

        if (!supports_custom_modes()) {
          VD_LOG(warning) << "kde_output_management_v2 v"sv << management_version_
                          << " has no set_custom_modes (needs v"sv << management_version_for_custom_modes
                          << "); leaving the output at its default mode"sv;
          return false;
        }

        if (!commit_custom_mode(device, width, height, refresh_mhz, timeout)) {
          VD_LOG(error) << "KWin rejected the custom mode "sv << width << "x"sv << height
                        << "@"sv << refresh_mhz << "mHz"sv;
          return false;
        }

        return select_mode(device, width, height, refresh_mhz, timeout);
      }

      /**
       * @brief Names of every enabled output except the one to keep.
       *
       * Captured before anything is switched off so the same set can be turned
       * back on afterwards, rather than guessing at restore time.
       */
      std::vector<std::string> enabled_outputs_except(const std::string &keep) const {
        std::vector<std::string> names;
        for (const auto &device : devices_) {
          if (device->removed || !device->enabled || device->name.empty() || device->name == keep) {
            continue;
          }
          names.push_back(device->name);
        }
        return names;
      }

      /**
       * @brief Enable or disable a set of outputs in one transaction.
       *
       * One transaction on purpose: applying them separately would leave the
       * desktop with no enabled output in between, which KWin can refuse.
       */
      bool set_outputs_enabled(const std::vector<std::string> &names, bool enable, std::chrono::milliseconds timeout) {
        if (names.empty()) {
          return true;
        }
        if (!available()) {
          VD_LOG(error) << "kde_output_management_v2 is missing; cannot enable or disable outputs"sv;
          return false;
        }

        bool any = false;
        const bool applied = run_configuration([&](struct kde_output_configuration_v2 *configuration) {
          for (const auto &name : names) {
            if (auto *device = find_device(name); device && !device->removed) {
              kde_output_configuration_v2_enable(configuration, device->proxy, enable ? 1 : 0);
              any = true;
            }
          }
        },
                                               timeout);
        if (!any) {
          // Every output named has gone away; nothing to do rather than a failure.
          return true;
        }
        return applied;
      }


      /// Whether KWin knows an output by this name, waiting for it to appear.
      bool has_output(const std::string &name, std::chrono::milliseconds timeout) {
        return wait_for_device(name, timeout) != nullptr;
      }

    private:
      /// Pump the connection until `ready` holds or the deadline passes.
      bool dispatch_until(const std::function<bool()> &ready, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!ready()) {
          if (wl_display_flush(display_) < 0) {
            return false;
          }
          const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()
          );
          if (remaining.count() <= 0) {
            return ready();
          }
          struct pollfd pfd {};
          pfd.fd = wl_display_get_fd(display_);
          pfd.events = POLLIN;
          const int polled = poll(&pfd, 1, static_cast<int>(remaining.count()));
          if (polled < 0) {
            return false;
          }
          if (polled == 0) {
            return ready();
          }
          if ((pfd.revents & POLLIN) && wl_display_dispatch(display_) < 0) {
            VD_LOG(error) << "wl_display_dispatch failed"sv;
            return false;
          }
        }
        return true;
      }

      device_info_t *find_device(const std::string &name) const {
        for (const auto &device : devices_) {
          if (!device->removed && device->name == name) {
            return device.get();
          }
        }
        return nullptr;
      }

      device_info_t *wait_for_device(const std::string &name, std::chrono::milliseconds timeout) {
        if (auto *device = find_device(name)) {
          return device;
        }
        // The output was very likely created moments ago, so its global may
        // still be in flight.
        dispatch_until([&] {
          return find_device(name) != nullptr;
        },
                       timeout);
        return find_device(name);
      }

      static mode_info_t *find_mode(const device_info_t *device, int width, int height, int refresh_mhz) {
        mode_info_t *best = nullptr;
        for (auto *mode : device->modes) {
          if (mode->removed || mode->width != width || mode->height != height) {
            continue;
          }
          const int delta = std::abs(mode->refresh_mhz - refresh_mhz);
          if (delta > refresh_tolerance_mhz) {
            continue;
          }
          if (!best || delta < std::abs(best->refresh_mhz - refresh_mhz)) {
            best = mode;
          }
        }
        return best;
      }

      /// Run one configuration transaction. `build` fills it in, apply() commits.
      bool run_configuration(const std::function<void(struct kde_output_configuration_v2 *)> &build, std::chrono::milliseconds timeout) {
        auto *configuration = kde_output_management_v2_create_configuration(management_);
        if (!configuration) {
          return false;
        }
        configuration_result_t result;
        kde_output_configuration_v2_add_listener(configuration, &configuration_result_t::listener, &result);
        build(configuration);
        kde_output_configuration_v2_apply(configuration);

        const bool settled = dispatch_until([&] {
          return result.finished;
        },
                                            timeout);
        kde_output_configuration_v2_destroy(configuration);

        if (!settled || !result.finished) {
          VD_LOG(error) << "timed out waiting for KWin to apply an output configuration"sv;
          return false;
        }
        if (!result.applied && !result.failure_reason.empty()) {
          VD_LOG(error) << "KWin refused the output configuration: "sv << result.failure_reason;
        }
        return result.applied;
      }

      bool commit_custom_mode(const device_info_t *device, int width, int height, int refresh_mhz, std::chrono::milliseconds timeout) {
        auto *mode_list = kde_output_management_v2_create_mode_list(management_);
        if (!mode_list) {
          return false;
        }
        kde_mode_list_v2_set_resolution(mode_list, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        kde_mode_list_v2_set_refresh_rate(mode_list, static_cast<uint32_t>(refresh_mhz));
        // A virtual output has no real link to fit into, and reduced blanking is
        // what KWin's own generator defaults to.
        kde_mode_list_v2_set_reduced_blanking(mode_list, 1);
        kde_mode_list_v2_add_mode(mode_list);

        const bool applied = run_configuration([&](struct kde_output_configuration_v2 *configuration) {
          kde_output_configuration_v2_set_custom_modes(configuration, device->proxy, mode_list);
        },
                                               timeout);
        kde_mode_list_v2_destroy(mode_list);

        // The generated mode arrives as a fresh mode event on the device.
        wl_display_roundtrip(display_);
        return applied;
      }

      bool select_mode(device_info_t *device, int width, int height, int refresh_mhz, std::chrono::milliseconds timeout) {
        auto *mode = find_mode(device, width, height, refresh_mhz);
        if (!mode) {
          VD_LOG(error) << "no mode matching "sv << width << "x"sv << height << "@"sv << refresh_mhz
                        << "mHz on "sv << device->name;
          return false;
        }
        if (device->current_mode == mode->proxy) {
          return true;
        }
        if (!run_configuration([&](struct kde_output_configuration_v2 *configuration) {
              kde_output_configuration_v2_mode(configuration, device->proxy, mode->proxy);
            },
                               timeout)) {
          return false;
        }
        VD_LOG(info) << "output "sv << device->name << " set to "sv << mode->width << "x"sv << mode->height
                     << "@"sv << (mode->refresh_mhz / 1000.0) << "Hz"sv;
        return true;
      }

      void add_device(struct kde_output_device_v2 *proxy) {
        auto &slot = devices_.emplace_back(std::make_unique<device_info_t>());
        slot->owner = this;
        slot->proxy = proxy;
        kde_output_device_v2_add_listener(proxy, &device_info_t::device_listener, slot.get());
      }

      // Registry
      static void on_registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
        auto *self = static_cast<output_configurator_t *>(data);
        if (!std::strcmp(interface, kde_output_management_v2_interface.name)) {
          self->management_version_ = std::min(version, management_version_max);
          self->management_ = static_cast<struct kde_output_management_v2 *>(
            wl_registry_bind(registry, name, &kde_output_management_v2_interface, self->management_version_)
          );
        } else if (!std::strcmp(interface, kde_output_device_registry_v2_interface.name)) {
          // Plasma 6.5 and newer announce outputs through this object instead of
          // one global per output. Older versions of it cannot be bound at all.
          if (version < device_registry_version_min) {
            return;
          }
          self->device_registry_ = static_cast<struct kde_output_device_registry_v2 *>(
            wl_registry_bind(registry, name, &kde_output_device_registry_v2_interface, std::min(version, device_version_max))
          );
          kde_output_device_registry_v2_add_listener(self->device_registry_, &device_registry_listener, self);
        } else if (!std::strcmp(interface, kde_output_device_v2_interface.name)) {
          // Older Plasma: one global per output.
          self->add_device(static_cast<struct kde_output_device_v2 *>(
            wl_registry_bind(registry, name, &kde_output_device_v2_interface, std::min(version, device_version_max))
          ));
        }
      }

      static void on_registry_global_remove(void *, struct wl_registry *, uint32_t) {
        // Outputs report their own disappearance through kde_output_device_v2.removed.
      }

      static constexpr struct wl_registry_listener registry_listener = {
        .global = on_registry_global,
        .global_remove = on_registry_global_remove,
      };

      // Output-device registry
      static void on_device_registry_output(void *data, struct kde_output_device_registry_v2 *, struct kde_output_device_v2 *output) {
        static_cast<output_configurator_t *>(data)->add_device(output);
      }

      static void on_device_registry_finished(void *, struct kde_output_device_registry_v2 *) {
        // Only sent in reply to stop(), which is never requested here.
      }

      static constexpr struct kde_output_device_registry_v2_listener device_registry_listener = {
        .finished = on_device_registry_finished,
        .output = on_device_registry_output,
      };

      struct wl_display *display_ = nullptr;
      struct wl_registry *registry_ = nullptr;
      struct kde_output_management_v2 *management_ = nullptr;
      struct kde_output_device_registry_v2 *device_registry_ = nullptr;
      uint32_t management_version_ = 0;
      std::vector<std::unique_ptr<device_info_t>> devices_;
      std::vector<std::unique_ptr<mode_info_t>> modes_;
    };

    void device_info_t::on_mode(void *data, struct kde_output_device_v2 *, struct kde_output_device_mode_v2 *mode) {
      auto *self = static_cast<device_info_t *>(data);
      self->modes.push_back(self->owner->track_mode(mode));
    }
  }  // namespace

  std::string kwin_output_name(const std::string &requested_name) {
    return "Virtual-" + requested_name;
  }

  std::vector<std::string> disable_other_outputs(
    struct wl_display *display,
    const std::string &keep_output_name,
    const std::chrono::milliseconds timeout
  ) {
    if (!display) {
      return {};
    }
    output_configurator_t configurator {display};
    if (!configurator.has_output(keep_output_name, timeout)) {
      // Without the output we are keeping there is nothing safe to switch to.
      VD_LOG(error) << "output "sv << keep_output_name << " never appeared; leaving the other outputs alone"sv;
      return {};
    }

    const auto names = configurator.enabled_outputs_except(keep_output_name);
    if (names.empty()) {
      return {};
    }
    if (!configurator.set_outputs_enabled(names, false, timeout)) {
      VD_LOG(error) << "KWin refused to disable the other outputs; leaving them on"sv;
      return {};
    }
    return names;
  }

  bool restore_outputs(
    struct wl_display *display,
    const std::vector<std::string> &output_names,
    const std::chrono::milliseconds timeout
  ) {
    if (!display || output_names.empty()) {
      return true;
    }
    output_configurator_t configurator {display};
    return configurator.set_outputs_enabled(output_names, true, timeout);
  }

  bool apply_custom_mode(
    struct wl_display *display,
    const std::string &output_name,
    const int width,
    const int height,
    const int refresh_mhz,
    const std::chrono::milliseconds timeout
  ) {
    if (!display || width <= 0 || height <= 0 || refresh_mhz <= 0) {
      return false;
    }
    output_configurator_t configurator {display};
    return configurator.apply_mode(output_name, width, height, refresh_mhz, timeout);
  }


  namespace {
    /// stream_virtual_output_with_description, and therefore virtual outputs.
    constexpr uint32_t screencast_virtual_output_version = 4;

    struct screencast_probe_t {
      uint32_t version = 0;

      static void on_global(void *data, struct wl_registry *, uint32_t, const char *interface, uint32_t version) {
        if (!std::strcmp(interface, zkde_screencast_unstable_v1_interface.name)) {
          static_cast<screencast_probe_t *>(data)->version = version;
        }
      }

      static void on_global_remove(void *, struct wl_registry *, uint32_t) {}

      static constexpr struct wl_registry_listener listener {
        .global = on_global,
        .global_remove = on_global_remove,
      };
    };

    bool probe_supported() {
      // Nothing is bound here: the advertised version is all that is needed, and
      // binding a restricted interface would fail without the KWin permission
      // file that only an actual stream needs.
      struct wl_display *display = wl_display_connect(nullptr);
      if (!display) {
        return false;
      }

      screencast_probe_t probe;
      struct wl_registry *registry = wl_display_get_registry(display);
      wl_registry_add_listener(registry, &screencast_probe_t::listener, &probe);
      wl_display_roundtrip(display);

      wl_registry_destroy(registry);
      wl_display_disconnect(display);
      return probe.version >= screencast_virtual_output_version;
    }
  }  // namespace

  bool supported() {
    static const bool available = probe_supported();
    return available;
  }

}  // namespace kwin::vdisplay
