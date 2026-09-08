# Linux frame limiter

Capping the game to the stream's frame rate is what makes a stream look
smooth. An unlimited game renders whenever it can, the encoder samples at a
fixed cadence, and the mismatch shows up as judder even when the average FPS
looks fine. On Windows Vibepollo drives RTSS for this. Linux has no single
equivalent, so three providers are supported.

## Providers

| Provider | How the limit is applied | Changeable mid-stream |
| --- | --- | --- |
| gamescope | `GAMESCOPE_FPS_LIMIT` property on its X root window | **yes** |
| MangoHud | `MANGOHUD_CONFIG=fps_limit=…` in the launch environment | no |
| libstrangle | `FPS=…` plus `LD_PRELOAD` in the launch environment | no |

Only gamescope is re-limitable while a game runs, because it polls a property
it owns. The other two read their configuration once at process start, so
Vibepollo sets them when it launches the game.

This is the honest difference from RTSS, which can be re-driven at any time. If
you want the limit to follow a client that changes its frame rate mid-session,
run the game under gamescope.

## Choosing a provider

```
frame_limiter_enable = enabled
frame_limiter_provider = auto
```

`auto` prefers gamescope when the session is running under it, then MangoHud,
then libstrangle. Naming a provider explicitly uses only that one: if it is not
installed, no limit is applied rather than silently substituting another.
`frame_limiter_fps_limit` overrides the rate; left at 0 the client's requested
frame rate is used.

The limit is derived the same way as on Windows: the client's exact display
mode rate wins, then the raw stream cadence, then the plain requested FPS.
Fractional rates survive where the provider supports them: a 59.94 Hz client
gets `fps_limit=59.94` under MangoHud, which parses its limits as floats.
libstrangle is given a whole number, matching what its own `strangle` wrapper
passes, and gamescope's property is an integer, so both are rounded.

MangoHud is configured with `no_display`, so it acts purely as a limiter — an
overlay would otherwise be captured into the stream.

## Detection

MangoHud counts as available when either `libMangoHud.so` or its implicit
Vulkan layer manifest is present; the layer alone is enough for Vulkan titles
via `MANGOHUD=1`, the library is only needed to reach OpenGL ones. libstrangle
needs its library. gamescope counts as available only when its property can
actually be found on the current X display, so Vibepollo never claims a limiter
it cannot drive.

## Measuring it

`tools/benchmark_frame_limiter.sh` runs a workload under each installed
provider and reports frame-time spread:

```bash
tools/benchmark_frame_limiter.sh 60 20 vkcube
```

It prints mean FPS, mean frame time against the target, standard deviation, p99
and max. Lower deviation and a p99 near the mean mean more evenly spaced frames
reaching the encoder, which is the property that matters for streaming — a
limiter that hits the right average with a wide spread will still judder.

Run it before and after enabling the limiter to see the difference on your own
hardware; numbers vary far too much between GPUs, drivers and titles for a
quoted figure to be meaningful.
