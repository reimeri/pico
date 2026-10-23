# Browser resource profiling

This is a repeatable **descriptive benchmark**, not performance gates or a page
suspension/eviction policy. See [browser contracts](extend/browser.md#failures-and-explicit-recovery)
for failure classification and explicit close/reopen recovery.

The [GPU-only baseline](#gpu-only-baseline--2026-10-23) records three full hardware-backed
runs. The [historical CPU-transport baseline](#historical-cpu-transport-baseline) is
preserved separately, not treated as an equivalent graphics configuration.
Current `frame_bytes` means metadata bytes;
conversion/upload counters have been removed in favor of import counters. New
runs require native display/DMA-BUF/fence-capable drivers; software Xvfb is no
longer a supported browser presentation configuration.

## Procedure

Use the pinned Nix development tools and build the current core/helper:

```sh
nix develop -c cmake --build app/build/debug --target pico pico_host_workspace_tests

# Engine-only matrix: native display/GPU required, no Pico window.
nix develop -c timeout -k 5s 300s python3 app/tests/browser_profile.py \
  app/build/debug/browser/runtime/pico_browser_helper \
  --seconds 10 --warmup 3 --output /tmp/browser-engine-new.json

# Add real Pico import measurements on an ISOLATED hardware-backed X11 display.
nix develop .#browser-tests -c env -u WAYLAND_DISPLAY \
  timeout -k 5s 360s python3 app/tests/browser_profile.py \
  app/build/debug/browser/runtime/pico_browser_helper --pico app/build/debug/pico \
  --seconds 10 --warmup 3 --output /tmp/browser-gpu-new.json
```

Output paths must be new files. Reports use mode `0600`, contain environment,
viewport, workload, visibility, process counts, before/after page progress, and
per-trial metrics. Failed/interrupted runs retain completed trials and an explicit
failure record with a nonzero exit status. Unavailable WebGL is recorded as
`status: "unavailable"`, with the page's error, **not** a successful WebGL timing.
Functional watchdogs bound readiness/cleanup; they are not speed assertions.

The default matrix uses 1280×720 and one or three independent session profiles:

- **static:** a static page plus a one-second activity heartbeat;
- **animated:** a changing full-size Canvas2D surface with 160 moving rectangles;
- **webgl:** a changing full-size fragment shader, with compilation/link checks,
  initial pixel readback, reported renderer, and context-loss reporting.

Canvas drawing-buffer dimensions are explicitly set and checked against the
requested viewport. Activity reports must be fresh; animated/WebGL draw counts
must advance, including when hidden. No FPS threshold is asserted.

Helper trials cover all-visible, all-hidden, and (for three sessions) one-visible
plus two-hidden. **Helper-visible is not actually presented in Pico**: these
trials receive GPU-frame metadata and return acquire fences without sampling images.
Host trials show one real Pico browser, then measure either expanded or collapsed
presentation. Static host trials verify actual website screenshot pixels;
animated/WebGL trials require imports after fixture readiness. Concurrent engine
work is measured separately, not described as three simultaneously visible UI
panes. Host tests require an isolated hardware-backed X11 display. **Do not
run `--pico` against your desktop's `DISPLAY`: host trials use X11 mouse/keyboard
input and change focus.** The script checks for X11, but cannot establish that an
arbitrary display is isolated. Native Wayland input/IME, HiDPI and GPU acceptance
remain manual.

## Metrics and limits

- CPU is sampled `/proc` user+system time for the live process tree, including
  CEF renderer, GPU, zygote and network processes (plus Pico in host trials).
  `100%` means one logical CPU. PID/start-time keys avoid PID-reuse confusion.
  Short-lived children/final CPU work between samples can be missed; process
  appearances/disappearances are reported. This is **sampled live-process CPU**,
  not cgroup accounting or a claim that exited-child CPU is fully retained.
- RSS is the sum of sampled process RSS. Shared pages can be counted repeatedly;
  this is neither PSS nor unique physical memory, and excludes graphics-driver
  allocations not charged as process RSS. Mean and peak apply only to the sampled
  measurement window, not startup or lifetime peaks.
- Helper `frame_bytes` counts GPU metadata payload received, excluding event
  headers, ancillary descriptors/fences, and command-channel release acknowledgments.
  It is not total socket traffic, pixel bandwidth, GPU copies,
  DMA-BUF allocation size, or PNG observation cost.
- Host counters are enabled by `PICO_BROWSER_PROFILE` in the isolated process.
  They include GPU frames/metadata, discarded frames, imports, and import wall time.
  Import time is CPU-side API
  wall time, **not GPU completion**. Counter deltas use their own exact
  `host_counter_seconds` span; process CPU/RSS use the separate `seconds` window.
  The queue high-water value is lifetime-wide, not a delta. Opt-in reporting adds
  roughly one idle host wakeup per second and excludes page contents/URLs.
- Warmup, readiness checks, first readback and screenshots are outside the sampled
  CPU interval. Page progress verification can wait for the next heartbeat after
  that interval. Local loopback traffic and benchmark sampling have overhead.
- The fixture does not saturate the input queue or benchmark large observation
  encoding. Existing observation/action integration tests cover those contracts;
  this baseline is not a fairness, latency, leak, stress or long-soak certification.
- Chromium sandboxing and GPU blocklists remain enabled. Do not force an
  unsandboxed or blocklist-bypassed “passing” run. Record unavailable backends.

Use `--width`, `--height`, `--sessions`, and `--workloads` for targeted repeats.
`--artifacts-dir /absolute/path/to/new-directory` retains private per-trial CEF
profiles/logs and, for host trials, `pico.log`, `metrics.jsonl`, and validation
screenshots. Without it, temporary trial directories are removed. The directory
must not already exist. Reports record binary and fixture SHA-256 hashes; retain
the source revision, harness changes, build type, and display/driver provenance
alongside them. Synthetic key events are held across input snapshots during setup,
outside the measured interval, rather than relying on very short XTest presses.
The artifact-path/lifetime harness check needs no GPU:
`nix develop .#browser-tests -c python3 -B app/tests/browser_profile_test.py`.
For an engine-only native-driver comparison, omit `--pico`; retain the reported WebGL renderer/environment. It is still off-screen
CEF, not native Pico/Wayland presentation.

## Reproducing an isolated GPU-backed run

The optional `browser-tests` Nix shell includes Weston, Xwayland, `glxinfo`, and
`xdotool`. Weston can provide an **offscreen but hardware-rendered display** without
connecting benchmark input to the desktop. This is not Chromium's unsupported
headless Ozone backend: CEF still uses native X11 through the private Xwayland server.
The compositor's headless backend is a different component.

Run in a dedicated shell; the cleanup trap stops only the compositor started here:

```sh
nix develop .#browser-tests
work=$(mktemp -d /tmp/pico-gpu-baseline-XXXXXX)
mkdir -m 700 "$work/runtime"
cat > "$work/weston.ini" <<'EOF'
[core]
idle-time=0
xwayland=true
[shell]
panel-position=none
background-color=0xff202020
locking=false
EOF
unset DISPLAY WAYLAND_DISPLAY WAYLAND_SOCKET PICO_BROWSER_OZONE_PLATFORM
unset LIBGL_ALWAYS_SOFTWARE GALLIUM_DRIVER
export XDG_RUNTIME_DIR="$work/runtime" XDG_SESSION_TYPE=x11
weston --backend=headless --renderer=gl --fake-seat \
  --width=1600 --height=1200 --refresh-rate=60000 --socket=pico-profile-wayland \
  --config="$work/weston.ini" --log="$work/weston.log" &
compositor=$!
trap 'kill "$compositor" 2>/dev/null; wait "$compositor" 2>/dev/null' EXIT
# Wait for readiness, not a guessed compositor startup delay.
for attempt in $(seq 1 200); do
  kill -0 "$compositor" || exit 1
  DISPLAY=$(sed -n 's/.*xserver listening on display \([^ ]*\).*/\1/p' "$work/weston.log" | tail -1)
  test -z "$DISPLAY" || break
  sleep .1
done
test -n "$DISPLAY" || exit 1
export DISPLAY
glxinfo -B | tee "$work/glxinfo.txt"
# Require hardware acceleration, not llvmpipe/software, before accepting results.
rg 'Accelerated: yes' "$work/glxinfo.txt" || exit 1
timeout -k 5s 60s app/build/debug/pico_browser_gpu_tests \
  "$PWD/app/build/debug/browser/runtime/pico_browser_helper" || exit 1
for repetition in 1 2 3; do
  timeout -k 5s 600s python3 -B app/tests/browser_profile.py \
    app/build/debug/browser/runtime/pico_browser_helper --pico app/build/debug/pico \
    --seconds 10 --warmup 3 --sessions 1 3 --workloads static animated webgl \
    --output "$work/run-$repetition.json" \
    --artifacts-dir "$work/run-$repetition-artifacts" || exit 1
done
```

Build `pico`, `pico_browser_helper`, and `pico_browser_gpu_tests` beforehand. Keep
compositor configuration/version, GL renderer/version, source/build identity, and
raw reports with the results. A driver lacking the required DMA-BUF/fence support
must be reported as unavailable; never substitute software rendering silently.

Runs are sequential: 21 scenarios per repetition (15 helper, 6 real-Pico), 63 total.
Report medians and min–max ranges, not confidence intervals from three samples.
Normalize helper traffic by each trial's `seconds` and host counters by its
`host_counter_seconds`. Workload heartbeat deltas establish progress but do not
measure exact FPS during either interval. Host imports/s are not display refresh
rate or end-to-end presentation latency.

Input/display isolation is **not exclusive GPU/CPU ownership**. The live desktop
and other applications share the machine. These measurements exclude external
Weston/Xwayland/Python process costs and do not control GPU clocks or thermal state.
They are reproducible descriptive resource samples, not hardware microbenchmarks.

## GPU-only baseline — 2026-10-23

The new baseline is retained in
[`browser-profile-gpu-baseline.json`](browser-profile-gpu-baseline.json), including
all **63 raw trials**, medians/ranges for the 21 scenarios, binary/fixture/harness
hashes, the measured harness patch, and environment/compositor configuration.
All three runs completed; all trials were measured, with no unavailable workloads.
The old [`browser-profile-baseline.json`](browser-profile-baseline.json) is preserved.

### Setup and validity

- Renderer revision: `5bda42f139cffd1150d79a7a9b380302b1ccbdf9`, **Debug**;
  CEF 151.3.16 / Chromium 151.0.7922.109, Raylib 5.5. Benchmark-only input/readiness
  and artifact/reporting changes are recorded with the results; production
  rendering was not changed for measurement.
- AMD Ryzen 7 5800X3D, 16 logical CPUs; AMD Radeon RX 7900 GRE; Mesa 26.2.5;
  Linux 7.3.0. Hardware acceleration and the cross-process GPU pixel/fence test
  passed on the isolated display before profiling.
- Weston 16.0.0, headless **GL** renderer, Xwayland 24.1.13, 1600×1200 at 60 Hz,
  scale 1. Pico uses X11/EGL and CEF uses native X11 Ozone. This is neither
  software Xvfb nor a direct native-Wayland presentation benchmark.
- 1280×720 logical browser viewports; **3-second warmup, 10-second samples,
  three sequential repetitions** per scenario. Full runs occurred between
  16:11:31 and 16:28:52 UTC (machine clock). Each trial starts fresh processes/profiles.
- Static host trials checked actual page pixels before measurement. Animated and
  WebGL workloads progressed, including while hidden. Every WebGL trial reported
  an initial readback pixel and an accelerated renderer:
  `ANGLE (AMD, AMD Radeon RX 7900 GRE (radeonsi navi31 ACO DRM 3.64 7.3.0), OpenGL 4.6)`.
- One initial smoke run was discarded after missed short synthetic key presses
  prevented navigation. Longer input events fixed setup **before** all 63 measured
  trials; no measured run was discarded or selected by its performance result.

### Real Pico presentation

Values are **median (min–max)** across three runs where a range is shown. CPU is a
percentage of **one logical core**, including Pico and its Chromium process tree.
RSS is the median of each trial's mean **summed process RSS**, not unique RAM or GPU
memory. Frame metadata excludes headers, descriptors/fences, and release-ack commands.

| Workload | Visibility | CPU % (range) | Mean summed RSS MiB (range) | Imports/s | Frame metadata KiB/s | Import API ms/s (range) |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| static | visible | 0.8 (0.8–0.9) | 1040.0 (1034.7–1042.8) | 0.0 | 0.00 | 0.00 (0.00–0.00) |
| static | hidden | 0.8 (0.8–1.0) | 1041.7 (1035.6–1043.0) | 0.0 | 0.00 | 0.00 (0.00–0.00) |
| animated | visible | 9.8 (9.7–16.6) | 1062.8 (1061.2–1067.9) | 30.0 | 2.11 | 2.35 (2.29–3.70) |
| animated | hidden | 1.1 (1.1–1.2) | 1053.3 (1052.2–1057.8) | 0.0 | 0.00 | 0.00 (0.00–0.00) |
| webgl | visible | 9.1 (8.9–9.1) | 1052.3 (1048.0–1053.5) | 30.0 | 2.11 | 2.33 (2.27–2.35) |
| webgl | hidden | 1.3 (1.0–1.3) | 1047.9 (1047.9–1049.8) | 0.0 | 0.00 | 0.00 (0.00–0.00) |

The animated-visible CPU range is intentionally retained: repetition 1 measured
16.6%, while repetitions 2 and 3 measured about 9.8% and 9.7%. That first trial also
observed two process arrivals and two departures; the other 62 trials recorded no
process churn during their sampled CPU windows. The cause is not established, and
sampled live-process accounting can miss work by short-lived processes. Do not
replace this range with a claim of a stable 9.8% ceiling.

### Helper-only concurrency matrix

These trials exercise Chromium rendering, owned-buffer copies/export, and descriptor
transport/release, but **do not sample the images in Pico**. Frame rates are aggregate
across the visible helper pages; mixed means one visible and two hidden.

| Workload | Sessions / visibility | CPU % (range) | Mean summed RSS MiB (range) | GPU frames/s | Frame metadata KiB/s |
| --- | --- | ---: | ---: | ---: | ---: |
| static | 1 / visible | 0.5 (0.4–0.6) | 922.3 (922.1–924.0) | 0.0 | 0.00 |
| static | 1 / hidden | 0.4 (0.3–0.5) | 917.3 (913.5–918.2) | 0.0 | 0.00 |
| static | 3 / visible | 1.3 (1.2–1.4) | 1160.9 (1157.4–1164.9) | 0.0 | 0.00 |
| static | 3 / hidden | 1.2 (1.1–1.3) | 1156.0 (1155.5–1157.0) | 0.0 | 0.00 |
| static | 3 / mixed | 1.2 (1.1–1.3) | 1162.6 (1159.7–1163.6) | 0.0 | 0.00 |
| animated | 1 / visible | 6.4 (6.0–6.4) | 938.9 (938.8–942.4) | 30.1 | 2.12 |
| animated | 1 / hidden | 0.8 (0.7–0.8) | 923.5 (921.3–927.7) | 0.0 | 0.00 |
| animated | 3 / visible | 18.2 (18.0–18.7) | 1208.5 (1205.7–1210.2) | 90.2 | 6.34 |
| animated | 3 / hidden | 1.7 (1.6–1.7) | 1169.9 (1167.9–1175.7) | 0.0 | 0.00 |
| animated | 3 / mixed | 7.9 (7.5–8.1) | 1187.0 (1184.5–1191.0) | 30.1 | 2.12 |
| webgl | 1 / visible | 5.8 (5.5–5.9) | 928.4 (926.6–930.5) | 30.1 | 2.11 |
| webgl | 1 / hidden | 0.7 (0.7–0.8) | 919.1 (912.2–921.0) | 0.0 | 0.00 |
| webgl | 3 / visible | 15.9 (15.6–18.4) | 1177.6 (1177.4–1178.5) | 90.2 | 6.34 |
| webgl | 3 / hidden | 1.6 (1.6–2.2) | 1165.4 (1162.4–1169.5) | 0.0 | 0.00 |
| webgl | 3 / mixed | 6.7 (6.6–6.8) | 1171.8 (1171.0–1175.1) | 30.0 | 2.11 |

### What this establishes—and what it does not

- Visible animation/WebGL transported about **30 GPU frames/s per page** and
  **2.1 KiB/s of frame metadata**; three helper-visible pages produced about
  90 frames/s and 6.3 KiB/s. The payload is 72 bytes of metadata per frame, not
  continuous pixel bytes. This is **not total bidirectional IPC bandwidth**:
  release acknowledgments use the command channel and are excluded.
- Settled static pages sent no continuing frames in the samples. Every hidden
  trial sent zero presentation frames, and hidden host trials performed zero
  imports, while page activity continued. No observation PNGs were requested or
  transferred during the host sample windows; screenshot latency is not measured.
- Visible host import API work was about **2.3 ms per second** at 30 imports/s
  (animated range 2.29–3.70 ms/s). This is host API wall time—not GPU execution,
  Chromium's callback completion wait, or end-to-end presentation latency.
- Summed RSS remains approximately 1.0 GiB for the host trials. Sharing GPU buffers
  does not imply reducing Chromium process memory, and this metric excludes GPU
  allocation accounting while double-counting shared resident pages.
- Input isolation did not isolate CPU/GPU resources from the desktop. External
  compositor/Xwayland costs are excluded. Three short repetitions establish a
  descriptive baseline, not a tail-latency, power, long-soak, or leak result.

### Historical comparison limits

The closest historical reference is the **native-driver helper-only WebGL** run:
matching GPU, logical viewport, workload, session counts, and Debug build type.
Even this is not a controlled transport-only comparison: the historical sample
was a single three-second interval after two seconds warmup, used headless CEF
Ozone, and reported ANGLE OpenGL ES 3.2 rather than the current OpenGL 4.6 path.

The following table preserves those distinctions. Old payload is raw BGRA;
new payload is frame metadata. Neither column includes all socket overhead.

| Sessions / visibility | Historical CPU % (single sample) | GPU-only CPU % (median, range) | Historical raw frame MiB/s | GPU-only frame metadata KiB/s |
| --- | ---: | ---: | ---: | ---: |
| 1 / visible | 8.3 | 5.8 (5.5–5.9) | 106.9 | 2.11 |
| 1 / hidden | 1.0 | 0.7 (0.7–0.8) | 0.0 | 0.00 |
| 3 / visible | 26.1 | 15.9 (15.6–18.4) | 319.5 | 6.34 |
| 3 / hidden | 1.7 | 1.6 (1.6–2.2) | 0.0 | 0.00 |
| 3 / mixed | 9.9 | 6.7 (6.6–6.8) | 107.0 | 2.11 |

The older **host** run forced software rendering under Xvfb, so its 78.7% animated
CPU figure is not an appropriate denominator for a renderer speedup percentage.
The new numbers demonstrate a working GPU-backed resource baseline and the removal
of continuous full-frame CPU payloads; they do not isolate the cause of the CPU
change. Release builds, native-Wayland host profiling, other GPUs, realistic dev
apps, GPU memory/power/time, larger observations, and long-soak behavior remain
separate follow-up measurements.

## Historical CPU-transport baseline

These short runs used the Debug build, pinned CEF 151.3.16 / Chromium
151.0.7922.109, Linux x86-64 with 16 logical CPUs, a **2-second warmup** and a
**3-second sampled interval** per measured trial. They are initial observations,
not statistically stable performance estimates. The complete structured report,
including machine-reported clock/environment, fresh workload progress and exact
host-counter spans, is retained in [browser-profile-baseline.json](browser-profile-baseline.json).

### Forced-software X11 run

Pico used Xvfb at 1600×1200 and `LIBGL_ALWAYS_SOFTWARE=1`; logical browser surfaces
were 1280×720. CPU includes the entire target process tree. RSS here is the
**mean summed RSS**, and significantly double-counts shared Chromium pages.

| Path | Workload | Sessions / visibility | CPU, one core % | Mean RSS MiB | Raw IPC MiB/s |
| --- | --- | --- | ---: | ---: | ---: |
| CEF only | static | 1 / visible | 0.3 | 843.9 | 0.0 |
| CEF only | static | 1 / hidden | 0.7 | 834.6 | 0.0 |
| CEF only | static | 3 / visible | 0.7 | 1110.6 | 0.0 |
| CEF only | static | 3 / hidden | 1.0 | 1085.3 | 0.0 |
| CEF only | static | 3 / mixed | 1.3 | 1098.2 | 0.0 |
| Pico + CEF | static | 1 / visible | 6.6 | 1042.9 | 0.0 |
| Pico + CEF | static | 1 / hidden | 4.3 | 1035.7 | 0.0 |
| CEF only | animated | 1 / visible | 7.9 | 876.2 | 105.8 |
| CEF only | animated | 1 / hidden | 1.3 | 858.3 | 0.0 |
| CEF only | animated | 3 / visible | 33.7 | 1208.2 | 318.4 |
| CEF only | animated | 3 / hidden | 2.6 | 1177.3 | 0.0 |
| CEF only | animated | 3 / mixed | 11.2 | 1191.1 | 106.9 |
| Pico + CEF | animated | 1 / visible | 78.7 | 1057.0 | 105.5 |
| Pico + CEF | animated | 1 / hidden | 4.6 | 1051.2 | 0.0 |

In the animated visible Pico trial, host counters measured approximately
**12.4 ms/s BGRA conversion** and **4.8 ms/s texture API wall time**, with about
30 uploads/s. These are CPU wall times, not GPU timings; the substantially larger
whole-tree CPU figure is not explained by the measured conversion/upload calls
alone. Hidden trials had no display frames/uploads while page activity continued.
Static pages sent no continuous frames after settling. No measured trial reported
process disappearance/churn during its CPU interval.

**WebGL was unavailable in all forced-software trials**: Chromium reported WebGL1
blocklisted and page context creation failed. Those trials have no successful
WebGL resource measurement; no sandbox/GPU-blocklist bypass was used.

### Native-driver engine-only WebGL run

A separate run omitted the forced-software variable. CEF reported
`ANGLE (AMD, AMD Radeon RX 7900 GRE (radeonsi navi31 ACO DRM 3.64 7.3.0), OpenGL ES 3.2)`.
This verifies context creation, shader execution, initial readback and continued
page progress in the **off-screen helper**, not native Wayland presentation,
HiDPI correctness, GPU-memory usage or GPU-completion timing.

| Sessions / visibility | CPU, one core % | Mean RSS MiB | Raw IPC MiB/s |
| --- | ---: | ---: | ---: |
| 1 / visible | 8.3 | 877.2 | 106.9 |
| 1 / hidden | 1.0 | 868.1 | 0.0 |
| 3 / visible | 26.1 | 1154.5 | 319.5 |
| 3 / hidden | 1.7 | 1138.2 | 0.0 |
| 3 / mixed | 9.9 | 1149.4 | 107.0 |

### Historical interpretation

- Full-frame transport is measurable: roughly **106 MiB/s per animated visible
  1280×720 surface at 30 fps**, and about **318–320 MiB/s for three**. The real UI
  presents one selected browser, not three at once. These figures motivated
  investigating GPU sharing; they describe the old CPU-pixel transport, not the
  now-implemented DMA-BUF path.
- Hidden rendering/texture suppression works for these fixtures without stopping
  page progress. Low synthetic hidden CPU does **not** bound arbitrary JavaScript,
  dev-server traffic or memory. Do not infer tab suspension or idle eviction.
- Summed Chromium RSS is large even for static pages. Measure PSS/cgroup/driver
  memory and longer session churn before calling this a leak or choosing limits.
- Native-driver WebGL worked in the old helper; forced-software WebGL did not
  in that environment. Software presentation is no longer supported by the
  GPU-only renderer; do not bypass sandboxing or blocklists to make a trial pass.
- Profile Release builds, larger logical viewports/observations, real dev apps,
  native Wayland/Pico GPU presentation, concurrent tool bursts and long-soak
  cleanup next. Live-model acceptance, IME/cursor/clipboard/HiDPI, packaged archive/
  AppImage startup, helper-idle shutdown, and cross-process sharing remain deferred.
