---
name: bd-analyzer
description: Deep reference for the bd_analyzer AV1 analyzer app - where each value on screen comes from, which decoder produces it, why a feature refuses to run, and how the repository is laid out. Load when a question needs more than the always-injected app summary: a refusal you must explain, a statistic whose provenance matters, or a walk through the source.
---

# bd_analyzer — deep reference

The session's system prompt already carries the feature summary and the read-only policy. This
skill is the next layer down: provenance of values, refusal reasons, and the source layout.

## Repository layout

`~/project/bd_analyzer`. The app is a fork of IENT/YUView that is **never edited in place**.

| Path | What lives there |
|---|---|
| `src/me/` | Motion estimation cores. Qt-free, unit tested without Qt |
| `src/bdrate/` | BD-rate maths (`psnrFromSse`, `polyFit`, `bdRate`). Qt-free |
| `src/assist/` | Assistant panel context model and serialization. Qt-free |
| `src/integration/` | The boundary that touches upstream types, decoders, and Qt |
| `third_party/yuview/upstream/` | Upstream YUView. **Never edited directly** |
| `third_party/yuview/patches/` | Every upstream change, as a numbered patch |
| `tests/unit/` | Qt-free unit tests |
| `tests/regression/` | Regression tests driving production classes headless (offscreen Qt) |
| `docs/ai/30-designs/` | Design documents, including `sb-bdrate-design.md` |
| `assets/assist/` | This skill, the injected system prompt, and the permission policy |

Rule when reading: a change to upstream behaviour is described by a patch in
`third_party/yuview/patches/`, not by the file in `upstream/`. Cite the patch number.

## Where each on-screen value comes from

| Value | Produced by | Notes |
|---|---|---|
| Block AV1 syntax | `playlistItem::getBlockInfoAt(pos, frameIdx)` | dav1d analyzer only |
| `sb_bitcount` per superblock | `getSuperblockBits(frameIdx)` | Folded onto the superblock grid |
| `sb_qindex` | Statistics overlay type | Per superblock |
| Per-block SSE | `getPixelBlockStats(pos, frameIdx)->sse` | Needs an attached original; computed off the GUI thread |
| PSNR | `bdrate::psnrFromSse(sse, sampleCount)` | `sampleCount` is the real overlap, not 64×64 |
| BD-rate points | `BdRateCollector` → `BdRateFrameData` / `BdRateSequenceData` | `perSuperblock[SB][group][point]` |
| ME vectors and costs | `src/me` estimators via `MeStatisticsAdapter` | Native cost + common SAD |

## Why a feature refuses to run

These refusals are deliberate and each carries its reason in the dialog. When a user reports one,
explain the cause rather than suggesting a workaround.

| Refusal | Cause |
|---|---|
| "no per superblock bit counts … decoded by something else" | The stream is on the FFmpeg fallback, not the dav1d analyzer |
| "has not reported its frame size or superblock size yet" | Sequence header not parsed — open the item once |
| "is WxH with a S superblock, but the others …" | One BD-rate curve must describe one grid |
| "is a single operating point" | A curve needs ≥2 points; the standard metric uses 4 |
| "More than one raw file is selected" | The original must be unambiguous |
| "No original to measure against" | Attach the source with `Load Org YUV`, or add it to the selection |
| "different originals attached" | The streams in one group disagree on their source |
| ME panel disabled on a compressed stream | No original attached; ME searches the source picture, and reference frames are not decoded |
| `Frame 0: no reference` | Default ME interval is +1 and there is no frame −1; step forward or use −1 |

## Statistics that are easy to misread

- **`bits + 1` on the rate axis.** Applied to every point, not only zeros — patching just the zeros
  would put a step in the axis. Tables show the real bits.
- **Averaging dB is wrong.** Frame and sequence PSNR come from summed SSE over summed samples.
- **ME cost is not comparable across algorithms.** SVT uses SAD with no MV rate; Odyssey open-loop
  uses SAD with a linear rate at step 2 and SSE with a squared rate at steps 3–4. That is why every
  block also carries a common SAD.
- **Odyssey open-loop runs on an 8-bit, half-resolution round trip**, not the original picture. MV
  differences against the bitstream can come from that alone.
- **Superblock BD-rate is often undefined.** Measured on 1080p synthetic content: of 510
  superblocks only 107 had two or more usable points; most were lossless.

## Verifying a claim in this repository

- Regression suite: `./tests/run-regression.sh` (37 tests). It builds and runs headless.
- Unit tests are compiled **without Qt** to prove the core stays Qt-free.
- Build: `./scripts/build.sh` applies the patches and builds.
- A "successful" build can still hide a file that never compiled — stale objects mask compile
  errors. Clean-rebuild before blaming a change.

In **full** mode you can run these yourself. In the narrower modes `make` and the package scripts
are refused — tell the user the command instead.

A build takes minutes and rewrites `build/` and the patched upstream tree, so say you are starting
one rather than doing it silently.

## Editing this repository

- **Never edit `third_party/yuview/upstream/` directly.** Every upstream change is a numbered
  patch in `third_party/yuview/patches/`. An edit in the upstream tree is silently lost the next
  time the patches are re-applied — write or amend a patch instead, then verify it with
  `git apply --reverse --check` from inside the submodule.
- **Keep the diff minimal.** Fix what was asked and leave the surrounding code alone; RTL and
  analyzer changes here are re-reviewed and re-synthesised, so incidental refactoring is a cost.

## Measuring an RD curve and BD-rate

The usual job. Anything below that writes or downloads needs **full** mode.

### 1. Get a source sequence

Standard test clips come as raw `.y4m` or `.yuv`. Common sources are `media.xiph.org/video/derf/`
and the AOM test set. Download into a directory with room — decoded YUV and encoder sweeps run to
gigabytes — and check the size before starting:

```bash
curl -L -o /data/work/seq.y4m https://media.xiph.org/video/derf/y4m/<clip>.y4m
ffprobe -hide_banner /data/work/seq.y4m        # confirm resolution, frame count, pixel format
df -h /data/work
```

Prefer `.y4m`: it carries its own resolution and frame rate, so nothing has to be guessed. A raw
`.yuv` needs the geometry supplied by hand everywhere it is used.

### 2. Encode the sweep

One curve is one encoder configuration at several rate points. Four is the number the standard
BD-rate uses; two is the minimum anything can be computed from.

```bash
for q in 20 32 43 55; do
  ffmpeg -hide_banner -loglevel error -y -i /data/work/seq.y4m \
         -c:v libaom-av1 -crf $q -cpu-used 4 -g 60 -pix_fmt yuv420p \
         /data/work/curveA_q$q.ivf
done
```

Keep everything except the rate point identical within a curve, and change exactly one thing for
the curve you are comparing against. Use `.ivf` (or `.av1` / `.obu`) — the analyzer's per-block
statistics come from the dav1d analyzer decoder, and an MP4 goes through the FFmpeg fallback
instead, which reports no block statistics at all.

### 3. Measure it in the app

The BD-rate is computed in the GUI, not from a command line — there is no headless entry point.

1. Add the sweep and the source to the playlist (`File → Open`, or start the app with the files as
   arguments: `bd-analyzer /data/work/curveA_q*.ivf /data/work/seq.y4m`).
2. Select the streams of **one** curve plus the source, and press `Ctrl+R`.
3. Select the second curve plus the same source, press `Ctrl+R` again — it becomes a second curve
   in the same window.
4. Pick the anchor with the radio button in the group table. BD-rate is reported against it.

Read **Statistics that are easy to misread** above before reporting the numbers.

### Measuring it without the GUI

For a figure in the terminal, measure PSNR with ffmpeg and feed the points to the analyzer's own
maths — same code the panel uses, so the answers agree.

```bash
# PSNR per rate point. Note the verbosity: the summary line is printed at *info* level, so
# -loglevel error (the habit everywhere else in this repo) silently produces an empty result.
ffmpeg -hide_banner -i curveA_q20.ivf -i src.y4m -lavfi psnr -f null - 2>&1 \
    | grep -o 'y:[0-9.]*' | head -1
```

Use the **y:** figure, not `average:` — the average is a weighted mix of Y, U and V, and BD-rate is
conventionally reported on luma. Rate can be the file size in bytes; BD-rate is a ratio, so the
unit only has to be consistent within one comparison.

Then call `bdrate(anchor, test)` from `src/bdrate/BdRateMath.h` with the `(rate, psnr)` pairs —
`tests/unit/bdrate-math.cpp` shows the call. Compile it standalone; it needs no Qt:

```bash
scl enable gcc-toolset-13 -- g++ -std=gnu++2a -I<repo>/src \
    yours.cpp <repo>/src/bdrate/BdRateMath.cpp -o bdrate_check
```

Worth knowing what a sane answer looks like: measured on 30 frames of `akiyo_qcif`, four CRF
points, `-cpu-used 2` against `-cpu-used 8` gave **-13.6 %** at cubic fit. A slower preset buying
low-double-digit bitrate is the expected shape; a number in the hundreds means the curves were
mismatched, usually a different source or frame count between them.

### Common ways this goes wrong

| Symptom | Cause |
|---|---|
| BD-rate window refuses the selection | The streams are on the FFmpeg fallback — re-encode to `.ivf`, or check the decoder in Properties |
| "is a single operating point" | Only one stream selected for that curve; a curve needs at least two |
| "One curve has to describe one grid" | The rate points differ in resolution or superblock size — one of them was encoded with different settings |
| "No original to measure against" | The source was not in the selection and is not attached with `Load Org YUV` |
| Most superblocks report `lossless` | Expected on flat synthetic content. Use a real sequence for per-superblock work |
