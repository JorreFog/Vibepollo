# Vibepollo on Linux — virtual displays on KDE Plasma Wayland

A Linux port of [Nonary/Vibepollo](https://github.com/Nonary/Vibepollo), forked at
`8bf0ef7d`. Upstream does not build on Linux; this branch does, adds a virtual
display that needs no kernel driver, makes the frame limiter actually limit, and
fixes three crashes in cross-platform code.

Everything below was measured on real hardware, streaming to a real client.

**Host:** CachyOS · KDE Plasma 6.7.4 Wayland · NVIDIA RTX 4080 SUPER (driver 610.57.04) · GCC 16.2 · CUDA 13.3
**Client:** AYN Odin 2 Portal (Snapdragon 8 Gen 2, 1080p120 AMOLED) running Artemis

---

## The headline: no kernel driver, and it comes up in ~185 ms

On Windows the virtual display is an **IddCx indirect display driver**: the OS
enumerates a new monitor, changes display topology, waits for the desktop to
settle, then applies a mode.

KWin can do the whole thing in one Wayland request.
`zkde_screencast_unstable_v1::stream_virtual_output_with_description` creates the
output **and** returns a PipeWire node carrying its contents — so the display is
its own capture source, and `kwingrab`'s existing PipeWire path consumes it
unchanged. The output is owned by the stream: close the stream and the display
disappears, so nothing is left behind if the host dies.

Measured, from a real session:

```
18:44:09.128  streaming session started
18:44:09.150  CLIENT CONNECTED                                    (+22 ms)
18:44:09.236  virtual display created at 1920x1080@119.877 Hz     (+108 ms)
18:44:09.239  screencasting the virtual output
18:44:09.313  AV1 encoder created                                 (+185 ms total)
```

Virtual outputs are built at a hard-coded 60 Hz, which would cap the stream, so
the requested rate is added as a custom mode through `kde_output_management_v2`
and then selected. Verified at **3840x2160@119.944 Hz**, **2560x1440@119.877 Hz**
and **1920x1080@119.877 Hz**.

## Streaming performance

1080p120, AV1 (NVENC) into the Odin's `c2.qti.av1.decoder.low_latency`:

| Metric | Value |
| --- | --- |
| Video encode call duration | **1.30 / 6.43 / 1.74 ms** (min/max/avg) |
| Frame network latency | **0.02 / 0.17 / 0.05 ms** (min/max/avg) |
| Dropped submissions | **0** |
| Effective encode bitrate | 63.4 Mbps |

Encoders found: **H.264, HEVC and AV1**, all NVENC.

## The frame limiter now limits

It previously applied no cap at all, while logging success. Measured with
`tools/frametime_probe.c`, which times its own buffer swaps:

| | fps | frametime | stdev | p99 |
| --- | --- | --- | --- | --- |
| unlimited | 28743.03 | 0.03 ms | 0.02 ms | 0.10 ms |
| **limited to 120** | **120.00** | **8.33 ms** | **0.01 ms** | **8.37 ms** |

Before the fix, `fps_limit=30` left the workload running at **240 fps**. After,
it holds **29.95 fps** at a limit of 30 and **89.99 fps** at a limit of 90.

## Arrival-based capture pacing

Poll-based backends grab the screen when they wake, so a fixed grid costs them
nothing. PipeWire pushes frames on the compositor's schedule, so the grid only
decides *when we look* — a frame landing just after a grid point sits finished in
memory until the next one, and at matched rates that phase offset is **constant
for the whole session**. That is latency, not jitter.

Simulated across rate ratios, averaged over random phase offsets. Both metrics
are shown, because there is a real trade-off:

| source→target | jitter | grid latency | grid sd | arrival latency | arrival sd |
| --- | --- | --- | --- | --- | --- |
| **120→120** | 0.0 ms | **4.24 ms** | 0.00 ms | **0.00 ms** | 0.00 ms |
| 120→120 | 0.5 ms | 4.18 ms | 1.00 ms | 0.00 ms | 0.47 ms |
| 120→60 | 0.0 ms | 4.52 ms | 0.00 ms | 0.00 ms | 0.00 ms |
| 144→60 | 0.0 ms | 3.41 ms | **0.00 ms** | 0.00 ms | **3.40 ms** |
| 240→120 | 0.0 ms | 2.26 ms | 0.00 ms | 0.00 ms | 0.00 ms |
| 60→120 | 0.0 ms | 4.24 ms | 0.00 ms | 0.00 ms | 0.00 ms |

Arrival pacing removes the latency outright in every case. **144→60 is the
honest exception**: a non-integer ratio is inherently uneven, and there arrival
passes the unevenness through as interval spread where the grid stays regular.
That is why it is a config key (`linux_capture_pacing`) and not a hardcoded
change. Default is `arrival`.

## Bugs fixed

Each was reproduced before and verified after.

| Bug | Impact |
| --- | --- |
| Double `tray_exit()` | SIGSEGV on **3 of 3** shutdowns → exit 0 on 3 of 3 |
| Uncaught `system_error` in `proc_t::running()` | **Any game quitting mid-stream aborted the host** |
| `process_environment` lifetime bug | Child processes received an environment built from **freed memory** |
| MangoHud detection | 0.8 moved the GL hook behind `libMangoHud_shim.so`; OpenGL titles got no limit |
| `gamescope::present()` | Trusted a *global, permanent* X atom name — reported gamescope on any desktop |
| Limiter published `GAMESCOPE_FPS_LIMIT` itself | Poisoned its own detection permanently |
| Limiter env nested in a Windows-only branch | `frame_limiter_launch_env()` was never called on Linux |
| `VirtualDisplayCapable` hardcoded false | Clients refused to offer virtual-display launches |

Three of these are in cross-platform code and are offered separately in
[`upstream-crash-fixes`](../../tree/upstream-crash-fixes).

## Also here

- **`linux_virtual_display_exclusive`** — turn the physical monitors off while
  streaming and restore them afterwards.
- **Thread priority actually applies.** RTKit for scheduling (5 threads at
  nice −15) and `cap_sys_nice` for the high-priority GPU context. Before: 50
  `CAP_SYS_NICE` warnings and 33 `setpriority` failures per session; after, zero.
- **`tools/kwin_virtual_display_probe`** — verify a Plasma install supports all of
  this without building the host.
- **`docs/examples/`** — a working configuration with the reasoning inline.

## Tests

**38/38 green**, including regression tests for the atom-name bug, the
self-poisoning write, the environment lifetime contract, and the pacer's
jitter-tolerance and re-anchor paths.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON \
      -DGLAD_SKIP_PIP_INSTALL=ON -DPython_EXECUTABLE=<venv>/bin/python
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Build notes, dependencies and the traps are in
[`docs/linux_virtual_display.md`](docs/linux_virtual_display.md) and
[`HANDOVER.md`](HANDOVER.md).

## Requirements

- KDE Plasma Wayland with `zkde_screencast_unstable_v1` **v4+** (virtual outputs)
  and `kde_output_management_v2` **v18+** (custom modes). Verified on v6 and v21.
- `capture = kwin`. KMS grab is not usable with the NVIDIA proprietary driver.

## Honest limitations

- **Windows was not benchmarked.** The ~185 ms figure is measured on Linux; the
  IddCx comparison is architectural reasoning, not a controlled A/B.
- **macOS is untouched by testing.** One shared fix updates a macOS call site
  identically, but it has not been compiled.
- **`linux_virtual_display_exclusive` cannot restore the monitors if the host is
  SIGKILLed.** It is off by default. Recovery is
  `kscreen-doctor output.DP-1.enable ...`.
- Arrival pacing passes source jitter through; see the 144→60 row above.
- Capabilities do not survive `cmake --install`, so `setcap` must be re-applied
  after every install.
