# Web port architecture and status

For the chronological engineering story—from the first wasm32 compiler gate
through renderer replacement, correctness work, and public deployment—see
[Engineering TH08 Web](WEB_PORTING.md). This document is the current technical
contract and verification record.

## Outcome

TH08 Web is built from the reconstructed C++ game sources with Emscripten and
runs in a desktop browser. JavaScript is limited to the browser boundary: the
launcher, local file selection, keyboard events, Blob range reads, canvas
startup, and status reporting. Gameplay, archive parsing, simulation,
rendering calls, and game state remain in the authored C++ implementation.

This is neither a TypeScript reimplementation nor an x86 emulator. The exact
VC7 build remains the reconstruction evidence product. The Web build is a
separate modern port from the same sources and makes no binary-exact claim.

The current flow is:

```text
browser page (main thread)
  file picker + keyboard + Web Audio
           |               ^
           | local File    | small synchronous proxies
           v               |
pthread worker running wasm32 authored game
  PBG/archive logic + simulation + D3D8-shaped renderer
           |                  |
           |                  v
           |            OffscreenCanvas / WebGL 2
           |
           +-- th08.dat: one volatile 46.8 MB session-memory copy
           +-- thbgm.dat: byte-range reads from the browser Blob
           +-- allowlisted cfg/score/replay IDBFS mounts
```

## Provenance boundary

The repository and deployable Web artifact contain only project source,
HTML/JavaScript glue, WebAssembly, and clearly licensed project-owned assets.
They must never contain `th08.dat`, `thbgm.dat`, an original executable, or
files extracted from the retail archives.

At runtime, the user selects `th08.dat` and `thbgm.dat` from a legally obtained
local installation. The launcher does not contain an upload API. It keeps the
two browser `File` objects in the page, copies `th08.dat` into volatile Wasm
memory, and services `thbgm.dat` reads from Blob slices. Empty MEMFS directory
entries provide only the filenames expected by original path and `stat` code;
they contain no retail bytes. Reloading or closing the page discards both
retail archive handles and the game archive's memory copy, so the user must
select the two legal local files again.

The different strategies are intentional. Game archive reads are synchronous,
frequent, and small enough for a one-time memory copy. The roughly 450 MB music
archive must not consume the fixed Wasm heap, so the compatibility layer sends
range requests to the browser main thread and atomically waits in the game
worker. Neither archive is fetched over HTTP, uploaded, bundled, or written to
browser persistence.

`scripts/check-web-provenance.py` rejects forbidden payloads from both tracked
source and a staged artifact directory. It is a minimum automated gate, not a
license detector. Release automation must publish an allowlisted build
directory, never the repository or a data directory.

## Runtime design

### Threads and browser isolation

The port preserves the authored frame body and Win32-shaped startup/BGM
threads. Emscripten `PROXY_TO_PTHREAD` moves `main()` off the browser UI thread,
and a three-thread pool supports the existing startup jobs. The outer blocking
loop is Web-only adapted into one authored frame per `emscripten_set_main_loop`
callback so the worker yields to the browser after every frame. This requires
`SharedArrayBuffer` and therefore a secure, cross-origin-isolated page.

`scripts/serve-web.py` supplies the required COOP and COEP headers. Localhost is
a valid development secure context. A remote machine should use HTTPS; the raw
LAN or Tailscale HTTP address can display the launcher but cannot start this
pthread build in a conforming browser.

Before enabling **Start TH08**, the launcher now checks cross-origin isolation,
`SharedArrayBuffer`/`Atomics.wait`, transferable worker canvases for the direct
path, and WebGL 2. Unsupported environments remain on the launcher with an
actionable reason instead of failing later in Emscripten startup. This is a
diagnostic boundary, not a single-thread fallback: the authored startup, BGM,
and blocking compatibility seams require the pthread architecture described
above.

### Data bridge

The C++ Win32 file compatibility layer recognizes only the two retail archive
basenames as browser-owned files. `CreateFile`, `ReadFile`, seeking, size, and
close operations retain their synchronous authored-facing behavior.

- `th08.dat` is allocated with `malloc`, filled through the exported Wasm heap,
  and read with ordinary memory copies.
- `thbgm.dat` stays a browser `File`. Reads use `File.slice().arrayBuffer()` on
  the main runtime thread, copy only the requested range into Wasm memory, and
  wake the blocked worker through an atomic word.
- Browser persistence is never mounted at the `/game` root. Top-level
  `th08.cfg`, `score.dat`, and `score.txt` links target a dedicated `/save`
  IDBFS mount. The `replay/`, `backup/`, and `snapshot/` directories are
  separate IDBFS mounts at their original `/game` paths, preserving authored
  `chdir("directory")` followed by `chdir("../")` semantics. The launcher
  restores all four mounts before `main()`, and IDBFS `autoPersist` writes
  changes back asynchronously. Diagnostics and the two empty retail-name
  entries remain in `/game`'s volatile MEMFS root.
- The launcher recursively removes any `th08.dat` or `thbgm.dat` entry found
  in any persistent mount before starting the game. This is a
  defense-in-depth invariant; the authored game has no mapped path capable of
  writing either retail archive into the persistent tree.
- If IndexedDB is unavailable, the same allowlisted layout remains usable as
  session-only MEMFS and the launcher reports the fallback in its runtime log.
- Modern resource readers keep the post-decryption size synchronized with the
  returned allocation. LZSS fetches never dereference past compressed input,
  retain the retail format's zero-padding terminator behavior, and reject output
  that exceeds or fails to reach the declared size. Score and replay loaders
  validate their compressed ranges and decoded structure before use. These
  checks do not alter the VC7 reconstruction path.

### Rendering and frame pacing

The existing `IDirect3D8`/`IDirect3DDevice8` compatibility surface remains the
portability seam. The Web backend creates a WebGL 2 context directly on the
transferred `OffscreenCanvas` and translates the reconstructed D3D8 call stream
into an explicit GLSL ES 3.0 shader, vertex array, and rotating vertex buffers.
The CPU converts the small D3D flexible-vertex-format family used by TH08,
queues compatible draw commands, uploads the frame's vertices once, and applies
cached blend, depth, sampler, scissor, alpha-test, fog, and texture-stage state.
Backbuffer and dialogue-snapshot copies use a separate shader blit. No legacy
immediate-mode or fixed-function GL emulation remains on the Web hot path.

Chromium uses implicit swap control: returning from each Emscripten main-loop
callback lets its compositor present the worker-owned canvas directly. Firefox
153 accepted the same completed WebGL 2 framebuffer but did not update the
transferred canvas placeholder in the page. The first correctness bridge used
`transferToImageBitmap()` and a main-thread `bitmaprenderer`, but Firefox turns
that WebGL snapshot into a synchronous GPU-to-CPU readback.

The release therefore contains two links of the same compiled C++ objects.
The Chromium build keeps the direct worker-owned `OffscreenCanvas`. The
Firefox build keeps simulation on the pthread worker but leaves the visible
canvas on the main thread, enables Emscripten's `OFFSCREEN_FRAMEBUFFER` WebGL
proxy, and explicitly commits each completed frame. The renderer's batching
keeps this proxy narrow: representative frames contain only a handful of draw
commands rather than the original immediate-mode call stream. The launcher
selects the Firefox artifact before retail files are requested. Its `Present`
keeps `glFlush()` before the explicit commit: removing the flush reduced frame
progress in a controlled Stage 1 A/B, so it is an intentional command-submission
boundary rather than an unmeasured legacy call.

The two presentation links have the following frame boundaries:

```text
Chromium
game pthread -> batched WebGL 2 -> worker OffscreenCanvas -> compositor

Firefox
game pthread -> batched WebGL 2 -> glFlush -> main-thread GL proxy
             -> Emscripten offscreen default FBO -> commit-frame GPU blit
             -> visible canvas -> compositor
```

Both links retain `PROXY_TO_PTHREAD` and `OFFSCREENCANVAS_SUPPORT`. The Firefox
link additionally sets `OFFSCREENCANVASES_TO_PTHREAD` to an empty list and
enables `OFFSCREEN_FRAMEBUFFER`; context creation then requests
`EMSCRIPTEN_WEBGL_CONTEXT_PROXY_ALWAYS`. These settings are link-time choices,
so `scripts/build-web-game.sh` compiles the game objects once, emits the direct
Chromium link, reconfigures and relinks the Firefox artifact, then restores the
incremental build tree to the Chromium configuration. The shared launcher path
predicate accepts both the `.html` filenames used by the development server
and Cloudflare Pages' extensionless canonical aliases.

This design is viable because the modern renderer already collapses the
D3D8-shaped sprite stream into a few commands and one batched vertex upload per
frame. Proxying the earlier immediate-mode stream would merely have replaced a
readback bottleneck with thousands of cross-thread calls. The current proxy
moves a small command stream while all framebuffer pixels remain on the GPU.
The final Web blit also owns its texture binding and sampler state, so `Present`
does not emit the former redundant bind and two filter commands. This removes
three GL calls per frame from both links and, most importantly, three pthread
proxy crossings per frame from Firefox.

A 2026-09-11 Chrome/macOS endurance report exposed a separate late-frame
problem: gameplay could settle at 30--40 FPS as scene density increased. The
old upload ring selected a new VBO for every upload, not every presented frame,
and rewrote offset zero with `glBufferSubData()` whenever the buffer was large
enough. Three object names alone do not guarantee that ANGLE has finished with
an object's current storage, so a later upload can make the CPU wait for the
Apple GPU.

The `reallyportable` branch of `some100/th07` provided adjacent-port
corroboration for the safer stream shape: it rotates VBOs at frame start,
orphans the selected store with `glBufferData(..., NULL, GL_STREAM_DRAW)`, and
appends all uploads within that frame. This is not TH08 target evidence and its
single-thread SDL3/Web build is not directly transferable: TH08 keeps its
pthread boundary so synchronous authored file access, browser `File` range
reads for the roughly 450 MB BGM archive, and Web Audio startup continue to
work.

TH08's Web renderer now uses the same bounded storage principle while retaining
its own architecture. It starts each presented frame with one fresh 1 MiB
store on the next of three VBOs, appends the batched game vertices and final
blit, and grows/orphans only if the frame exceeds that store. Vertex conversion
writes directly into the persistent frame queue, avoiding the former temporary
vector and second copy, and adjacent triangle lists are combined only when
their complete captured draw state is byte-identical. This removes possible
in-flight overwrite stalls without changing draw order, primitive topology, or
authored calculation timing.

TH07 avoids browser file stalls by preloading its packaged assets, but copying
TH08's roughly 450 MB BGM archive into Wasm would consume most of the fixed heap
before gameplay. TH08 instead keeps a bounded two-chunk stream: the authored
synchronous read consumes one 1 MiB cache while the browser asynchronously
prefetches the next expected cache miss. The lookahead accounts for the
authored 44,100-byte DirectSound notification reads: it advances by the number
of complete requests that fit and skips the cache tail that cannot satisfy the
next request. A sequential cache miss normally
copies an already-resolved `ArrayBuffer` into shared memory; seeking or a failed
prefetch falls back to the existing direct Blob range read and restarts the
lookahead. At most one future range is retained, so the optimization removes
periodic I/O waits without making the retail archive persistent or bundling it.

`?perf=1` enables presentation diagnostics without changing the default
release hot path. It measures main-thread animation-frame intervals, bitmap
creation, worker-to-main message latency, bitmap arrival, and bitmap
presentation separately. C++ counters independently report browser callbacks,
authored calculation frames, draw commands, submitted vertices, streaming
upload time, and average/maximum submission time in repeating windows. A
visible five-second summary reports browser rAF, worker callback, and authored
game rates separately. Keeping these clocks separate prevents a nominal
browser callback rate or the in-game counter from hiding slower simulation
progress. The same diagnostic mode logs every direct or prefetched retail Blob
read with its byte range and worker wait time, separating I/O stalls from VBO
submission stalls.

Those separated counters exposed a third boundary in the original-shaped Web
loop: a proxy sample could receive 60 worker callbacks per second but execute
only about 50--51 authored calculations. The old timestamp gate performed at
most one calculation per callback and advanced past missed intervals, so rAF
jitter or a message callback permanently discarded logical time. The Web-only
loop now accumulates elapsed time, clamps one callback to 100 ms, executes the
required 60 Hz calculation steps, and draws/presents once after catch-up. Replay
input is still consumed once per authored calculation in its original order.
The native/VC7 path is unchanged, and TH08 does not yet interpolate render
state between calculations; worker rate remains separately visible so catch-up
cannot disguise an actual 30--40 Hz presentation bottleneck.

A replay-driven Chromium 150/SwiftShader check measured Stage 5 for 20 seconds
after a separate 10-second warm-up. Direct presentation recorded 1,200 worker
callbacks and 1,205 authored calculations (59.99 and 60.24 Hz); forced proxy
presentation independently recorded the same deltas and rates. Both runs kept
the same route and player-shot snapshots, produced valid gameplay screenshots,
and reported no test failure, page crash, or console error. In the dense part
of the sample the direct renderer submitted approximately 1,900--2,100 vertices
per frame with about 0.03 ms average streaming-upload CPU time; proxy upload
averaged approximately 0.19--0.24 ms. Prefetched BGM cache fills normally
completed in roughly 1--7 ms. These are bounded software-renderer results, not
a Mac hardware performance claim.

Firefox pacing diagnostics isolate browser rAF, bitmap creation, message
latency, bitmap presentation, worker callbacks, and authored calculations. In
an Xvfb `llvmpipe` Lunatic Stage 1 comparison, the old bitmap path spent an
average 38.82 ms and as much as 82.32 ms in `transferToImageBitmap()`, reaching
about 24 FPS. The proxied build removed that readback and reached about 33--36
FPS in the same test while preserving movement, shooting, bullets, HUD, audio,
and callback/calculation lockstep. A separate title sample improved from about
15--19 FPS to about 30--31 FPS. These ratios are repeatable software-renderer
evidence, not a hardware Firefox performance claim.

After the optimized artifact was exposed through the normal HTTPS launcher on
2026-08-26, real-hardware Firefox play was reported as generally sustaining
more than 50 FPS with a clear responsiveness improvement. This is preliminary
operator evidence, not a controlled benchmark: the browser build, GPU, scene,
and complete-route duration were not captured. It validates that the
no-readback path translates beyond `llvmpipe`, while controlled traces and a
complete-route endurance run remain open.

The earlier bitmap build remains important correctness evidence: a 45-minute
Firefox Lunatic Final-B endurance route completed all six route stages and
returned through Result to title without a browser, worker, or Wasm memory
error. Equivalent long-route coverage for the new proxy build remains pending.

An earlier blocking-loop experiment rendered correctly into the WebGL default
framebuffer but remained black on screen. In Emscripten 6, the native
OffscreenCanvas `emscripten_webgl_commit_frame()` path is a no-op because modern
browsers present implicitly. Moving the outer loop to yielding callbacks fixed
the actual display boundary without changing authored frame behavior.

Instrumentation continues to separate browser main-loop callbacks from
authored calculation frames. This distinction caught the earlier legacy
renderer regression, where callbacks held at 60 Hz while calculation fell to
about 40 FPS, and remains more reliable than the displayed in-game counter
alone.

### Input and audio

Browser keyboard events are translated to the Win32 virtual-key set used by
the compatibility layer and stored in shared atomic state. Each authored input
poll merges that state with SDL's keyboard state. A key-down edge remains
latched until one authored poll consumes it, preventing short Z/X taps from
falling entirely between 60 Hz polls. Blur clears held and pending keys to avoid
stuck movement or firing.

SDL's Web Audio device must be opened, paused, and closed on the main browser
runtime thread. Small synchronous proxies preserve the DirectSound-shaped C++
interface, while mixing remains in the shared Linux audio implementation. BGM
streaming reads `thbgm.dat` through the Blob range bridge.

### Wasm ABI adaptations

WebAssembly validates indirect call signatures more strictly than native x86.
The port uses Web-only typed adapters for Win32 thread entry points and callback
tables whose reconstructed x86 calls intentionally ignore a non-void return.
The VC7/native branches remain unchanged. These are ABI boundary adaptations,
not gameplay replacements.

Normal Wasm globals start at 32 MiB so existing low raw-address views remain
available. The pthread build currently uses a fixed 256 MiB initial shared
memory and a 4 MiB stack. The music archive is not part of that memory budget.

The original PE also gives several named globals overlapping identities inside
larger manager objects. A native Linux linker script can preserve those
addresses, but Wasm globals are relocatable. Web-only references therefore bind
ECL state, player/gauge fields, effect and GUI tables, and callback lifetimes to
their real aggregate owners. This is correctness-critical: split callback
storage previously left old spell jobs alive across stage reloads, producing
missing effects, unstable scores, and an eventual out-of-bounds trap during a
result transition. Runtime diagnostics verify the most failure-prone aliases
before endurance tests.

The native Windows i386 prerequisite pass also exposed gameplay defects that
were independent of browser APIs. The Web build now carries the same
target-evidenced behavior at its modern boundary: stage backgrounds use the
stage-finished flag rather than dialogue presence, the enemy-name copy uses the
front ANM owner, item popups use their correct pools, randomized player shots
use the signed RNG result, bomb effects test the current timer value, and the
retry menu distinguishes Spell Practice from ordinary Practice. Keeping these
corrections explicit prevents a successful Web link from masking stale
reconstruction behavior.

## Build and run

Docker is the only Emscripten prerequisite. The build image is pinned by tag
and digest:

```bash
scripts/build-web-game.sh
python3 scripts/check-web-provenance.py --artifact build/web-dist
scripts/serve-web.py --bind 127.0.0.1 --port 8000
```

Compilation is single-job. Docker defaults to a two-CPU, 4 GiB, no-extra-swap
envelope; `TH08_WEB_BUILD_CPUS` and `TH08_WEB_BUILD_MEMORY` override those caps
when a different builder budget is required.

Open `http://127.0.0.1:8000/`, select local files named exactly `th08.dat` and
`thbgm.dat`, and choose **Start TH08**. Keyboard controls are listed in the
launcher. The generated static artifact consists of the Chromium
`th08-web.html`/`.js`/`.wasm` triplet, the Firefox
`th08-web-firefox.html`/`.js`/`.wasm` triplet, the project-owned
`th08-web-icon.png`, and the two static-host metadata files `_headers` and
`_redirects`. CMake metadata stays in `build/web-game`; only these nine
allowlisted files are staged in `build/web-dist`.

The normal script builds `Release`; the staged JavaScript and Wasm are
approximately 232 KiB and 1.4 MiB respectively. The old Debug Wasm was about
21 MiB and is not the public build path. The staged directory also contains
Cloudflare Pages `_headers` and `_redirects` metadata. The headers preserve
cross-origin isolation, while the root redirect leads to `th08-web.html`.

For another device, place the same server behind HTTPS and preserve the COOP,
COEP, CORP, and no-store headers. Static hosts that cannot provide
cross-origin isolation cannot run this pthread build.

Cloudflare Pages can publish `build/web-dist` directly because it applies the
staged `_headers` file to static responses. GitHub Pages can publish the same
files, but its static hosting does not provide repository-defined response
headers, so a bare GitHub Pages deployment is not a supported host for this
pthread build. GitHub remains the source and build-automation host; only the
allowlisted `build/web-dist` payload belongs in a Pages deployment.

The production deployment is `https://th08-web.pages.dev/`. A direct upload
verified that the root redirect, HTML, and Wasm responses preserve the staged
isolation headers, that the Wasm response uses `application/wasm`, and that its
downloaded SHA-256 matched the tested artifact for that initial direct upload.
A Chromium navigation reported a secure, cross-origin-isolated document
without browser errors.
`.github/workflows/deploy-web.yml` repeats repository validation, the pinned
Release build, and the artifact-boundary check before deploying a `main` push.
The workflow reads Cloudflare credentials only from repository secrets.

## Reproducible evidence

All Web builds use
`emscripten/emsdk:6.0.8@sha256:f174124ff798a3ead1abef247d9a849c270b642d552fea500a42565ff210f765`.

- The authored compiler gate builds all 44 shared game/PBG translation units
  as wasm32 objects.
- The full target links the authored objects with the existing Linux platform
  adapters and the Web-specific browser boundaries.
- With user-selected retail archives of 46,838,025 and 449,961,024 bytes, the
  title loaded, rendered, and accepted keyboard input through difficulty and
  character/team selection. Stage setup then completed without a Wasm trap.
- A separate data probe read the first and last bytes of both browser-local
  files. Browser resource inspection showed only HTML, JavaScript, Worker, and
  Wasm requests; neither DAT appeared as a network resource.
- Browser screenshots show the full title, menus, Japanese dialogue, the Music
  Room, and gameplay on the browser canvas. After the direct
  WebGL 2 renderer landed, bounded samples held authored calculation frames in
  lockstep with browser callbacks near 60 Hz. Stage 2 Normal registered
  spell-card numbers 14, 18, 22, 26, and 29 when its documented Last Spell
  time-orb requirement was met, and returned to the title without the former
  Wasm out-of-bounds trap.
- A Chromium Lunatic Border Team Final-B endurance run crossed all six route
  stages, credits, result/score writing, title reconstruction, and a second
  start without a runtime trap. Per-stage spell observations were 1, 5, 9,
  12; 16, 20, 24, 28, 31; 35, 38, 42, 46, 50, 53; 80, 84, 88, 92, 96, 99;
  103, 107, 111, 115, 118; and 150, 154, 158, 162, 166, 170, 174, 178, 182,
  186. Failing Kaguya's fourth Last Spell correctly reached the original 5:00
  cutoff and skipped the remaining request. A separate collision-free Stage
  6B practice run followed the real ECL flow through spell 190 and its All
  Clear transition, completing conditional coverage of all 37 expected route
  spells. That pre-persistence result path wrote a 17,074-byte `score.dat`;
  the unchanged authored path now resolves to the persistent `score.dat`
  overlay.
- A clean-profile Chromium persistence test created an authored 60-byte
  `th08.cfg` and a replay-directory probe through IDBFS `autoPersist`, reloaded
  the page without an explicit sync, reselected the legal local DAT files, and
  recovered both byte-for-byte. Injected three-byte fake `th08.dat` and
  `thbgm.dat` entries in two different persistent mounts were removed during
  restore; both `/game` DAT entries remained zero bytes, every persistent mount
  was DAT-free, and all three authored directory round trips returned to
  `/game`.
- A headed Firefox 153 test used the Firefox-only `ImageBitmap` presentation
  bridge to render the title and difficulty menu, accepted Z input, reloaded,
  reselected both local retail files, and recovered an auto-persisted probe.
  Recursive inspection after restore found no retail DAT in any persistent
  mount. A subsequent 45-minute Lunatic Border Team Final-B endurance run
  crossed Stages 1, 2, 3, 4B, 5, and 6B, wrote `score.dat`, returned through
  Result to the title, and raised no page, worker, or Wasm memory error. It
  observed the expected route spells through 186; the original 5:00 cutoff
  skipped 190, which is separately covered by the Chromium Stage 6B practice
  run. These Firefox tests used Xvfb's software `llvmpipe` renderer and
  therefore establish correctness rather than production GPU performance.
- A later Firefox 153 A/B test measured presentation stages separately. In
  Lunatic Stage 1, bitmap creation averaged 38.82 ms and peaked at 82.32 ms;
  the browser-specific proxy build removed that readback, increased authored
  progression from about 24 FPS to about 33--36 FPS, moved the player through
  the expected shared input path, created active/hit shots, and rendered the
  gameplay field without a page or worker error.
- On 2026-09-11, both Release links were rebuilt with the pinned Emscripten
  image under a two-CPU, 4 GiB, single-job limit and passed the staged-artifact
  provenance gate. Headless Chromium 150 with SwiftShader loaded the legal
  46,838,025-byte and 449,961,024-byte retail files, repeatedly ran bundled
  demonstrations, and loaded a 3,246-byte external `th8_03.rpy`. The replay
  menu resolved its recorded Stage 5, ignored a separate synthetic 16-byte
  truncated replay, and entered Stage 5 with the background, bullets, player,
  and HUD visible. A 15-second gameplay-only pacing sample contained 899 rAF
  intervals averaging 16.666 ms, with a 16.670 ms maximum and no interval over
  20 ms. These are bounded software-renderer correctness/pacing observations,
  not a hardware GPU benchmark. Serving the same artifact without isolation
  headers kept Start disabled and displayed the expected COOP/COEP diagnostic.
- The follow-up performance artifact at commit `93aa518` passed repository
  validation and the staged-artifact provenance gate in GitHub Actions run
  `34560049728`. The retained replay test ran the direct and forced-proxy links
  in clean Chromium 150/SwiftShader contexts. After 10 seconds of Stage 5
  warm-up, each 20-second sample recorded 1,200 worker callbacks and 1,205
  authored calculations (59.99 and 60.24 Hz), remained on the expected stage,
  and had no runtime-test failure or console error. Screenshots showed the
  background, player, bullets, enemies, HUD, and diagnostic FPS counter. The
  retail files and replay remained caller-supplied test inputs outside the
  repository and artifact.

Run the bounded probes with:

```bash
scripts/build-web-probe.sh
scripts/build-web-layout-probe.sh
scripts/build-web-data-probe.sh
scripts/build-web-renderer-probe.sh
python3 scripts/check-web-provenance.py
```

For a replay-driven browser test of the release artifacts, install the pinned
automation library (it does not download a browser) and pass local retail files
explicitly:

```bash
npm ci --ignore-scripts
npm run test:web-runtime -- \
  --artifact build/web-dist \
  --game-data /path/to/th08.dat \
  --bgm-data /path/to/thbgm.dat \
  --replay /path/to/replay/th8_03.rpy \
  --expected-stage 5
```

The test starts an ephemeral isolated-header server, creates clean browser
contexts, injects only the named replay into session MEMFS, navigates the
authored Replay menus, and samples both direct and proxy presentation after a
separate warm-up. It rejects browser errors, runtime traps, stage mismatches,
and worker/calculation rates below `--minimum-fps`; screenshots and JSON go to
an untracked temporary directory unless `--output-dir` is supplied. Chrome is
auto-detected on macOS and common Linux paths. `--swiftshader --no-sandbox` is
available for controlled headless environments and should not be used for a
real-hardware performance claim.

## Remaining work

The port has crossed the full-link, title/menu, input, audio-device, BGM-range,
direct-renderer, full-route, conditional-spell, isolated persistent-save, and
public-deployment gates. It is a public engineering release. The next work is:

1. capture controlled hardware Firefox proxy pacing and complete-route
   endurance,
   pause/focus, audio-underrun, and repeated stage-reload regressions;
2. measure and tune the fixed shared-memory ceiling under repeated long
   sessions;
3. add gamepad mapping and broaden the tested desktop-browser compatibility
   matrix.

## Primary references

- [Emscripten pthreads support](https://emscripten.org/docs/porting/pthreads.html)
- [Emscripten file system API](https://emscripten.org/docs/api_reference/Filesystem-API.html)
- [Emscripten OpenGL support](https://emscripten.org/docs/porting/multimedia_and_graphics/OpenGL-support.html)
- [Emscripten compiler settings](https://emscripten.org/docs/tools_reference/settings_reference.html)
- [Mozilla Bug 1864882: WebGL OffscreenCanvas bitmap transfer performance](https://bugzilla.mozilla.org/show_bug.cgi?id=1864882)
