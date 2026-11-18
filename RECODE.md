# Unified GPU core: design for a recode, draft 1

Written 2026-11-18. On 2026-11-18 the user asked for the recode to be set up on its own branch
and carried on without prompts; the decisions in section 5 are taken as recorded there and can
be reversed by the user. Branch `recode/main` of the fork (XatiX/AnyPS5), started from
`upstream/main` 604828bd44; local folder `C:\Games\PS5\Recode`.

This is a design for one new GPU backend that takes the parts of the three existing code
bases that are measured to work and drops the parts that hold each of them back. The sources
are:

| Name here | Tree | What it is |
|---|---|---|
| Main | `C:\Games\PS5\AnyPS5`, upstream main | AnyPS5's own driver: relinker, libraries, recompiler, `libSceAgcDriver` |
| Direct | `C:\Games\PS5\Rewrite-Testing\wt-rewrite`, `rewrite/direct` | Main plus the direct draw path, protection tracking, event waits and synced shadows |
| Kyty | `C:\Games\PS5\AnyKyTy`, `core/kyty` behind `-DANYPS5_GPU_BACKEND=kyty` | The KytyPS5-GTA graphics core (tag v24) with a shim onto AnyPS5 |

Target (user, 2026-11-18), at native resolution on machine A (RTX 5080, Ryzen 7 9800X3D,
3840x2160), with at most 16 GiB of commit plus VRAM:

| Titles | Frame rate | Frame | Work per thread per frame | GPU work per frame |
|---|---|---|---|---|
| FF Resonance, LEGO, Age of Mythology, GTA5, Subnautica | 100 fps or more | 10 ms | about 6 ms | about 8.4 ms |
| 007 First Light | 60 fps at least | 16.7 ms | about 10 ms | about 14 ms |

The thread and GPU budgets keep the proportions of `DESIGN.md` (10 and 14 ms of a 16.7 ms
frame). This replaces that file's 16.7 ms target for every title and its 30 fps milestones for
Age of Mythology and 007.

## 1. Why a recode and not more steps on Direct

Three measured facts decide it.

1. **Five of the six test titles are bound by the CPU, and their GPU work already fits.** From
   `TITLES.md`, 2026-11-17:

   | Title | Frame | CPU work per frame over the workers | GPU work per frame | GPU busy span |
   |---|---|---|---|---|
   | FF Resonance | 28.1 ms | 30.4 ms | 3.4 ms | 15.1 ms |
   | LEGO Builder's Journey | 26.3 ms | 25.3 ms | 7.6 ms | 15.3 ms |
   | Subnautica: Below Zero | 40.6 ms | 38.6 ms | 2.0 ms | 19.7 ms |
   | 007 First Light | 54.9 ms | 75.8 ms | 9.3 ms | 35.7 ms |
   | Age of Mythology: Retold | 66 ms | 63.2 ms | 12.7 ms | 35.7 ms |
   | Grand Theft Auto V | 202.3 ms | 336 ms | 83 ms | 127 ms |

   Against the budgets above: GPU work is inside 8.4 ms on FF, LEGO and Subnautica, and
   inside 14 ms on 007. Age of Mythology is 4.3 ms over and GTA5 is ten times over on Direct.
   CPU work is 4 to 56 times the 6 ms budget on every 100 fps title. So for four titles the
   whole distance is how the CPU side is organised; Age of Mythology and GTA5 need GPU work
   as well.

2. **On the one title where both were measured on this machine, Kyty is five to eight times
   faster.** GTA5 in game: Direct 202 ms per frame; Kyty on Windows about 24 ms in Fidelity
   mode, about 15 ms with native ray tracing, about 7 ms in Performance mode (41, 66 and 139
   FPS, uncapped, 2026-11-17). Only Performance mode is past 100 fps. Kyty has not been timed
   on the other five titles here, and it was tuned on GTA5, so this proves the structure on
   one title only. Phase R0 fixes that before anything is built.

   Kyty's stage threads in that measurement (prologue bank scene standing still, 360 Hz
   virtual blank, immediate present, warm caches, one session per mode, six windows of 5 s;
   `Kyty-Testing\anykyty-gta5-uncapped-fidelity-vs-performance.png`):

   | | Fidelity | Performance |
   |---|---|---|
   | Frame | 24.5 ms (40.8 FPS) | 7.18 ms (139.4 FPS) |
   | Draws per frame | 1,363 | 1,330 |
   | Command processor, per draw / per frame | 5.48 us / 7.5 ms | 4.01 us / 5.3 ms |
   | Resolve, per draw / per frame | 17.89 us / 24.4 ms | 3.99 us / 5.3 ms |
   | Execute, per draw / per frame | 6.75 us / 9.2 ms | 3.97 us / 5.3 ms |
   | Record, per draw / per frame | 8.89 us / 12.1 ms | 3.92 us / 5.2 ms |
   | Whole process, per draw / per frame | 85.9 us / 117 ms | 27.4 us / 36 ms |
   | GPU time per frame (utilisation times frame, includes the desktop) | 20.5 ms | 4.27 ms |

   Three readings. In Performance mode each of the four stages costs about 4 us per draw and
   5.3 ms per frame, so one thread per stage already fits a 6 ms thread budget at 1,330
   draws. In Fidelity mode the resolve thread is busy for the whole frame, so that mode is
   bound by one CPU thread at least as much as by the GPU. And Direct's cost on the same
   title is 86 us per draw and 859 us per dispatch on its workers.

3. **The costs left on Direct belong to its structure, not to missing tuning.** Each of these
   was probed and each probe ended at the same place:

   | Cost on Direct | Measured | Why steps do not remove it |
   |---|---|---|
   | CPU per dispatch | 350 us on 007, 859 us on GTA5 | The dispatch cache validates and captures guest memory per dispatch; 25 to 64 % hits |
   | One GPU mutex | 007 waits 24.5 ms per frame; held 5.9 s of 10 s | No poll left to remove; it only falls as holds get shorter |
   | Copy-back and storage copies | GTA5 21 ms, 007 20 ms, Subnautica 6.3 ms of GPU per frame | Shaders write a shadow, then a kernel copies into a host import; readers in between need the import |
   | Walks over memory both sides write | GTA5 about 8 ms | Blocks that GPU stores keep reopening stay unprotected |
   | Footprint | AoM, 007, GTA5 and Subnautica above 16 GiB | Import, shadow and baseline are three copies of one range |
   | Two paths | Old path is the per-draw fallback | Both must agree on memory at every draw, so each new mechanism serves both |

   Direct took LEGO from 59 to 26 ms, FF from 40 to 28 ms and AoM from 6 s to 66 ms. That work
   is the evidence for most rules below. What it cannot do is remove a mutex, an import or a
   second path that the rest of the code depends on.

## 2. What each code base contributes

"Take" means the code moves over with interface changes only. "Port" means the mechanism is
rebuilt in the new core from that source's design. "Drop" means the new core has no
equivalent.

### From Main

| Part | Decision | Reason |
|---|---|---|
| Relinker, kernel and libraries | Take, untouched | Not part of this design |
| RDNA recompiler (`core/shader/recompiler`) | Take as the only compiler | Full instruction coverage, hardware oracle, exact float rules; it gains two output modes (section 4.5) |
| Shader disk cache, pipeline journal and replay, pipeline libraries with fast link | Take | A second run starts with its pipelines built; keyed on content, not on run state |
| Command buffers and register lists copied and decoded at submit | Port | A title may rewrite a buffer after submitting it; Kyty reads by pointer and stops on LEGO's second launch for this reason |
| Texture formats, tiling tables, depth and multisample handling | Take | Tested per format |
| Driver tests (1,821) and the hardware oracle | Take the cases, retarget the fixture | The only broad correctness check any of the three has |
| Host imports of guest memory for shader access | Drop | NVIDIA on Windows reads an import far slower than its own buffer (164 against 45 ms of draws per frame in Main's own note); imports also force the settle and compare machinery |
| Draw cache, dispatch cache, recipes, captures validated against memory | Drop | Replaced by section 4.4 |
| Write-watch walks and collect epochs | Drop on Windows for framed titles | Replaced by section 4.3 |

### From Direct

| Part | Decision | Reason |
|---|---|---|
| Red-zone framing in the relinker, the executable mark, the fault storm | Take | It is what makes page protection safe on Windows guest threads. Kyty's memory model needs it too and has no proof of its own |
| Block ownership by page protection, events instead of polls | Port, as the only model | Phase 2 and 3 results; here it is not layered over walks |
| Descriptors known on the CPU, shaders independent of data, heap slots, table mirror | Port | 12 us per draw on LEGO, 23 on FF; AoM's table stages 233 to 13 us |
| Memos valid until a write event on their source | Port | Replaces per-epoch revalidation |
| GPU-owned ranges with read traps set and released in parts | Port, as the only way GPU results reach RAM | GTA5 225 to 202 ms; LEGO copy-back 7.65 to 0.70 ms |
| Waits resolved from the recorded order | Port | No GPU completion for a value the CPU already knows |
| Pass merging, barriers only for hazards, hazards from bound storage slots | Port | Phase 5 timeline results |
| Gates in ms per frame against a budget, fixed scenes, `TITLES.md`, the samplers and `[gputime]` | Take | The method that found every number in this file |
| Staging shadows, baselines, changed-byte copy-back kernels, address tables and table search | Drop | Section 4.3 has no import to copy back into |
| Per-draw fallback to the old path | Drop | Section 5, decision 1 |
| The committer thread and the GPU mutex | Drop | Replaced by section 4.2 |

### From Kyty

| Part | Decision | Reason |
|---|---|---|
| Stage threads joined by single-writer queues (command processor, resolve, execute, record, host copy) | Port | One owner per stage, no shared lock on the draw path |
| Register state snapshot rings with an id per snapshot | Port | State is hashed once per change, not once per draw |
| Draw templates: one key gives pipeline, targets and a descriptor commit | Port | The per-draw record step becomes a lookup |
| Resolve ahead of execution (Scout tickets) | Port | Descriptor walks run on another thread before the draw reaches the recorder |
| Device-local buffer cache with a GPU page table, GPU fault and write bitmaps | Port, for one use only (section 4.4) | Removes per-dispatch validate and capture; the GPU reports what it wrote |
| Second view of guest memory | Take (already in AnyKyTy's libkernel for Windows) | The emulator reads and writes guest memory without touching protection |
| Readback prediction: hot readbacks, small mirrored lines, packet stores written around GPU pages | Port | A page the game reads every frame is copied back before the read, not on a fault |
| Deferred re-protection off the upload path | Port | Protect calls do not sit inside the record stage |
| GPU tiling kernels | Port | No CPU detile on upload or write-back |
| Native ray tracing of the guest's BVH walks | Port as a recompiler pass | GTA5 Fidelity 41 to 66 FPS on Kyty |
| Stage threads placed on the cores that share the largest cache | Port | +7 to +22 % measured on Linux in AnyKyTy; less to gain on a single-CCD 9800X3D |
| DLSS and FSR for the title's own PSSR requests | Port later | A feature, not a way to meet the target; the target stays native resolution |
| Its second shader recompiler | Drop | One compiler; only the ray traversal pass moves |
| Command buffers parsed in place | Drop | See Main, submit copy |
| Per-shader hack marks, helper processes, exit on any unknown packet | Drop | Not general; helper processes restart the converted title |

## 3. Principles

1. One path. A title runs on the new core or on an existing backend, chosen per title, never
   per draw.
2. No shared lock on the path of a draw or dispatch. Each stage has one thread; stages talk
   through single-writer queues and sequence numbers.
3. Work happens when memory changes, and the change is an event.
4. What the CPU can know about a draw is resolved on the CPU and baked into data. The GPU
   looks up an address only where the address is computed by the shader.
5. Shaders read and write device-local memory only. No shader reads a host import.
6. A GPU result stays on the GPU until the CPU reads it. A range the CPU read last frame is
   copied back ahead of the read.
7. One ledger owns every byte of device memory under one budget. A guest range has at most
   one device copy.
8. Every stage has a per-item cost target in microseconds and a counter that reports it.

## 4. Architecture

### 4.1 Front end

- `sceAgcDriverSubmit*` copies the command buffer and every register list it points to into a
  ring arena and returns. Decoding starts from the copy.
- One decode thread per guest queue that is in use (queue 0 and the compute queues). It turns
  packets into operations: draw, dispatch, label store, wait, copy, fill, flip. Each operation
  carries a sequence number in its queue and the ids of the register snapshots it uses.
- Register state lives in snapshot rings (context, user config, shader registers). A write
  makes a new snapshot; draws between two writes share one id.
- A wait is an operation. It names the address, the test and the operations before it. The
  stages after decode use it as an ordering edge between queues, so a queue that waits on
  another queue's label needs no lock and no poll.

### 4.2 Stage pipeline

```
guest threads -> submit copy -> decode (per queue) -> resolve -> record -> GPU
                                                         |          |
                                                    memory core  host copy
```

| Stage | Threads | Does | Target per draw or dispatch |
|---|---|---|---|
| Decode | One per active queue | Packets to operations, snapshots | 4 us |
| Resolve | One by default, more when a title needs them, each taking a share of the operations | Template lookup, descriptor memo, slots, addresses, access set | 4 us on average |
| Order | One, only when resolve or record is shared out | Merges the resolved operations into one order; pass and barrier decisions from access sets | 0.5 us |
| Record | One by default, more when a title needs them, one command buffer each | Bind, push, draw for a run of operations | 4 us |
| Host copy | One | Uploads into device memory, readback landing through the second view | n/a |
| Compile | Pool | Recompiles and pipeline builds; never on the threads above | n/a |

- The targets are Kyty's measured cost in GTA5's Performance mode: about 4 us per draw on
  each stage thread, 5.3 ms per frame at 1,330 draws, inside the 6 ms thread budget with one
  thread per stage. Kyty has a fourth stage (execute, also 4 us); here its work is split
  between resolve and record, so those two start with less margin than the table suggests.
- One thread per stage stops being enough when the cost per draw rises. Kyty's Fidelity mode
  shows it: resolve at 17.9 us fills the whole 24.5 ms frame and record at 8.9 us takes
  12.1 ms. At 6 ms per thread that mode needs resolve on at least five threads or at a
  quarter of the cost, and record on at least two. This is why both stages can be shared
  out; a title that fits on one thread each runs without the order stage.
- Why Fidelity costs 4.5 times more per draw in resolve: the existing samples point at one
  loop that re-checks guest memory per draw, which a memo with event validity removes
  (section 4.9). So the first answer to that mode's resolve cost is the memo, and sharing the
  stage out comes second.
- Resolve can be shared out because a template hit reads only its own memo and the snapshots
  it names. The order stage puts the results back in sequence. The order is the sequence
  numbers within a queue plus the edges from waits and from overlapping access sets.
- Record can be shared out because the order stage has already decided passes and barriers.
  Each record thread fills its own command buffer for a run of operations; a pass that spans
  two buffers is suspended in one and resumed in the next (dynamic rendering allows this).
  Buffers are submitted in order.
- An operation's access set (ranges read, ranges written, images) is produced by resolve. It
  is the only input to pass merging, barriers, residency and trap decisions. Nothing later
  re-derives it.
- For scale: Direct's best per-draw cost is 12 us (LEGO) on one worker under a shared lock;
  Kyty's is 16 us summed over four threads with no shared lock, 4 us on each. The targets ask
  for what Kyty has shown on one title in one mode. R1 measures decode and resolve on all six
  before the rest is built.
- Thread count: in Performance mode Kyty's whole process uses 36 ms of CPU per 7.18 ms
  frame, about five cores, of which the four stages are three. Machine A has eight cores, so
  there is room for about three more busy threads before stages compete with the title's
  own. Fidelity with resolve on five threads would not fit; that mode needs the cost per
  draw to fall, not only more threads.

### 4.3 Memory core

One component replaces Main's imports and write watch, Direct's shadows and Kyty's buffer
cache.

- **Tracking unit:** 4 KiB pages, grouped in 4 MiB regions with per-region dirty flags, as in
  Kyty. Direct tracks 64 KiB blocks; the titles that mix CPU and GPU writes inside a block
  (FF, GTA5) are the reason for the smaller unit. Whether 4 KiB protect calls cost more than
  they save is measured in phase R2.
- **States per page:** CPU-clean (read-only, device copy current), CPU-dirty (read-write),
  GPU-owned (no access, device copy is the newest), untracked (not GPU-registered).
- **Second view:** every emulator thread reads and writes guest memory through an alias that
  is never protected. Only guest threads fault.
- **Faults:** a write to a CPU-clean page marks it dirty, adds it to the dirty list and opens
  it. A read or write of a GPU-owned page reads back the pages of that access, waits for the
  batch that wrote them, then opens them. Precondition: the title is converted with
  `--frame-red-zone` and carries the mark. Stacks are never armed.
- **Residency:** device-local buffers, one per registered range, sub-allocated from the
  ledger. A submission uploads the dirty list once. Least recently used ranges are evicted
  under the budget from `VK_EXT_memory_budget`.
- **GPU writes:** a write through a descriptor the CPU knows has its range in the access set.
  A write through a computed address sets a bit in the GPU write bitmap, which is read after
  the batch. Both make the pages GPU-owned.
- **Readback ahead:** a range that faulted for reading in the last frames is copied back as
  soon as its writing batch ends, landed through the second view and opened before the game
  reads it. Labels and other values of a few bytes use small mirrored lines.
- **Images over guest memory:** an image's source is the residency buffer, detiled by a GPU
  kernel. A GPU-written image is written back into the residency buffer, not into RAM, and
  only when another shape or a buffer reader overlaps it. This is the answer intended for
  007's ring of temporary images; it removes the RAM round trip, not the GPU copy.

Consequence: staging shadows, baselines, copy-back kernels, store imports, import windows,
settle reads, collect epochs and walks have no counterpart. A guest range has one RAM copy and
at most one device copy.

### 4.4 Descriptors and addresses

- **Images and samplers:** one global heap. Resolve turns each T# and S# into a slot through a
  memo that a write event on its source pages invalidates. Image tables keep Direct's mirror.
- **Buffers the CPU knows:** resolve turns the V# into a device address and a bound and writes
  both into the per-draw data. The shader does no lookup.
- **Addresses the shader computes** (pointer chasing in compute, address-based builds): the
  shader translates through the GPU page table, one load per dereference. A miss sets a fault
  bit; the page is made resident for the next submission.
- **Per-draw data:** a ring buffer bound once per batch; a draw pushes one offset.
- **Templates:** key = program ids, interface, snapshot ids. Value = pipeline, target set,
  descriptor commit and the list of words to refill. A hit skips everything except the refill
  and the access set.

This replaces the draw cache, the dispatch cache, recipes, capture, validation, address tables
and the table search. A dispatch and a draw take the same route.

Principle 4 keeps Direct's rule that descriptors are not read on the GPU. The page table is
used for data addresses only. Direct's note "GPU-side descriptor reads, hash tables and miss
rings (measured slower)" stays true and stays excluded.

### 4.5 Shaders and pipelines

- Main's recompiler emits one module per code and static interface, independent of the data
  bound to it (Direct's direct form). Two additions: an addressing mode that goes through the
  page table, and Kyty's traversal pass that turns a recognised BVH walk into a ray query when
  the device has ray queries.
- Disk cache, journal replay and library linking stay as in Main. Recompiles and pipeline
  builds run on the pool; a draw whose pipeline is not ready waits for the fast link only.

### 4.6 GPU timeline

- Consecutive draws into the same targets share a pass. A barrier is recorded only where two
  access sets conflict.
- Compute dispatches with no conflict between them get no barrier and no flush.
- Indirect draws and dispatches read their arguments on the GPU unless a wait orders a CPU
  read of them.

### 4.7 Frame rates above the console's

A title written for 30 or 60 fps does not run at 100 because the emulator is fast. Three
things must hold per title, and none is known for any of the six today.

- **Pacing.** A title presents on vertical blanks and may ask for a flip rate. The core
  offers a virtual blank rate above 60 Hz and can ignore the flip rate a title sets
  (`ANYPS5_VBLANK_HZ` in AnyKyTy and `APS5_UNPACED_FLIPS` in upstream commit b3691289ef do
  parts of this; GTA5's 139 FPS was measured with `ANYPS5_VBLANK_HZ=360`). Presentation uses
  mailbox or immediate mode so the display does not pace the title.
- **Game speed.** A title that advances its simulation by a fixed step per frame runs fast
  when the blank rate is raised. Such a title cannot go above its own rate without a
  per-title patch, which is outside this design.
- **Guest threads.** The frame cannot be shorter than the title's own work per frame. The
  processor of machine A is roughly twice as fast per thread as the console's, so a title
  that fills a 33 ms frame there needs about 15 ms here and stops near 65 fps whatever the
  GPU core does.

R0 checks all three per title on the existing backends: lift the pacing on the lightest
scene, see whether the rate passes 60, whether the game runs at normal speed, and how long
the busiest guest thread works per frame.

### 4.8 Platform

Windows with a framed title is the designed case. Linux can deliver faults on an alternate
stack, so it should need no framing and gets the same core; that is not checked here. A title that fails framing stays on an existing
backend.

### 4.9 GTA5 Fidelity with ray tracing

The target covers both of GTA5's modes. Performance mode is inside every budget on Kyty
already. Fidelity is the hard one, and Kyty's runs of 2026-11-17 say where
(`Kyty-Testing\gta5\fidelity-*.gpu.csv` and `*.resolve-samples.tsv`):

| Fidelity, Kyty | Frame | GPU utilisation | GPU time per frame | Resolve thread |
|---|---|---|---|---|
| Ray walks emulated | 24.5 ms (41 FPS) | about 85 % | about 20.5 ms | Busy the whole frame; 54 % of its samples at one address |
| Native ray tracing | about 15 ms (66 FPS) | about 94 % | about 14 ms | 30 % of its samples in a system wait, so it is no longer the limit |

- **With native ray tracing the mode is bound by the GPU.** 100 fps needs about 14 ms of GPU
  time to become 8.4 ms, a cut of 40 %, at 3840x2160. That is the gap this design has no
  measured answer for. It belongs to R5: time per program with native ray tracing on, then
  the largest programs one by one.
- **Without it the mode is bound by resolve, and the cause looks like per-draw
  re-validation.** The hot address falls in `GuestReadLog::FirstMismatch` by nearest symbol
  (not confirmed in a debugger): a comparison of the guest memory a cached program read
  against what memory holds now, repeated per draw. That is the same kind of cost as Direct's
  dispatch validation, and section 4.4 removes it by rule: a memo stays valid until a write
  event on its source pages, so nothing is re-read. The same sample share is 9 % with native
  ray tracing on.
- Stage times per draw with native ray tracing on have not been measured (the table in
  section 1 is the emulated run). R0 adds that run.
- Parts that carry this mode: the traversal pass in the recompiler (section 4.5), acceleration
  structures built from the guest's BVH data and refitted on the GPU, and their memory under
  the one ledger (Kyty budgets 4 GiB for them on this card, a quarter of the 16 GiB target).

## 5. Decisions this design needs

Decisions 1 to 5 were recommendations when the user asked for the work to go ahead without
prompts (2026-11-18); they are taken as recommended, except decision 4, which changed with the
base (see its text).

1. **Fallback per title, not per draw.** This reverses principle 6 of `DESIGN.md`. A per-draw
   fallback means two memory models that must agree at every draw, which is the largest
   source of cost and of bugs on Direct. The price: a title using a feature the new core
   lacks does not run on it at all until the feature exists.
2. **A GPU page table for computed addresses.** `DESIGN.md` principle 2 says shaders never
   chase guest pointers on the GPU. This keeps that for descriptors and relaxes it for data
   addresses. The evidence is Kyty on GTA5 only, until R0.
3. **A page that faults on the GPU reads as absent for that submission.** Kyty accepts this.
   A strict mode that re-runs the dispatch is possible for tests and costs a GPU round trip.
4. **Base tree: current `upstream/main`, not Direct (taken 2026-11-18).** Measured that day:
   Direct was 3,223 commits behind upstream with 55 of its own files in conflict, most of them
   the files this design replaces (`GuestBufferMemory.cpp`, `Recorder.cpp`, the copy-back
   shaders); simnJS's `AnyKyty` branch was 3,381 behind with 945 files changed. Upstream
   already has the relinker's red-zone framer. So the branch starts on upstream, the new core
   lives in a directory upstream does not have, selected like Kyty's
   (`-DANYPS5_GPU_BACKEND=<name>`, default `anyps5` builds exactly what upstream builds), and
   what it needs from Direct (protection tracking in libc, the executable mark, the fault
   storm, the tests and scripts) and from AnyKyTy (the second view of guest memory) is ported
   piece by piece as its own commits. Kyty is run from the AnyKyTy tree for comparison and is
   not merged. The default build staying identical to upstream is what keeps the branch a
   possible replacement for upstream main, and keeps the hourly merges near conflict-free.
5. **4 KiB tracking instead of 64 KiB blocks.**
6. **Which GTA5 graphics mode the 100 fps target means. Answered by the user on 2026-11-18:
   both, Performance and Fidelity with ray tracing.** Fidelity is therefore built and judged
   with native ray tracing only (the device must have ray queries; the emulated walk is kept
   for correctness and has no rate target). Where that stands on Kyty is in section 4.9.
7. **What happens to a title that fails section 4.7. Agreed by the user on 2026-11-18:** its
   target becomes the highest rate its own pacing, speed and guest threads allow, recorded in
   `TITLES.md`, with no per-title game patches.

## 6. Phases and gates

Gates are costs in ms per frame against a budget, on the fixed scenes of `TITLES.md`, as in
`DESIGN.md`. A phase is finished when every item is built, every title that runs on the new
core has its result recorded, and the driver tests that apply pass.

| Phase | Work | Gate |
|---|---|---|
| R0 | Kyty timed on all six scenes on machine A, with its per-stage time per draw and dispatch and its footprint (GTA5's bank scene is done, section 1; the other five and GTA5's footprint are not). GTA5 Fidelity with native ray tracing: stage times per draw and GPU time per program. The hot resolve address confirmed by name. The three checks of section 4.7 per title, on Direct or Kyty. | A table per title for Direct and Kyty: frame, per-stage CPU, GPU work, footprint, rate with pacing lifted, game speed, busiest guest thread per frame. If Kyty is not faster than Direct on at least three titles, sections 4.2 to 4.4 are revised before R1. A title that fails section 4.7 has its target restated (decision 7) |
| R1 | Front end and decode, run beside the existing backend without rendering; resolve added the same way | Decode and resolve each at or under 4 us on average per operation and at or under 6 ms per frame on all six titles, with the template hit rate recorded; the operation stream matches the existing backend's draws and dispatches one to one |
| R2 | Memory core with the second view, on its own: unit tests, fault storm, a replay of each title's write and read pattern | Coherence work at or under 0.3 ms per frame (5 % of a thread's 6 ms); protect calls and faults per frame recorded; storm clean on all six |
| R3 | Draw path: templates, heap, order, record on several threads, passes, lifted pacing. First titles: LEGO, FF, Subnautica | Pictures match the existing backend; no stage thread above 6 ms per frame; frame at or under 10 ms on at least one of the three |
| R4 | Compute path: dispatches on the same route, page table, GPU-owned writes, readback ahead. Titles: GTA5, 007, AoM | Copies at or under 0.8 ms of GPU per frame (1.5 ms on 007); resolve plus record at or under 8 us per dispatch; GTA5 no slower than Kyty in the same mode; 007 at or under 16.7 ms on its title screen and menus |
| R5 | GPU stage: ray traversal pass and acceleration structures, then the largest programs per title | GPU work at or under 8.4 ms per frame (14 ms on 007), or a floor per program above 0.8 ms. GTA5 is judged in both modes, Fidelity with native ray tracing |
| R6 | Footprint, remaining draw kinds, tests retargeted, existing backend no longer needed for the six titles | 16 GiB per title; driver tests pass on the new core; every title at its target or at a recorded floor with the cause named |

R0 costs days and uses nothing new. It is the gate for the whole design: every claim in
section 2 that rests on Kyty rests on one title today, and whether 100 fps is open to a title
at all is decided by section 4.7, not by this core.

## 7. What this can and cannot deliver

- It removes the CPU structure costs of section 1. It cannot make a title faster than its own
  guest threads or its own shader work.
- Where each title stands against its target, from what is measured today:

  | Title | GPU work against budget | What 100 fps (60 for 007) depends on |
  |---|---|---|
  | FF Resonance | 3.4 of 8.4 ms | The stage targets and section 4.7. Most likely to arrive |
  | Subnautica | 2.0 of 8.4 ms | The same. Its title screen has shown a 33.4 ms cap, so pacing is the first check |
  | LEGO | 7.6 of 8.4 ms, 3.2 of it storage upload that section 4.3 removes | The same, with little GPU margin. `DESIGN.md` doubted 60 fps here when its top program cost 23 ms; it is 1.8 ms now |
  | Age of Mythology | 12.7 of 8.4 ms on its title screen | A third off the GPU time (R5) as well as the CPU. No in-game scene is measured |
  | GTA5, Performance | Kyty: 4.3 of 8.4 ms | Already 139 FPS on Kyty with every stage inside 6 ms, so the new core has to match Kyty, not beat it |
  | GTA5, Fidelity with ray tracing | Kyty: about 14 of 8.4 ms | 66 FPS on Kyty, bound by the GPU. Needs 40 % off the GPU time with nothing measured that gives it (section 4.9). The least certain target in this design |
  | 007 | 9.3 of 14 ms | Per-dispatch CPU (R4). Measured on the title screen and menus only; it crashes past them on Direct |

- Section 4.7 is answered for GTA5 only: its pacing lifts (139 FPS at a 360 Hz virtual
  blank) and its own threads fit a 7 ms frame. Game speed at that rate was not checked.
- Not measured anywhere: Kyty on five of the six titles; whether any of the other five runs
  above 60 fps at normal speed; their guest thread time per frame; the cost of 4 KiB
  protection on Windows at these write rates; resolve at 4 us for AoM's table stages (13 us
  for capture alone on Direct today); how often a computed address misses the page table;
  GTA5's GPU time in Fidelity with native ray tracing.
- The per-draw targets are what Kyty reaches on GTA5 in Performance mode, from one session
  with no run-to-run spread. Kyty's own Fidelity mode misses them by a factor of 2 to 4.5,
  so they are shown possible for one workload, not in general.
- Known risks: a title that puts a stack inside GPU-registered heap memory (carried from
  `DESIGN.md`); a title whose CPU and GPU write the same pages every frame, which pays a
  readback per frame whatever the unit; the size of the work, which is a new backend and not a
  change to one.

## 8. Not in this design

- Descriptor reads, hash tables and miss rings on the GPU.
- Upscaling or frame generation as a way to reach the target.
- Per-title game patches to change a title's frame cap or time step.
- A second shader recompiler.
- Any change to the relinker, kernel or libraries beyond the second view and what framing
  already has.
