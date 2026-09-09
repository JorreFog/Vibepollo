# Vibepollo on Linux — handover

Work done against **`Nonary/Vibepollo`**, base commit **`8bf0ef7d3dbc0402e553deb93bb52e50447c4225`** (master at the time).
Target host: **CachyOS, KDE Plasma 6 Wayland, NVIDIA proprietary driver.**

Twelve commits, delivered as a `git format-patch` series. Three themes:

1. Make Vibepollo build on Linux at all (it does not, upstream).
2. Virtual displays on KDE Plasma Wayland — the Linux answer to the Windows IddCx driver.
3. A frame limiter — the Linux answer to RTSS.

Plus one deliberately unfinished piece (arrival-based capture pacing), described in *Unfinished work*.

---

## Apply and build

```bash
git clone --recurse-submodules https://github.com/Nonary/Vibepollo.git
cd Vibepollo
git checkout -b linux-work 8bf0ef7d3dbc0402e553deb93bb52e50447c4225
git am /path/to/patches/00*.patch

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Arch/CachyOS dependencies and the CUDA flag are in `docs/linux_virtual_display.md`.

**Submodule warning.** A `--depth 1 --recurse-submodules` clone can leave some
submodules present-but-empty (`third-party/wlr-protocols`,
`Simple-Web-Server`, `tray` did). Symptom: wayland-scanner reports
"Could not open input file", or CMake cannot find a subdirectory. Fix:

```bash
git submodule update --init --force --recursive --depth 1 third-party/<name>
```

`glad` also needs `jinja2` and `setuptools<81` in the Python CMake picks up;
`pkg_resources` was removed in setuptools 81, and glad still imports it.

---

## Verification status

Read this before trusting anything below.

**Verified in a Linux container (Ubuntu 24.04, GCC 13, no GPU, no compositor):**

- Clean from-scratch configure + full `ninja` build of `sunshine` and the whole
  test suite, web UI included. Binary links and runs.
- `ctest`: **36/36 green, 5 consecutive clean-tree runs.** Before this work the
  suite could not build on Linux at all.
- 50 tests written here (31 frame-limiter policy, 13 provider detection,
  6 gamescope), each run 8x with `--gtest_shuffle` and varying seeds. No
  failures, no order dependence.
- The gamescope tests are not mocked: they drive a real X server (Xvfb) and
  assert the `GAMESCOPE_FPS_LIMIT` property round-trips.
- Control-path cost: limiter decision ~0.25 us mean / 0.43 us p99; gamescope
  property write ~0.30 ms mean (opens an X connection per call; runs once per
  stream start/stop).

**Not verified — no GPU, compositor, client or network path existed:**

- That KWin actually creates the virtual output, that it encodes, or that a
  client can stream it.
- That the frame limiter measurably smooths a real stream.
- Any end-to-end latency number whatsoever.

Two things to run on the real machine first:

```bash
kwin-virtual-display-probe 3840 2160 120     # build steps in docs/linux_virtual_display.md
tools/benchmark_frame_limiter.sh 120 20 vkcube
```

---

## The patches

### Build fixes — 0001, 0002, 0005, 0006, 0008

Upstream Vibepollo does not compile on Linux. These are independent faults;
each alone is fatal.

| Patch | Fault |
| --- | --- |
| 0001 | `mem_type_e` lost its `vulkan` enumerator while `pipewire.cpp` still switches on it, so **every** PipeWire capture backend (KWin and portal) failed to compile. Also `nvenc_config` declared a member with the same name as its own enum, which GCC rejects outright (`-Wchanges-meaning` is a permerror, not a warning — Arch is on GCC 15, so this bites there too). |
| 0002 | CMake still listed the pre-glad2 `third-party/glad/src/*.c` files. glad2 generates into the build tree and is consumed through the `glad` interface target that was already linked, so configure died before generating anything. |
| 0005 | `has_stream_session_activity` (nvhttp.cpp), `native_amf_lifecycle_gate` and `encode_session_teardown_mutex` (video.cpp) are defined inside `#ifdef _WIN32` but called unconditionally. None touch anything Windows-specific; each moved to the neighbouring cross-platform unnamed namespace. |
| 0006 | A GUID compared with `==` (a Windows-only operator; the file already had `equal_guids()` for this), an undeclared `_shutdown` member in the NVML loader (the real pointer is `nvmlShutdown`), and a `string_view` passed where avahi wants a C string. |
| 0008 | Two test targets linked `ws2_32` (Winsock) unconditionally. Tests also ran with the build tree as their working directory while some read `src_assets/` through repository-relative paths, so passing depended on where ctest was started. |

**If you rebase onto a newer upstream, expect more of these.** The pattern is
Windows-only development drifting; grep for symbols defined inside `#ifdef
_WIN32` blocks but used outside them. A script that finds them:

```python
# compute _WIN32 nesting depth per line, list symbols defined at depth>0 and used at depth==0
```

### Virtual display — 0003, 0004, 0007, 0009

**No kernel driver is needed on KDE.** KWin can create an output at runtime and
`DrmVirtualOutput` makes it behave like a real monitor — the desktop extends
onto it, windows can be moved to it.

The mechanism is one Wayland request:

```
zkde_screencast_unstable_v1::stream_virtual_output_with_description(
    stream, name, description, width, height, scale, pointer)
```

KWin creates the output **and** returns a PipeWire node carrying its contents,
so the display is its own capture source and `kwingrab.cpp`'s existing PipeWire
path consumes it unchanged. The output is owned by the stream: closing the
stream removes the display, so nothing is left behind if Vibepollo dies.

Requires `zkde_screencast_unstable_v1` **v4+** (`stream_virtual_output_with_description`).

**Refresh rate is the subtle part.** KWin builds virtual outputs with a single
hard-coded 60000 mHz mode (`drm_virtual_output.cpp`), which would cap the stream
at 60 fps. So the requested rate is added as a custom mode through
`kde_output_management_v2` (**v18+**, `set_custom_modes`) and then selected.
That is necessarily **two** commits — `apply()` may only be called once per
configuration object, and a mode cannot be made current until the compositor has
generated it.

Files:

- `src/platform/linux/kwin_virtual_display.{h,cpp}` — custom-mode application.
- `src/platform/linux/kwingrab.cpp` — `start_virtual()` and the `kwin_t` wiring.
- `tools/kwin_virtual_display_probe.cpp` — standalone verifier; shares the
  implementation via `KWIN_VDISPLAY_STANDALONE`, which swaps Boost logging for stderr.

Enabled by `linux_virtual_display`, by selecting output `virtual`, or per-app.

**Two traps found the hard way:**

- **libwayland aborts the process on a NULL listener slot.** `kde_output_device_v2`
  has 40 events; every one needs a handler. The no-op stubs in
  `kwin_virtual_display.cpp` were generated mechanically from the protocol XML so
  signatures cannot drift. If you bump the protocol submodule, regenerate them.
  The compiler caught one such slot for me (`kde_output_configuration_v2::failure_reason`,
  a `-Wmissing-field-initializers` warning) — that would have been a runtime abort.
  Build these files with `-Wmissing-field-initializers` on.
- **KWin recently moved output devices behind `kde_output_device_registry_v2`**
  (binding it below version 21 is a protocol error). Older Plasma advertises one
  `kde_output_device_v2` global per output instead. Both paths are implemented;
  keep both until you know the minimum Plasma you support.
- `kde_output_device_v2` gained a `release` destructor request in v21 — plain
  proxy destroy leaks the compositor-side resource.

### Frame limiter — 0010, 0011

Capping the game to the stream rate is what makes streaming look smooth: an
uncapped game renders whenever it can, the encoder samples at a fixed cadence,
and the mismatch is judder even when average FPS looks right.

**There is no single RTSS on Linux.** Researched, not assumed:

- **MangoHud's control socket cannot do this.** `src/control.cpp` implements
  exactly three commands — `hud`, `logging`, `fcat`. There is no runtime
  `fps_limit`, and `reload_cfg` is a *keybind*, not a socket command. MangoHud is
  therefore launch-time only.
- **gamescope can.** `steamcompmgr.cpp` watches the `GAMESCOPE_FPS_LIMIT`
  property on its own X root window and applies whatever is written there. This
  is a genuine RTSS-style control channel.

| Provider | Applied via | Re-limitable mid-stream |
| --- | --- | --- |
| gamescope | `GAMESCOPE_FPS_LIMIT` X root property | **yes** |
| MangoHud | `MANGOHUD=1` + `MANGOHUD_CONFIG=fps_limit=…` (+ `LD_PRELOAD` for OpenGL) | no |
| libstrangle | `FPS=…` + `LD_PRELOAD` | no |

The rate is derived with the **exact precedence the Windows limiter uses**
(`src/platform/windows/frame_limiter.cpp`), so a stream is capped identically on
either platform: client display-mode rate, then raw stream cadence, then plain
requested FPS; a lossless-scaling limit overrides those, and an explicit config
override beats everything. Fractional rates survive where the provider parses
them (MangoHud takes floats, so 59.94 stays 59.94).

Naming a provider explicitly uses **only** that one — if it is missing, no limit
is applied rather than silently substituting a different limiter.

Split three ways so it is testable without a GPU:

- `src/frame_limiter_policy.{h,cpp}` — pure decision logic, no syscalls.
- `src/platform/linux/frame_limiter_detect.{h,cpp}` — provider lookup; takes its
  search directories as arguments so tests point it at a scratch tree.
- `src/platform/linux/frame_limiter_gamescope.{h,cpp}` — the X property, isolated
  so it can be driven against a bare X server.
- `src/platform/linux/frame_limiter.{h,cpp}` — orchestration, config, logging.

Integration points: `src/process.cpp` (launch environment) and `src/stream.cpp`
(gamescope runtime apply/clear, in the existing `#else` branches).

**Gotcha:** an X server drops its atoms when the **last** client disconnects.
The gamescope test passes because the fixture holds a connection open — which
correctly mirrors gamescope still running. A benchmark that interned the atom
and closed its connection saw it vanish. Do not "fix" that by weakening the test.

A gamescope limit set by the user is read before the stream overwrites it and
restored afterwards; only a limit this process applied is ever undone.

---

## Unfinished work: arrival-based capture pacing

**Commit 0012 adds the policy but deliberately does not wire it in.** This is the
one real latency optimisation found, and the one that cannot be validated here.

### The problem

Every Linux capture backend runs the same loop:

```cpp
auto now = steady_clock::now();
while (next_frame < now) next_frame += delay;
if (next_frame > now) std::this_thread::sleep_until(next_frame);
snapshot(...);   // takes the newest frame; returns at once if one is ready
```

For **poll-based** backends (`x11grab`, `kmsgrab`, `wlgrab`, `cuda`) this is
correct and free: they grab the screen at the instant they wake, so the frame is
fresh.

For **push-based** PipeWire (`pipewire.cpp:836-844`, used by `kwingrab` and
`portalgrab` — i.e. the KDE Wayland path) it is not. The compositor delivers
frames on its own schedule and `on_process` marks them ready immediately. The
grid only decides *when we look*. A frame that arrives just after a grid point
sits finished in memory until the next one, and **when source and target run at
the same rate the phase offset is constant for the whole session** — so this is
a fixed latency addition of anywhere from ~0 to one full frame interval
(16.7 ms at 60 fps, 8.3 ms at 120).

### The proposed fix

`src/platform/linux/capture_pacing.{h,cpp}` (already written, compiles, unused):
forward a frame as soon as it arrives, and hold the rate down by **dropping**
early frames instead of **delaying** kept ones.

```cpp
if (!started)                       { started = true; next_deadline = arrival + interval; return emit; }
if (arrival + tolerance < next_deadline)                                    return drop;
next_deadline += interval;
if (next_deadline <= arrival)         next_deadline = arrival + interval;   // fell behind: re-anchor
return emit;
```

Deadlines advance by exactly one interval so the cadence stays anchored rather
than drifting with jitter. Tolerance defaults to a quarter interval: without it,
a fraction of a millisecond of jitter drops a frame and leaves a two-interval
gap. Worked examples — source 120 / target 60 alternates cleanly; source 144 /
target 60 is inherently uneven whatever you do, short of buffering (which
re-adds the latency you just removed).

### Remaining steps

1. Add `capture_pacing.{h,cpp}` to `PLATFORM_TARGET_FILES` in
   `cmake/compile_definitions/linux.cmake`.
2. In `pipewire.cpp::capture()`, when mode is `arrival`: drop the `sleep_until`,
   call `snapshot()` directly (it already blocks on the frame condition
   variable), and gate `push_captured_image_cb` on `pacer.should_emit(now)`.
   Keep the existing grid path for `interval` mode.
3. Config key `linux_capture_pacing` = `arrival` (default) | `interval`.
   **This is not optional plumbing:** a `bool_f`/`string_f` key that is not also
   added to `docs/configuration.md` (as an `### key` H3) *and* to the `config`
   object in `src_assets/common/assets/web/public/assets/locale/en.json` fails
   `test_component_resource_config_catalog`. That test caught exactly this
   omission for `linux_virtual_display`; see commit 0009 for the shape.
4. Unit-test the pacer: source == target (expect every frame kept, no drops),
   2x, 2.4x, source slower than target, jitter, and the re-anchor path.
5. Simulate both policies over a range of source/target combinations and report
   **two** metrics — added latency *and* emitted-interval standard deviation.
   There is a real tradeoff: the grid emits on a perfectly regular cadence at the
   cost of latency, arrival pacing minimises latency but passes source jitter
   through. Do not report only the metric that flatters the change.
6. Validate on hardware. This is a core loop; a mistake here causes the judder
   the change is meant to remove.

---

## Optimisations considered and rejected

Recorded so they are not re-litigated.

- **ffmpeg NVENC options** (`src/video.cpp`, the `#elif !defined(__APPLE__)`
  `nvenc` encoder) are already tuned: `delay=0`, `zerolatency=1`, `surfaces=1`,
  `tune=ULTRA_LOW_LATENCY`, `rc=CBR`, `forced-idr=1`. Nothing to gain by
  churning them. `REF_FRAMES_INVALIDATION` is correctly absent — ffmpeg's nvenc
  does not expose it (the Windows *native* NVENC path has it).
- **Socket QoS / DSCP** already exists on Linux — `enable_socket_qos()` in
  `src/platform/linux/misc.cpp`, with `IP_TOS`/`IPV6_TCLASS` and values chosen
  to match Windows.
- **Thread priority** already exists and is already applied to the capture,
  encode, stream and audio threads (`adjust_thread_priority`, RTKit with a
  `setpriority` fallback; on Linux `PRIO_PROCESS` with `who=0` correctly means
  the calling thread). Moving to `SCHED_RR` was not attempted — a runaway RT
  thread can wedge a machine, and RTKit already brokers this.
- **The PipeWire frame path** is already good: `on_process` drains to the newest
  buffer and returns the older ones, dmabuf is a zero-copy fast path, the memcpy
  fallback happens outside the lock with only a pointer swap under it. dmabuf is
  correctly gated off for hybrid GPUs, where buffers come from the iGPU and
  cannot be imported into CUDA.

Not investigated, and the most likely place for further wins:

- The **encode → packetise → send** path (`src/stream.cpp`, `src/video.cpp`) —
  batching (`sendmmsg`/UDP GSO), buffer sizing, and whether encoder output is
  drained promptly.
- **Vulkan video encode** (`src/platform/linux/vulkan_encode.cpp`) as an
  alternative to CUDA/NVENC on newer drivers.

---

## Files added

```
HANDOVER.md                                       this file
docs/linux_virtual_display.md                     virtual display: design, setup, troubleshooting
docs/linux_frame_limiter.md                       frame limiter: providers, config, measurement
src/frame_limiter_policy.{h,cpp}                  pure limiter decision logic
src/platform/linux/frame_limiter.{h,cpp}          limiter orchestration
src/platform/linux/frame_limiter_detect.{h,cpp}   provider lookup (injectable search paths)
src/platform/linux/frame_limiter_gamescope.{h,cpp} gamescope X property
src/platform/linux/kwin_virtual_display.{h,cpp}   KWin custom-mode application
src/platform/linux/capture_pacing.{h,cpp}         arrival pacing policy (NOT wired in)
tools/kwin_virtual_display_probe.cpp              standalone virtual display verifier
tools/benchmark_frame_limiter.sh                  frame-time spread benchmark
tests/unit/test_frame_limiter_policy.cpp          31 tests
tests/unit/test_frame_limiter_detect.cpp          13 tests
tests/unit/test_frame_limiter_gamescope.cpp       6 tests, needs an X display
```

## Conventions worth matching

- Pure logic goes in `src/*_policy.cpp` and gets a `sunshine_register_component`
  test in `tests/CMakeLists.txt` that compiles only the sources it needs. Follow
  that split — it is why the limiter is testable without a GPU.
- Enums use an `_e` suffix (`mem_type_e`, `provider_e`).
- Comments explain *why*. The codebase does not narrate what the code does.
