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

- Regression suite: `./tests/run-regression.sh` (36 tests). It builds and runs headless.
- Unit tests are compiled **without Qt** to prove the core stays Qt-free.
- Build: `./scripts/build.sh` applies the patches and builds.
- A "successful" build can still hide a file that never compiled — stale objects mask compile
  errors. Clean-rebuild before blaming a change.

Do not run builds or tests yourself; you are read-only. Tell the user the command to run.
