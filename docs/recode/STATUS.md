# Recode status and to-do (2026-11-25)

Local only (the repository allows no notes). Source: `notes\progress.json`, `notes\NEXT-SESSION.md`,
`notes\PICTURE-FAULTS.md`, `RECODE.md` 6.0 and 6.1. Trust `NEXT-SESSION.md` if they disagree.

## Stage

Core steps (RECODE.md 6.1) in order. Done: R1 (front end), C0 to C11 except C12 and C13, C9a,
C9b (recorded as not needed), C10, C11, P1, P2, M1, I0 to I7, C4b, C5, C5b, C8.
Open: **C12** (stage threads, resolve on its own thread, record outside the GPU mutex) and
**C13** (full route default; item 5, the Yōtei GPU hang, is open). I8 is built but not marked
done: its "done when" asks that LEGO and FF clips match the old path, and they do not.
After the core: R5, R6, RT (ray tracing), U (shapes no base title uses).

Best frame so far (progress.json; ms per frame, target in brackets):

| Title | Best | Target |
|---|---|---|
| FF Resonance | 48.1 (r861) | 10 (100 fps) |
| LEGO | 51.6 (r858) | 10 (100 fps) |
| Age of Mythology | 50.1 (r861, cold) | 10 (100 fps) |
| 007 First Light | 245.6 (r824) | 16.7 (60 fps) |
| Ghost of Yōtei | 98.9 (r859) | 16.7 (60 fps assumed; no user target yet) |

## Performance to-do

1. **C12**: resolve on its own thread; record outside the GPU mutex (measured not limiting).
   Already in: stage costs per loop stage, host copy thread default, lock-free device read,
   residency touch per frame (FF 78.5 to 51.8 ms, LEGO 64 to 45.9 ms).
2. **C13 item 5, Yōtei GPU hang**: an indirect dispatch reads arguments the CPU has not written
   yet (queue 0x30's sort scratch). Ruled out (RECODE.md C13): stale conditions, snapshot
   arguments, early labels, the host copy thread, late async compute, guards, label-table waits.
   Open leads: who writes the first argument record (address-store probe); the sort count varies
   (4,455 vs 65,681), so compare the count producer's output across runs.
3. **C13 mirror-only draw inputs**: mirror inputs are the default (c8c2026231); known buffers
   still through host imports and the staging-shadow layer. Per-title loop costs (10 s):
   FF copy reads 3.7 s + draws 3.3 s; AoM draws 5.5 s + dispatches 3.4 s; LEGO loop half idle
   (upstream limit).
4. **Fault path**: 500 to 1,150 write faults per frame, 3 to 7 ms in the handler (C5b). FF:
   800 to 870 faults and 12 to 13 ms in the handler per 44 ms frame (C5). Growing unit is
   off (more hashing than it saves).
5. **Build queue**: a build the loop waits on should go first; cold modules take 40 to 55 s and
   block the loop in "dispatch precheck" (Yōtei).
6. **Cold start**: recorded in RECODE.md section 10 as cold waits AoM 1.45 s / 80 s and FF
   1.62 s / 90 s (the two numbers are as written there); not treated as a blocker; re-check
   after C12.
7. **Yōtei loading**: 10.9 GB commit + 7.4 GB VRAM before the first scene (put to the user, no
   answer recorded).
8. **Re-measure after C12/C13**: LEGO, AoM, FF pairs (alternating, 8+ runs per side, machine
   left alone) before any further per-piece A/B (rule: optimise after C13).

## Picture to-do (PICTURE-FAULTS.md)

- **LEGO A (open)**: about 3 in 10 runs wrong: dark or washed menu, black after Continue, or a
  one-frame grey scene with a black tower. In black runs the 2572x1448 12-mip chain at
  0x303890000 holds 11.2 M non-finite values (right runs 106,728). Next: the NaN source.
- **LEGO B (open, both paths)**: no glossy reflections; the 384-layer probe cube array (vk122)
  is empty.
- **LEGO C (open, both paths)**: black arch and NaN corner pixels.
- **FF A (open)**: the 3D layer is the normals G-buffer (flat 0x8080ff with a few shapes), not
  the lit scene; the HUD is right. With mirror inputs the world draws (detail 0.12 to 2.46) but
  the final layer is still normals. Next: which record the composite pass or flip reads
  (`[recode-uses]` before the flip, `APS5_RECODE_TRACE_TARGETS`); mirror and guest memory
  disagree for small per-draw inputs in FF's pools at 0x11 to 0x14 GiB (sampled compare
  planned).
- **AoM A (open)**: world black; particles and clouds draw since 06b0c57752; terrain, units,
  buildings stay black. Old path has no reference for the world (black after the splash too).
- **FF B**: runs ended at 37 to 75 s; worked around (`-ExistingPath drop`). Real fix was
  mesh/tessellation on the recode (M1), done.
- **Ghost of Yōtei, 007**: no picture verdict recorded yet; Yōtei reaches the intro (r835),
  007 reaches its title screen (ca5ef8ed8c).

## Repository state

- Pushed to the fork: `origin` = https://github.com/XatiX/AnyPS5, branch `recode/main`, head
  `d1cd00f07f` (contributing section) on top of `01dc36e9b7` (another session's commit).
- CONTRIBUTING: the recode section is in the root `CONTRIBUTING.md` (the convention check
  rejects any new `.md` file except `AGENTS.md`, so `docs/recode/CONTRIBUTING.md` was dropped).
- `tools/check_conventions.py --base upstream/main` still reports 3 errors, none from the
  contributing change:
  1. `DrawRecorder.cpp:5904` reads `APS5_RECODE_MIRROR_STORES_SKIP`, which has no row in
     `docs/dev/ENVIRONMENT.md`.
  2. `ENVIRONMENT.md` has a row `APS5_PIPELINE_DISPATCH_MISSES` that the code no longer reads.
  3. Commit `f8e62712c9` ("Fix the build after merging upstream/main") is not a Conventional
     Commit subject (it is in history, so it needs a rewrite only if the fork is ever
     proposed upstream).
