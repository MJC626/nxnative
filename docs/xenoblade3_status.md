# Xenoblade Chronicles 3: port status

Title `010074F013262000`, base game 1.0.0 (SDK 12.3.9), main build `60887a25b66529910275a1066003a002`, sdk build `e8cdceb1e77a8465fe20f21928e8d296`. Profile: [dm/games/010074F013262000](../dm/games/010074F013262000). State as of 2026-10-24 (commit `02d72b0`), measured on an iPhone with a `--no-romfs` IPA and the game files in `Documents/romfs`.

This page replaces `xenoblade3_handoff.md` and `xenoblade3_porting_guide.md`, whose root-cause analysis was partly wrong (see the end of this page).

## Where it stands

| Area | State |
|---|---|
| Boot, language, title screen | Works; 30 fps, Wwise audio |
| New Game → first map `ma01a` | Loads; the opening field and the first battle play |
| Characters | Not drawn (see "Open problems") |
| Cutscene movies | Black: the opening movie `mv01005100.webm` is opened but never plays |
| Other rendering | Partly wrong: missing passes, 9 pipelines rejected, 4 geometry shaders not built |
| Saving | Not exercised yet (the title only probes `save:/bf3system00.sav`, `bf3game00a..04.sav`) |
| Update 2.2.0 | Not supported: the profile registers only build v0 (1.0.0) |

## What was wrong, and the fixes

Each row was confirmed by a device log; patch offsets are in [dm/tools/patch_nso.py](../dm/tools/patch_nso.py) under the two build IDs above.

| Symptom | Cause | Fix |
|---|---|---|
| Title never drew, game asked for `bdat/us/...` | The host's `GetDesiredLanguage` returned `en-US`; XC3 has no American English (NACP languages `0x70de`) and maps `en-US` to folder `us`, which does not exist | `hle_oe.c` language selection, profile `default_lang zh-Hans`, `DM_LANG` |
| Two GPU memory pools (410 / 420 MB) not allocated | The 60 GB guest address region had been cut to 3.0 GB with a 2 GB small-object arena, leaving 1 GB for large blocks | Region and arena resized (final values below); heap budget check at `0xfa1b70` bypassed |
| New Game stalled forever on "Loading..." after reading only `ma01a`'s headers | A 277 MB block (size class 46, reserved as a 384 MB chunk) requested at boot failed: large blocks had only 2.25 GB. The game carries on without it and the map body (`ma01a.wismda`, 443 MB) is never streamed | Large-block space raised to 3.25 GB (next row) |
| 390 MB map buffer failing during the `ma01a` load | Same space exhausted: the allocator never returns a size class's chunks to another class | sdk cut `0x34662c`: regions of 1 GB or more aligned to 32 MB instead of 1 GB, so the region starts at alias +64 MB rather than +0.75 GB; region 4 GB; arena 0.75 GB |
| Crash at boot in `main+0xf9bad8` once an older cut was removed | `operator delete` routes page-aligned blocks of another allocator into the dlmalloc heap's free, whose chunk-header read faults | Heap free cut at `0xf9bac8`–`0xf9bad4`: skip pointers at or below the heap's mspace, but keep the listener's `onFree` call (the previous cut skipped it for every free) |
| Metal assertion "pixel format RG32Uint is not blendable", crash on entering the field | The game enables blending on integer targets; Maxwell ignores it, Metal rejects the pipeline | [nvn_mtl.m](../dm/gfx/nvn/nvn_mtl.m) `pso_desc`: no blending on integer attachments (all games) |
| About 45 shaders missing on the title screen | XC3's maps embed their own SPCH shader packs; `shader_split.py` only read the five global `.wishp` packs, and only a pack at the start of a block | `shader_split.py` reads every pack in every xbc1 block of the entries listed in the new profile key `shader_spch_entries` (5 global packs + 23 maps); 3,659 → 11,319 shaders; the title screen and the first map no longer miss any |
| Two pipelines rejected for integer outputs on the title screen | Translator declares every fragment output float | Profile `shader_facts_manual.tsv` rows `531d637463e53486`, `d3c541d46f389225` |

### Memory layout (5 GB guest reservation, the iOS ceiling every profile stays under)

| Range (from the alias region base) | Use |
|---|---|
| +0 .. +62 MB | sdk AddressRegionManager node area (cut from 1 GB at `0x346430` / `0x3464cc`) |
| +64 MB .. +4.06 GB | The game's virtual address region, 4 GB (`0xfa188c`): small-object arena 0.75 GB (`0xf55010`–`0xf55088`), then 3.25 GB of large-block chunks |
| +4.06 .. +4.125 GB | The game's 64 MB emergency reserve region |
| +4.125 .. +4.375 GB | Headroom |

The profile's memory keys are unchanged (`mem_reserve 0x140000000`, `mem_alias 0x118000000`, `mem_stack 0x10000000`); the comment above them in `game.tsv` still describes an older split and needs updating. The physical RAM probe is bypassed at `0xf56c94` (reports 2.5 GB). In the last run small-object allocations never failed (2.6 million); large blocks failed 69 times, all 65 MB (class 42), right after the opening movie was opened.

## Diagnostics added during the bring-up

All are off unless set in `Documents/env.txt` (or baked in with `--env-file`).

| Switch | What it logs |
|---|---|
| `DM_THREAD_DUMP=<frame>,…,*<N>` | Every guest and host thread with wait object and backtrace at those presented frames (replaces the dump that used to run at frames 60 and 300 for every game) |
| `DM_PCSAMPLE=<start s>,<len s>[,<handle> \| all]` | Guest call-stack sampler: most frequent stacks and an inclusive call tree, every 15 s; `all` samples every thread |
| `DM_CV_KICK=<s>` | Spuriously wakes long-waiting condition-variable waiters (a lost-wakeup test; ruled out for XC3) |
| `DM_STATEPOLL=1` | XC3 observation gates 21–28 ([hle_statepoll.c](../dm/host/hle_statepoll.c)): scene state updates, "is this sound playing" polls, the opening-movie chain, and every failed guest allocation with its size class, size and caller |
| `DM_FSLOG=fslog.tsv` | Every file read (existing switch); offsets into `bf3.ard` map to file names through the decoded archive list in the work folder |

Analysis tools and the decoded archive list (`arh_lang/files.tsv`, 76,345 entries) are in `<NSP folder>/010074F013262000-work/`.

## Open problems

1. **Characters not drawn.** 354 distinct shaders are still missing from the shader map once the field loads (546 `is not in the shader map` lines); characters (`/chr/**/*.wimdo`, `.wismt`) and effects (`*.wipac`) most likely embed their own SPCH packs, as the maps do. The game also uses transform feedback (`nvnCommandBufferBind/Begin/End/DrawTransformFeedback`, 63 calls in one run) and `SetRasterizerDiscard`, which the host does not implement; the translator has no transform-feedback output either. 9 more pipelines are rejected for integer outputs (`RG32Uint` / `R32Uint`, keys in the log), and `nvnColorStateGetBlendEnable` (201 calls), `nvnCommandBufferFenceSync`, `nvnCommandBufferClearColori` and `nvnTextureWriteTexelsStrided` are unimplemented. 4 geometry shaders fail to build (`ef6ec0914d348ad3`, `a841a5d995c26b26`, `95bfab6122539993`, `3220a72a11398bdf`).
2. **Cutscene movies black.** `movie/mv01005100.webm` is opened, then the 65 MB allocations fail and no movie decoder is created (no `rom:/movie` read in `fslog.tsv`).
3. **`c1_full.tsv` is stale** (the host logs it at boot): step 3 of the shader chain procedure was not rerun after the shader tables grew.
4. **Shared-code changes from the first attempts still affect other games**: `dmsh_chain.lock` rewritten for this Mac's toolchain (every other NVN profile's `dmsh_chain.h` is now stale); `hle_hid.c` reports both the handheld and No. 1 controller to every game; `main.m` sets `DM_NVN_SHDUMP` for every game; `hle_oe.c` follows the iOS language for every game and hardcodes XC3's program ID; `hle_fs.c` logs every missing file; the `hle_nvn.c` pool fallback leaks its marker into `nvnMemoryPoolGetFlags` and logs `[WARN]` instead of `[RED]`.

## Corrections to the earlier hand-off documents

- `GetDesiredLanguage` was always called; it did not log before commit `1ce91bf`.
- `oe` notification 15 is FocusStateChanged; `main+0xf604` is a one-shot "initialized" callback, not a notification handler. The cuts at `0xf618` / `0xf61c` and `0x36e7c` change nothing and can go.
- Patching the language getter `0x36ed8` alone would not work: `0x36f28` and `0x36f48` read the global directly. Redirecting `/us/` paths in `hle_fs.c` would not work either: files reach `nn::fs` only after the archive lookup misses, and there are no loose copies.
- The black screen after the memory fixes was not "still loading": the title needed the right language, and New Game later needed address space.
