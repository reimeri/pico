# File completion: architecture and performance

The builtin `@` completer uses `app/file_index.c`, a lazy service per workspace.
It indexes paths only, with one contiguous string arena and two pointer orderings
(path and basename). Prefix queries use binary search; substring matching remains
a cancellable linear scan on a search worker. A separate discovery worker handles
enumeration, persisted caches, watches and reconciliation, so refreshing a large
project does not stop searches against the pinned generation.

The main thread copies only a request and at most 24 paths through short locked
mailboxes. It never walks, sorts, loads a cache, spawns an enumerator, or searches
the corpus. Workers have no host/extension callback pointers. Close is nonblocking;
reference-counted core state survives until both workers finish and children are
reaped. Snapshot construction is budgeted at 512 MiB (including sort scratch),
with visible failure rather than truncated results. This is a per-snapshot bound,
not a whole-process RSS guarantee: latest/pinned/scoped generations and a replacement
can coexist. The reported `index_bytes` is the latest snapshot's arena and retained
pointer allocations; peak RSS also includes old generations, construction scratch,
worker stacks, allocator retention and the runner itself.

See [the completion contract](extend/completers.md#builtin-file-mentions) for ignore
rules, freshness, explicit scopes and cache semantics.

## Reproduce

```sh
nix develop --command sh -c '
  cmake -S app --preset release &&
  cmake --build app/build/release --target pico_file_index_bench
'
nix develop --command python3 app/tests/file_index_profile.py \
  --runner app/build/release/pico_file_index_bench \
  --output /tmp/pico-file-index-git.json
nix develop --command python3 app/tests/file_index_profile.py \
  --runner app/build/release/pico_file_index_bench --backend rg \
  --output /tmp/pico-file-index-rg.json
```

The driver makes isolated temporary projects with **real filesystem entries**:
10k, 100k and 1M empty files, 1,000 files per source directory and a Markdown file
in a separate notes directory. Git runs use an initialized repository with
untracked, non-ignored files; rg runs have no repository. No existing project is
modified. Each project gets a private cache and a cold-index run followed by a
persisted-cache run. The temporary tree is deleted afterward.

“Cold” means **no Pico cache**, not a cold OS page cache: files were just created,
and no privileged page-cache dropping is performed. Query latencies use 31 samples
per query (median and p95), each starting a new token, with 100 μs readiness polling.
The runner also measures the search worker, CPU spent polling/querying on the
calling thread, maximum individual Query-call wall time per sample, a burst of
1,000 superseding edits, refresh duration, and peak self RSS (`getrusage`; child
Git/rg memory is not included). End-to-end popup latency also depends on the
application's frame/pump cadence; these are engine measurements, not screenshots.
Timing measurements are descriptive and deliberately not unit-test thresholds.

## Measurements

Measured on an AMD Ryzen 7 5800X3D (8 cores / 16 threads), Linux x86-64,
GCC 16.2, CMake Release (`-O3 -DNDEBUG`) through `nix develop`. Git and rg suites
ran sequentially, under normal host load; regression tests could run concurrently.
[Raw measurements](benchmarks/file-index.json) retain all query samples' summary
statistics. Startup timings have one cold/cached run per corpus; they are not
startup percentiles. `getrusage` RSS is a process high-water mark and can include
inherited pre-exec high-water, so small-corpus RSS has a runner/launcher floor.

### Git-backed discovery

| Files | Cold first result | Cached first result | Basename-prefix p95 | No-match p95 | Latest index | Peak self RSS* |
|---:|---:|---:|---:|---:|---:|---:|
| 10,000 | 31.0 ms | 5.7 ms | 0.164 ms | 0.9 ms | 0.49 MiB | 19.1 MiB |
| 100,000 | 299.3 ms | 61.6 ms | 0.163 ms | 4.7 ms | 4.86 MiB | 19.1 MiB |
| 1,000,000 | 3026.7 ms | 732.2 ms | 0.162 ms | 45.1 ms | 48.64 MiB | 124.8 MiB |


### Non-Git / ripgrep discovery

| Files | Cold first result | Cached first result | Basename-prefix p95 | No-match p95 | Latest index | Peak self RSS* |
|---:|---:|---:|---:|---:|---:|---:|
| 10,000 | 53.8 ms | 5.2 ms | 0.162 ms | 0.6 ms | 0.49 MiB | 18.9 MiB |
| 100,000 | 500.9 ms | 60.8 ms | 0.166 ms | 6.6 ms | 4.86 MiB | 21.8 MiB |
| 1,000,000 | 5464.7 ms | 782.8 ms | 0.166 ms | 80.2 ms | 48.64 MiB | 195.1 MiB |

*Peak includes cold/cached runs and a forced refresh with a pinned generation;
not just the steady-state path arena. It is not a whole-Pico process measurement.

### Interpretation

- The notes file was discoverable in every corpus, including after 999,999 source
  files. All 1M files were indexed; only displayed suggestions were bounded.
- Basename-prefix work on the worker was about **0.007 ms** at 1M files. The
  roughly **0.16 ms** observed request latency includes readiness polling.
- Individual main-thread Query-call p95s stayed at or below **0.013 ms** across
  these measured query classes. The “main CPU” field is cumulative over all polls
  for one request, not the duration of a single UI-thread stall.
- At 1M files, fresh-token explicit path-prefix requests took roughly **2–3 ms**,
  including an ignore-policy subprocess check. Arbitrary substring/no-match
  requests still scale linearly: about **42–80 ms p95** in these runs, on the
  worker. A burst of 1,000 superseded edits returned the latest requested file
  in about **42–53 ms**, rather than doing 1,000 full searches.
- Cache startup is faster, not instantaneous: the path/name orderings still need
  reconstruction. At 1M files the first cached suggestion took **0.73–0.78 s**,
  versus **3.03 s** Git or **5.46 s** rg without a Pico cache. The UI remains usable
  with loading feedback during that work.
- Reconciliation remains real work: 1M-file forced refreshes took roughly **3 s**
  with Git and **6 s** with rg. The separate discovery worker keeps query service
  available, but does not make background CPU/I/O free. Current periodic scans
  trade that cost for recovery from missed watches/global-ignore changes.

These results support binary prefix lookup plus cancellable worker substring
scans for this corpus. Much longer paths, many more directories, slow/network
storage, ignored-directory scopes, and multi-million-file projects need separate
measurement; this is not a universal latency guarantee. Specialized substring
indexes or a disk-backed corpus would be follow-up optimizations if those
measurements justify their memory and maintenance cost.

## Correctness verification

`pico_file_index_tests` covers discovery beyond the old cap, ignored/tracked files,
explicit ignored scopes, stale token generations, query coalescing, create/rename
refresh, cached results during a deliberately blocked reconciliation, corrupt
caches, symlink/nested-project boundaries, deep native trees, own-cache exclusion,
missing-tool warnings, cancellation and child reaping. Existing file/composer
and full host/workspace tests cover quoted attachment acceptance, nonselectable
loading feedback and shell layout stability. The index suite was also run with
AddressSanitizer and UndefinedBehaviorSanitizer (leak checking disabled for the
process-isolated test harness).
