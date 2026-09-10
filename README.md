<p align="center">
  <img src="resources/modern-icon.png" width="128" alt="TH08 Web icon">
</p>

<h1 align="center">TH08 Web</h1>

<p align="center">
  <strong>One browser tab. Two legal DAT files. One endless night.</strong>
</p>

<p align="center">
  <em>Touhou Eiyashou ~ Imperishable Night, source-built for the Web.</em>
</p>

<p align="center">
  <a href="https://th08-web.pages.dev/"><strong>Enter the endless night</strong></a>
  ·
  <a href="https://github.com/N0zoM1z0/th08-web/releases/latest">Latest release</a>
  ·
  <a href="docs/WEB_PORTING.md">How we brought TH08 to the Web</a>
  ·
  <a href="docs/WEB_ARCHITECTURE.md">Architecture and verification</a>
</p>

<p align="center">
  <a href="https://github.com/N0zoM1z0/th08-web/actions/workflows/deploy-web.yml"><img src="https://github.com/N0zoM1z0/th08-web/actions/workflows/deploy-web.yml/badge.svg?branch=main" alt="Web deployment status"></a>
  <a href="https://github.com/N0zoM1z0/th08-web/actions/workflows/ci.yml"><img src="https://github.com/N0zoM1z0/th08-web/actions/workflows/ci.yml/badge.svg?branch=main" alt="Repository validation status"></a>
</p>

<p align="center">
  <img src="resources/th08-web-social-preview.jpg" width="1280" alt="TH08 Web source-built browser port and Imperishable Night title screen">
</p>

The false moon is up, the clock is moving, and Gensokyo has found one more
window: your browser.

**TH08 Web** brings the original Japanese TH08 version 1.00d across the browser
boundary. The reconstructed C++ game code runs as WebAssembly; a small
JavaScript layer handles browser file access, input, audio, saves, and the
canvas. This is the game logic compiled for the Web, not a TypeScript remake or
an executable running inside an emulator.

> [!IMPORTANT]
> This project does not distribute `th08.dat`, `thbgm.dat`, `th08.exe`, or
> extracted retail assets. You must provide the two DAT files from your own
> legally obtained copy of TH08. They remain local to your browser and are
> never uploaded to the site.

## Enter the night

1. Open **[th08-web.pages.dev](https://th08-web.pages.dev/)** in a desktop
   browser. Current desktop Chrome is recommended.
2. Select `th08.dat` and `thbgm.dat` from your legally obtained TH08
   installation.
3. Choose **Start TH08**, then click the game canvas if it does not already
   have keyboard focus.

The launcher copies `th08.dat` into volatile session memory. The much larger
`thbgm.dat` remains a browser `File` and is read in small ranges as music is
needed. Neither archive is bundled, transmitted, cached by the site, or stored
in browser persistence.

Settings, scores, replays, backups, and snapshots are stored separately in the
browser's IndexedDB storage. They survive a normal reload and remain private to
that browser profile. Clearing site data for `th08-web.pages.dev` removes them.

Replay recording and playback are supported. Complete the original save flow
by choosing a replay slot and confirming **End** on the name-entry screen; the
saved run then appears in the title screen's **Replay** menu.

That is the whole ritual. There is no installer, account, upload, or server-side
game session: once the static Web build arrives, the night unfolds entirely on
your machine.

Prefer to keep a copy or host the page yourself? The
**[latest GitHub Release](https://github.com/N0zoM1z0/th08-web/releases/latest)**
contains a provenance-gated static build and its SHA-256 manifest. It still
contains no game data; local legal DAT selection is always required. Builds
from this revision use the exact nine-file layout documented below.

## Choose your browser

| Browser | Status | Notes |
| --- | --- | --- |
| Chrome | **Recommended** | Best observed performance and frame pacing. |
| Chromium-based desktop browsers | Expected to work | Use a current version with hardware acceleration enabled. |
| Firefox | Supported | Automatically uses a dedicated no-readback build; preliminary hardware play now generally reports 50+ FPS. |
| Safari and mobile browsers | Not verified | Keyboard play, WebAssembly threads, and the current presentation path are desktop-oriented. |

The game requires WebAssembly threads, `SharedArrayBuffer`, WebGL 2, Web Audio,
and a cross-origin-isolated HTTPS page. The production site supplies the
required COOP and COEP headers. The launcher checks the isolation, shared-memory,
canvas-transfer, and WebGL 2 boundaries before enabling Start and reports the
missing requirement directly.

For the smoothest bullet-hell input and pacing, close heavily loaded tabs,
leave browser hardware acceleration enabled, and avoid power-saving modes that
throttle the display refresh rate.

## Danmaku controls

| Key | Action |
| --- | --- |
| Arrow keys | Move / navigate menus |
| `Z` | Shoot / confirm |
| `X` | Bomb / cancel |
| `Shift` | Focus movement and show the hitbox |
| `Esc` | Pause |

## Inside the spell circle

- The reconstructed authored C++ game and PBG archive code compile to
  WebAssembly with Emscripten 6.0.8.
- `PROXY_TO_PTHREAD` keeps the authored game loop off the browser UI thread and
  retains the existing startup and BGM thread structure.
- A direct WebGL 2 renderer translates the D3D8-shaped draw interface into
  shaders, batched vertex uploads, and browser canvas presentation.
- Chromium composites the worker-owned canvas directly. Firefox receives the
  same batched renderer through a main-thread WebGL proxy, avoiding its costly
  per-frame `OffscreenCanvas` snapshot readback.
- The authored DirectSound-shaped mixer feeds Web Audio while BGM data is
  range-read from the user-selected local file.
- Browser key events enter shared atomic state and are merged with the
  DirectInput-shaped polling path, preserving short key presses between frames.
- IDBFS persists only the allowlisted save paths. Retail archives stay outside
  persistent storage.

The implementation and its verified boundaries are documented in
**[docs/WEB_ARCHITECTURE.md](docs/WEB_ARCHITECTURE.md)**. For the complete
from-zero engineering story—including the failed bring-up paths, renderer
replacement, browser boundaries, correctness work, and public release—read
**[Engineering TH08 Web](docs/WEB_PORTING.md)**.

## Route status

The imperishable night is playable from title to ending. The following paths
have been exercised with locally selected retail data:

- title, difficulty, team, practice, Music Room, dialogue, and result screens;
- keyboard movement, shooting, focus, bombs, score, browser-local replay
  recording and playback, and other browser-local saves;
- an external Stage 5 replay plus rejection of a deliberately truncated replay;
- direct WebGL 2 rendering and Web Audio playback;
- a complete Lunatic Border Team Final-B route in Chromium;
- a complete Lunatic Border Team Final-B endurance route on the earlier
  Firefox bitmap bridge, including return through Result to the title screen;
- a separate Stage 6B practice run covering the final spell sequence;
- reload and persistence tests confirming that no DAT enters IndexedDB.

Remaining engineering work includes controlled hardware profiling and
long-route endurance for the new Firefox presentation build, additional replay
endurance, and browser memory-ceiling measurement. See the architecture
document for the exact evidence and limitations behind each claim.

## Build your own night

Requirements:

- Docker
- Python 3
- a desktop browser
- your own legal `th08.dat` and `thbgm.dat`

Build the pinned Release configuration and start the repository development
server:

```bash
scripts/build-web-game.sh
python3 scripts/check-web-provenance.py --artifact build/web-dist
scripts/serve-web.py --port 8000
```

The build is intentionally single-job and limits each Docker invocation to two
CPUs and 4 GiB by default. Override the caps only when needed, for example:

```bash
TH08_WEB_BUILD_CPUS=1 TH08_WEB_BUILD_MEMORY=3g scripts/build-web-game.sh
```

Open `http://127.0.0.1:8000/`. Do not use a generic static server for this
build: Emscripten pthreads require the COOP, COEP, and CORP headers supplied by
`scripts/serve-web.py`.

The generated deployment directory contains exactly nine allowlisted files:

```text
_headers
_redirects
th08-web.html
th08-web.js
th08-web.wasm
th08-web-firefox.html
th08-web-firefox.js
th08-web-firefox.wasm
th08-web-icon.png
```

Any extra file, missing file, symbolic link, executable, retail archive, or
common archive container causes the provenance check to fail.

After packaging or testing, the generated trees can be reclaimed explicitly:

```bash
cmake -E remove_directory build/web-game
cmake -E remove_directory build/web-dist
cmake -E remove_directory build/emscripten-cache
```

## Release boundary

Cloudflare Pages hosts the public static build because it applies the checked-in
`_headers` policy required by WebAssembly threads. Bare GitHub Pages hosting is
not used because it cannot attach the repository-defined isolation headers.

Pushes to `main` run the following release chain:

1. repository-owned validation;
2. the digest-pinned Emscripten Release build;
3. the exact deployment-boundary check;
4. Cloudflare Pages Direct Upload.

The workflow is defined in
[`deploy-web.yml`](.github/workflows/deploy-web.yml). It requires these GitHub
repository secrets:

- `CLOUDFLARE_ACCOUNT_ID`
- `CLOUDFLARE_API_TOKEN`, scoped to **Account → Cloudflare Pages → Edit**

Pull requests do not deploy and do not receive these secrets.

Tagged GitHub Releases attach a compressed nine-file static
artifact plus a SHA-256 manifest. Release archives are for self-hosting and
offline retention; they do not include retail data and still require a host
that supplies the staged isolation headers.

Build and package a tagged artifact locally with:

```bash
scripts/build-web-game.sh
scripts/package-web-release.sh v0.1.0
```

For an authorized manual deployment:

```bash
export CLOUDFLARE_ACCOUNT_ID=<account-id>
export CLOUDFLARE_API_TOKEN=<token-with-pages-edit>
npx wrangler@4.125.0 pages deploy build/web-dist --project-name=th08-web --branch=main
```

Never write deployment credentials into this repository or command logs.

## Map of the boundary

| Path | Purpose |
| --- | --- |
| `src/` | Reconstructed authored game code and modern host adapters |
| `src/modern/web/` | Browser launcher, Web compatibility boundary, and Pages metadata |
| `scripts/build-web-game.sh` | Digest-pinned Emscripten Release build and exact staging |
| `scripts/package-web-release.sh` | Deterministic nine-file release archive and SHA-256 manifest |
| `scripts/check-web-provenance.py` | Retail-data and exact-artifact deployment gate |
| `scripts/serve-web.py` | Local server with cross-origin-isolation headers |
| `docs/WEB_PORTING.md` | From-zero engineering narrative and reproducible porting method |
| `docs/WEB_ARCHITECTURE.md` | Architecture, provenance model, and observed verification |
| `.github/workflows/deploy-web.yml` | Validated `main` deployment to Cloudflare Pages |

## Credits

Touhou Project and `東方永夜抄 ～ Imperishable Night` are works of Team Shanghai
Alice / ZUN. This project is unofficial and is not affiliated with or endorsed
by Team Shanghai Alice.

This repository is the Web-focused fork of our source reconstruction,
[N0zoM1z0/th08](https://github.com/N0zoM1z0/th08). Its authored C++ game code is
the foundation beneath the browser boundary, and its contributor authorship
and commit history remain intact here.

The Web icon is project-owned artwork carried over from the modern port. It is
not extracted from the retail executable or archives.

## License

Repository code and documentation are provided under the included MIT License.
That license does not grant rights to the original game, executable, DAT files,
music, dialogue, graphics, or other retail content.
