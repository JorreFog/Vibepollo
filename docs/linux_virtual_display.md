# Linux virtual display

On Windows, Vibepollo creates a virtual monitor with a bundled IddCx driver so a
stream can run at the client's own resolution without touching the physical
desktop. On Linux the same result needs no kernel driver at all: KWin can create
an extra output at runtime and hand back a PipeWire stream of it in a single
request.

This is implemented for **KDE Plasma on Wayland**. Other sessions are covered
under [Other desktops](#other-desktops).

## How it works

When a stream starts with a virtual display configured, Vibepollo asks KWin for a
new output through `zkde_screencast_unstable_v1`:

```
stream_virtual_output_with_description(name, description, width, height, scale, pointer)
```

KWin then:

1. creates a real output (`DrmBackend::createVirtualOutput`), so the desktop
   extends onto it and windows can be moved to it like any other monitor;
2. names it `Virtual-Vibepollo`;
3. starts a PipeWire stream carrying its contents and returns the node id.

Because the output *is* the capture source, there is no separate display to
select and no window to capture — the existing PipeWire path in `kwingrab.cpp`
consumes the node directly.

The output only lives as long as the stream. Vibepollo closes the stream when the
session ends, and KWin removes the display again, so nothing is left behind if
Vibepollo crashes or the client disconnects.

### Refresh rate

KWin builds virtual outputs with a single hard-coded 60 Hz mode, which would cap
the stream at 60 fps. Vibepollo therefore adds a custom mode matching the
client's requested refresh via `kde_output_management_v2` and switches the output
to it. This needs:

- `kde_output_management_v2` **version 18 or newer** (`set_custom_modes`), and
- `zkde_screencast_unstable_v1` **version 4 or newer**
  (`stream_virtual_output_with_description`).

If the compositor is older, the display is still created but stays at 60 Hz and a
warning is logged.

Rates such as 59.94 Hz are preserved: the client's `framerateX100` is converted
straight to mHz.

## Enabling it

Either:

- set `linux_virtual_display = enabled` in `sunshine.conf`, or
- pick **virtual** as the output in the web UI (Configuration → Video), or
- set it per-app / per-client as a `linux_virtual_display` override.

Capture must be using the KWin backend (`capture = kwin`, which is the default on
a Plasma Wayland session).

The virtual display is sized from the client's requested resolution, so a client
asking for 3840x2160@120 gets a 3840x2160 output running at 120 Hz.

## Permissions

`zkde_screencast_unstable_v1` is a restricted interface. KWin only hands it to
executables named in a `.desktop` file that declares:

```ini
X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1
```

Vibepollo installs one at package install time and, failing that, writes a
temporary one into `~/.local/share/applications` on first use. If neither works,
run Vibepollo with `KWIN_WAYLAND_NO_PERMISSION_CHECKS=1` to bypass the check.

## Verifying without rebuilding

`kwin-virtual-display-probe` creates a virtual display, applies the mode and
holds it open until interrupted. Use it to confirm a Plasma install supports
everything before rebuilding Vibepollo:

```bash
# from the source tree
mkdir -p /tmp/probe && cd /tmp/probe
for p in zkde-screencast-unstable-v1 kde-output-device-v2 kde-output-management-v2; do
  wayland-scanner client-header "$OLDPWD/third-party/plasma-wayland-protocols/src/protocols/$p.xml" "$p.h"
  wayland-scanner private-code   "$OLDPWD/third-party/plasma-wayland-protocols/src/protocols/$p.xml" "$p.c"
  gcc -std=c11 -I. -c "$p.c" -o "$p.o"
done
g++ -std=c++20 -DKWIN_VDISPLAY_STANDALONE -I. -I"$OLDPWD" \
    "$OLDPWD/tools/kwin_virtual_display_probe.cpp" \
    "$OLDPWD/src/platform/linux/kwin_virtual_display.cpp" \
    ./*.o -lwayland-client -o kwin-virtual-display-probe

./kwin-virtual-display-probe 3840 2160 120
```

A new display should appear in *System Settings → Display Configuration* at the
requested resolution and refresh rate, and disappear on Ctrl+C.

## Troubleshooting

**`zkde_screencast_unstable_v1 not found in registry`**
: The permission `.desktop` file is missing or does not match the running
  executable. Re-run after `export KWIN_WAYLAND_NO_PERMISSION_CHECKS=1` to
  confirm, then fix the desktop file.

**`KWin offers zkde_screencast_unstable_v1 v<n> but virtual outputs need v4`**
: Plasma is too old. Virtual outputs need the protocol version shipped with
  Plasma 5.25 and later.

**Display appears but is stuck at 60 Hz**
: `kde_output_management_v2` is older than version 18, so custom modes cannot be
  generated. The log names the version it found. Streaming still works at 60 fps.

**`KWin refused the output configuration: <reason>`**
: KWin rejected the custom mode; the reason it reports is logged verbatim. Very
  high resolution/refresh combinations may exceed what the compositor will
  generate.

## Other desktops

The mechanism above is KWin-specific. Equivalents on other sessions, none of
which are wired into Vibepollo yet:

| Session | Mechanism |
| --- | --- |
| GNOME (Wayland) | `org.gnome.Mutter.ScreenCast.CreateVirtualMonitor` |
| wlroots (Hyprland, sway) | headless outputs — `hyprctl output create headless`, `swaymsg create_output` |
| X11 / any | `evdi` kernel module, or a forced connector with an injected EDID (`drm.edid_firmware=`) |

On X11 with the NVIDIA proprietary driver the usual approach is a custom EDID
plus `ConnectedMonitor`, configured statically at X startup rather than per
stream.
