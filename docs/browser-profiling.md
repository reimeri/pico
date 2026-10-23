# Browser resource profiling

Slice 5 adds a repeatable **descriptive benchmark**, not performance gates or a
new page suspension/eviction policy. The first increment also hardens failure
classification and explicit close/reopen recovery; see [browser contracts](extend/browser.md#failures-and-explicit-recovery).

The recorded baseline below is **historical CPU-pixel transport**, not a
measurement of the GPU-only renderer. Current `frame_bytes` means metadata bytes;
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
  headers and ancillary descriptors/fences. It is not pixel bandwidth, GPU copies,
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
For an engine-only native-driver comparison, omit `--pico`; retain the reported WebGL renderer/environment. It is still off-screen
CEF, not native Pico/Wayland presentation.

## Recorded baseline

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

### Interpretation and next measurements

- Full-frame transport is measurable: roughly **106 MiB/s per animated visible
  1280×720 surface at 30 fps**, and about **318–320 MiB/s for three**. The real UI
  presents one selected browser, not three at once. A realistic next investigation
  is damage/copy profiling on actual dev apps before designing dirty-region
  transport or GPU sharing; this baseline alone does not justify either change.
- Hidden rendering/texture suppression works for these fixtures without stopping
  page progress. Low synthetic hidden CPU does **not** bound arbitrary JavaScript,
  dev-server traffic or memory. Do not infer tab suspension or idle eviction.
- Summed Chromium RSS is large even for static pages. Measure PSS/cgroup/driver
  memory and longer session churn before calling this a leak or choosing limits.
- Native-driver WebGL works in the helper; forced-software WebGL does not on this
  environment. Portable/software-rendering WebGL availability needs separate
  platform investigation, not automatic unsafe flags.
- Profile Release builds, larger logical viewports/observations, real dev apps,
  native Wayland/Pico GPU presentation, concurrent tool bursts and long-soak
  cleanup next. Live-model acceptance, IME/cursor/clipboard/HiDPI, packaged archive/
  AppImage startup, helper-idle shutdown, and cross-process sharing remain deferred.
