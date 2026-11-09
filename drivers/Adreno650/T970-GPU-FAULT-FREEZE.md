# SM-T970: GPU page fault freezes the whole system (gpufault1, 2026-11-08)

## Symptom

Opening Settings (Settings > Wi-Fi especially, also Sounds) and sometimes swipe-up freezes the whole UI about 10-15 s later.
Afterwards backboardd may crash in Turnip (`tu_init_hw` from `tu_BeginCommandBuffer`, FAR 0) and cannot come back.

## Evidence (boot logs under `deploy/gts7xlwifi-20261030/logs/`)

| boot | first stall | SMMU FSR | FAR | FSYNR0 | IB1 at the stall | IB2 |
|---|---|---|---|---|---|---|
| int5 | t84 | 402 | `0x0_1f311000` | 20002 (PLVL 2) | 484de000+7c1 | **1f311000** |
| int1b | t94 | 402 | `0x1_106ee2a0` | 20001 (PLVL 1) | 328ec000+9a8 | 32f49000+f7 |
| periph12 | t110 | 80000402 | `0x1_086de2c0` | 20001 | 3349c000+981 | 4856e000 |
| combo6 | t96 | 80000402 | `0x1_1fbd7360` | 20001 | 2dee5000+e1e | 4851e000 |

FSR 0x402 = TF (translation fault) plus the AArch64 format bit 0x400; 0x8000xxxx adds MULTI. SS (bit 30) is never set, so no
transaction was stalled: the GPU unit got an error response, and the CP then stopped (SQE pc 0xaa, IB1 with dwords left, the
fence never written). The `fsr400 far 63...` rows in older boots (v-series, next) are the boot-time FORMAT bit, not faults.

### What the faulting address is

- It is **never mapped**, not recently unmapped. Every process window is `[0x20000000, 0x1_00000000)` (GpuClientVaStart,
  vaBits 32). Three FARs are above 4 GiB, which no table maps (PLVL 1: no level-1 entry); int5's is below the window start
  (PLVL 2). `[gpu far]` shows the nearest map/unmap ring entries are hundreds of MiB away (e.g. combo6: 0x50350000 maps, an
  unmap of 0x4f066000). So this is not use-after-unmap or TLB ordering.
- In int5 the FAR equals the IB2 base: the CP fetched a `CP_INDIRECT_BUFFER` target 0x1f311000 from IB1 0x484de000, so a
  command stream held a bad IB pointer. The others are data reads (FSYNR0 read, not instruction fetch).
- Pattern: each FAR is a plausible in-window address minus 0x20000000 (= va_start), and the 64-bit ones also carry bit 32
  (= that address + va_size 0xE0000000, or + 2^32). The Metal bridge (metal2vulkan-runtime `address.rs`) gives every buffer a
  *Metal* address in `[2^32, 2^47)` and translated shaders turn those into Vulkan device addresses through the resident table.
  A raw Metal address reaching the GPU would look exactly like 0x1_xxxxxxxx. **Unproven lead** for the userspace side: a pointer
  that bypasses translation (root entry not written: `host + bind_offset + offset` with host 0, a stored pointer that is a guest
  address, or an indirect buffer pointer). Settings > Wi-Fi/Sounds and the app switcher all draw blurred backdrops/materials, so
  CA/RenderBox's filter path is the likely shader.
- Which process: the fault's TTBR0 (`[gpu ttbr] ask 13000274d2f000`) has ASID 0x13 = 19, and the map ring near the FAR is all
  ASID 19 with ~1160 maps (the busiest client, very likely backboardd = the render server).

### Why one fault froze everything (the kernel bug, fixed here)

1. The tablet's GpuProbe never installed `native_adreno_set_restart`, so `nativeAdrenoHangcheck` only counted the hang. It
   never called `endFault` (that call is inside `if(g.restart)`) and never restarted the GPU: the CP stayed hung for good.
2. The fault stayed latched in context bank 0's FSR. `ArmSmmu4KObserveOwnedContext` returns `ArmSmmu4KFault` (11) whenever
   FSR has a fault bit. `NativeAdrenoVm::observeCheap` runs it every 64th map/unmap and `attach` runs it for every new VM, and
   any failure there goes to `NativeAdrenoVm::fail`, which poisons the VM and quarantines its memory.
3. So each process's VM was poisoned in turn: `[gpu vmfail] first st 11 stage 11` (stage 11 = unmap's observeCheap), map
   fails `e00002d8` (NotReady), `[gpu tables] err e00002ca at b30` and `mount fail stage 1 st e00002ca` (a restarted
   backboardd cannot get a VM), `r e00002d5` busy rows. The UI froze for good.

Also: `native_adreno_dram_read32` was never set on this board, so the stall dump's IB words were `deadbeef` (user BOs have no
kernel mapping).

## Fix (READY as gpufault1; no new boot argument)

- `drivers/Adreno650/NativeAdrenoVm.cpp`: new board hook `native_adreno_bank_fault_end(bank)`. Each table operation first
  ends a latched fault on its bank (one FSR read); `observeCheap`, `attach` and the whole-bank `invalidateTlb` retry their
  proof up to 3 times when it failed with ArmSmmu4KFault and the hook ended a fault. A global fault (sGFSR), an image mismatch
  or a fault that keeps coming back still poisons, as before. Counters: `native_adreno_bank_fault_retries`.
- `drivers/Adreno650/NativeAdrenoPublish.cpp`: `native_adreno_asid_names[16]` (ASID and process name per connection).
- `package/GpuProbe.cpp`:
  - `gpuBankFaultEnd`: records the first and the latest fault (FSR, FAR, FSYNR0, **FSYNR1**, **UCHE_CLIENT_PF**, ASID,
    uptime), sends CB_RESUME terminate if stalled, clears FSR. Also run from the 2 s heartbeat.
  - `gpuRestart` (installed with `native_adreno_set_restart`): GBIF drain, RBBM_SW_RESET_CMD 100 us, `hwInit`, SQE base,
    `ring->restart`, SQE start, `a6xx_cp_init` unfenced + `ring->idle`, CP_SET_SECURE_MODE 0 again (zap loaded). This is
    UniXNU-sheng's gpuRestart adapted to the A650. The driver's `recover` then drops the hung job, reinstalls the kernel
    table and replays the other jobs, so the other processes go on.
  - `dramRead32` (UniXNU-alioth's) into `native_adreno_dram_read32`: `[gpu stall] br ib1 ...` now shows real IB words.
  - `t970-gpu-recover=0` turns all three off (old behaviour), for a rollback test only.

### New log row: `[gpu 28]`

Once a fault was ended or a restart was asked, row 28 changes from `FAULTS cr0 ...` to:

```
FAULTS n<faults> fsr<FSR> far<FAR> s<FSYNR0>/<FSYNR1> pf<UCHE_CLIENT_PF> a<ASID> <process> t<s> | 1st <FAR> a<ASID> t<s> | rs <done>/<asked> <step>:<status> | re <observe>/<attach>
```

How to read it on the next Settings > Wi-Fi test:
- `rs 1/1 0:0`: the GPU restarted and the UI should come back after about 2 s (the faulting app's frame is lost).
  `rs 0/1 <step>:<status>`: the restart failed at step 1 no ring, 2 ring restart, 3 CP init, 4 idle after the init, 5 secure-mode exit.
- FSYNR1 low byte = the a6xx fault "mid" (0 CP, 4 CCU, 6 CDP prefetch, 1-3 UCHE clients), and UCHE_CLIENT_PF low 3 bits
  with mid 3 = VFD, SP, VSC, VPC, HLSQ, PC, LRZ (Linux `a6xx_fault_block`). mid 2 = TP (or TP|VFD).
- `<process>` names the guilty client. backboardd = render server shader/command stream; Preferences = the app itself.
- With the IB words now readable, `[gpu stall] br ib1` shows the packets around the CP's position.

## Next steps (userspace root cause)

1. Boot gpufault1, open Settings > Wi-Fi. Expect a short hitch instead of a freeze. Collect row 28, `[gpu stall]` and `[gpu far]`.
2. If FSYNR1 says SP/TP (shader access): take the AdrenoMetal/metal2vulkan route. Check whether any pointer path in the
   translated CA/RenderBox blur/material shaders can emit a guest (Metal) address unchanged (a root entry whose `host` is 0, or
   profile-F `SlotGuestBase`), and compare the FAR minus 2^32 against the render server's Metal address ranges.
3. If it is CP (mid 0) as in int5: decode the IB1 words around the `CP_INDIRECT_BUFFER` with the bad target. That is a Turnip
   cs-entry or IB that was recycled/rewritten while queued (a userspace lifetime bug), not a kernel mapping problem.

## Results

- combo8 (kernel f0d12cbb, gpufault1): Settings > Wi-Fi opened and worked, with no freeze. But the log shows no GPU fault on that
  boot: row 28 is still the live `FAULTS ... cb0 fsr 400`, all 7553 submissions completed, and there is no stall/far/vmfail row.
  The fix was not exercised. The trigger is intermittent (periph13 did not reproduce either). Repeat Settings > Wi-Fi / Sounds /
  app switcher for several rounds on this build to capture a fault with the new diagnostics.
- Kernel check: `NativeAdrenoUserClient` only bounds-checks map requests against `[window_start_, +window_bytes_)` and never
  subtracts the window start. The out-of-window addresses therefore come from userspace (Turnip `tu_knl_darwin.cc` IOVAs or the
  Metal bridge). metal2vulkan's default `DevicePolicy::UNTRUSTED` has containment on, so a translated shader dereferencing a raw
  guest address is less likely. int5's fault was a CP IB2 fetch, which points at a Turnip command stream.

## combo8b: "hung ~2 s, then SpringBoard restarted" (gpufault2, 2026-11-08)

What the log shows (logs/combo8b/alioth-0000000010008620):
- **No GPU fault and no GPU hang.** Every `[gpu 28]` is `cb0 fsr 400`, gpuRestart never ran (row 28 never changed format), and
  submissions kept completing (sub 3302→3382→3454 at 307/309/311 s). The `[gpu far] 6342574175e6` rows are the FAR register's
  boot-time leftover value, printed by the stall snapshot, not a fault. The stalls at 241 s (fence 1881) and 319 s (fence 3599)
  were the known missed write pointer, and each ended at the 50 ms kick (`[gpu unstick] ... ended: kick=n`).
- **backboardd ended itself.** Exit rows: `p55 backboardd status=f sig=15 ns=2` (SIGTERM) at ~310 s, then `p27 SpringBoard sig=9 ns=15`
  (RunningBoard took SpringBoard down with its render server), and backboardd/SpringBoard reconnected at 313-321 s. The only
  SIGTERM in the GPU stack is AdrenoMetal `NativeBridgeMetalFrontend.mm` `endRenderServerAfterGpuLoss()`. It runs when a
  command buffer completes with Metal code 11 = Vulkan DeviceLost, and is deliberate ("launchd starts it again, SpringBoard with it").
- **Why the device was lost: one refused kernel submission.** `[gpu methods]` method 5 (Submit) went 3431/0 → 3455/**1**
  between 309 and 311 s. Every other method had 0 failures, apart from method 6, whose "failures" are wait timeouts. Turnip
  `tu_knl_darwin.cc darwin_submit_chunk` retries only `kIOReturnBusy`; any other status is `vk_device_set_lost("submit failed")`.
  So it was not our restart, nor a guilty context: the kernel's fault counter never moved.
- The refusal's status was not logged. Candidates in the kernel path: an IB handle this client no longer has mapped
  (BadArgument; a userspace lifetime bug, which fits int5's stale IB2 pointer), VM describe/hold, or a table switch whose drain
  timed out. The last one (`useVm` mode 1 waited once for 2 s, and a single long wait never reached the 50 ms kick, so a missed
  write pointer would always time out into a refused submit) is not supported by this boot: no kick at 311 s, and submissions
  flowed in the 2 s before.

gpufault2 changes (kernel only):
- `NativeAdrenoSubmit::useVm`: drains in 1 ms slices (the stall handler kicks at 50 ms, as for any other wait). After 2 s it
  answers `kIOReturnBusy` (Turnip waits for its oldest fence and submits again) instead of `kIOReturnTimeout` (device lost).
  Counter `native_t970_usevm_busy`.
- `NativeAdrenoUserClient::submit`: every refusal other than Busy is recorded (count, status, pid, stage 1 request,
  2 queue, 3 wait fence, 4 IB size, 5 IB handle unknown/unmapped, 6 submission object).
- Row 28 is now `FAULTS n.. <fsr>@<far> s<fsynr0>/<fsynr1> pf.. a<asid> <proc> t..|sf <n> <status>:<stage> p<pid>|rs <ok>/<asked> <step>:<st>|vb <n>|1st ..|re ..`.
  It switches to this format on the first fault, restart, refused submit or busy table switch.

Can backboardd survive? Only if its Vulkan device is not lost. That is the kernel's job (never refuse a valid submission; answer
Busy for transient conditions). Making the plugin keep a lost render server alive is a userspace change. Upstream chose the
restart on purpose, because CoreAnimation cannot continue on a dead VkDevice.

## combo10: real fault, restart succeeded, still frozen (gpufault3, 2026-11-08)

Log: logs/combo10/alioth-0000000011af32d8. Sequence:
- ~130 s: backboardd (ASID 19) faults at `0x1_1d4c16e0`. Row 28: `80000402@11d4c16e0 s20001/0 pf1 a19 backboardd`.
  **FSYNR1 = 0 means the CP block** (Linux `a6xx_fault_block`: mid 0 = CP), as int5's IB2 fetch was. The bad address is in
  Turnip's command stream (an IB/draw-state/indirect address), not a shader pointer. The CP sat at pc 0xaa in IB1
  2b53a000 (0x981 dwords left).
- The GPU raised a CP_HW_ERROR storm (`seen 900200`, last 0x200, 123218 IRQs), so gpuIrq masked the line (`m1`).
- 132-138 s: three table switches answered Busy (vb 1→3, 2 s each under the device lock), then the hangcheck restarted the GPU
  at ~138 s (`rs 1/1 0:0`).
- **The replay then poisoned the submission object.** `recover()` switched to each replayed job's table through `useVm`, and
  in TTBR mode 1 that drains *all* outstanding jobs, including the ones still waiting to be replayed. That drain can never
  end (vb went to 3 at the restart). After that every open failed `vm/submit/ring poisoned → BadArgument` (`mount fail stage 2
  st e00002c2`, refusals e00002d5), backboardd crashed (`CRASH p55 backboardd sig11`), its relaunch could not get the GPU,
  and the screen stayed frozen. This was broken before gpufault2 too: a Timeout poisoned it the same way.
- The stall dump now reads DRAM: the stalled IB1's first dwords are **0** (`br ib1 @ffff000c: 0 0 0 ...`, the same in combo8b's
  stall). The CP was executing command memory that had been zeroed or reused, which also explains the CP_HW_ERROR storm.

gpufault3 (kernel):
- `NativeAdrenoSubmit::recover`: the replay's table switch waits only for the fence of the job replayed just before
  (ring 0, 1 ms slices, 2 s), then invalidates the ASID and writes TTBR0. A job whose VM was poisoned since runs as a CP_NOP, so
  its fence still arrives.
- `useVm`: busy budget 250 ms instead of 2 s, so the device lock is released often enough for the hangcheck.
- `gpuRestart`: resets gpuIrq's storm counter and mask flag after hwInit (which re-enables RBBM_INT_0_MASK).
- Row 28: `|pz <step>:<status>` = native_adreno_recover_trace (0:0 = the recovery did not poison; 5 = replay table switch,
  6 = replay submit, 2 = restart hook, 3 = TTBR write).

Userspace lead (not changed here; Turnip, `.cache/mesa-work/src/freedreno/vulkan/tu_knl_darwin.cc`): `darwin_bo_finish` stamps a
freed BO with `dw.last_fence`, the process's last *submitted* fence at the moment of the free, and `darwin_reap_locked`
recycles it into the BO cache (`darwin_cache_get` hands it to the next allocation of that size) once that fence completes.
Two cases would let the GPU read a recycled (rewritten or zeroed) command BO:
1. A BO freed while a submission that references it is still being built or queued on another thread. Metal's encoding thread
   / the queue's submit thread: the referencing fence is not `last_fence` yet.
2. A BO referenced by a submission on a different `tu_queue` whose fence is numbered later.
Either fits the zeroed IB1, int5's stale IB2 pointer and these CP reads of out-of-window addresses. Next step on that side:
stamp a freed BO with the fence of the last submission that *referenced* it (Turnip keeps the per-submit BO list), or disable
the darwin BO cache for command-stream BOs (TU_BO_ALLOC cs flags) as a quick test.

## Userspace: Turnip BO retire fix, `t970-bo-safe` (2026-11-08)

Patch: `sources/UniXNU-next/patches/mesa/turnip-darwin-bo-retire-safe.patch`, appended to `scripts/data/adreno-compiler-inputs.json`
(and applied in `.cache/mesa-work`; `validate_recorded_patches` passes). It touches `src/freedreno/vulkan/tu_knl_darwin.cc` only:
- `darwin_queue_submit` registers itself while it runs (`darwin_submit_scope`, up to 32 at once).
- `darwin_bo_finish`: a freed BO is stamped with `max_fence` (the newest fence of the process; `last_fence` could go backwards
  with two queues). If any submission was inside `darwin_queue_submit` at the free, the record waits until all of those left,
  then takes `max_fence` and waits for it. Only then is the BO cached or released.
- `t970-bo-safe=0` on the boot line restores the old stamping. `t970-bo-safe=2` also keeps every freed BO out of the BO cache.
  The default (no argument) is 1. Read from `kern.bootargs` by Turnip itself, no plugin change.
- Built for iOS: `build/mesa-tu-ios/.../libvulkan_freedreno.dylib` sha256 c863c9b7…. Apart from this patch it is the deployed
  Turnip: gpu-build.json's source_diff 550c6b07… and extra-source hashes match the tree. Only the darwin backend changed.

Variant files (device repo, made by `scripts/t970-turnip-variant.py` from the addon/trust current at 16:47):
- `.cache/firmware/gpu/gpu.addon.bosafe` sha256 2ccf5794b3b00e2fc1fa758b48287b9fe841f01e39a312fa990e8144008eba64
  (from gpu.addon f5df5c22…; same 27 files, only NativeAdrenoMetal.bundle's dylib, re-sealed binary and CodeResources differ)
- `.cache/firmware/trust/owner-gpu.trustcache.bosafe` sha256 b2a0c595225a818e2c492adb9f74c88c71b438eae7ae8e5a5530910d28d8872a
  (from owner-gpu.trustcache d6081247…, +1 module: bundle 2231ada7…, Turnip 687b032f…). Already appended to board.toml.
- Report: `.cache/firmware/gpu/gpu.addon.bosafe.json`.

Regenerating after another session changes the addon or trust (order does not matter: each script replaces only its own
files and appends one trust module; always feed the latest output into the next step):
1. `cd sources/UniXNU-next && python3 scripts/build-turnip-darwin.py --target ios` (only if the Turnip source changed).
2. `cd UniXNU-gts7xlwifi-next && python3 scripts/t970-turnip-variant.py --addon <latest addon> --addon-out <new addon>
   --trust <latest trust> --trust-out <new trust> --report <json>`.
3. Add the printed trust sha256 to board.toml `trust_cache_sha256`. Stage with `unixnu build payload --trust-cache <new trust>`
   and copy the new addon to the payload's gpu.addon.
Caveat: under mode 1 a submission blocked for a long time in its semaphore wait delays reuse of BOs freed meanwhile (memory
grows until it leaves). A later plugin rebuild (build-adreno-metal-plugin.py) picks up this Turnip automatically.

## combo13c: respring from a submission naming a freed BO (gpufault4 + bosafe2, 2026-11-09)

Log: logs/combo13c/alioth-00000000118f0fa1 (Settings opened and closed very fast; the log ends at 90 s).
- No fault, no hang. Row 28 caught it: `sf 1 e00002c2:5 p55` = backboardd's submission refused with BadArgument at stage 5,
  meaning an IB's BO handle the kernel no longer has. Then `p55 backboardd sig=15` (the plugin ends the render server on DeviceLost) at
  84 s, `p27 SpringBoard sig=9 ns=15` at 86 s, and a new backboardd 263 / SpringBoard 265 starting. The "freeze" was this respring,
  cut off when the log stopped. The `[gpu stall] fence=195` rows are an old 37 ms stall that ended at the kick, not the cause.
- Cause class: Turnip copies each IB's `bo->darwin_handle` when the submission is built (`darwin_submit_add_entries`). The BO
  was already freed (the command buffer released while its submission was still queued, e.g. by a context torn down during the
  fast open/close). bosafe cannot cover this: the free happened before any submission named the BO.
Changes:
- Turnip (same patch file `turnip-darwin-bo-retire-safe.patch`, regenerated): an entry whose BO struct is already zeroed
  (handle 0 or refcount 0) marks the submission stale. Under `t970-bo-safe>=1` a stale submission is not sent; its signals resolve
  with the previous fence (as an empty submission), a note kind 0x5354 tells the kernel, and `mesa_loge` names the handle. Result:
  one lost frame of a closing context instead of a respring. `t970-bo-safe=0` keeps the old behaviour.
- Kernel: Note kind 0x5354 is counted (`native_t970_stale_submit`). Row 28 gains `h<handle>` after `sf` (the refused IB's handle at
  stage 5) and `|ds <drops> p<pid> h<handle>`.
- Variant: `gpu.addon.bosafe2` 6de2b218… and `owner-gpu.trustcache.bosafe2` 36b95cf1… (from the default addon/trust of
  2026-11-09 07:24, appended to board.toml).
- Limits: a freed BO whose Turnip slot was already reused by a new BO is not caught (its handle is valid). The kernel would then
  refuse at stage 6 (IOVA outside that handle's mapping), and `sf ... :6 ... h` names it.
