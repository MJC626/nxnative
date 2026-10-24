# Xenoblade Chronicles 3: next steps

Follows [xenoblade3_status.md](xenoblade3_status.md). Ordered by what most improves play; each step names how a device log confirms it.

## 1. Characters and scene rendering

| Step | Work | Size | Confirmed by |
|---|---|---|---|
| 1.0 | Find the 354 missing shaders: scan every `bf3.ard` entry (not only maps) for SPCH packs on the Mac, key each shader as the host does (`dm_ctl_sha16` of its control section) and match the keys the log reports; list the matching entries in `shader_spch_entries` (or select entries by file type) and rerun the shader split. The device's `shdump` folder is not needed for this, only the log | Medium | No `is not in the shader map` lines in the field |
| 1.1 | Add the 9 integer-output rows the last log printed to `dm/games/010074F013262000/shader_facts_manual.tsv`, then run the shader chain procedure (AGENTS.md): `dmsh_chain.py --header` after the tables change, rebuild shaders, regenerate `c1_full.tsv`, rebuild the host | Small | No `failed to create render pipeline` and no `c1_full.tsv is stale` lines |
| 1.2 | Implement `nvnColorStateGetBlendEnable` (read back what `SetBlendEnable` stored) and `nvnCommandBufferClearColori` (integer clear; the clear-quad path already handles uint / int attachments) | Small | The `[RED] NVN unimplemented` lines for them disappear |
| 1.3 | Implement `nvnCommandBufferFenceSync` (a sync signalled at that point of the command stream) and `nvnTextureWriteTexelsStrided` (CPU upload with a row stride) | Small | Same |
| 1.4 | Find what XC3 uses transform feedback for. Hook the four calls with logging (buffer, size, primitive, bound program keys) and dump one frame with `DM_NVN_PASSDUMP`; likely character skinning or cloth, captured once and drawn with `DrawTransformFeedback` | Medium | A list of the programs and buffers involved |
| 1.5 | Implement transform feedback: the translator emits the vertex shader's captured outputs as buffer writes (layout from the NVN program's transform-feedback state), the host runs that vertex stage with rasterization off (`SetRasterizerDiscard`), and `DrawTransformFeedback` draws the vertex count the capture recorded | Large | Characters appear |
| 1.6 | The 4 geometry shaders that fail to build (`dmsh_build.log`): fix in the translator or emulate | Medium | No `(geometry) failed to load` lines |

## 2. Cutscene movies

| Step | Work | Size | Confirmed by |
|---|---|---|---|
| 2.1 | Make the 65 MB (class 42) blocks fit. Measure the arena's peak commit first (log it in `hle_statepoll.c`); if it stays well under 0.75 GB, shrink it to 0.625 GB, or raise the region past 4 GB with a two-instruction size (needs a code cave, since `0xfa188c` holds one instruction; 0.25 GB of headroom remains above the reserve region) | Small–medium | `large … / 0 failed` after the movie opens |
| 2.2 | Check the movie path end to end: `movie::Extractor` (SDK, subsdk0) reading `rom:/movie/mv01005100.webm`, the host's `movie::Decoder` (VP9 via libvpx, `hle_movie.c`), and the NV12 frame upload to the game's textures (as Age of Calamity needed) | Medium | `rom:/movie` reads in `fslog.tsv`, the opening movie on screen |

## 3. Stability and memory on the phone

- Watch `footprint` in the `[nvn] on-screen` lines through the first hours of play; without the increased-memory-limit entitlement iOS ends the app at its limit (lower on 6 GB devices). Decide whether the sideloading tool can carry `com.apple.developer.kernel.increased-memory-limit`.
- Saving: play to the first save point; check `save commit` lines and that `bf3system00.sav` / `bf3game00a.sav` reappear after a restart.
- Remove the null-check cuts that only masked earlier failures (singleton at `0xd942a0`, container at `0xb7fcf8` / `0xb7fe90`) once a run shows they are not reached, so a real failure crashes loudly instead of drawing nothing.

## 4. Clean-up before committing as a supported game

- Undo or gate the shared-code changes listed in the status page (open problem 4): restore `dmsh_chain.lock` and use a local override for this Mac's toolchain; restore `hle_hid.c` or gate it behind a profile key; drop the unconditional `DM_NVN_SHDUMP` in `main.m`; replace the hardcoded program ID in `hle_oe.c` with a profile `languages` key filled from the NACP (`0x70de`); put `hle_fs.c` logging back behind `DM_FS`; keep `[RED]` and a separate marker in the pool fallback.
- Remove the language cuts that do nothing (`0x36e7c`, `0xf618`, `0xf61c`).
- Profile: fix the memory comment in `game.tsv`, drop "provisional", add the profile to `README.md`'s game list and `AGENTS.md`.
- Delete the two superseded hand-off documents.

## 5. Later

- Performance: frame time is about 33 ms with a light GPU load on the title; measure the field and battles before tuning (`frame step`, pipeline prewarm list `pso_manifest.tsv` from `pso_user.tsv`).
- Update 2.2.0 (the XCI carries it): carry the addresses of every cut and gate to the new build with `dm/tools/nso_fnmap.py`, check each by disassembly, and register the build in the profile.
- Rendering polish: the window crop path, transform-feedback-dependent effects, and the remaining shader-chain failures (7 of 11,319 shaders).
