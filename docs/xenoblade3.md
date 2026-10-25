# Xenoblade Chronicles 3

Title `010074F013262000`, base game 1.0.0 (SDK 12.3.9); main build `60887a25b66529910275a1066003a002`, sdk build `e8cdceb1e77a8465fe20f21928e8d296`. Profile: [dm/games/010074F013262000](../dm/games/010074F013262000). State as of 2026-10-25, measured on an iPhone with a `--no-romfs` IPA and the game files in `Documents/romfs`.

## State

| Area | State |
|---|---|
| Boot, title, New Game, field, first battle | Playable; 30 fps, Wwise audio |
| Rendering | Scenery, UI and effects drawn; no torn blocks since the move to upstream (`xc3` at `dca4729`, checked on the iPhone). Characters (party and enemies) are not drawn |
| Cutscene movies | Not shown: `movie/mv01005100.webm` is opened, then every 65 MB block (size class 42) the player asks for fails |
| Saving | Not exercised yet |
| Update 2.2.0 | Not supported: the profile registers only build v0 (1.0.0) |

## Branch and upstream

The work lives on branch `xc3` of the fork (`origin` = MJC626/nxnative), on top of upstream `seedds/nxnative` (remote `upstream`). Upstream rewrote its history once, so the fork's old `main` shares no commit with it: the XC3 changes were carried over onto upstream's `0f99c35` (2026-10-25) by hand, and the old `main` stays as it was. Keep the XC3 commits few and on top: to follow upstream, `git fetch upstream && git rebase upstream/main` on `xc3`, rebuild, and `git push --force-with-lease origin xc3`.

What upstream already had, so the fork's own versions were dropped: transform feedback (`main0_tf`, rasterizer discard), geometry shaders replayed on compute, `nvnCommandBufferFenceSync`, integer clears, `nvnColorStateGetBlendEnable`, `nvnTextureWriteTexelsStrided`, no blending on integer attachments, `album::SaveCurrentScreenshot`, one chain version for every game (`dm/gfx/nvn/dmsh_chain.h`), `c1_full.tsv` made by each checkout, and the game language chosen at packing (`--language`; the NSP script falls back to a language the game has). Per-game patches now live in the profile: `dm/games/010074F013262000/patches.py` (main) and `dm/tools/sdk_patches.py` (the SDK build). The observation gates moved from 21–28 to 22–29 (upstream took 21).

What the XC3 branch adds to upstream: the profile; `shader_split.py` readers for `bf3.arh` / `bf3.ard` (SPCH packs and GLSLC outputs in every entry, `arh_jobs`), NVSD containers in main and the device dump folder `<version dir>/shader_dump` (read when present, no profile key); the NSP script's `--no-romfs` and its check that every dumped shader the map names is in that folder; `hle_statepoll.c` and its gates; two `oe` stubs; the `blr` case for observation gates in `patch_nso.py`; `DM_NVN_SHDUMP` creating its folder.

## Build and test

The build script lives with the game files, not in the repository: `<NSP folder>/010074F013262000-work/build_xc3_up.sh` (the old `build_xc3.sh` builds the old `main`). It runs `scripts/build_ipa_from_nsp.py` from the `xc3` worktree with the keys, `--no-romfs`, `--language zh-Hans` and the diagnostics in `env.txt` beside it, and writes `~/Desktop/Xenoblade3_slim.ipa`. The Python needs `backports.zstd` (bf3.ard's xbc1 blocks; `pip3 install --user backports.zstd`). The Mac host needs Apple silicon, so on an Intel Mac only the IPA is built.

For shader dumps: set `DM_NVN_SHDUMP=shdump` in the phone's `Documents/env.txt`, copy the phone's `shdump/*.ctl` and `*.code` into `<NSP folder>/010074F013262000_build/extract/v0/shader_dump`, regenerate the profile's tables with `shader_split.py` (the NSP script only checks them for a registered profile), then build.

On the phone: install over the existing app (deleting it removes `Documents/romfs`), keep no `env.txt` in Documents unless a test needs one (it overrides the baked one key by key), and send back `log.txt`, `fslog.tsv` and, when the log has `is not in the shader map` lines, the `shdump` folder (Files app: long press, Compress).

## What was fixed

On the old `main`. The rows on transform feedback, geometry shaders, `FenceSync`, integer clears and blending, the unimplemented NVN calls, the language and the shader split's profile keys describe that branch's own versions; on `xc3` upstream's replace them (Branch and upstream above).

Patch offsets are in [dm/tools/patch_nso.py](../dm/tools/patch_nso.py) under the two build IDs above; each row was confirmed by a device log.

| Symptom | Cause | Fix |
|---|---|---|
| Title never drew; the game asked for `bdat/us/...` | The host's `GetDesiredLanguage` returned `en-US`; XC3 has no American English (NACP languages `0x70de`) and maps it to the missing folder `us` | `hle_oe.c` language selection, profile `default_lang zh-Hans`, `DM_LANG` |
| GPU memory pools (410 / 420 MB) not allocated | The 60 GB guest region had been cut to 3.0 GB with a 2 GB small-object arena | Region and arena resized (layout below); heap budget check at `0xfa1b70` bypassed |
| New Game stalled on "Loading..." after reading only `ma01a`'s headers | A 277 MB block (size class 46) requested at boot failed; the game went on without it and never streamed the map body | More large-block space (next rows) |
| 390 MB map buffer failing during the `ma01a` load | Large-block chunks are never returned across size classes; space ran out | sdk cut `0x34662c`: regions of 1 GB or more aligned to 32 MB instead of 1 GB; region 4 GB; arena 0.75 GB |
| Crash in `main+0xf9bad8` at boot | `operator delete` routes page-aligned blocks of another allocator into the dlmalloc heap's free | Heap free cut `0xf9bac8`–`0xf9bad4`: skip pointers at or below the heap's mspace, keep the listener's `onFree` call (an older cut skipped it for every free) |
| Metal assertion "RG32Uint is not blendable" | The game enables blending on integer targets; Maxwell ignores it | [nvn_mtl.m](../dm/gfx/nvn/nvn_mtl.m) `pso_desc`: no blending on integer attachments (all games) |
| Shaders missing from the shader map | XC3 keeps shaders in SPCH packs inside maps (`.wismda`), stored model streams (`.wismt`), and GLSLC outputs inside `/effect/cmn_assets.wipac` | `shader_split.py`: profile keys `shader_spch_entries all` and `shader_glslc_entries 53899`; 3,659 → 22,474 shaders (chain `f8f101135383`) |
| Pipelines rejected for integer outputs | The translator declares every fragment output float | 11 rows in the profile's `shader_facts_manual.tsv` |
| Unimplemented NVN calls | | `nvnColorStateGetBlendEnable`, `nvnCommandBufferClearColori` / `ClearColorui`, `nvnTextureWriteTexelsStrided` ([hle_nvn.c](../dm/gfx/nvn/hle_nvn.c)) |
| 55 shaders still missing from the map | 35 are `cmn_assets.wipac` GLSLC outputs the game patches at load time; 20 are in no container the split reads | Profile key `shader_dump on`: `shader_split.py` reads the phone's `shdump` (`<key>.ctl` + `<key>.code`) copied to `<NSP folder>/010074F013262000_build/extract/v0/shader_dump`; the NSP script stops if a dumped key the map names is missing there. Not yet confirmed on the device |
| `nvnCommandBufferFenceSync` unimplemented (34 a run) | | Recorded as `OP_FENCE_SYNC`; the replay (inside `QueueSubmitCommands`) gives the sync the fence value of its submission. Not yet confirmed on the device |
| Transform feedback draws rasterized | `SetRasterizerDiscard` was ignored, so the capture draws painted their vertices into the bound targets | `OP_RASTER_DISCARD`: draws are skipped while discard is on, except transform feedback captures (all games). Confirmed: the log shows discard on, no blocks from it |
| Characters (party and enemies) invisible | The game skins them on the GPU with transform feedback: 8 vertex shaders of `shd_post.wishp` capture positions / normals (strides 16–144 bytes) into a buffer with the rasterizer off, and later draws read it. Nothing was captured | Emulated. The layout is in the control section (`+0xdc` output attribute bytes 0x20 + n, `+0x4dc` component counts, `+0x6dc` stride, `+0x6e8` output count; `dmsh_build.py` `xfb_of`). The translator answers Ryujinx's transform feedback queries and lets its fallback store the outputs into `tfe_data0` (moved to descriptor set 0); the capturing shaders' position goes outside the clip volume. The host records Bind / Begin / End / Pause / Resume / DrawTransformFeedback (`OP_XFB_*`), binds the buffer, gives each capture draw `tfe_offset` / `tfe_vertex_count` in a per-draw support buffer copy, ends the pass at End (Metal 4 does not order a vertex store before a later vertex fetch), and draws DrawTransformFeedback with the captured count over the stride of the stream attribute 0 reads. Translator rebuilt (`dmsh_xlate.dll` only; the Ryujinx DLLs are the committed ones), lock updated. Not yet confirmed on the device |
| 9 more pipelines rejected for integer outputs | R32Uint target 3 in `shd_lgt.wishp` lighting variants | Rows added to `shader_facts_manual.tsv`. A rule was tried: 4,353 float outputs are also written as bit casts in the MSL, so the rows stay hand-written |
| Geometry-stage draws drawn without the stage | 79 of the 91 geometry shaders are point-to-quad expansions (4-vertex strips, mostly effects), so the draw without them is points of undefined size | Profile key `gs_skip on`: those draws are skipped until geometry shaders are emulated. Not yet confirmed on the device |

### Memory layout (5 GB guest reservation, the ceiling every profile stays under)

| Range from the alias region base | Use |
|---|---|
| +0 .. +62 MB | sdk AddressRegionManager node area (cut from 1 GB at `0x346430` / `0x3464cc`) |
| +64 MB .. +4.06 GB | The game's virtual address region, 4 GB (`0xfa188c`): small-object arena 0.75 GB (`0xf55010`–`0xf55088`), then 3.25 GB of large-block chunks |
| +4.06 .. +4.125 GB | The game's 64 MB emergency reserve region |
| +4.19 GB .. +4.875 GB | Extra address regions of 512 MB the region grows over once it is used up (`mem_alias` 4.875 GB) |

The physical RAM probe is bypassed at `0xf56c94` (reports 2.5 GB).

The large-block allocator never gives addresses back to its region (a freed chunk stays in its size bucket, unmapped), so after enough churn the 4 GB region is used up even with physical memory to spare: the opening movie's 128 MB chunk, new maps' terrain (193 MB; the player fell through the ground) and character data failed. The stub at `0xf56234` (`patches.py` `STUBS`, observation gate 31, [hle_monolib_alloc.c](../dm/host/hle_monolib_alloc.c)) first joins a run of free chunks lying side by side in the buckets into one chunk (the first descriptor takes the size and class, the others leave their buckets with size 0; no descriptor moves) and hands it on as a chunk from a bucket (`0xf56060`); without such a run it takes an address region of 512 MB with `nn::os::AllocateAddressRegion` above the region, moves the region's offset to it and its size to its end, and the allocator bumps again. Growing alone ran out at the second map: 4.75 GB of addresses used with 2.25 GB mapped, then 2,721 failed 256 MB chunks. Two earlier versions failed: taking chunks back from the buckets left descriptors out of address order (crash in `0xfa19cc`); serving chunks from outside the region played the movie but crashed at its end in the movie heap's free (`0xf9bad8`), because `operator delete` and the chunk queries (`0xf54320`, `0xfa1ca4`..) treat anything outside [base, base + size) as another allocator's.

## Diagnostics added during the bring-up

All off unless set in `env.txt`.

| Switch | What it logs |
|---|---|
| `DM_THREAD_DUMP=<frame>,…,*<N>` | Every thread's wait object and backtrace at those presented frames (replaces the dump that ran at frames 60 and 300 for every game) |
| `DM_PCSAMPLE=<start s>,<len s>[,<handle> \| all]` | Guest call-stack sampler: most frequent stacks and an inclusive call tree every 15 s |
| `DM_CV_KICK=<s>` | Spuriously wakes long-waiting condition-variable waiters (a lost-wakeup test) |
| `DM_STATEPOLL=1` | XC3 observation gates 21–28 ([hle_statepoll.c](../dm/host/hle_statepoll.c)): scene state updates, sound polls, the opening-movie chain, every failed guest allocation |
| `DM_FSLOG=fslog.tsv` | Every file read; offsets into `bf3.ard` map to names through the decoded archive list `<work>/arh_lang/files.tsv` |

## Rendering fix plan

From the iPhone log of `dca4729` (2026-10-25): every shader in the map, 22,528 of 22,529 built (geometry shaders replayed on compute), the skinning captures running (`transform feedback: first capture`), and three red kinds left.

| # | Work | Why | Size | Confirmed by |
|---|---|---|---|---|
| 1 | **Done, to confirm on the device** (`OP_TF_DRAW`, `tf_auto` in `nvn_mtl.m`). **`nvnCommandBufferDrawTransformFeedback`.** Upstream records the captures but its `DrawTransformFeedback` is an empty function (`[RED] DrawTransformFeedback: drawing the captured vertex count is not implemented`). End / Pause keep the bytes captured under the control buffer's address; the draw then draws bytes / stride vertices, stride being that of the vertex stream attribute 0 reads (an `OP_TF_DRAW` replayed as a plain array draw) | The characters: their skinned vertices are captured, and the draws that read them are these (34 a run in the old branch's logs) | Small | Characters drawn |
| 2 | **Geometry shader with transform feedback** (`[RED] geometry draw cannot capture this pipeline (… geometry c01e3ade8e15a842, feedback 1)`, 8 a run): the compute replay of the geometry stage does not write feedback buffers. Capture the geometry stage's output vertices in `nvn_geometry.inc` the way `vs_tf` does for a vertex shader | One effect (`cmn_assets.wipac`'s point-to-quad shader) drawn while a capture is active | Medium | No such red line |
| 3 | **Done, to confirm on the device.** **Large-block address region used up** (layout above): the region grows over extra address regions | The opening movie, new maps' terrain, characters | Medium | Movie, its end, terrain in the second map |
| 6 | **Done, to confirm on the device.** Indexed and instanced transform feedback captures (`tf_draw`): 070083826f16072b of `shd_post.wishp` skins characters with them and was not captured (`[RED] transform feedback draw that is indexed, indirect, instanced …`). An instanced capture runs once per instance with its own write pointer; an indexed one captures its index range into scratch by vertex number, then `tf_gather` copies it out in index order | Characters missing at first, torn shapes | Medium | No such red line; characters from the start |
| 7 | **Done, to confirm on the device.** Geometry draws waited for the GPU (`mdi_wait`, the submission so far committed and finished) before every indexed or indirect draw; the particles' DrawTransformFeedback took 3–12 ms each (`DM_NVN_OPPROF=1`). It now reads the capture's count as the last finished submission left it, and other waits happen once per submission and GPU write epoch, none when the earlier submissions are done | Frame rate in the second map | Small | `geometry draws per frame` line: few waits |
| 8 | Occlusion queries: about 60 a frame, most results 0 early on. The `counter reports per frame` line now splits zeros into counted draws, no draws, and every draw skipped before counting | Characters culled until a battle? | Diagnosis | |
| 9 | **Done, to confirm on the device.** Field grass torn into long strokes across the screen. Found with a pass dump taken at a screenshot (`DM_SHOT_PASSDUMP=1`, `DM_NVN_PASSDUMP_ALL=1`): one scratch buffer (`0x705e4c13d0`, 576,000 bytes) is reused within a frame for the grass vertices (10-byte stride, read by `5827febe006f85ac` with about 200 instances), the proxies' box corners (the `070083826f16072b` captures, each from offset 0) and the skinned characters. The grass vertices come from `5f6491f9bda5ae4d` under rasterizer discard, which writes them with atomic float operations in a helper function SPIRV-Cross defines before `main0`; `vs_stores` searched only `main0`, so the draw had no store-only entry point and was skipped | Tearing on new maps | Small | No `draw under rasterizer discard … (skipped)` lines |
| 10 | Characters missing in a new game: in the pass dump of such a frame the game submits no skinned character at all (no skinning draws, no box proxies), while a loaded save draws about 140 skinning draws a frame. The only GPU results the game reads back are its occlusion queries, read by `main+0xbb5c7c` from the other of two slots without checking that the result landed. Test: `DM_NVN_SAMPLES_MIN=1` makes every result at least 1 | Characters missing until a battle | Diagnosis | Characters at the start with the switch |
| 4 | The one SPIR-V failure: `Subgroup builtins are not available in this type of function` (`dmsh_build.log`) | | Small | Builds |
| 5 | UI boxes drawn black (enemy HP bar's right half, portrait frames, counters; seen before the move to upstream): check again on `xc3` | | Diagnosis | |

## Before calling it supported

- Undo or gate the early shared-code changes that affect every game: `dmsh_chain.lock` rewritten for this Mac's toolchain (every other NVN profile's `dmsh_chain.h` is stale; use a local override); `hle_hid.c` reports both the handheld and No. 1 controller to every game; `main.m` sets `DM_NVN_SHDUMP` for every game; `hle_oe.c` follows the iOS language for every game and hardcodes XC3's program ID (use a profile `languages` key from the NACP); `hle_fs.c` logs every missing file; the `hle_nvn.c` pool fallback leaks its marker into `nvnMemoryPoolGetFlags` and logs `[WARN]` instead of `[RED]`.
- Remove cuts that change nothing or only masked failures: language `0x36e7c`, `0xf618`, `0xf61c`; null checks `0xd942a0`, `0xb7fcf8` / `0xb7fe90` once a run shows they are not reached.
- Let `build_ipa_from_nsp.py` run `c1_guard.py` for every profile (or fail on a stale `c1_full.tsv`) instead of only provisional ones.
- Memory on the phone: watch `footprint` over long sessions; decide on `com.apple.developer.kernel.increased-memory-limit`. Saving: play to a save point and restart.
- Profile: fix the memory comment, drop "provisional", add the game to `README.md` and `AGENTS.md`.
- Later: update 2.2.0 (carry every cut and gate with `dm/tools/nso_fnmap.py`), performance in the field and battles.

## Corrections to the first hand-off notes

`GetDesiredLanguage` was always called (it did not log before `1ce91bf`); `oe` notification 15 is FocusStateChanged and `main+0xf604` is a one-shot "initialized" callback; patching the language getter `0x36ed8` alone would miss `0x36f28` / `0x36f48`; redirecting `/us/` paths in `hle_fs.c` could not work (files reach `nn::fs` only after the archive misses). The black screens after the memory fixes were the language, then address space, not slow loading.
