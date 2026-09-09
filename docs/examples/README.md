# Example configurations

Working configurations, with the reasoning for each choice recorded alongside
it. Copy one and edit the paths rather than starting from an empty file.

| File | Host | Client |
| --- | --- | --- |
| `kwin_1080p120.conf` | KDE Plasma Wayland, NVIDIA | 1080p120 handheld (Artemis) |

## Host setup these assume

Two things live outside the configuration file and are easy to miss, because
nothing fails loudly without them - streaming simply runs at lower priority.

**Thread priority.** The capture, encode, stream and audio threads ask for
nice -15, brokered through RTKit. Without the daemon installed the request fails
with `ServiceUnknown` and every thread stays at normal priority:

```bash
sudo pacman -S rtkit && sudo systemctl enable --now rtkit-daemon   # or your distro's equivalent
```

**GPU context priority.** Separately from the above, the driver gates the
high-priority EGL context on a capability held by the process itself, which no
daemon can broker. Distribution packages set it; a hand-built binary has to be
given it explicitly:

```bash
sudo setcap cap_sys_admin,cap_sys_nice+p /path/to/sunshine
```

Note that capabilities do not survive the binary being replaced, so this has to
be re-applied after every `cmake --install`. `getcap` is the first thing to
check if a rebuilt host suddenly feels worse.

## Client settings that matter as much as the host

Measured on an Odin 2 Portal, but the reasoning is general:

- **Match the panel.** Streaming 4K to a 1080p handheld costs bitrate and forces
  a downscale, and its low-latency decoder may not even reach the frame rate at
  that resolution - the Odin's manages 1080p at 240 but 4K only at 60.
- **Raise the bitrate.** Defaults are tuned for constrained networks. On a local
  link with an efficient codec there is a lot of quality left on the table; check
  what your decoder is rated for before going far above it.
- **Ultra Low Latency**, where the client offers it, is what selects the
  vendor's low-latency decoder variant rather than the general-purpose one.
