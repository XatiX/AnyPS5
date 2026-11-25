# Unified GPU core (the recode): build guide, revision 3

Written 2026-11-21 for an agent that picks up the recode of AnyPS5's GPU driver with no memory of
earlier sessions. It says what the finished core looks like, why, and what to build next, one
step at a time. This file is local (the repository allows no design files). It is the authority
on what to build and in what order. `notes\NEXT-SESSION.md` is the authority on the current
state: when the two disagree about state, trust the handoff.

This revision replaces draft 1 and revision 2, which are kept unchanged in
`notes\RECODE-rev2.md`. Read that file only for history: why a recode was chosen over more work
on `rewrite/direct`, what each of the three older code bases contributed, and the full
measurement tables. Sections 3 to 8 have the same numbers and subjects in both files, so older
notes that cite "RECODE 4.4" or "6.1" still point at the right place.

**What revision 3 changed (user, 2026-11-21, after a review of the design against the console's
architecture and this PC):**

| Change | Where | Step |
|---|---|---|
| The mirror's device memory is mapped into the CPU's address space; small reads, labels and driver stores no longer need a GPU submission | 4.3, decision 8 | C1a, C1b |
| A completion the guest can see (label, end-of-pipe event, flip) is released only after the GPU results before it have landed; the guest's lead over the host GPU is capped | 4.3, principle 9, decision 9 | C4b |
| A guest fault takes no core-wide lock; the unit a write fault opens grows; an image's range opens whole | 4.3, principle 2, decision 10 | C5b, C5c |
| An image-only range keeps no mirror memory after its detile; sparse binds are batched and not waited on by the CPU | 4.3, principle 7, decision 11 | C2b, C7 |
| The 16 GiB target is measured on Ghost of Yōtei before it is restated | decision 12 | C5 |
| Every timing carries the render size the title chose | 4.7, principle 11, decision 13 | C0 |
| One mirror buffer over one address window, so a shader's mirror address is one add | 4.4 | C8 |
| Pipeline state made dynamic only if cold starts are still slow with one module per program | 4.5 | C9b |
| Ray tracing stays in the design, built and tested after the base games | 4.9, decision 14 | RT1 to RT5 |

## 0. How to use this file

### 0.1 Reading order and the work loop

1. Read, in this order: `CLAUDE.md` in this folder (standing rules, commands, debugging tools),
   this file, `notes\NEXT-SESSION.md`, `notes\progress.json`.
2. Section 6.0 says where the build stands. Section 6.1 lists the core steps in order. Take the
   first one that is not done.
3. For each step:
   1. Read the step and the architecture section it names.
   2. Locate the code it names and confirm it still looks as described (the `code-locator`
      agent is cheap). The "state" lines in this file date from 2026-11-21.
   3. If the step says **measure first**, take the measurement and write the number into
      section 10 before choosing a variant.
   4. Build the piece in `wt-recode\core\libs\prx\libSceAgcDriver\Unified\`. A file shared with
      upstream gets a hook into our code at most.
   5. Prove it with unit tests (`tests\Unified*.cpp`, one cmake file per test in `tests\cmake\`)
      and the `agc_unified` ctest selection.
   6. Build libraries (`scripts\build.ps1 -Libs rNNN`), then make one game run of the title the
      step names with `scripts\debug-run.ps1`. The run only checks that nothing that worked
      before broke (picture against the step before, no `[recode-fatal]`, no crash) and records
      the step's counters. Run the same libraries twice and read the second run: the first
      launch after a library change is cold.
   7. Add a downgrade kind (`Unified\src\Downgrades.cpp`) for every new quiet fallback or
      degraded binding.
   8. Commit in small pieces. Update `notes\progress.json` and the "State per step" table of
      the handoff.
4. Report at the end of a step: what was built, test results, the counters, what was not run.

### 0.2 Rules that shape the work (all from the user)

- **No old path at all (2026-11-24).** "Ensure old path is not used at all. If something is
  missing from design build it." Nothing on the route may be served by the existing driver:
  draws, dispatches, images, packets, memory, presentation. A use found is a missing design
  step (as M1 and P1 were): write the step here and build it; never bridge to the old path.

- **Order (2026-11-21).** Build the core steps of 6.1 in order, starting at the first one not
  done. Do no picture fixes for any title until all of them are in place. A picture fault seen
  on the way is written into the handoff, not chased.
- **No arbitrary gates (2026-11-18).** The targets in this file are things to measure and
  report against. They never hold the next step.
- **Its own core (2026-11-21).** The new path depends on nothing outside its own code. Kyty is
  not run, read or cloned. `rewrite/direct` may be read through git for how a mechanism was
  done. Nothing new is built on the old path's caches (image and depth caches,
  `ShaderResources`, host imports, draw and dispatch caches); each one the route still uses is a
  step below.
- **No fallback to the old path (2026-11-21).** Work that cannot be recorded stops the game
  with a `[recode-fatal]` line.
- **New pathways only (2026-11-18).** Upstream's `Graphics::Draw`, `ShaderResources`, recipes
  and caches stay untouched until replaced.
- **Default build identical to upstream.** `ANYPS5_GPU_BACKEND` unset builds exactly what
  upstream builds. Linux keeps compiling: platform code sits behind `_WIN32` branches with a
  working or stub Linux side.
- **Ask the user before:** anything outward-facing (an upstream pull request, a post), a change
  to a target, and anything that deletes data outside `C:\Games\PS5\Recode`.

### 0.3 Words used in this file

| Word | Meaning |
|---|---|
| Guest | The PS5 title and its memory. Host: this PC and the emulator. |
| Old path, existing path | Upstream's own driver code (`Execution\`, `Graphics\`) and its caches. |
| Route | The set of work the new core records: `default` (draws), `full` (draws and every dispatch), `old` (none). `debug-run.ps1 -Route`. |
| Operation | One decoded draw, dispatch, label store, wait, copy, fill or flip. |
| Program hash | Stable id of a guest shader (its instruction words without literals). |
| T#, S#, V# | The console's texture, sampler and buffer descriptors: a few words of guest memory. |
| Label | A value the GPU stores into guest memory when it reaches a point in the stream; the title reads or waits on it to learn that GPU work is finished. |
| Mirror | One sparse device buffer whose offset equals the guest address. The only device copy of guest buffers. |
| Chunk, pool | 64 KiB of mirror (the sparse bind unit); a 256 MiB device allocation that chunks come from. |
| Ledger | `Residency`: which chunks are bound, to which ranges, under the budget. |
| Page states | Untracked, CPU-clean (read-only, mirror current), CPU-dirty (read-write), GPU-owned (no access, mirror is newest). 4 KiB pages. |
| Second view | An alias of guest memory that is never protected. Emulator threads use it; only guest threads fault. |
| Image record | The new core's own image for a guest memory range (`Unified\Images`). |
| Batch, serial | One host submission of recorded work, and its number. "Completed serial" is the newest batch the GPU has finished. |
| Access set | Per operation: guest ranges read, ranges written, images. Produced by resolve. |

## 1. Goal

One GPU backend for AnyPS5 that runs the test titles at their targets on machine A (RTX 5080,
driver 617.42, Ryzen 7 9800X3D, 3840x2160), at native resolution.

| Title | Bench name | Target | Frame | Work per stage thread per frame | GPU work per frame |
|---|---|---|---|---|---|
| LEGO Builder's Journey | `lego` | 100 fps or more | 10 ms | about 6 ms | about 8.4 ms |
| Age of Mythology: Retold | `aom` | 100 fps or more | 10 ms | about 6 ms | about 8.4 ms |
| FF Resonance | `ff` | 100 fps or more | 10 ms | about 6 ms | about 8.4 ms |
| 007 First Light | `007` | 60 fps at least | 16.7 ms | about 10 ms | about 14 ms |
| Ghost of Yōtei | `yotei` | 60 fps assumed; the user has set none | 16.7 ms | about 10 ms | about 14 ms |

- Memory target (user, 2026-11-18): at most 16 GiB of commit plus VRAM per title. See
  decision 12.
- Upscaling and frame generation do not count toward a target.
- GTA5 and Subnautica are no longer run (user, 2026-11-21). What was measured on them stays as
  evidence (section 10).
- Ghost of Yōtei is the hard title: 3,789 draws and 2,616 dispatches per frame, draw tables
  built by compute. At 6,400 operations per frame a stage thread with a 10 ms budget has
  1.5 us per operation, so one thread per stage is not enough there (section 4.2).

## 2. The console and this PC: the facts the design rests on

| On the PS5 | On machine A | What the core does about it |
|---|---|---|
| One 16 GB memory shared by CPU and GPU. The GPU reads and writes guest memory in place, at no cost. | A discrete GPU with 15.6 GiB of its own memory (driver budget 14.85 GiB) behind PCIe. A shader that reads imported host memory is far slower than one that reads device memory (164 against 45 ms of draws per frame in upstream's own note). | One device mirror of the guest memory the GPU uses, page ownership that says which side holds the newest bytes, and copies only when the other side reads (4.3). |
| A title declares GPU-visible memory by protection bits when it maps it, and usually maps far more than the GPU uses (whole heaps). | Protecting all of it would make ordinary heap writes fault. | Memory is tracked from its first use by a draw or dispatch, not from the mapping. The mapping only bounds what a shader may reach (4.3). |
| A draw is a few register writes into a buffer the title fills; it costs the title almost nothing. Titles issue thousands of draws and dispatches per frame. | Every host draw costs CPU time in the emulator and in the Vulkan driver. | Stage threads with no shared lock, register snapshots, templates, a cost target per operation (4.1, 4.2, 4.4). |
| Shaders ship as GPU machine code. They reach resources through pointers held in scalar registers and read descriptors (T#, S#, V#) from guest memory. | Vulkan wants SPIR-V modules, pipelines and bound resources. | One recompiler, one module per guest program, descriptors resolved on the CPU into heap slots and device addresses (4.4, 4.5). |
| An image instruction takes its T# and S# from scalar registers, so a descriptor is the same for every pixel of a wave. | | Resolving T# and S# on the CPU loses nothing; only tables indexed by the shader need a mirrored slot table (4.4). |
| Scalar registers hold one value per wave of 32 or 64 pixels. | A SPIR-V invocation is one pixel; the subgroup size here is 32. | A per-wave load can turn into a load per pixel. Step C10 checks for it. |
| The title learns that GPU work is finished from a label, an end-of-pipe event or a flip. Only then may it read results or reuse memory the GPU reads. | The host GPU finishes later than the emulator decodes, and results sit in device memory. | Completions are released in stream order after the results before them have landed (4.3, step C4b). |
| Titles commonly choose their render size from the GPU time they measure through those completions. | A faster emulator can make the title render more pixels. | Every timing carries the render size (4.7, step C0). |
| Textures lie tiled in guest memory and are sampled in place. Depth and colour compression data (HTILE, DCC, CMASK) is visible to the title. | Vulkan images are opaque and linear to the application. | Image records, detile kernels on the GPU, compression data derived from the record (4.3, steps C6, C7). |
| Ray tracing: the title's shader walks its own BVH in guest memory with an intersect instruction. | Ray queries and acceleration structures (`VK_KHR_ray_query` is present). | The emulated walk exists in the recompiler. A native pass is phase RT, after the base games (4.9). |
| Guest code follows the System V ABI: a leaf function may use 128 bytes below the stack pointer. | Windows pushes an exception frame onto the faulting thread's stack. | Titles are relinked with `--frame-red-zone` and carry the mark `APS5RZ01`; only then are page faults used. |
| 8 cores, 16 threads (Zen 2); titles pin their threads. | 8 cores, 16 threads, about twice as fast per thread. | About three busy emulator threads fit beside the title's own (4.2, 4.7). |

Machine A, checked with `vulkaninfo` on 2026-11-21: memory type 4 is device-local and
host-visible on the 15.6 GiB heap; sparse address space and largest buffer are 1 TiB;
`residencyNonResidentStrict` is true (an unbound mirror page reads zero); present are
`VK_EXT_extended_dynamic_state3`, `VK_EXT_shader_object`, `VK_EXT_graphics_pipeline_library`,
`VK_EXT_memory_budget`, `VK_KHR_ray_query`. `VK_EXT_map_memory_placed` is not present.

## 3. Principles

1. One path. A title runs on the new core or on an existing backend, chosen per title, never
   per draw.
2. No shared lock on the path of a draw, a dispatch or a guest page fault. Each stage has one
   owner; stages talk through single-writer queues and sequence numbers.
3. Work happens when memory changes, and the change is an event.
4. What the CPU can know about a draw is resolved on the CPU and baked into data. The shader
   reads a word itself only where the shader computes its address or the GPU wrote it. Such a
   word never enters a module key, a template key, a specialisation or a memo.
5. Shaders read and write device-local memory only. No shader reads a host import. The CPU
   reaches the mirror through a mapping where the device offers one.
6. A GPU result stays on the GPU until the CPU reads it. A range the CPU read in recent frames
   is copied back ahead of the read.
7. One ledger owns every byte of device memory under one budget. A guest range has at most one
   device copy: a buffer's copy is its mirror chunks, an image's copy is its record. Image
   bytes are in the mirror only while a reader that is not the image needs them.
8. Every stage has a cost target per operation in microseconds and a counter that reports it.
9. A completion the guest can see is released in stream order, after everything before it is
   resolved and uploaded and after the GPU results it vouches for have landed.
10. The new core stands on its own code (section 0.2).
11. A timing without the render size the title chose is not a measurement.

## 4. Architecture: the finished core

### 4.1 Front end

- `sceAgcDriverSubmit*` copies the command buffer and every register list it points to into a
  ring arena and returns. Decoding starts from the copy, because a title may rewrite a buffer
  after submitting it.
- One decode thread per guest queue in use. It turns packets into operations. Each operation
  carries a sequence number in its queue and the ids of the register snapshots it uses.
- Register state lives in snapshot rings (context, user config, shader registers). A write
  makes a new snapshot; draws between two writes share one id, so state is hashed once per
  change and not once per draw.
- A wait is an operation. It names the address, the test and the operations before it. Later
  stages use it as an ordering edge between queues, so a queue that waits on another queue's
  label needs no lock and no poll. This answers GPU-side waits only; when the guest sees a
  label is section 4.3.

State: built (`Front.cpp`, `Decoder.cpp`, `Order.cpp`, `SnapshotRing.hpp`, `SpscRing.hpp`).
Decode measured 0.33 to 1.04 us per operation in the R1 runs.

### 4.2 Stage pipeline

```
guest threads -> submit copy -> decode (per queue) -> resolve -> order -> record -> GPU
                                                         |                   |
                                                    memory core          host copy
```

| Stage | Threads | Does | Target per operation |
|---|---|---|---|
| Decode | One per active queue | Packets to operations, snapshots | 4 us |
| Resolve | One by default, more when a title needs them | Template lookup, descriptor memo, slots, addresses, access set | 4 us on average |
| Order | One, only when resolve or record is shared out | Merges resolved operations into one order; pass and barrier decisions from access sets | 0.5 us |
| Record | One by default, more when a title needs them, one command buffer each | Bind, push, draw for a run of operations | 4 us |
| Host copy | One | Uploads, batched sparse binds, landing of readbacks, release of completions | n/a |
| Compile | Pool | Recompiles and pipeline builds; never on the threads above | n/a |

- The 4 us targets are what Kyty reached per stage on GTA5 in Performance mode (1,330 draws,
  5.3 ms per stage thread per frame). They fit a 6 ms thread budget only up to about 1,500
  operations per frame.
- Resolve can be shared out because a template hit reads only its own memo and the snapshots
  it names. The order stage puts results back in sequence: sequence numbers within a queue,
  plus the edges from waits and from overlapping access sets.
- Record can be shared out because the order stage has already decided passes and barriers.
  Each record thread fills its own command buffer for a run of operations; a pass that spans
  two buffers is suspended in one and resumed in the next. Buffers are submitted in order.
- An operation's access set is produced by resolve and is the only input to pass merging,
  barriers, residency and ownership decisions. Nothing later derives it again.
- Ghost of Yōtei needs both stages shared out and the cost per operation below the target: at
  4 us one stage takes 25.6 ms of a 16.7 ms frame.
- Thread count: machine A has eight cores. About three busy emulator threads fit before stages
  compete with the title's own threads.

State: resolve has its own thread; record still runs under the old path's GPU mutex; there is
no order stage and no host copy thread yet (step C12).

### 4.3 Memory core

One component replaces upstream's imports and write watch.

**Which memory is tracked.** A range is registered when a draw or dispatch first uses it. The
ledger then holds the whole guest mapping the range belongs to. The protection bits a title
gave at map time (`libkernel\DirectMemory\DirectMemory.cpp`) only bound what a shader may
reach through a computed address.

**Pages.** 4 KiB pages in 4 MiB regions with per-region dirty flags. States: untracked,
CPU-clean, CPU-dirty, GPU-owned. Stacks are never protected.

**Second view.** Every emulator thread reads and writes guest memory through the alias
(`GuestMemory::OwnStoreTarget`). Only guest threads fault.

**Write tracking.**
- A guest write to a CPU-clean page faults, marks the page dirty and opens it. The fault takes
  no core-wide lock: the page state changes by compare-and-swap, the dirty set is a bitmap per
  region, and the protection call is made outside any lock of the core (step C5b).
- The opened unit grows: a second fault next to the first opens the rest of the 64 KiB chunk,
  further adjacent faults open up to the registered range. Pages opened without a fault of
  their own are checked by hash before upload. The range of an image opens whole on its first
  write, because the image is detiled again whole.
- A page written in three consecutive submissions becomes busy: it stays writable and is
  compared by hash at each submission instead of faulting.
- If faults still cost more than 0.3 ms per frame after that, the store that faults is patched
  to write through the second view and mark a card table; the fault stays as the fallback
  (step C5c).

**The mirror.**
- One sparse device buffer whose offset is the guest address, over the one address window in
  which GPU-used memory lies (step C8 makes it one buffer; today there is one per 64 GiB).
  Any guest range is one contiguous device range whatever was bound when.
- Device memory comes from 256 MiB pools and is bound in 64 KiB chunks to the ranges in use. A
  chunk two ranges share is bound once.
- Binds and unbinds are collected and sent as one `vkQueueBindSparse` per batch. The batch
  that first uses the chunks waits on the bind through a semaphore or queue order; no CPU
  thread waits on a fence for it. A chunk is unbound only after every batch that used it has
  completed.
- Budget from `VK_EXT_memory_budget`; ranges not used for longest are unbound first. A range
  with GPU-written bytes that have not landed is never evicted.

**CPU access to the mirror (revision 3).** Pools are allocated from the device-local,
host-visible memory type and stay mapped. Rules:
- A CPU read through the mapping is valid once the batch that last wrote those bytes has
  completed. It needs no submission. Used for labels, small reads and page-sized readbacks.
  Bulk readback still goes through a GPU copy into a cached staging buffer, because reads
  through the mapping are slow per byte.
- A CPU write through the mapping is allowed only when no submitted, uncompleted batch reads
  or writes that chunk. Otherwise the write is a staged copy recorded in stream order.
- On a device without such a memory type, both directions fall back to GPU copies.

**Uploads.** A submission uploads the dirty list once, in stream order. Bytes the GPU wrote
and that have not landed are skipped: the mirror is newer there.

**GPU writes.** A write through a descriptor the CPU knows has its range in the access set. A
write through a computed address sets a bit in the GPU write bitmap, read after the batch.
Both go into the written-byte set (`GpuWritten`: exact byte ranges, each with the serial of
its last writing batch). Whole pages of a write become GPU-owned unless the page is CPU-dirty
or busy ("the CPU wins the page state"); partly written pages stay open and are merged by
byte range.

**Reading GPU results.**
- A guest access to a GPU-owned page faults. The driver never runs on the faulting thread: the
  thread parks, a resolver thread waits for the writing batch's serial, reads the page from
  the mirror, lands it through the second view and reopens the page. A thread that holds the
  GPU mutex, and the resolver itself, are not parked: their page is opened at once with the
  bytes before the write remembered, and merged when the batch ends.
- Emulator reads go through `GuestMemory::FlushGpuWrites`, which lands the written bytes of
  the span first.
- Readback ahead: pages that faulted for reading in recent frames are landed as soon as their
  writing batch ends.
- What a readback may land: only bytes the GPU wrote. A range a shader declares as written
  but did not write is never copied over RAM (the bytes are compared with the bytes uploaded
  before the dispatch), and a page the CPU also writes is merged by byte range.
- A small driver store into a GPU-owned page goes to RAM through the second view and into the
  mirror, and its bytes leave the written-byte set.

**Releasing completions (revision 3).** A label store, an end-of-pipe event and a flip event
become visible to the guest in stream order, and only after
1. every operation before it in its queue has been resolved and its uploads captured, and
2. the readback-ahead pages written by batches before it have landed.
Then predicted reads do not fault, and a landing cannot overwrite a CPU write the title was
entitled to make. The guest may run at most a fixed number of frames ahead of the host GPU's
completed work (two unless measurement says otherwise). No side path releases a label early.

**Images over guest memory.**
- One record per image memory range, decided at record time; other formats are views of it.
- A CPU-written image is detiled by a GPU kernel from a staging copy of its guest bytes. Its
  range holds no mirror chunks afterwards.
- A GPU-written image is written back into the mirror only when another shape or a buffer
  reader overlaps it, and its mirror chunks are released again when that reader is gone.
- HTILE, DCC and CMASK are derived from the record when something reads them; a write the
  core recognises as a clear or a resummarise changes the record's state, not stored bytes.
- The flip reads the record.

Consequence: staging shadows, baselines, copy-back kernels, store imports, import windows,
settle reads, collect epochs, the GPU page table, written imports, chunk switches and mapped
prefixes have no counterpart. A guest buffer range has one RAM copy and at most one device
copy; an image has its RAM copy and its record.

### 4.4 Descriptors and addresses

- **Images and samplers:** one global heap. Resolve turns each T# and S# into a slot through a
  memo that a write event on its source pages invalidates. A table of T# that a shader indexes
  is mirrored as a table of slots (`TableMirror`).
- **Buffers the CPU knows:** resolve turns the V# into a device address and a bound and writes
  both into the per-draw data. The shader does no lookup.
- **Addresses the shader computes:** the shader adds the mirror's base to the guest address.
  There is no table and no load per dereference. One buffer over one window makes this a
  single add (step C8). A page with no memory bound reads as zero for that submission
  (decision 3).
- **Words the GPU wrote** (indirect arguments, V# formed by compute, tables a dispatch
  stores): resolve does not read them. The shader loads them from the mirror at execution
  time. Indirect draws and dispatches take their arguments from the mirror. None of these
  values enters a module key, a template key, a memo or a specialisation, so no CPU read waits
  for the GPU. T# and S# words stay resolved on the CPU; one that the GPU wrote keeps today's
  ordering, a wait for the batch that wrote it.
- **Per-draw data:** a ring buffer bound once per batch; a draw pushes one offset.
- **Templates:** key = program ids, interface, snapshot ids. Value = pipeline, target set,
  descriptor commit and the list of words to refill. A hit skips everything except the refill
  and the access set.

A dispatch and a draw take the same route. Descriptor reads, hash tables and miss rings on the
GPU stay excluded (section 8).

### 4.5 Shaders and pipelines

- Upstream's recompiler (`core\shader\recompiler`) is the only compiler. It emits one module
  per guest program and static interface, independent of the data bound to it. Additions: the
  mirror addressing mode, and a load form for words the GPU wrote in place of specialising on
  captured values. Today a program is built once per captured resource state.
- A per-wave scalar load must not become a buffer load per pixel where the value is the same
  for the whole draw (step C10).
- Disk cache, pipeline journal and replay, and pipeline libraries with fast link stay as in
  upstream. Recompiles and pipeline builds run on the pool; a draw whose pipeline is not ready
  waits for the fast link only.
- The console has no pipeline objects: its state registers are independent of its shaders.
  If cold starts are still slow with one module per program, the state still baked into
  pipelines (blend, topology class, polygon mode, sample count) moves to dynamic state
  (step C9b).

### 4.6 GPU timeline

- Consecutive draws into the same targets share a pass. A barrier is recorded only where two
  access sets conflict.
- Compute dispatches with no conflict between them get no barrier and no flush.
- Indirect draws and dispatches read their arguments on the GPU unless a wait orders a CPU
  read of them.
- Guest submissions are merged into few host submissions per frame; a batch ends where a
  completion or a readback needs it.

### 4.7 Frame rates above the console's, and what a timing means

A title written for 30 or 60 fps does not run at 100 because the emulator is fast. Three
things must hold per title; none is known for the five titles today.

- **Pacing.** The core offers a virtual blank rate above 60 Hz and can ignore the flip rate a
  title sets (`APS5_UNPACED_FLIPS` in upstream commit b3691289ef does part of this).
  Presentation uses mailbox or immediate mode.
- **Game speed.** A title that advances its simulation by a fixed step per frame runs fast
  when the blank rate is raised. Such a title cannot pass its own rate without a per-title
  patch, which is outside this design.
- **Guest threads.** The frame cannot be shorter than the title's own work per frame.

A title that fails one of these has its target restated as the highest rate its own pacing,
speed and threads allow (decision 7).

**Render size.** Many console titles lower or raise their render size from the GPU time they
measure. The route therefore prints, per 10 s, the viewport sizes its draws used, and
`bench.ps1` puts the most used one into the BENCH line (step C0). Two runs whose render sizes
differ are not a comparison, and a title that renders below its full size is not at native
resolution.

### 4.8 Platform

Windows with a framed title is the designed case. Linux can deliver faults on an alternate
stack, so it should need no framing and gets the same core; that is not checked. Nothing
Windows-only (`VirtualQuery`, `VirtualProtect`, WindowsMappings) is called from shared Unified
code. A title that fails framing stays on an existing backend. A device without sparse
residency buffers and device addresses stays on an existing backend.

### 4.9 Ray tracing (kept; built and tested after the base games)

User, 2026-11-21: ray tracing is not dropped. Emulated ray walks would be far too slow on the
titles that use them, so the native path stays in the design as an addition that is built and
tested once the base games run.

- A title that traces rays does it in its own shader: a loop that walks a BVH in guest memory
  with an intersect instruction. The recompiler translates that instruction today
  (`RdnaImageOpDecoder.cpp`, `Translation\src\MemoryInstructions\ImageInstructions.cpp`; test
  `tests\execution\Bvh64IntersectRay.cpp`), so the emulated walk is the correctness reference.
- The native path is a recompiler pass that recognises the walk and replaces it with a ray
  query, plus acceleration structures built from the guest's BVH data and refitted on the GPU,
  with their memory under the one ledger.
- Evidence (Kyty, GTA5 Fidelity, this machine, 2026-11-17): 41 fps with emulated walks, 66 fps
  with native ray queries. With native queries that mode was bound by the GPU (about 14 ms of
  GPU time per frame); with emulated walks it was bound by one CPU thread re-checking guest
  memory per draw, which section 4.4's memos remove. Kyty budgets 4 GiB for acceleration
  structures on this card.
- Which of the five titles have a ray-traced mode is not checked. Phase RT in section 6.3
  starts by finding out.

## 5. Decisions

Decisions 1 to 7 are from draft 1 and revision 2. Decisions 8 to 14 are revision 3. The user
can reverse any of them.

1. **Fallback per title, not per draw.** A per-draw fallback means two memory models that must
   agree at every draw.
2. **A contiguous device mirror for computed addresses and GPU-written words** (not a GPU page
   table). Evidence: AnyKyty's bench in issue 168 (mirror +6 % GPU on small serial dispatches,
   page table +28 %, host memory +57 %) and this recode's own faults with chunked residency.
3. **A mirror page with no memory bound reads as absent for that submission.** A strict mode
   that reruns the dispatch is possible for tests.
4. **Base tree: current `upstream/main`.** The new core lives in a directory upstream does not
   have.
5. **4 KiB tracking by page faults; a card table only if the counts call for it.** AnyKyty
   measured a write fault at about 5 us alone and 71 to 77 us under contention.
6. **Ray tracing.** Was: GTA5 judged in both modes, Fidelity with native ray tracing. GTA5 is
   no longer run; see decision 14.
7. **A title that fails section 4.7** has its target restated as the highest rate its own
   pacing, speed and guest threads allow, with no per-title game patches.
8. **The mirror is mapped on the CPU where the device offers device-local, host-visible
   memory.** Machine A does. Step C1a checks that a sparse buffer accepts that memory type and
   measures the read speed; if it does not, the GPU-copy path stays and this decision is
   recorded as not available here. **Not available on machine A (C1a, 2026-11-21):** the
   sparse buffer's `memoryTypeBits` is 0x2 (type 1 only, device-local, not host-visible), so
   type 4 cannot back the mirror. C1b is skipped; CPU reads of the mirror stay GPU copies.
9. **Completions are released after the results before them have landed, and the guest's lead
   is capped.** This follows the console's own contract between a title and its GPU.
10. **No core-wide lock in a guest fault; the opened unit grows.** Yōtei had about 2,000 write
    faults per frame on Kyty. Today a fault opens one 4 KiB page under the memory core's one
    mutex.
11. **An image-only range holds no mirror memory, and sparse binds are batched.** Today a
    CPU-written texture exists three times (RAM, tiled bytes in the mirror, the detiled image),
    and each bind is a queue call plus an untimed fence wait on a stage thread.
12. **The 16 GiB target is measured before it is restated.** RAM plus one device copy counts
    GPU data twice, so a title that fills the console's memory cannot meet it with any mirror.
    Ghost of Yōtei on the old path reached 20.4 GB of private commit and 6.0 GB of VRAM. Step
    C5 records the numbers on the route; the target is the user's to restate.
13. **Every timing carries the render size.**
14. **Ray tracing is kept as an addition after the base games (user, 2026-11-21).** Phase RT.

## 6. Build steps

### 6.0 Where the build stands (2026-11-24)

Built and committed on `recode/main`:

| Part | State |
|---|---|
| Front end (4.1) | Done: submit copy, decoder, snapshot rings, decode threads, wait edges |
| Memory core (4.3) | Done: relinker mark, protect tracking, page table and dirty list, protector, lock-free fault path, second view, GPU-owned pages, diff landings, readback ahead, completions released after landings |
| Mirror | Done: ledger, one sparse buffer over a window from guest address 0 (C8), recorded work reads and writes the mirror only, batched sparse binds |
| Draw path | Recompiler heap mode with mirror addressing, global heap and slot memo, resolve with templates, record; mesh-only and layered draws (M1) |
| Compute path | Dispatches of every queue, indirect dispatches, fill and copy kernels recorded |
| Image records | Colour targets with mip chains, layered and volume targets, sampled textures of every shape, storage shapes, depth and stencil records, DCC and CMASK fast clears, the flip from the record |
| No old path (rule 0.2) | Bridges removed (d2b12c1860): no draw, dispatch, image, render-target view, flip source or existing kernel serves the route; refused work stops or is skipped. Still on the existing driver: the packets of P1, dispatches before the recode has a device (P2), draw inputs and known buffers through host imports and the staging-shadow layer (C13) |
| Tools | Downgrade report, strict mode, `[recode-fatal]`, `[recode-packets]`, output probes, argument census, clips, NaN census, dumps of sampled and depth records (see `CLAUDE.md` and `docs\dev\ENVIRONMENT.md`) |

Picture state (not a gate): LEGO right in most runs, wrong in about 3 of 10 (LEGO A); FF draws
only its character, not its world, and shows the normals G-buffer instead of the lit scene
(FF A): the world's indirect draws carry no instances because the occlusion counts do not reach
the recorded culling (P1). Frame times: FF about 460 ms on the route while occlusion counting
syncs (P1); LEGO and the other titles not re-run since the bridges went.

### 6.1 Core steps, in order

Each step names the title for its check run. "Counters" are the `[recode-*]` lines printed
every 10 s. A step is marked done only when its whole Build list is in; a Build item no check
title uses may be moved to 6.3b (user, 2026-11-23), and only then.

**Done** (full text, measurements and commits in `notes\RECODE-done-steps.md`): I0 to I7 (image
path), I8 (build list; its picture condition is G1's), C0 (render size), C1 (ledger), C1a
(machine check), C2 (mirror only), C2b (batched binds), C3 (GPU-owned pages), C4 (landing,
readback ahead), C4b (release after landing), C5 (counters), C5b (fault path), C8 (mirror
addressing), C9a (smaller modules), C9 (closed: freezing kept, user 2026-11-23), M1 (mesh-only
and layered draws; the rest moved to U5). Not built by decision: C1b (the mirror's memory is
not host-visible here), C5c (only if a frame is shown to wait on faults).

#### P1. Every packet on the recode (user 2026-11-24: "Recode owns them")

- **Why.** The route moved draws, dispatches, kernels, images and memory to the recode, but
  other packets still run on the existing driver's handlers: occlusion sample dumps
  (`Driver::enqueueSampleDumpPacket` -> `VulkanDevice::DumpSamplesOnGpu` -> `Graphics::Recorder`
  sample segments), label stores (`Driver::recordDeferredLabels` -> `WriteLabelOnGpu`; C4b
  ordered them but did not move them), DMA, constant RAM dumps and clock stores. FF A's empty
  world is one of them: the recorded GPU culling reads the dump blocks (a per-frame ring;
  966d4409 and b11475af read them as known buffers) and sees no visible samples, so the
  world's indirect draws carry no instances; the CPU waits on those dumps every frame.
- **Build.**
  1. Inventory: `[recode-packets]` lists per queue and opcode every packet the existing loop
     processes on the route (dea1a7515d). Still to add: a downgrade kind `existing-packet` that
     strict mode stops on.
  2. Occlusion queries owned by the recode: its own query segments around recorded draws
     (precise occlusion), the dump folded on the GPU into the mirror, the range marked
     GPU-written (so recorded readers bind the mirror) and noted as a GPU write of its batch
     (so CPU readers wait for its landing); ready bit and block layout as the console writes
     them. In work 2026-11-24 (r642).
  3. Label stores, end-of-pipe events, DMA, constant RAM dumps and clock stores recorded by the
     recode in stream order against the mirror, released by C4b's rules.
  4. Then delete the route's use of the existing handlers for these packets.
  5. The recode's own record loop (user 2026-11-24: build it first; it is also C12's
     hand-off channel). Today the old queue worker sequences the route: it finds each
     resolved draw in `PacketTable` and enqueues a commit into the old `DrawPipeline`
     (`Draw.cpp` 423-458), where labels and waits are ordered too. Steps:
     L1. The decode lane hands every operation it emits (draws and dispatches with their
         resolved value, label stores, end-of-pipe events, waits, copies, fills, flips) in
         stream order to a per-queue record queue, with the packet words for the sync kinds.
     L2. A recode record thread per active queue takes them in order: draws and dispatches
         recorded as now; label stores through `StoreToMirror` and the recode's label table
         (`Labels::LabelTable`, 5328b21597) with end-of-pipe interrupts after the recorded
         work; waits answered from that table in stream order or, for CPU-set labels, from
         guest memory while recorded work is submitted; copies and fills into the mirror;
         ACQUIRE_MEM as a recorded barrier.
     L3. Route switch: the old worker no longer handles those packets on the route; a flip
         waits until the record thread has passed the operations before it.
     L4. Checks: `existing-packet` empty on FF and LEGO, pictures against the old path, no
         fatal; then flips and presentation (rule 7) as the next item.
     **Status 2026-11-24 19:50 (02387f127f, opt-in `APS5_RECODE_OWN_LOOP=1`): L1 to L4 met
     on FF and LEGO.** Downgrade table "0 using fallbacks" (no `existing-packet` row) on both,
     no fatal, FF 100 s and LEGO 105 s scenes complete. Built on the way: run-time stores
     (GPU clock COPY_DATA, GDS DMA) on the CPU in stream order; memory copies on the CPU in
     stream order after the recorded work before them has landed (in the mirror only with
     mirror stores, as the default route); occlusion dumps recorded in stream
     (`DumpSamplesToMirror`); COND_EXEC and SET_PREDICATION as per-operation guards read
     after settling recorded work; ACQUIRE_MEM and cache-flush EVENT_WRITEs seal the pass's
     write-only ranges. Frame time: FF p50 81 ms (default route 53 ms, the same scene),
     LEGO 65.9 ms (r752 loop 81.7). Picture: FF's random geometry (fault A) as on the default
     route; LEGO's fog and corners as before. Later the same night: CONST_RAM dumps resolved
     at decode from the decode state's CE RAM and stored in stream (01fa370d4b; no title
     issues them yet); indirect buffers changed after submission are counted
     (`[recode-segments]`, bcadf9477f; 0 in FF and LEGO, re-decode to be built when a title
     shows one); flips released by the record loop in stream order through
     `Driver::handFlipToRecode` (`Unified/src/DriverFlips.cpp`, user 2026-11-24 "Recode
     releases flips"; 806753177c): LEGO 64.1 ms with 1,028 of 1,029 flips released by the
     loop, FF 76.1 ms, 0 fallbacks on both. AoM on the loop: 0 fallbacks, 97 ms. The loop owns the
     route's stream by default since 5a86477aa4 (`APS5_RECODE_OWN_LOOP=0` turns it off;
     FF route 78.5 ms, 0 fallbacks). **P1 done 2026-11-25.** Note: a new build that changes
     the pipeline-cache key makes the loop wait on cold compiles synchronously (LEGO needed
     three runs at r778); the loop should not block on a build it can defer. Loop cost to cut later: a full GPU settle
     per condition read and per memory copy, 100 µs wait polls.
- **Inventory (r635, FF 45 s).** Queue 0: WRITE_DATA 0x37 1,710; COPY_DATA 0x40 3,129;
  EVENT_WRITE 0x46 7,416 (sample dumps); RELEASE_MEM 0x49 6,551; DMA_DATA 0x50 93; WAIT_REG_MEM
  0x3c 264 and 0x93 1,804; ACQUIRE_MEM 0x58 3,285. Queue 0x20: COPY_DATA 66, RELEASE_MEM 132,
  ACQUIRE_MEM 44, waits 66. Order of work: sample dumps (FF's world), labels, COPY_DATA and
  DMA_DATA, waits and ACQUIRE_MEM.
- **Check.** FF and LEGO: `[recode-packets]` shows only register, draw and dispatch packets;
  FF's world draws carry instances (`APS5_RECODE_ARGUMENT_CENSUS`); no per-dump waits in the
  frame time; picture against the old path.
- **Done when.** No packet on the route reaches an existing handler.

#### P2. The recode follows device replacement (new, 2026-11-24)

- **Why.** A title's first dispatches can run before its window exists. The driver then makes
  a windowless `VulkanDevice` and replaces it in `Driver::AttachWindow`
  (`Execution\src\Driver\Presentation.cpp`). The recode attaches at its first draw, after the
  window, so FF's first 4 dispatches (2097abdf, ec514f26) have no recode device; today they
  are dropped with a `[recode-fatal]` line. Attaching at the first dispatch made the recorder
  refuse the replacement ("a second device"); waiting for the windowed device deadlocked,
  because the game opens its window only after that work completes (r639, r640, reverted).
- **Build.** The recode attaches to the first device. On replacement, before the old device
  goes: wait for its recorded work, land every GPU-owned page and pending landing, write back
  GPU-written records; then drop every object of the old device (recorder ring, pools,
  pipelines, records, depth records, heap, mirror) and attach to the new one. Records and the
  mirror refill from guest memory as on a cold start.
- **Check.** FF: no "before the recode has a device" line; its first dispatches recorded;
  picture of the first seconds against the old path.
- **Done when.** No work on the route is dropped for want of a device.

#### C12. Stage threads with no shared lock

- **Build.** Resolve, order and record on their own threads; record outside the GPU mutex;
  one host copy thread that owns uploads, bind batches (C2b), landings and the release of
  completions (C4b).
- **State.** Resolve runs on each queue's decode thread, not on a thread of its own; record
  runs under the GPU mutex.
- **Check.** LEGO and FF Resonance: cost per operation and per frame for each stage.
- **Done when.** No lock is shared between two stages, and each stage reports its cost.

#### C11. Memos valid until a write event

- **Build.** Descriptor memos and image-table mirrors stay valid until a write event on their
  source pages. No revalidation per draw.
- **State.** Tables behind `APS5_RECODE_TABLES`.
- **Check.** Age of Mythology (its table stages cost 13 us for capture alone on Direct).
- **Done when.** Resolve shows no per-draw comparison of guest memory in its samples.

#### C10. Scalar-load check

- **Why.** Kyty's recompiler emitted per-wave scalar loads as buffer loads per pixel; making
  them uniform made one full-screen pass 22 times faster and took 15 % off Yōtei's GPU time.
- **State (code reading only).** `S_LOAD_DWORD` becomes `IrOpcode::ReadConstBuffer`
  (`Translation\src\MemoryInstructions\ScalarBufferInstructions.cpp`), a storage-buffer load
  per invocation; every data block is declared as a storage buffer.
- **Build.** Count such loads in the largest programs of Age of Mythology and Ghost of Yōtei.
  If they matter, give words that are the same for the whole draw a uniform form (push
  constants or a uniform block) and time one pass with it.
- **Done when.** The count and the timing are in section 10, and the uniform form is built or
  recorded as not needed.

#### C9b. Cold start check (only if still slow)

- **When.** A cold start still waits on pipeline builds for more than a few seconds in total
  (`downgrades.py compile`; `[recode-build]` shows cache hits).
- **Build.** Move the state still baked into pipelines (blend enable and equation, topology
  class, polygon mode, sample count) to dynamic state; `VK_EXT_extended_dynamic_state3` is on
  machine A. Graphics pipelines built on the builder threads instead of synchronously at
  record. Shader objects are a later option, not part of this step.
- **Done when.** Pipelines per program fall and the cold wait with them, or the step is
  recorded as not needed.

#### C13. The full route becomes the default

- **Build.** Whatever `-ExistingPath drop` still lists on the five titles; draw inputs and
  known buffers served from the mirror only (no host imports, `Shadow`'s staging-shadow layer
  off the route: `DrawRecorder` `drawInput` and `importedInput`); the route's switches
  (`APS5_RECODE_MIRROR_WRITES` and the others every route run sets) become the default.
- **Done when.** The full route is the default and a LEGO run prints no `[recode-fatal]`.
- **Status 2026-11-25 11:30.** The full route is the unified build's default (ff484e624d,
  `Unified::RouteValue`, `APS5_RECODE_ROUTE=0` off); LEGO prints no `[recode-fatal]`. Built
  for 007 on the way: draw pipelines built before the GPU mutex (d153ca49c8), known buffers
  to 1 GiB (4e4ffbb705), large known-buffer translation memo (8755a8cfc2); 007 reaches its
  title screen, 0 fallbacks. Still listed by `-ExistingPath log`:
  1. Ghost of Yōtei: a 1024x1 1D image (format 71, 0x50a131b800) written as storage and
     sampled: records accept only 2D, 2D array, cube and 3D (`VulkanSurfaceDevice::
     CreateFromResource` "surface dimension"; views know dimensions 2, 3, 6). Build 1D and 1D
     array records (image type 1D, 1D views for sampled and storage, detile and write-back
     of one row).
  2. Ghost of Yōtei: "resolve refused it: sampler tables" and "sampler lookup failed" with
     samplers using unnormalized coordinates (sampler-mode rows: "bound as normalized").
     **Built 2026-11-25:** 1D records (7dfa84d353); unnormalized samplers created from the
     recompiler's proof, coordinate-ignoring ones cleared (5c2356251a); sampler tables as
     heap slots after the image slots of the table mirror, read through the map in heap
     mode (7f2ad5b745); images proven read at their base level by an unnormalized sampler
     bound with a single-level view. Yōtei r827 reaches 32 s with no sampler fatal.
  4. Ghost of Yōtei (r827): two dispatches (7d30c9a4, 2d210331) sample a 1x1 format-56
     image at 0x50091c0000 with no record; then the existing presenter aborts on the DCC
     display buffer 0x5005160000 ("mixed DCC keys"): presentation still reads guest memory
     through the existing VideoOut presenter (rule 7 gap: the recode presents its own
     display records, decompressing DCC itself). **Built 2026-11-25:** constant-swizzle image
     modes bind inactive as on the old path (the 1x1 image asked a 1D view of a 2D record;
     6d0febaefb); DCC display buffers present from their GPU-written record (873d8af0ec).
     Yōtei r829: no fatal, 0 fallbacks, flips from records, the PlayStation intro at 60 s
     (17 fps), 2.2 fps at 110 s on a cold shader cache. LEGO r829: right run 47.6 ms.
     Then: retired records were kept alive by the recorder's table-image entries (4.3 GB in
     435 surfaces, then `vkAllocateMemory` -2; `[recode-footprint]` now prints "retired but
     alive"; 755b65416b); colour target merge images (partial write masks) appended to the
     slot snapshot (88d8dfc04a); storage images in image tables (dcace895a9). Yōtei r835:
     0 severe, 0 fallbacks, no fatal; the intro plays.
  5. Ghost of Yōtei (r835): an intermittent GPU hang ("recorded batch still running on the
     GPU"), then every queue waits behind it. Not seen with `APS5_RECODE_HANG_MARKS=1`
     (which serializes dispatches): likely a race between queues or a shader spinning on
     memory another queue writes. **Narrowed 2026-11-25 13:30** (`APS5_RECODE_HANG_PROGRAMS`,
     1346b8466c): the hung work is an indirect dispatch on queue 0x0 (57214581, 5e08b0c9,
     7d30c9a4 or 2d210331) whose 12 argument bytes hold a sort's (index, float) pairs, e.g.
     1065560064x57x1065560064 groups. The arguments live in one of two alternating arenas
     (0x20029xxxxx / 0x20096xxxxx) that queue 0x30's sort (2e06ea3d, 07a4407d) also uses as
     scratch; the old path never reached this content in 150 s. With
     `APS5_RECODE_WATCH=2e06ea3d644e03a8,07a4407de43a896e` the CPU write-faults on the
     argument pages at 97.3 s, after the consumer was recorded (~92 s): the CPU writes the
     arguments after the recode already ran the dispatch. Next: the queue-0 wait (WAIT_REG_MEM
     on a CPU-set value) that should hold those dispatches until the CPU has written them;
     find its address and why it passed (stale value, compare function, or a wait the
     decoder does not see). Ruled out: stale conditions (`APS5_RECODE_SETTLE_CONDITIONS=1`
     still hangs), snapshot arguments (mirror-first arguments still hang), early label
     stores (`Recorder::Idle` counts the open batch). r845 (`[recode-hang-wait]`, the last 64
     waits per queue): queue 0's waits before the hang all pass on GPU-issued labels; none
     waits for a CPU value. In the hung batch the four consumers' records sit 0x20 apart;
     on the GPU the last three held valid arguments (0x1x1, 26x1x1) while the CPU's copy held
     sort pairs, and only the first record (57214581's) held sort pairs on the GPU too. So
     the program that writes these records on the GPU either skips the first one or writes
     it after 57214581 runs. Next: find that producer (`APS5_RECODE_PROBE_OUTPUT` census of
     the programs recorded just before; its writes may go through addresses, "stores through
     addresses yes", which the uses list does not show). Order seen in r844 history: queue
     0x30's sort is submission 5402 but is recorded after queue 0's submission 5405 work;
     queue 0's 5412 consumer then reads the clobbered first record. Before each group of
     indirect consumers queue 0 runs single-group dispatches (1eaaa4f4, ffc07e78, e20c4491,
     c920f027, a72588d0) that store through addresses: the likely argument writers. Not the
     cause: the host copy thread (`APS5_RECODE_HOST_COPY_THREAD=0` still hangs, 2 runs).
     Open question: what on the PS5 keeps the sort's scratch use of the arena from
     overlapping the consumers' records (a wait the recode passes early, or async compute
     simply finishing first); a runner that keeps cross-queue submission order would test
     the second. Tested r860 (`APS5_RECODE_SUBMISSION_ORDER=1`, yield up to 5 ms per item):
     still hangs in 2 of 2 runs on the same consumer and a clobbered first record, so late
     async compute is not the cause (within that yield). r862: no COND_EXEC or predication
     guards near the hung consumers (`[recode-hang-guard]`). r863
     (`APS5_RECODE_WAITS_FROM_GUEST=1`, waits pass on guest memory only): still hangs, so
     waits passed early by recorded label stores are not the cause. Left: who writes the
     first argument record on the GPU (a shader-level probe of the address stores of the
     single-group dispatches before the consumers: 1eaaa4f4, ffc07e78, e20c4491, c920f027,
     a72588d0). Second lead: the sort's item count varies between runs (4,455 usually, 65,681
     once); a count too large makes its scratch reach the argument records. Many programs of
     this GPU-driven chain (the four consumers, 00689fdc, 69d0756c, b32e483a, 8032f587, ...)
     bind known buffers as null: `known-null` rows "descriptor 4 words, available mask 00,
     words 00000000 00080050 00000000 00005204" (base 0x5000000000, stride 8, NUM_RECORDS 0).
     **Closed:** the availability mask is set only on image-table descriptors; every
     descriptor captured through the SRT carries mask 00, so those words are the captured
     values: real empty placeholder buffers (NUM_RECORDS 0), bound null correctly.
     **r865/r866 (`APS5_RECODE_PROBE_OUTPUT`, new `APS5_RECODE_GPU_WATCH`):** the sort fills
     its whole 0x96C0-byte buffer (4,824 key/index pairs) on every pass, and the argument
     records lie inside that buffer (offset 0x9E0 on). Writers of the arena seen on the GPU:
     the sort (queue 0x30), dad9307e (queue 0x52, writes the records), the next sort
     (clobbers them), 17ad0344 (queue 0x48). Queue 0x30's only wait before the sort is
     WAIT_REG_MEM ">= 0" at 0x2000008180, never stored to: a no-op on the PS5 too. So the
     reuse is the game's design, and on the PS5 the sort must stop before offset 0x9E0,
     i.e. sort fewer than ~316 items. Our item count is likely too large (a culling or
     visibility result upstream). The count is the 4 bytes at 0x200963e8c0 written by
     35541115 (sort dispatches are fixed at 76 groups). Next: census that count
     (`APS5_RECODE_PROBE_OUTPUT=35541115301704f3`, `APS5_RECODE_PROBE_AFTER=0`) and trace
     what feeds it. **r866/r867 (GPU watch over the whole sort buffer):** 35541115 writes
     0x14 (20) words, not the sort length; the sort always covers 4,824 entries, so the
     overlap is by design. Per frame: sort (queue 0x30) -> 80bde45e or 17ad0344 rewrite the
     front (up to ~0xbf3) -> dad9307e (queue 0x52) writes five 32-byte records at
     base+0xc00..0xc93 -> 35541115 writes counts at 0xbe0.. -> the consumers read
     base+0xea0, 0xec0, ... In both measured runs the consumer reads 0x2A0 bytes past
     where dad9307e wrote (0x2002964521 vs 0x20029647c0; 0x2002964c01 vs 0x2002964ea0):
     nothing writes the records the consumers read. **Lead:** the indirect argument address
     (`Pm4::DispatchArgumentAddress`: SET_BASE dispatch base + packet[1]) is off by 0x2A0
     for these dispatches, or dad9307e writes to the wrong place. Next: trace SET_BASE
     (0x11) values and the DISPATCH_INDIRECT offsets on queue 0 for submission N together
     with dad9307e's store addresses. **r868/r869:** the consumers' SET_BASE comes from the
     command stream (`APS5_RECODE_TRACE_INDIRECT`: base 0x2002964b40, offset 0) while
     dad9307e wrote its records at 0x2002964891 (0x2B0 earlier): the arena layout moves
     every frame and dad9307e writes where an earlier frame's records were. With dad9307e
     materialized in full instead of refilled from its result template
     (`APS5_RECODE_PROBE=dad9307e29ce4108` 2 runs, `APS5_RECODE_NO_TEMPLATES=dad9307e29ce4108`
     2 runs) the hang does not occur; `APS5_RECODE_TEMPLATE_VERIFY=1` reports no word
     difference, so either the template differs in something the verify does not compare
     or the slower full path only shifts timing. With the hang gone the run ends at 95 s:
     "resolve refused it: direct slots lie outside the push data" and a stack overflow
     (0xc00000fd). Next: compare dad9307e's store address inputs (pointer words) between a
     refilled and a full materialize; then the push-data refusal and the stack overflow.
  3. Mirror-only draw inputs (no host imports, `Shadow` staging off the route): the global
     consume on dirty draw inputs costs 200 ms per FF frame on the loop (r809); needs a
     ranged consume that covers what the draws really read. **2026-11-25 (69b20b6bf8):**
     with `APS5_RECODE_MIRROR_INPUTS=1` a CPU-dirty input consumes only its own range
     (FF 110 -> 47.5 ms against default 50.9) and small CPU-written inputs are copied into
     the ring. Picture: FF's character is missing with mirror inputs (global or ranged
     consume, with or without ring copies) and present on the default route, so some
     input served from the mirror lacks data the import or staging path has (default:
     204,618 resident and 118,680 GPU-written inputs; mirror: 247,847 read from the mirror).
     Next: find the character's draw and its inputs (a compute result written back to
     guest memory but not to the mirror is the first suspect). Ruled out (r848,
     `APS5_RECODE_MIRROR_INPUTS_LANDED_IMPORTS=1`): inputs on GPU-written pages whose writes
     have landed (116,076 taken through imports, character still missing); small CPU-written
     inputs (copied into the ring). Left: the ~123,000 inputs on clean pages read from the
     mirror. Next: split the mirror-served inputs by address (an environment mask) until the
     character's range is found, then compare the mirror's bytes with guest memory there.
     **Found (r852 to r855):** (a) the record loop's CPU memory copies (6,753 per FF run,
     16.7 MiB) wrote guest memory but never the mirror: driver stores now queue an upload
     into the mirror (`APS5_RECODE_DRIVER_STORE_UPLOADS`, default on); mirror/guest
     mismatches of sampled inputs fell from 17,259 to 7 of ~280,000 (`APS5_RECODE_INPUT_COMPARE`).
     (b) Within each declared input range the mirror now matches guest memory, yet the
     character stays missing; making 1 MiB past each input resident
     (`APS5_RECODE_MIRROR_INPUTS_MARGIN=100000`) brings part of it back (a stretched shape):
     the character's draws read past their declared input range, which the staging path
     hides because it stages whole allocations. Next: compute the range a draw really reads
     (vertex count against max index, instance data, strides) and serve and consume that.
     r856: vertex fetches not clamped to NUM_RECORDS (OOB_SELECT 0/1) now extend to the end
     of their guest allocation on the mirror path; the character is still missing, so its
     attributes are not of that kind. A 16 MiB margin stalls FF at its first frame. Open
     reading of the 1 MiB margin result: the extra residency binds pages that other reads
     of the character's draws use (buffers the shaders read through descriptors in the same
     allocations), which the staging path binds by serving whole allocations. Next: list the
     character draw's known buffers and whether their pages are resident in mirror mode
     (`[recode-uses]` of its programs; unbound mirror pages read zero).
     **Solved (r857/r858, 35f7560e7d):** the mirror path never registered the input
     allocations with the memory core (`Shadow::ServeOwned` reserves them and makes them
     resident; without it CPU writes to those pages were never tracked, so they looked clean
     and the mirror kept old bytes). Mirror inputs now go through `ServeOwned` first (it
     serves from the mirror; `APS5_RECODE_MIRROR_INPUTS_SERVE_OWNED=0` off). FF with mirror
     inputs: character back, picture as the default route, 48.7 ms, 0 fallbacks, no input
     from staging or imports. LEGO with mirror inputs: right (2.29/2.38). **Item 3 done
     2026-11-25 (r861):** mirror inputs are a route default (`Unified::RouteValue`,
     `APS5_RECODE_MIRROR_INPUTS=0` off). Single runs: FF 48.1 ms (staging ~51), LEGO 50.8
     (staging 47.8 warm), AoM 50.1 cold (staging 44.9); pictures as before, 0 fallbacks.
     The LEGO/AoM cost is for after C13 (per-draw owned-serve lookups).
     Same day: today's table-image sweep (755b65416b) rebuilt AoM's tables whole on every
     retirement (AoM 220 ms); retired records are now released in place and only their
     positions rebound (35f7560e7d): AoM 44.9 ms (was 65), Yōtei retired-but-alive stays
     near 0.

### 6.2 After the core: the base games

The order rule ends here. From now on work goes to whatever has the highest impact on frame
time, one title at a time.

- **G1. LEGO's picture against the PS5 reference** (`runs\lego\reference-ps5.png`). Parked
  faults to start from: black at 94 s on the full route since r388, missing specular and wear
  (highlights 0.2 % against 0.61 %). Tools first: `repeat-run.ps1`, `detail.py`, the clip
  tools, `bisect-programs.ps1`.
- **G2. Each title on the full route**, in the order Ghost of Yōtei, 007 First Light, Age of
  Mythology, LEGO, FF Resonance:
  1. `debug-run.ps1 -Route full -ExistingPath log`; every `[recode-fatal]` reason is a build
     item.
  2. Picture against an old-path clip of the same length.
  3. Stage cost per operation and per frame, faults per frame, GPU time per program, render
     size. Fix the largest cost, measure again.
  4. Share resolve and record out when a stage thread is over its budget at the measured cost
     per operation.
  5. The three checks of section 4.7: pacing lifted, game speed, busiest guest thread.
- **G3. GPU stage** (phase R5): the uniform scalar form if C10 found the pattern, then the
  largest programs per title one by one. Report GPU work against 8.4 ms per frame (14 ms on
  007 and Yōtei), or a floor per program with the cause named.
- **G4. Footprint, tests, cut-over** (phase R6): footprint against decision 12, remaining
  draw kinds, the driver tests retargeted to the new core, and the existing backend no longer
  needed for the five titles. Every title at its target or at a recorded floor with the cause
  named.

### 6.3 Ray tracing, after the base games (phase RT)

Start when G2 is done for the five titles in their default modes.

- **RT1. Find the users.** Add a downgrade kind for a program that contains a BVH intersect
  instruction, so `debug-run.ps1` lists them. Run each title's ray-traced mode if it has one
  and write down which programs walk a BVH. If none of the five does, say so to the user;
  GTA5 Fidelity is the known case and is theirs to bring back.
- **RT2. Measure the emulated walk.** Picture and GPU time per program in that mode. This is
  the reference for correctness and the baseline for speed.
- **RT3. Traversal pass.** A recompiler pass that recognises the walk and emits a ray query
  when the device has `VK_KHR_ray_query`. The emulated walk stays for devices without it and
  for tests.
- **RT4. Acceleration structures.** Built from the guest's BVH data, refitted on the GPU,
  rebuilt on a write event to their source. Their memory comes from the one ledger under a
  budget of its own (Kyty used 4 GiB on this card).
- **RT5. Test.** Each ray-traced mode against RT2: same picture, GPU time per program, frame
  time with the render size. No target is set; report what it reaches and what bounds it.

### 6.3b Shapes no base title uses, after the base games (phase U)

(User, 2026-11-23: items of a core step that neither check title uses become steps here, and
the core step is marked complete.) Each is counted or refused loudly today, so a title that
needs one names it in its first run. Start one when a title shows its counter or refusal.

- **U1. Multisampled rendering** (from I5 "multisample clears"). Colour and depth records with
  samples, resolves, sample order, multisample clears, multisampled sampled and storage images.
  Today: every multisampled draw is refused (`DrawRecorder.cpp` "multisampled color target",
  "multisampled depth") and image records refuse multisampled images. Shows as `[recode-fatal]`.
- **U2. Layered depth targets** (from I5). Layered colour targets and volume slices are drawn
  since M1 (174f8a0825); layered depth targets are still refused ("depth target without a
  record"). Shows as `[recode-fatal]`.
- **U3. HTILE whose tiles disagree** (from I5). A GPU write that clears only some tiles of a
  depth target. Today: left alone; counted in the status line ("HTILE checks finished: ...
  tiles disagree (not applied)"), 0 in FF and LEGO. Needs the tile order inside an HTILE block
  (1024x512 texels, 32768 bytes), then a per-tile clear in place of the whole-target one.
- **U4. DCC keys that disagree** (from I6). A fast clear of part of a colour target. Today:
  left alone; counted ("DCC checks that found keys that disagree (not applied)"), 0 in FF; LEGO
  writes no metadata. Needs per-block DCC addressing (one key per 256 bytes of the tiled
  surface) in `Unified\shaders\DccClear.comp`.
- **U5. The rest of M1's draw kinds** (from M1). Indirect mesh draws, mesh strips with primitive
  restart, emulated barycentrics on the mesh path, tessellation with and without geometry. Today:
  refused by name ("mesh: indirect draw", "mesh: strip with primitive restart", "mesh: emulated
  barycentrics", "tessellation"); FF and LEGO issue none.

### 6.4 Phases in `progress.json`, and the targets to report against

| Phase | Covers | Target to report against (never a gate) |
|---|---|---|
| R1 | Front end, decode, resolve (done) | Decode and resolve each at or under 4 us per operation |
| C | Section 6.1 | Coherence work at or under 0.3 ms per frame; copies at or under 0.8 ms of GPU per frame (1.5 ms on 007); resolve plus record at or under 8 us per dispatch |
| R5 | G3 | GPU work at or under 8.4 ms per frame (14 ms on 007 and Yōtei) |
| R6 | G4 | Footprint per decision 12; driver tests pass on the new core |
| RT | Section 6.3 | None set |
| U | Section 6.3b | None set |

Keep `progress.json` current: `current` is the phase in work, `"active": true` marks the step
in work, and `next` names what follows. Each phase may carry a `kind`: none for a core phase
(counted in the pane's overall bar), `"after"` for RT and U (shown under "after the base games",
not counted). Step names start with their id (`C4b ...`);
the pane then shows no running number.

## 7. What this can and cannot deliver

- It removes the costs that come from how the CPU side is organised. It cannot make a title
  faster than its own guest threads or its own shader work.
- Where each title stands (2026-11-21):

  | Title | Measured | What its target depends on |
  |---|---|---|
  | FF Resonance | 16.7 ms in its last run (libs r232); GPU work 3.4 of 8.4 ms on Direct | The stage targets and section 4.7. 16.7 ms may be its own pacing |
  | LEGO | 68.2 ms on the full route, 25.5 to 26.9 ms on the old path; GPU work 7.6 of 8.4 ms on Direct | The core steps (the route still mixes both memory models), then section 4.7, with little GPU margin |
  | Age of Mythology | 424.2 ms in its last run (libs r168d); 66 ms and GPU work 12.7 of 8.4 ms on Direct, title screen only | A third off the GPU time as well as the CPU. No in-game scene is measured |
  | 007 First Light | Not measured on the route; 54.9 ms and GPU work 9.3 of 14 ms on Direct, title screen and menus only | Cost per dispatch |
  | Ghost of Yōtei | 85 to 90 ms on the old path, then a standstill; 11.6 fps on Kyty with three walls at 45 to 75 ms (one stage thread, the GPU, guest threads in write faults) | Resolve and record shared out, GPU-written words left to the shader, the fault path, the scalar-load form. The least certain target |

- Not measured anywhere: whether any of the five runs above 60 fps at normal speed; their
  guest thread time per frame; which of them change their render size; the numbers of step
  C1a; write faults per frame on this core; how often a computed address reaches a mirror
  page with no memory bound; which of them trace rays.
- Known risks: a title that puts a stack inside GPU-used heap memory; a title whose CPU and
  GPU write the same pages every frame, which pays a merge per frame whatever the unit; the
  size of the work.

## 8. Not in this design

- Descriptor reads, hash tables and miss rings on the GPU (measured slower on Direct;
  AnyKyty's step E5 has a synthetic bench only). Data words and V# the GPU wrote are not
  descriptors in this sense.
- Upscaling or frame generation as a way to reach a target.
- Per-title game patches to change a frame cap or a time step.
- A second shader recompiler.
- Changes to the relinker, kernel or libraries beyond the second view, what framing already
  has, and the store patcher of step C5c if it is built.
- Running guest queues on several host queues at once.

## 9. Code map

All paths are under `wt-recode\core\`. Check them before relying on them.

| Subject | Files |
|---|---|
| The new core | `libs\prx\libSceAgcDriver\Unified\{include,src}` |
| Front end | `Front`, `Decoder`, `Operation.hpp`, `SnapshotRing.hpp`, `SpscRing.hpp`, `Order` |
| Resolve | `Resolve`, `Templates`, `Heap`, `VulkanHeap`, `SlotRequests`, `TableMirror`, `TableResolutions`, `Programs` |
| Record | `DrawRecorder`, `RecordDraw`, `RecordState`, `Targets`, `DataRing`, `DrawBindings` |
| Memory core | `MemoryCore`, `MemoryCoreOwned.cpp`, `PageTable`, `Protector`, `GpuWritten`, `Events`, `Shadow` (glue and the `[recode-memory]` line) |
| Mirror | `Residency` (ledger), `VulkanResidency` (pools, sparse buffer, binds), `ResidencyService` (serve, uploads, readback), `ResidentUploads`, `MirrorHost`, `GpuPageTable` (goes with C8) |
| Images | `Images` (`SurfaceTable`), `VulkanImages`, `ImageDetile` (`SurfaceDetiler`) |
| Reports | `Downgrades` (table, strict, fatal), `ProofVerdict`, `RecordVerify` |
| Tests | `libs\prx\libSceAgcDriver\tests\Unified*.cpp`, `tests\cmake\agc_unified_*`, device tests in `tests\execution\` |
| Guest memory, second view, flush hook | `libs\prx\libSceAgcDriver\Execution\{include,src}\GuestMemory.*` |
| Fault entry, protection, arena | `libs\prx\libc\src\GuestArena.cpp` |
| GPU protection bits at map time | `libs\prx\libkernel\DirectMemory\DirectMemory.cpp` |
| Labels in the existing driver | `libs\prx\libSceAgcDriver\Execution\src\Driver\Synchronization\` |
| Recompiler | `shader\recompiler\` (`RdnaDecoder`, `Translation`, `Optimization`, `SpirvBackend`) |
| Relinker and red-zone framer | `relinker\` |

## 10. Measured facts

Numbers this design rests on. Add a row when a step measures something.

| Fact | Value | Source |
|---|---|---|
| Shader read of a host import against a device buffer (NVIDIA, Windows) | 164 against 45 ms of draws per frame | Upstream's note |
| Mirror against GPU page table, small serial dispatches | +6 % against +28 % GPU; host memory +57 % | AnyKyty, issue 168 |
| Write fault | about 5 us alone, 71 to 77 us under contention | AnyKyty |
| Write faults in game | GTA5 about 103,000 per 10 s (2.3 s in the handler); Yōtei about 2,000 per frame | AnyKyty |
| Stage cost per draw, GTA5 Performance (1,330 draws) | about 4 us on each of four stage threads, 5.3 ms per frame each | Kyty on machine A, 2026-11-17 |
| Stage cost per draw, GTA5 Fidelity | resolve 17.9 us, record 8.9 us | Kyty on machine A |
| GTA5 Fidelity, emulated ray walks against native queries | 41 against 66 fps | Kyty on machine A |
| Per-wave scalar loads made uniform | one full-screen pass 22 times faster, 15 % off Yōtei's GPU time | AnyKyty |
| Yōtei per frame | 3,789 draws, 2,616 dispatches | Issue 168 |
| Yōtei on the old path, libs r394 | 85 to 90 ms for a minute, then a standstill; 20.4 GB private commit, 6.0 GB VRAM; pictures black | `runs\yotei\old-boot2`, 2026-11-21 |
| LEGO, full route against old path | 68.2 ms against 25.5 to 26.9 ms | r393, 2026-11-21 |
| LEGO mirror | 2,848 to 2,872 ranges, 1,921 to 1,935 MiB bound | r393 |
| LEGO cold start | 304 of 330 s compiling; modules of 3.5 to 4.2 MB | 2026-11-20 |
| Decode | 0.33 to 1.04 us per operation | R1 runs |
| Direct, per title (frame; CPU work over the workers; GPU work) | FF 28.1; 30.4; 3.4 ms. LEGO 26.3; 25.3; 7.6 ms. 007 54.9; 75.8; 9.3 ms. Age of Mythology 66; 63.2; 12.7 ms | `TITLES.md`, 2026-11-17 (file deleted) |
| Machine A memory | Type 4 device-local and host-visible on a 15.6 GiB heap, budget 14.85 GiB; sparse address space 1 TiB | `vulkaninfo`, 2026-11-21 |
| C1a: sparse buffer accepts the mapped type | No: memoryTypeBits 0x2 (type 1, device-local only); decision 8 not available, C1b skipped | `tests\execution\MirrorMachineCheck.cpp`, 2026-11-21 |
| C1a: CPU read of the mirror through a GPU copy (record, submit, fence, copy out) | 41.1 us for 4 KiB, 40.7 us for 64 KiB, 59.2 us for 1 MiB (median of 50) | Same |
| C1a: `vkQueueBindSparse` plus fence wait, sparse queue | 1 chunk 152 us (unbind 148), 16 chunks 207 us (201), 256 chunks 1,034 us (883) (median of 50) | Same |
| C4b: bound on the guest's lead over the host GPU | Two frames, already in the existing driver: a queue-0 submission that carries a flip waits until the worker has passed the previous flip (`holdFlipBehindWorker`, `flipsAhead`) and then until the recorded work of the flip before it has finished and is reaped (`holdFlipBehindWork`, `waitFlipWork`). Nothing added | `Execution\src\Driver\Queues\Submission.cpp:527-593`, read 2026-11-22 |
| C4b: cost of releasing completions after landings | LEGO 54.6 ms against 52.8 ms without (+3.4 %, 8 alternating runs each, 2572x1448+2048x2048); FF 42.5 ms against 44.0 ms (one run each) | libs r540, 6da397a9e6, 2026-11-22 |
| C5: FF Resonance per frame (full run, warm, 44.2 ms) | 800 to 870 write faults and 12.4 to 13.2 ms in the fault handler (15 us each), 1,430 to 1,520 protect calls, 4 faults on GPU-owned pages (all parked), 209 to 220 KiB read back, 175 to 184 KiB landed. Footprint: 2,170 MiB GPU-visible in 4,624 ranges, 795 MiB registered, 687 MiB bound in the mirror, 487 MiB in 161 image records; 4.2 GB private commit, 2.2 GB VRAM | `runsf
559-ff-c5`, 5012ce0934, 2026-11-23 |
| C5: 007 First Light per frame (300 s, never past its first 360 frames: 252 s waiting on 98 pipeline builds, single compute pipelines of 4 to 8 MB of SPIR-V taking 57 to 79 s each) | 397 write faults and 1.2 ms in the handler, 406 protect calls, 0.01 faults on GPU-owned pages, 1,017 KiB read back, 15 KiB landed. Footprint: 5,562 MiB GPU-visible in 524 ranges, 792 MiB registered, 1,202 MiB bound, 812 MiB in 115 image records; 9.4 GB private commit (peak), 2.9 GB VRAM | `runs
559-007-c5` |
| C5: Age of Mythology per frame (80 s: 471 frames of menu, then 8 frames in 20 s behind 127 pipeline builds, 65 s waited) | Menu: 5.8 write faults, 21 us in the handler, 26 protect calls, 69 KiB read back and landed. Loading: 170 to 2,191 write faults and 0.4 to 53.5 ms in the handler per frame, up to 16 MiB read back. Footprint: 11,785 MiB GPU-visible in 26 ranges, 4,456 MiB registered, 893 MiB bound, 661 MiB in 91 image records; 5.4 GB private commit, 2.4 GB VRAM | `runsom
559-aom-c5` |
| C5: Ghost of Yotei (120 s, 1 frame: 78 s waiting on 35 pipeline builds, 159 s of build time over the builder threads; 66 `[recode-fatal]` lines, 62 of them "not resolved in time") | That frame: 31 write faults, 0.4 ms in the handler, 53 protect calls, 132 KiB read back and landed. Footprint: 4,271 MiB GPU-visible in 849 ranges, 5,604 MiB registered, 4,271 MiB bound, no image records yet; **10.9 GB private commit and 7.4 GB VRAM (18.3 GB) before the first scene** | `runs\yotei
559-yotei-c5` |
| C10: scalar loads that become per-invocation loads, Age of Mythology (r735, `APS5_RECODE_SCALAR_CENSUS`, 2026-11-24) | Static counts per program (upper bounds: `S_LOAD`s of descriptors are mostly folded on the CPU by the SRT plan). Most resolved: 2f9e83a0 (234k resolves, a 1-instruction driver program, 0 loads), ab355feb (92 instructions, 16 buffer dwords, 18 address dwords), b100f396 (58, 1, 22), 9b596275 (320, 17, 42). Largest: a48b39a6 1,825 instructions, 82 buffer dwords, 216 address dwords; 29c8fd1e 815, 80, 144. GPU 28-47 % busy (`nvidia-smi`, 20 samples at 60-80 s) at p50 55-67 ms: the frame is bound by the record thread, not the GPU. **Uniform form recorded as not needed now**; revisit under R5 (GPU work against 8.4 ms) with a pass timing. Yotei (r735, 120 s, no present yet, 100 programs): largest 8032f587 5,708 instructions, 273 buffer dwords, 243 address dwords; 9d8344e5 3,544, 273, 237; 3f9f2ddc 3,215, 0, 1,210 address dwords; 00689fdc 3,681, 43, 335. Yotei's GPU time cannot be measured before it reaches a scene | `runs\aom\r735-census`, `r735-gpu`, `runs\yotei\r735-census` |
| C9b: cold-start wait on pipeline builds (`ANYPS5_NO_SHADER_CACHE=1`, `downgrades.py compile`, 2026-11-24) | AoM r735: 1.45 s waiting on 135 builds in 80 s (warm 0.32 s on 97). FF r737: 1.62 s on 264 builds in 90 s (warm 0.27 s on 132). Under "more than a few seconds": **step recorded as not needed**. 007's and Yotei's long waits are single compute modules of several MB (C9a's subject), which dynamic graphics state does not touch | `runs\ff\r737-cold`, AoM runs deleted |
| C8: one buffer or a placement limit | One sparse buffer over a window from 0, sized at start-up (960 GiB requested, 864 GiB granted; 884 GiB largest single sparse buffer in a clean process; 13 buffers of 64 GiB fit at once) | `MirrorMachineCheck`, 13cfa96c67, 2026-11-23 |
| C10: scalar loads in the largest programs | Not counted | Step C10 |
