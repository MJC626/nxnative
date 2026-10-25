# Xenoblade Chronicles 3 (010074F013262000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py (this game's sdk: the node area and region alignment cuts)
from nso_asm import B, BEQ, CBZ, CMPI
CUTS = {
  # Xenoblade Chronicles 3 1.0.0 (v0) main: virtual address region reservation 60 GB (0xf00000000) → 4 GB (0x100000000),
  # internal dynamic allocation arena 4 GB (0x100020000) → 0.75 GB (0x30020000),
  # allocator capacity check bypass (allows large GPU texture pools to allocate from 3.25 GB large-block headroom),
  # (with 2.25 GB a 277 MB block, size class 46, failed at boot after the two 512 MB GPU pool chunks: New Game then read only map ma01a's headers and stalled on its loading screen;
  # with 2.75 GB New Game loaded map ma01a's files and then failed a 390 MB block (class 47) and 65 MB ones; the sdk cut at 0x34662c drops the 1 GB alignment that put the region at
  # alias +0.75 GB, so the 4 GB region starts at +64 MB, ends at +4.06 GB, and the 64 MB reserve region plus 0.25 GB headroom fit under the 4.375 GB alias region),
  # and physical RAM probe bypass (original probes memory by allocating 1 MB pages in a loop until failure; on host SVC 0x2C never fails,
  # leaving all 3 GB mapped and held in memory, causing subsequent NVN memory pools to fail). Bypass directly sets available RAM to 2.5 GB.
  # iOS caps guest address space to 5 GB (alias region 4.375 GB).
  '60887a25b66529910275a1066003a002': [
    (0xfa188c, 'e001c0d2', '2000c0d2', 'virtual address region size: mov x0,#0xf00000000 (60 GB) → mov x0,#0x100000000 (4 GB)'),
    (0xf55010, 'e98340b2', 'e9ff9fd2', 'arena alloc check: mov x9,#0x1ffffffff → movz x9,#0xffff'),
    (0xf55020, '4900a0f2', '4900a6f2', 'arena alloc check: movk x9,#2,lsl #16 → movk x9,#0x3002,lsl #16'),
    (0xf55078, 'e97b5fb2', '2900a692', 'arena base adjust: mov x9,#-0x200000000 → movn x9,#0x3001,lsl #16'),
    (0xf5507c, 'c9ffbff2', '090080f2', 'arena base adjust: movk x9,#0xfffe,lsl #16 → movk x9,#0,lsl #0'),
    (0xf55084, '4800a0d2', '4800a6d2', 'arena capacity: mov x8,#0x20000 → movz x8,#0x3002,lsl #16'),
    (0xf55088, '2800c0f2', '1f2003d5', 'arena capacity: movk x8,#1,lsl #32 → nop'),
    (0xfa1b70, '1f0109eb', '09000014', 'allocator capacity check: cmp x8, x9 → b 0xfa1b94 (bypass artificial heap cap)'),
    (0xf56c94, 'a80240f9', '0800b4d2', 'RAM probe bypass: mov x8,#0xa0000000 (2.5 GB pool)'),
    (0xf56c98, 'e8feffb5', 'a80200f9', 'RAM probe bypass: str x8,[x21]'),
    (0xf56c9c, '940240f9', 'c94100f0', 'RAM probe bypass: adrp x9,0x1791000'),
    (0xf56ca0, 'f6031faa', '29dd45f9', 'RAM probe bypass: ldr x9,[x9,#0xbb8]'),
    (0xf56ca4, '8002168b', '280100f9', 'RAM probe bypass: str x8,[x9]'),
    (0xf56ca8, '0102a052', '20008052', 'RAM probe bypass: mov w0,#1'),
    (0xf56cac, '15470194', 'f2ffff17', 'RAM probe bypass: b 0xf56c74'),
    (0xd942a0, '09108052', 'e80000b4', 'null check on singleton: cbz x8, 0xd942bc'),
    (0xd942a4, '09550039', '1f7101f8', 'null check on singleton: stur xzr, [x8, #0x17]'),
    (0xd942a8, '01590039', '09108052', 'null check on singleton: mov w9, #0x80'),
    (0xd942ac, '1f5d0039', '09550039', 'null check on singleton: strb w9, [x8, #0x15]'),
    (0xd942b0, '01610039', '01590039', 'null check on singleton: strb w1, [x8, #0x16]'),
    (0xd942b4, '1f9101b8', '01610039', 'null check on singleton: strb w1, [x8, #0x18]'),
    (0xd942b8, '1fd10178', '1f2003d5', 'null check on singleton: nop'),
    # Heap free (dlmalloc heap class, vtable 0x1772570 slot +0x40): operator delete also routes page-aligned blocks of another allocator here (first seen: 0x70803a0000 freed by the
    # archive reader thread at boot); reading their chunk header at ptr-8 faults (main+0xf9bad8). Skip pointers at or below the heap's mspace [this+0x30] (this also covers ptr == 0),
    # and keep the listener's onFree call for every real free (an earlier version of this cut skipped it for all frees: whatever tracks memory through the listener never saw a release
    # and New Game stayed on its loading screen). The null-mspace check gives way: a heap is only freed into after it is set up
    (0xf9bac8, '010300b4', '081840f9', 'heap free: cbz x1,skip → ldr x8,[x0,#0x30] (mspace)'),
    (0xf9bacc, '081840f9', '7f0208eb', 'heap free: ldr x8,[x0,#0x30] → cmp x19,x8'),
    (0xf9bad0, 'e10300aa', 'c9020054', 'heap free: mov x1,x0 → b.ls 0xf9bb28 (pointer at or below the mspace, or null: not this heap\'s, skip)'),
    (0xf9bad4, 'a80200b4', 'e10300aa', 'heap free: cbz x8,skip → mov x1,x0 (the listener call that follows is unchanged)'),
    (0xb7fcf8, 'a02300d1', '611300b4', 'tutorial container null check: sub x0,x29,#8 → cbz x1,0xb7ff64'),
    (0xb7fcfc, '78890694', 'a1831ff8', 'tutorial container null check: bl 0xd222dc → stur x1,[x29,#-8]'),
    (0xb7fe90, 'a02300d1', '410300b4', 'tutorial container null check: sub x0,x29,#8 → cbz x1,0xb7fef8'),
    (0xb7fe94, '12890694', 'a1831ff8', 'tutorial container null check: bl 0xd222dc → stur x1,[x29,#-8]'),
    (0x36e7c, '53a33c94', '1ebf3c94', 'language init: bl 0xf5fbc8 → bl 0xf66af4 (call GetDesiredLanguage directly)'),
    (0xf618, 'e80100b4', '179e0094', 'language notification: cbz x8,0xf654 → bl 0x36e74 (unconditional language init)'),
    (0xf61c, '169e0094', 'c80100b4', 'language notification: bl 0x36e74 → cbz x8,0xf654 (branch after language init)'),
  ],
}
OBS = {
  # Observation gates 22-29 (OBS_STATE_CHILD .. OBS_ALLOC_SMALL in dm/host/dm.h; callbacks in dm/host/hle_statepoll.c, attached with DM_STATEPOLL=1)
  # Xenoblade Chronicles 3 1.0.0 main: the scene / sequence framework updates each child state through vtable +0x40 and treats a return of 1 as done (main+0x550c2c in the
  # child loop, main+0x5505d0 for the current state). The New Game loading screen never finishes; the host logs which state classes keep returning "not done" (hle_statepoll.c, DM_STATEPOLL=1)
  '60887a25b66529910275a1066003a002': [
    (0x550c30, 22, '7100041f', 'child state updated (x21 → entry {state object, ...}, w0 = result, 1 = done); orig cmp w0, #1'),
    (0x5505d4, 23, '7100041f', 'current state updated ([[x19+0x20]-0x10] = state object, w0 = result, 1 = done); orig cmp w0, #1'),
    (0x502f94, 24, 'f9400bf3', 'sound handle still playing? (main+0x502f4c; x19 = handle {sound manager, u32 id}, w0 = 1 still playing; caller = [x29 + 8]); orig ldr x19, [sp, #0x10]'),
    (0x2b3c30, 25, 'd102c3ff', 'event movie player update entry (x0 = player: +0x80 state 0 open / 1 opening / 2 playing / 3, +0x218 must be set before it opens, +0x220 name); orig sub sp, sp, #0xb0'),
    (0x343d68, 26, 'd10503ff', 'movie request entry (x1 = request: +0x10 movie name, +0x34 / +0x38 flags; builds movie/%s.webm when both are 0); orig sub sp, sp, #0x140'),
    (0x3e874, 27, 'a9bd7bfd', 'movie open entry (x0 = player, +0x40 must be set; x2 = path); orig stp x29, x30, [sp, #-0x30]!'),
    (0xf558e8, 28, 'b94003fb', 'large-block allocation returned (main+0xf55fb4; x0 = block or 0, x27 = size class, x25 = size; caller of main+0xf542a0 = [x29 + 8]); orig ldr w27, [sp]'),
    (0xfa1bb0, 30, '2a0003e8', 'heap allocation returned (main+0xfa1b3c, after bl main+0xf542a0; x19 = heap: +0x30 bytes in use, +0x40 cap, +0x48 count; w0 bit 0 = done); orig mov w8, w0'),
    (0xf5585c, 29, '8b1b16c8', 'small-object allocation returned (main+0xf55a90; x0 = block or 0, x27 = size class, x25 = size); orig add x8, x22, x27, lsl #5'),
  ],
}

# The large-block allocator (main+0xf55fb4) keeps a list of free chunks per size bucket and takes new chunks from the 4 GB address region by moving one offset forward
# (main+0xf5611c: [region+8] += chunk size, 64 KB aligned, up to [region+0x18]); a freed chunk goes back to its own bucket's list with its memory unmapped and its addresses
# kept, never to the region. On the Switch the region is 60 GB and never runs out; at 4 GB the offset reached the end after the buckets' peaks (512 KB chunks alone peaked at
# 1.3 GB), and every later new chunk failed: the 65 MB the opening movie asks for, the 193 MB of a new map's terrain (the player fell through the ground), 16 MB character data.
# The stub: where the offset would pass the end (the failure path at main+0xf56234, reached from the bound check at +0xf56148), observation callback 31 (dm/host/hle_monolib_alloc.c)
# first joins a run of free chunks lying side by side in the buckets into one chunk of the size asked for (its first descriptor takes the size and class, the others leave their
# buckets; no descriptor moves, so their order by address stays), and the allocator goes on as with a chunk taken from its bucket (main+0xf56060: x28 = the descriptor, the
# allocator's lock released). Without such a run it grows the region over an extra address region above it (the offset moves to its start, the end to its end), and the allocator
# takes the chunk again from the top (main+0xf5611c), so every check against the region (main+0xf54320: operator delete's routing, the chunk queries at main+0xfa1ca4..) covers it
def _stubs_v0(ctx):
    ctx.site(0xf56234, 'e00319aa')   # mov x0, x25: the failure path's first instruction (unlock the allocator, return 0)
    st = ctx.here()
    ctx.gate(31)                     # host: x0 = 2 with x28 = a joined chunk's descriptor (lock x25 released), 1 when the region grew, 0 = no room
    fail = ctx.here() + 20
    ctx.emit(CBZ(ctx.here(), fail, 0))
    ctx.emit(CMPI(0, 1))
    ctx.emit(BEQ(ctx.here(), ctx.here() + 8) | 1)    # b.ne: a joined chunk
    ctx.emit(B(ctx.here(), 0xf5611c))                # the bump again (x20 = allocator, x21 = class, x25 = its lock, held)
    ctx.emit(B(ctx.here(), 0xf56060))                # a chunk from a bucket (x19 = the bucket's lock, x27 = its list, w23 = bytes to map now)
    ctx.emit(0xaa1903e0)             # mov x0, x25 (the original instruction)
    ctx.emit(B(ctx.here(), 0xf56238))
    ctx.put(0xf56234, [B(0xf56234, st)], 'large-block allocator, address region used up: free chunks joined, or the region grows over an extra one (dm/host/hle_monolib_alloc.c)')
STUBS = {'60887a25b66529910275a1066003a002': _stubs_v0}
