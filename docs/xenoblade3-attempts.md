# Xenoblade Chronicles 3 on the iPhone: our own attempt (branch `xc3`), October 2026

This is the record of the fork's own bring-up of Xenoblade Chronicles 3 (title `010074F013262000`, base game **1.0.0**, `main` build `60887a25b66529910275a1066003a002`, SDK 12.3.9) on top of upstream nxnative `0f99c35`. It was stopped on 2026-10-27, when upstream gained its own XC3 profile (update **2.2.1**, `main` build `82d187fe9ef9be921b8e1b1cddf7bd28`). From then on the fork builds upstream unchanged (branch `upstream-sync`, see the end). The running notes from the bring-up are in [xenoblade3.md](xenoblade3.md).

Everything below was tested on one iPhone (sideloaded slim IPA, game files in the app's `Documents/romfs`), with `log.txt`, `shdump/` and screenshots coming back by hand. The host changes are generic, so they may help upstream or other games. The XC3-specific parts were made for 1.0.0 and do not carry over to 2.2.1 (other build, other addresses).

## Final state on the phone (1.0.0, last build `af3a61b`)

- Boots, plays the opening movie and its end, reaches the field and battles, cutscenes run in real time.
- Characters and enemies visible from the start of a new game, with real occlusion results (no workaround).
- No tearing (grass and skinning fixed), shadows without black blocks.
- Still wrong: flicker of the whole picture while the camera moves (dark / bright, strongest on water), some character trailing, 15–30 fps (the GPU at 35–67 ms a frame in heavy scenes), some effects missing while their shaders are not dumped yet.

## What was fixed, and how it was found

| Symptom | Cause | Fix (commit) |
|---|---|---|
| Crash or missing assets once the game had churned through memory (opening movie, new maps' terrain, the player falling through the ground) | The large-block allocator never returns addresses to its region (free chunks stay in their size bucket, unmapped); the region cut to 4 GB ran out with physical memory to spare | Observation gate 31 at `main+0xf56234` (`dm/host/hle_monolib_alloc.c`): first join runs of free chunks side by side into one chunk (no descriptor moves; the first descriptor takes size and class, the others are left with size 0), otherwise grow the region over an extra address region above it (`bce2642`, `1f39840`). Two earlier versions failed: taking chunks from other buckets broke the descriptors' address order (crash in `main+0xfa19cc`); serving chunks from outside the region crashed at the movie's end, because `operator delete` and the chunk queries treat anything outside `[base, base + size)` as another allocator's |
| Characters invisible (early) | Skinning runs as vertex shaders that write storage buffers under rasterizer discard; the host skipped them | `vs_stores` in `dmsh_build.py` gives such shaders a store-only entry point; the host runs them (`f79ba0b`, `d1de9f9`) |
| Box proxies empty, characters missing | Indexed and instanced transform feedback captures were dropped (`070083826f16072b` builds the proxies' box corners) | Instanced: one draw per instance with its own write pointer; indexed: capture the index range by vertex number into scratch, then `tf_gather` copies in index order (`7eb88f6`) |
| GPU particles | Geometry shaders under transform feedback refused | Drawn, and captured on the GPU (`geo_tf` kernel, count in the control buffer) (`8bc99fd`) |
| 3–12 ms per particle draw, up to 11 CPU–GPU waits a frame | The geometry-shader emulation committed and waited for the GPU before every indexed or indirect draw | Feedback draws of a geometry capture read the last finished count; indexed draws wait only when the GPU wrote their index range in this submission (`7eb88f6`, `874d283`) |
| Long strokes across the screen on the field (torn grass) | The grass vertices come from `5f6491f9bda5ae4d` under rasterizer discard, whose atomic stores SPIRV-Cross puts in a helper function before `main0`; `vs_stores` searched only `main0`, so the draw was skipped and the grass read a reused scratch buffer | `vs_stores` searches the whole source (2 more shaders; chain `e51e93d2a42b`) (`5ba42bf`) |
| Characters missing in a new game, back with every occlusion result forced to 1 | Metal 4's default visibility result type resets the visibility data with each render encoder; every counting pass has its own slot in one buffer, so the counts of a submission's earlier passes came back 0 and the game culled its characters | Passes accumulate (`MTLVisibilityResultTypeAccumulate`) into slots zeroed when a submission takes the buffer (`874d283`). Zero results dropped from about 9,800 to 6,600 per 300 frames against 13,000 nonzero |
| Black blocks across ground and walls (shadows) | The game renders each shadow map through one texture object and samples it through another over the same storage (serials 1903 → 1904, 6055 → 6056); they differ only in flags (`0xc` / `0x8`), and the host's sharing key compared the whole description | One texture for storage seen through objects that differ only in flags, stride or block height (`448ed71`) |
| Shaders missing from the shader map | The game builds shaders at run time (no GLSLC in the code; dumped code differs from the nearest shipped shader in 20 or more instructions) | Dumped on the phone (`DM_NVN_SHDUMP=shdump`), merged through `<version dir>/shader_dump`: 22,800 shaders at the end, still growing as the game goes on |

## Tried and dropped

- **Forcing occlusion results to at least 1** (`DM_NVN_SAMPLES_MIN=1`, `5ba42bf`): characters visible, but effects and shadows disappeared (the game reads some counts as coverage). It was a test that pointed at the visibility reset.
- **Depth EQUAL as LESS_EQUAL with a small negative bias** (`DM_NVN_DEPTH_EQ`, `f32c383`): the always-zero queries stayed zero, and the picture turned white. Pre-pass / G-buffer depth mismatch was not the cause.
- **Present waits for the frame's GPU work** (`DM_NVN_PRESENT_SYNC=1`, `88c3e59`): the flicker stayed, so it is not the occlusion results arriving late (the exposure histogram reads them one frame later without waiting).

## Open findings at the stop

- **Flicker in motion.** Most likely temporal anti-aliasing: TAA writes new texture objects every frame (`4460` → `4506`, `6439` → `6440`) and samples fixed ones (`4396`, `4400`, `4494`, `4398`). If the game writes a target in one format and samples it in another over the same storage, the host keeps two textures and the history goes stale. The last build (`af3a61b`) logs every texture whose storage overlaps a texture it cannot share (`overlaps a texture … kept apart`) and dumps two frames' passes every 3,600 frames (`DM_NVN_PASSDUMP_EVERY`); no log came back.
- **Queries that always count 0:** the full-screen depth-EQUAL test `c0fa1056`, the boxes `8a42b69f` (indexed quad lists with 8-bit indices against the full-size depth), and the decals testing stencil `EQUAL 0x80`. The main depth-stencil buffer (`4395`, Depth32Float_Stencil8) is sampled as stencil through a second object (`4397`, X32_Stencil8); stencil reads may still be wrong.
- **Exposure:** 12 occlusion queries a frame on a 128×128 luminance target form a histogram (the counts add up to 16,384).
- **GPU time:** about 1.7 GB of attachment loads and stores a frame; the TBDR optimizations are off for bindless games.

## Diagnostics added (all generic, in `nvn_mtl.m` / `hle_nvn.c` / `front_ios.m`)

`DM_NVN_OPPROF=1` (replay CPU time by operation), the `counter reports per frame` line (report types; samples-passed results split by why they are 0), the `occlusion query frame … samples` lines (each result with its first counted draw), `geometry draws per frame` (waits), `DM_SHOT_PASSDUMP=n` (a screenshot on the phone dumps the next n frames' passes), `DM_NVN_PASSDUMP_EVERY=n,k`, `DM_NVN_VIS_RESET=1`, the texture sharing / overlap lines, transform feedback statistics.

## Workflow notes

- Builds: `<NSP folder>/010074F013262000-work/build_xc3_up.sh` (slim IPA, `--no-romfs`, env switches from `env.txt`). The work folder's build directory from this attempt is kept as `010074F013262000_build.xc3-own-attempt` (extracted 1.0.0 files, 1,264 dumped shader files in `extract/v0/shader_dump`).
- The Mac (Intel) cannot run the game, so shaders cannot be captured there as for other games; and `xcodebuild` repeatedly crashed with APFS "compressed data hash mismatch" page-in errors (reading the dyld shared cache files once made it work again): check the disk.

## From now on: upstream as it is

- Branch `upstream-sync` of the fork = `upstream/main`, no changes of its own; worktree `~/Documents/nxnative-upstream`.
- `<NSP folder>/010074F013262000-work/build_xc3_upstream.sh` fast-forwards it to upstream, pushes it to the fork, builds with upstream's `scripts/build_ipa_from_nsp.py` unchanged (Python 3.12 venv `venv312` next to it), then deletes `Payload/DmIOS.app/romfs.img` from the IPA: a slim IPA, the game files on the phone in `Documents/romfs` (or `Documents/romfs.img`, `scripts/push_romfs.sh`).
- Upstream supports **2.2.1** only. The XCI in the NSP folder holds the base game and a 2.2.0 update, and upstream takes one title per file: the 2.2.1 update NSP (title `010074F013262800`) has to be added next to it.
