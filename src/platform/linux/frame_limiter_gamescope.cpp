/**
 * @file src/platform/linux/frame_limiter_gamescope.cpp
 * @brief gamescope's runtime frame cap, driven through its X root property.
 */
// local includes
#include "src/platform/linux/frame_limiter_gamescope.h"

#ifdef SUNSHINE_BUILD_X11
  // lib includes
  #include <X11/Xatom.h>
  #include <X11/Xlib.h>
#endif

namespace platf::gamescope {

#ifdef SUNSHINE_BUILD_X11

  namespace {
    /// RAII wrapper so every early return closes the display.
    class display_t {
    public:
      display_t():
          handle_ {XOpenDisplay(nullptr)} {}

      ~display_t() {
        if (handle_) {
          XCloseDisplay(handle_);
        }
      }

      display_t &operator=(display_t &&) = delete;

      explicit operator bool() const {
        return handle_ != nullptr;
      }

      Display *get() const {
        return handle_;
      }

      /// The interned atom name, or None if nothing ever named it on this server.
      Atom existing_property() const {
        return XInternAtom(handle_, fps_limit_property, True);
      }

      /**
       * @brief The property, but only when it is actually published on the root window.
       *
       * Interning proves nothing: atom names are global to the X server and outlive
       * whoever created them, so a name is still resolvable long after the client
       * that mentioned it is gone. Only gamescope publishes the property itself.
       */
      Atom published_property() const {
        const Atom property = existing_property();
        if (property == None) {
          return None;
        }

        Atom actual_type = None;
        int actual_format = 0;
        unsigned long items = 0;
        unsigned long bytes_after = 0;
        unsigned char *data = nullptr;

        if (XGetWindowProperty(
              handle_, DefaultRootWindow(handle_), property, 0, 1, False, XA_CARDINAL,
              &actual_type, &actual_format, &items, &bytes_after, &data
            ) != Success) {
          return None;
        }
        const bool published = data && items >= 1 && actual_format == 32;
        if (data) {
          XFree(data);
        }
        return published ? property : None;
      }

    private:
      Display *handle_ = nullptr;
    };
  }  // namespace

  bool present() {
    // Interning the atom name is not enough to conclude gamescope is running.
    // X atom names are global to the server and permanent for its lifetime, so
    // any client that ever names GAMESCOPE_FPS_LIMIT - a benchmark, an xprop
    // one-liner, a previous tool - makes XInternAtom(..., True) succeed forever
    // afterwards. On a plain XWayland session that made the limiter select
    // gamescope, write the property, report success and cap nothing, which is
    // exactly the silent wrong-provider substitution this code avoids
    // elsewhere. Require the property to actually be published on the root
    // window with a readable CARDINAL value instead.
    return get_fps_limit().has_value();
  }

  bool set_fps_limit(const int fps) {
    const display_t display;
    if (!display) {
      return false;
    }
    // Deliberately not existing_property(): XChangeProperty *creates* the property
    // if it is missing, so writing through a merely-interned atom publishes it on a
    // desktop that has no gamescope at all - and every later detection then sees a
    // published property and believes gamescope is running. The limiter poisoned its
    // own detection that way. gamescope owns this property; only ever modify it.
    const Atom property = display.published_property();
    if (property == None) {
      return false;
    }
    // A format-32 property is passed as an array of long, not int32_t.
    const unsigned long value = fps > 0 ? static_cast<unsigned long>(fps) : 0ul;
    XChangeProperty(
      display.get(),
      DefaultRootWindow(display.get()),
      property,
      XA_CARDINAL,
      32,
      PropModeReplace,
      reinterpret_cast<const unsigned char *>(&value),
      1
    );
    XFlush(display.get());
    return true;
  }

  std::optional<int> get_fps_limit() {
    const display_t display;
    if (!display) {
      return std::nullopt;
    }
    const Atom property = display.published_property();
    if (property == None) {
      return std::nullopt;
    }

    Atom actual_type = None;
    int actual_format = 0;
    unsigned long items = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = nullptr;

    if (XGetWindowProperty(
          display.get(),
          DefaultRootWindow(display.get()),
          property,
          0,
          1,
          False,
          XA_CARDINAL,
          &actual_type,
          &actual_format,
          &items,
          &bytes_after,
          &data
        ) != Success) {
      return std::nullopt;
    }

    std::optional<int> limit;
    if (data && items >= 1 && actual_format == 32) {
      limit = static_cast<int>(*reinterpret_cast<const unsigned long *>(data));
    }
    if (data) {
      XFree(data);
    }
    return limit;
  }

#else

  bool present() {
    return false;
  }

  bool set_fps_limit(int) {
    return false;
  }

  std::optional<int> get_fps_limit() {
    return std::nullopt;
  }

#endif  // SUNSHINE_BUILD_X11

}  // namespace platf::gamescope
