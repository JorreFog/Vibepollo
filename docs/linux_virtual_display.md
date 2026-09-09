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

## Building on CachyOS / Arch

```bash
sudo pacman -S --needed base-devel cmake git ninja nodejs npm \
  boost libcap libdrm libevdev libnotify libpulse libva libx11 libxcb \
  libxfixes libxrandr libxtst libpipewire wayland wayland-protocols \
  numactl openssl curl miniupnpc avahi nlohmann-json

git clone --recurse-submodules <your fork> vibepollo
cd vibepollo
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Add `-DSUNSHINE_ENABLE_CUDA=ON` for NVENC on NVIDIA (needs the `cuda` package).
`SUNSHINE_ENABLE_KWIN` is on by default and is what this feature needs.

On an NVIDIA + Plasma Wayland host, prefer the KWin capture backend. KMS grab is
not usable with the proprietary driver, and the virtual display path described
here goes through KWin anyway.

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

**The refresh rate is close to what was asked for, but not exact**
: KWin does not honour the requested rate literally. `set_custom_modes` takes a
  rate in mHz, but KWin generates the mode and reports back whatever its own
  timing math produces. Asking for 120.000 Hz at 1920x1080 yields **119.877 Hz**;
  the same request at 3840x2160 yields **119.944 Hz**, which is how you can tell
  the number comes from mode generation and not from the client.

  The reachable rates are coarse. Near 120 Hz at 1080p the generated modes land
  about 1 Hz apart - measured, 119.877 and then 120.928 - so an arbitrary target
  such as a panel calibrated to 120.064 Hz cannot be matched. Requests for
  120.000, 120.030 and 120.064 all produce the same 119.877 mode.

  `refresh_tolerance_mhz` (1000, i.e. 1 Hz) is what makes this invisible in
  normal use: a generated mode within 1 Hz of the request is accepted as a match.
  Tightening it does not get you closer to the requested rate, it only turns the
  silent approximation into a failure - the mode KWin offers genuinely is not the
  one asked for.

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
