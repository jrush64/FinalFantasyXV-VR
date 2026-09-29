# FINAL FANTASY XV (Windows Edition) engine reference for the VR proxy

This file collects every game address, structure offset and engine mechanism that the
`dxgi.dll` proxy in `src/` relies on. The source code is the primary record; where a
comment in the code and this file disagree, the code wins. Statements marked "role not confirmed",
"unverified" or "inferred" are exactly that; nothing in this file is guessed silently.

---

## 1. Scope, build identity and conventions

### 1.1 Target build

| Item | Value |
|---|---|
| Executable | `ffxv_s.exe` (retail Steam build, x64, Direct3D 11, Luminous engine) |
| PE `TimeDateStamp` (`kRetailTimestamp`) | `0x5F85274E` |
| PE `SizeOfImage` (`kRetailImageSize`) | `0x0FECB000` |
| File size of the analysed executable | 252,818,928 bytes |
| Preferred image base | `0x140000000` (the image is loaded at that base; the code stores it as `g_exeBase`) |

The proxy refuses to patch anything unless both the timestamp and the image size match
(`ValidateRetailExecutable`). A different build is rejected rather than patched with stale offsets.

### 1.2 Address notation

- Every address in this file is an **RVA** (offset from the module base) written as `+0x2DBEC90`.
- Ghidra names functions and data by absolute address at the preferred base. To convert, subtract
  `0x140000000`: `FUN_142dbec90` is at `+0x2DBEC90`; `FUN_1404d04b0` is at `+0x4D04B0`;
  `DAT_144f29430` is at `+0x4F29430`; `DAT_144f294d8` is at `+0x4F294D8`.
- "Return-address RVA" means the RVA of the instruction after a `call`. The output policy (section 4)
  matches on return addresses, because it identifies the caller of a getter.
- Offsets written `+0xNNN` after a pointer-typed name (for example `ctx+0xB80250`) are structure offsets,
  not RVAs. Text says which is meant.
- Units: the engine world is metres for camera matrices. Row-vector maths: `clip = [x y z 1] * P`,
  view-space is right-handed with -Z forward, Y is up. The native stereo baseline is stored in
  centimetres (see section 3).

### 1.3 Byte verification before patching

Almost every patch or hook site is compared against a byte array before it is touched. These arrays are
`constexpr std::array<std::uint8_t, N> k...Prefix` (or `kXxxBytes` in `stereo_task_fix.cpp`) and
hold the first 8 to 27 bytes of the function (or the whole instruction at a call site). If any array
fails to match, the patch transaction is rolled back and the feature stays off. The installer loops
(up to 120000 iterations, sleeping 1 ms after the first 10000) until all prologues match, because the
executable's code is only valid in memory after the executable has unpacked itself.

Prefix arrays that exist in `main.cpp`: `kDisplayGetterPrefix`, `kCurrentOutputPrefix`,
`kStrippedMultiOutputPrefix`, `kFullMultiOutputPrefix`, `kMatrixCacheWriterPrefix`,
`kMatrixCacheConstructorPrefix`, `kPoseBufferRecorderPrefix`, `kPassQueuePrefix`,
`kUploadMatricesPrefix`, `kActorLodBudgetPrefix`, `kActorLodManagerRunPrefix`,
`kActorPointLodKernelPrefix`, `kActorBoundedLodKernelPrefix`, `kReplayDispatchCallPrefix`,
`kReplayMapCallPrefix`, `kReplayDrawCallAPrefix`, `kReplayDrawCallBPrefix`,
`kPostChain0Prefix`..`kPostChain5Prefix`, `kBankRotatorPrefix`, `kPassRegistrarPrefix`,
`kFiberOutputRestoreJmpPrefix`, `kViewRestorePrefix`, `kOutputBeginPrefix`,
`kWorldStepPrefix`, `kVolumetricEntryPrefix`, `kLensGetterPrefix`, `kLensSiblingPrefix`,
`kUpdateRenderViewAPrefix`, `kCamParamSignature`, `kCamFixExpected` (the four setter calls), and the
full-function arrays in `stereo_task_fix.cpp` (`kWrapBytes`, `kLoopBytes`, `kRuleBytes`, `kThunkBytes`,
`kInvokerBytes`, `kJobBytes`, `kUpdateBytes`, `kRequestBytes`, `kListBytes`, `kAiBytes`, `kHolderBytes`,
`kTrayBytes`, `kStateCallbackBytes`, `kStateJobBytes`, `kMotionCallbackBytes`, `kMotionJobBytes`,
`kMotionSetterBytes`, `kTargetResolverBytes`). The four eye-builder/commit call sites are validated by
decoding the `E8` call target and comparing it with the expected function.

Sites that are **not** compared byte-for-byte before patching (the code steals a fixed number of bytes on an
instruction boundary chosen from the disassembly, or patches a data pointer): the probe-weight producer
`+0xEBF3830` (16 bytes), the quality-mask apply `+0x679E050` (15 bytes), the update dispatcher `+0x5BC3870`
(15 bytes), the replay `memcpy` IAT slot `+0x3067370` (only readability and a non-null pointer are checked),
and the `camera+0x4F5` store at `+0x1074241` (7 bytes; left unpatched by default). They are protected only
by the image timestamp/size check.

### 1.4 Glossary

| Term | Meaning |
|---|---|
| Native stereo | The engine's own two-output renderer. Both eyes are rendered in the same game frame from two engine views. Toggled by writing the display gate byte (section 3). |
| AER | Alternate-eye rendering. The engine stays single-output and renders one eye per game frame; the mod alternates a per-eye camera offset. |
| Mono | The AER pipeline with a single (unoffset) camera shown to both eyes, still head tracked. |
| Output | One of the engine's render outputs (eyes). Output index 0 is the primary; output 1 is the second eye in native stereo. |
| Head source | The 4x4 camera matrix that the mod overwrites with head pose every frame (section 6). |
| Bank | A per-output block of view data hanging off the display-area object (section 5.5). |
| DR0..DR3 | x64 hardware debug registers. The mod uses them as execute or write breakpoints handled by a vectored exception handler (section 15). |

---

## 2. Modes of operation and where they touch the engine

| Mode | Engine state | What the mod changes |
|---|---|---|
| Native stereo | `display+0x244 = 1` (two outputs). Full multi-output camera job substituted. | Eye builders wrapped, eye offset added at the right-eye camera source, shared-camera eye-0 writes suppressed, lifecycle callers given the true output, lighting/quality state repaired, task scheduler repaired (sections 3 to 10). |
| AER | Single output. Same head-writer hook. | Head pose plus per-eye lateral offset written into the head source. Engine simulation held to one moment per eye pair by `[AERSYNC]` (section 8.6). |
| Mono | AER pipeline, no offset. | As AER but one picture goes to both eyes. |
| Flat scene | Loading screens, menus, videos: no 3D projection uploaded for two consecutive presents. | Presenter shows one image to both eyes. |

---

## 3. Display object and the stereo gate

| Address / offset | Name | Meaning | Use |
|---|---|---|---|
| `+0xED75A80` | display getter | `mov rax,[rip+disp]; mov rax,[rax+0x348]; ret`. The RIP-relative global is at `+0x4F3C318` (decoded from the checked prefix bytes). Returns the display object. | Called (not hooked) by the mod: `ResolveStereoData`, `[HISTFIX]`. |
| display `+0x244` (byte) | stereo enable / "two outputs" gate | 1 = multi-output rendering. Also reachable as `(display+0x230)+0x14`. `kStereoEnableOffset`. | Written by `SetStereo`; a hardware read/write watch showed 34 reader sites, all renderer or display code, none in gameplay/UI/AI. |
| display `+0x218` (dword) | output count | The engine's multi-output test is `stereoEnable != 0 \|\| outputCount > 1`. `kOutputCountOffset`. | Read only. |
| display `+0x230` | stereo object | Holds the values below. `kStereoObjectOffset`. | Base for the four floats. |
| stereo object `+0x18` | baseline (cm) | Stock 6.0. `kStereoBaselineOffset`. | Written by the mod (user eye separation). |
| stereo object `+0x1C` | scale | Stock about 115.118. `kStereoScaleOffset`. | The engine builds projection skew `P[2][0] = baseline / scale` for the right pass (6.0/115.118 = 0.0521). The mod sets `scale = baseline / 1e-5` so the skew is negligible (`[SKEWZERO]`, `kNativeResidualSkew`). |
| stereo object `+0x20` | separation input | Stock value 1.0, already at the engine clamp. `kStereoSeparationOffset`. | Preserved at stock. Setting it to 0 produces invalid eye-0 matrices. |
| stereo object `+0x24` | convergence | `kStereoConvergenceOffset`. | Saved and restored by the mod. |

Observations that matter:

- The engine's own native stereo moves no camera. Measured: the right pass gets only a projection skew
  and neither eye gets a view translation, i.e. a sideways image shift, not depth. All real parallax comes from the
  mod's camera offsets (section 5.4).
- The enable byte, separation, baseline, scale and convergence are saved on first enable and restored on
  toggle-off and on DLL unload.
- The gate byte and the output count also determine `passes` for the world step (section 8.1).

---

## 4. Current output index and the caller-aware output policy

### 4.1 Where the index lives

The current output index is **per-thread** state:

```
tlsIndex = *(u32*)(base + 0x5371AF0)              // kTlsIndexRva
slot     = TEB->ThreadLocalStoragePointer[tlsIndex]
context  = *(void**)(slot + 0x10)                 // or base + 0x4F448B0 (kFallbackRenderContextRva) if null
output   = *(int*)(context + 0x344)               // kCurrentOutputOffset
```

`ReadTrueOutput()` reads exactly this and clamps to 0/1.

### 4.2 The getter and the policy

`+0xEDB7160` (`kCurrentOutputGetterRva`, `FUN_14edb7160(display)`) is the engine's current-output getter
(prologue: `mov ecx,[rip+tlsIndex]; mov rax,gs:[0x58]; ...`). The mod overwrites it with a 14-byte absolute
jump to `RetailCurrentOutputPolicy` (tag `[OUTPOLICY]`, counters `[OUTCALL]`). The policy returns the
**true output** to a fixed set of callers and **0** to every other caller:

| Return-address RVA / range | Role | Policy |
|---|---|---|
| `+0xE8D4FC9` | DrawPhaseBegin: primary-eye reset gate | true output |
| `+0xE8D5091` | DrawPhaseBegin: frame-time/update gate | true output |
| `+0xE8D51BF` | DrawPhaseBegin: per-output preparation gate | true output |
| `+0xE8D9926` | DrawPhaseEnd: last-eye resource release gate | true output |
| `+0xE8DA05A` | DrawPhaseEnd: output-1 phase 0x3A release | true output |
| `+0xE8DAC93` | post-End teardown: phases 0x50/0x51/0x55 | true output |
| `+0xE96D753` | renderer gate: output 1 must skip output-0-only transient construction | true output |
| `+0x1074000` .. `+0x1074290` | inside the full camera job (`kFullCameraJobBeginRva`, `kFullCameraJobEndRva`) | true output |
| `+0x480800` .. `+0x482000` | native SBS presenter (`kPresenterBeginRva`, `kPresenterEndRva`) | true output |
| `+0x2BBBF22` | frame-end bank rotator handoff gate (call at `+0x2BBBF1D`, `kBankRotatorHandoffCallerRva`) | true output (`g_bankRotGateTrue`, default on) |
| `+0xEBF55EA` (`kDeferredManagerOutputCallerRva`) | deferred manager | 0 (compile-time switch `kUseTrueOutputForDeferredManager` is off) |
| `+0x67A1A70` (`kDisplayAreaBankOutputCallerRva`) | display-area bank selector | 0 by default; `g_useTrueOutputForDisplayAreaBank` (runtime toggle, default off) returns the true output |
| every other caller | shading-resource paths | 0 |

Why: retail never constructs many per-output resource families for output 1. Giving every consumer the true
index causes output-1 resource failures. Giving every consumer 0 breaks the presenter and, worse, the
lifecycle gates (next paragraph).

**Constraints.** Returning 0 to `+0xE96D753` makes the engine rebuild a set of output-0-only transient
targets (about 120 new 1024x1024 R32_TYPELESS textures per second) on both output passes, and process
memory climbs by hundreds of MB per second. Returning 0 to the three teardown callers (`+0xE8D9926`,
`+0xE8DA05A`, `+0xE8DAC93`) restores the same growth. Construction (`+0xE96D753`) and release must both
see the true output. Force-releasing the renderer pools at Present causes use-after-free crashes
(observed at `+0xEBB3078`).

### 4.3 Fiber output poisoning ([OUTSET])

The job system saves and restores the per-thread output index as part of fiber switches. The restore path
tail-jumps into the setter `+0xEDB7540` (`kSetCurrentOutputRva`, `void(mgr, uint32 value, uint8 flag)`)
through a `jmp` at `+0xED79434` (`kFiberOutputRestoreJmpRva`, bytes `E9 07 E1 03 00`). The mod redirects that jump
through a relay to `RetailFiberOutputRestorePolicy`:

- outside stereo, any non-zero value restored by a fiber is forced to 0 (fibers that ran output-1 jobs would
  otherwise keep output 1 forever);
- in stereo, a restore of 1 on any thread other than the recorded output-1 phase thread is clamped to 0 (`g_stereoFiberClampEnabled`, default on).

The setter's own prologue is RIP-relative, so it cannot be hooked directly; the two other callers hard-code
output 0.

---

## 5. Multi-output camera path (native stereo)

### 5.1 Stripped job versus full job

| Address | Description |
|---|---|
| `+0x9207950` (`kStrippedMultiOutputRva`) | Retail's normal multi-output camera update. 126-byte reduced function that installs only output 0 and only ever clears `camera+0x4F5`. Takes the camera manager in `RCX`, returns void. |
| `+0x1074000` (`kFullMultiOutputRva`) | A complete multi-output camera job that is still present in the executable and is calling-convention compatible with the stripped one. Extent used by the mod: `+0x1074000` .. `+0x1074290`. |

The mod overwrites the entry of the stripped job with a 14-byte absolute jump to `RetailCameraJobPolicy`,
which calls the full job (`g_realCameraJob`). The substitution is unconditional (it also runs with the gate
off), except in the compile-time diagnostic mode `kNativeStereoOnly`. `CallDeferredCameraJob` skips the job for output 0 while
`camera+0x4F4 == 0` (an incomplete output-0 camera; tag `[CAMDEFER]`).

**Constraints.** Skipping the full job entirely blacks out an eye. Restoring camera bytes or the external
view cache after the job does not undo its side effects.

### 5.2 Eye builders and render-context commits

The full job builds each eye's matrices with the engine's own functions and commits them to the deferred
render context. These are the four call sites the mod redirects through executable relays (validated: the
call target is decoded and compared before the site is patched):

| Call site | Target thunk | Implementation | What it does | Mod wrapper / tag |
|---|---|---|---|---|
| `+0x10740F9` (`kEyeViewBuilderCallRva`) | `+0x2D83FC0` | `+0xEDBB260` | view builder `float*(stereoObj, out, source, scalar, u32 metadata)` | `RetailEyeViewBuilder`, `[EYEPAIR]` |
| `+0x1074182` (`kEyeProjectionBuilderCallRva`) | `+0x2D84290` | `+0xEDBB680` | projection builder, same signature | `RetailEyeProjectionBuilder` |
| `+0x1074263` (`kSepProbeProjectionCallRva`) | `+0x2BC0780` | `+0xE8DFC90` | projection commit into the render context (manager, matrix, aux/inverse, u32 bank) | `RetailSepProbeProjectionUpload` |
| `+0x1074277` (`kSepProbeViewCallRva`) | `+0x2BC0850` | `+0xE8DFE50` | view commit, same signature | `RetailSepProbeViewUpload` |

Facts about the builders:

- Both read the true output from the thread context (`context+0x344`). Output 0 selects the native left-eye
  matrix, output 1 the right-eye matrix; the two branches contain opposite eye offsets. In native stereo the
  substituted job is observed building with true output 1 (the right pass); the left pass renders from the plain
  gameplay camera.
- `+0xCDB00` (`kMatrixInverseRva`) is the engine's 4x4 inverse `void(const float* in, float* out)`; the mod calls it
  to derive the companion (inverse) matrices.
- The full job hard-codes `R9D = 0` (bank 0) for both commits. The stock behaviour is therefore to build the
  matrix chosen by the current output but always commit it to bank 0.
- The mod's wrapper calls the real builder twice around the original call, once with the thread output forced to
  0 and once to 1, keeps both results, and returns the one matching the original output (`BuildNativeEyePair`).
  `RouteSepProbeUpload` then commits eye 0 to bank 0 and eye 1 to bank 1 with `+0xCDB00` inverses
  (`CommitSymmetricEyePair`, counters `built`/`committed`/`fallback` in `[EYEPAIR]`).
- Builder argument `source` (`r8`): the projection builder receives a 32-float struct: the camera's
  world-to-view transform at floats 0..15 and the projection at `source+0x40` (P00 at float 16, P11 at float 21,
  `P[2][0]` skew at float 24). The view builder receives a pointer 16 floats further into the same struct (its
  transform is at `source-16 floats`). `source+0x40` is only a staged copy; writing it changes nothing on screen,
  but reading it reports the lens.
- The engine's own projection matrices are composed on the GPU from CPU parameters; patching the CPU-side
  constant-buffer copy of a projection does not change the image.

**Constraints.** Calling `+0xE8DFC90` / `+0xE8DFE50` outside the camera job crashes (`RCX` is not a valid
manager). Copying eye matrices straight into bank 0 at DrawPhaseBegin yields a black image in both eyes.

### 5.3 Shared camera protection ([CAMFIX])

When the game has no per-output camera objects (retail has none), the full job also writes the eye matrices
into the shared camera object and sets `camera+0x4F5 = 1`. Gameplay projects world positions to the screen
with that shared camera (interaction prompts, trigger visibility, markers), so a live eye camera in it breaks them.

| Site (RVA) | Size | Instruction | State |
|---|---|---|---|
| `+0x10741EE` | 3 | eye 0 setView call `FF 53 08` | NOPed |
| `+0x1074203` | 3 | eye 0 setProj call `FF 50 10` | NOPed |
| `+0x1074226` | 3 | eye 1 setView call | left running |
| `+0x107423E` | 3 | eye 1 setProj call | left running |
| `+0x1074241` | 7 | store of `camera+0x4F5` (index 4 in `kCamFixSites`; no expected-bytes check) | left active (`g_camFlagStoreDisabled = false`) |

Gameplay projects with the eye-0 block, so blocking only the eye-0 pair restores interaction while the remaining
setters keep every side effect that scripted scenes need. The patch is applied at install
(`ApplyCamFix(true)`) and stays applied; whether these calls execute in mono is not established.

Camera object facts gathered here (`camera` = the camera manager passed in `RCX` to the job):

| Offset | Meaning |
|---|---|
| vtable `[+8]` | setView, `+0x9202EC0`; writes the per-eye view block at `camera+0x210 + eye*0x260` and a dword at `camera+0x300 + eye*0x260` |
| vtable `[+0x10]` | setProj thunk `+0x9202FB0`, which calls `+0x91F9930` and stores projection at `camera+0x110 + eye*0x260` |
| vtable `[+0x58]` | getter |
| `+0x110`, `+0x210`, `+0x370`, `+0x470` | per-eye blocks: proj eye 0, view eye 0, proj eye 1, view eye 1 (`kCamViewBlocks`); stride `0x260` |
| `+0x4E0`..`+0x51F`, `+0x520`..`+0x55F` | eye-1 projection and inverse as decoded from the setter; this overlaps the full job's flat inputs at `+0x500`/`+0x540` and the flag bytes |
| `+0x4F4` (byte) | "ready" flag (`kCamFlagByteA`), never modified by the mod |
| `+0x4F5` (byte) | stereo handshake flag (`kCamFlagByteB`); retail's stripped job clears it, the full job sets it |
| `+0x280`..`+0x55F` | the range the job actually touches |

The setters' inner routine is `+0x92140F0` (`kMatrixCacheWriterRva`; constructor `+0x92131B0`). It writes
through a pointer held in each block: object layout used by the guard is matrix array pointer `+0x00`, dirty-flags
array pointer `+0x10`, count (u32) `+0x18`, current index (u32) `+0x20`, entry stride `0x120`, at most 128
entries, 16-byte aligned. Retail passes an unconstructed (all zero) eye-1 cache object to the writer, which
crashes at `+0x9215D19`. The mod hooks the writer (`RetailMatrixCachePolicy`, 15 bytes stolen) and returns
without calling the original when the object fails validation (`matrix-cache skip` log lines). Nothing is
allocated (the compile-time variants `kConstructEye1MatrixCache` and `kInjectUploadMatrices` are off).

### 5.4 Eye separation seat ([SPLIT], [CAMSWAY], [SPLITSYM], [RIGHTSYNC])

Native stereo is asymmetric by engine design: the left pass renders from the plain gameplay camera and the
camera job builds only the right pass.

- `ApplyCamSourceSway` (mode `SwayCam`, the default) subtracts the full baseline (`baseline_cm * 0.01`, metres)
  from row 3 (x component) of the right pass's world-to-view `source` transiently around each real builder call
  (both the projection call at `source` and the view call at `source-16 floats`), then restores it. Sign flips
  when "swap eyes" is on. Only valid, unit-length matrices with `w == 1` are touched. Only the right pass (true output 1) is affected.
  The whole separation therefore sits on the right eye; the left eye is the game camera.
- `[SPLITSYM]` (`g_nativeSymmetric`, default off): optionally shifts the game camera itself by half the baseline
  the other way at the head write, so the pair is centred. Left = centre - b/2, right = centre + b/2.
- `[RIGHTSYNC]` (`g_syncRightCameraToLeftRender`, default on; logic in `right_camera_sync.h`): the right builder's
  input camera is one camera update newer than the render camera already bound to the left pass. When the right
  source equals the current head matrix and bank 0's view equals the renderer's current view (`ctx+0xB804C0`), the
  right build uses bank 0's view (local copy of the 128-byte source: view replaced, projection preserved) and then
  applies the normal baseline. This removed a whole-right-eye shake on moving vehicles. The native builder and the engine's source are not modified.
- `SetGameProjection` publishes P00/P11/skew from `source+0x40` to the presenter so the declared frustum can
  equal the rendered one.

### 5.5 Render views, banks and the renderer context

`ctx` below is `[base + 0x4F29430]` (`DAT_144f29430`, `kDeferredCtxGlobalRva`, the deferred renderer context).

| Address / offset | Meaning |
|---|---|
| `[base+0x45DF038]` (`kDisplayAreaObjectGlobalRva`) | display-area object |
| area `+0x300`, `+0x308` | pointers to bank 0 and bank 1 (each `0x2B0` bytes). Selector: caller `+0x67A1A70` indexes `[area + 0x300 + output*8]` and passes the bank to `+0xE8E3580`. |
| bank `+0x000` | 64-byte view matrix (row-vector world-to-view) |
| bank `+0x200`, `+0x240` | copied by `+0xE8E3580` into `ctx+0xB804C0` / `ctx+0xB80540` |
| bank `+0x280`..`+0x2A0` | handles (5 pointers sampled) |
| bank `+0x2A8` | state (8 bytes hashed) |
| `ctx+0xB80250` | view context ("view"), base of the current per-view data. Native uploads `+0xE8DFC90` / `+0xE8DFE50` target `ctx+0xB80250 + eye*0x650`. |
| view `+0x000`/`+0x180`/`+0x570`/`+0x5B0` | projection, inverse, duplicate, inverse duplicate |
| view `+0x040`/`+0x140`/`+0x5F0` | view, inverse, duplicate |
| `ctx+0xB804C0` | the renderer's "current view" matrix (64 bytes) |
| `ctx+0xB821EC` | ping-pong index used by `+0xE8E3580` |
| `ctx+0xB8204B` (bit `0x10`) | dirty bit set by the commit routines |
| `ctx+0xB820A8` | pointer to a holder; `[holder+8]` is the bindings/targets object read by the bank rotator |
| `+0x6768390` (`kUploadMatricesRva`) | `uploadMatrices` pass; hooked (16 bytes) for `CaptureNativeRenderHeadPose` and passive counters |
| `+0xEABA5A0` | pass registrar (`kPassQueueRva` and `kPassRegistrarRva` are the same address; two hooks are chained on it: `RetailPassQueuePolicy` and `RetailPassRegistrarPolicy`) |
| `+0xED942C0` (`kPoseBufferRecorderRva`) | records constant-buffer uploads: `void(queue, wrapper, source, size, flags)`, `[wrapper+0x30]` is the `ID3D11Resource*` |

The 768-byte camera constant buffer (`IView_Combined_cbView`) that geometry shaders bind, at these byte
offsets (from `mv_fix.h`): Projection `0x000`, View `0x040`, ViewProj `0x080`, InvView `0x0C0`, ViewPort
`0x140` (integer width/height at byte `0x148`), ViewPoint `0x150`, PreviousView `0x160`, PreviousProj `0x1A0`,
PreviousViewProj `0x1E0`. Typical focals for the main lens: P00 = 0.5928, P11 = 1.0538 (a 16:9 view
with 8:9 halves), P22 = -1.0000167, P32 = -0.2 (near 0.2 m), P23 = -1. The engine uploads it with
`UpdateSubresource` (whole buffer) and also maps a 128-byte and a 512-byte family of projection buffers; a
992-byte vertex constant buffer at VS slot 2 also carries camera/view data (first `0x140` bytes; layout
approximate, role of the rest not confirmed).

**Constraints.** Selecting the native per-eye bank at `+0x67A1A70` (`g_useTrueOutputForDisplayAreaBank`) did
not fix right-eye motion jitter. Restoring pre-call bank values around the bank rotator (`g_bankRotGuardEnabled`)
dangles a live pointer that `ForwardOnPostEffect` uses (crash at `+0xE8DA982`) and is disabled.

---

## 6. Camera ownership and head tracking

### 6.1 Principle

The gameplay camera is baked CPU-side into per-object matrices before upload. Editing GPU constant buffers
moves only secondary views (reflections, shadows, lights, UI). Freezing a downstream copy of the camera does
nothing, because the engine recomputes it every frame. The camera is therefore owned at the instruction that
writes it, and the head pose is applied to that matrix in place.

### 6.2 Head-writer discovery (`DynamicHeadWorker`, tag `[VRHEAD]`)

The head writer is found at runtime with hardware breakpoints. It is not hard-coded, because the writer
changes across scenes.

1. **Capture site.** `ResolveDynamicHeadCaptureSite` first tests the known site `+0x1073C7B` for the 7-byte
   pattern `FF 50 28 44 0F 28 38` (`call [rax+0x28]; movaps xmm15,[rax]`) and uses the address 3 bytes later
   (`+0x1073C7E`, the instruction after the call). If the bytes differ it searches executable sections for the
   frustum-getter pattern `FF 50 38 0F 28 30 0F 28 78 10 44 0F 28 40 20 44 0F 28 48 30` (up to four hits) and
   looks for the getter-A pattern within `-0x260`..`+0x180` of that anchor. At the capture site `RAX` points to the
   live gameplay camera matrix.
2. **Source capture (about 3 s).** DR0 is set as an execute breakpoint at the capture site on every thread.
   The handler takes `RAX`, validates it as a camera matrix (`IsDynamicHeadMatrix`: 16 floats, `|m[15]|` in
   0.75..1.25, first three row lengths in 0.75..1.25, translation below 100000), and stores the first valid pointer
   as the head source.
3. **Writer watch (about 4 s).** DR0 becomes a 4-byte data write breakpoint on `source+0x30` (the translation row's x).
   Each trap records its RIP; a data breakpoint traps after the write, so the recorded RIP is the instruction
   after the store. The RIP with the most hits is selected.
4. **Active.** DR0 becomes an execute breakpoint at the writer RIP (all threads, re-armed every 2 s to catch new
   threads). When it fires the game has just finished writing the matrix; the handler rewrites the matrix at `source` in
   place (head rotation, 6DOF offset, and in AER the eye offset) and resumes with the resume flag set.
5. **Recovery (`[HEADRECOVER]`, `head_tracking_recovery.h`).** A second breakpoint (DR2, stereo only) watches the same
   capture site and compares the matrix the renderer's getter returns with the mod's last committed matrix.
   After at least 12 consecutive mismatches lasting at least 300 ms, with a getter observation within 500 ms, the
   worker disarms DR0, watches writes to the current camera's `+0x30` for 100 to 1000 ms, and selects any
   executable-image writer seen at least 3 times. A changed camera pointer during collection rejects the result;
   at most one attempt per 5 s. This survives scenes where the camera object stays the same but its writer moves.

Writer RVAs observed at runtime: `+0x921424E` (typical in gameplay, stereo and AER alike), `+0x9215560`
(seen in some sessions; the handler additionally records `[rsi+0xA0]` for it), `+0x5BE3037` (selected after a scene
change; it directly follows the fourth 16-byte matrix store to `RDI+0/10/20/30`). `+0x921424E` lies 0x15E bytes
after the entry of the matrix-cache writer `+0x92140F0`; whether it is inside that function is not confirmed. A first
return-test scan lists `+0x6770CD4`, `+0x67B8074`, `+0xEDCCBA4` as copy-chain writers and `+0x2CCE940`,
`+0x2CE9745`, `+0x2D03AE0` as callers; none of these are used now.

One vectored handler serves all four debug registers. Any single-step trap produced by one of the mod's own debug
registers is resumed unconditionally, even if the feature that armed it has been switched off in the meantime
(`[ORPHANTRAP]`); otherwise a trap raised just before disarming would be passed on as a fatal exception.

### 6.3 Head source layout and the transform applied

- The head source is 16 floats in row-vector form: rows 0..2 are the camera axes, row 3 (`+0x30`) is the
  translation. Code that reads or writes it treats it as **world-to-view**: eye position =
  `-(t . axis_k)`, `ViewEyePosition`, `ApplyHeadRotation`, `ApplyAerEyeOffset`, `ApplyCameraLocalOffset`.
- The head pose from OpenXR arrives as yaw, pitch, roll and a local offset (`right`, `up`, `forward`, metres). The mod
  applies, in this order: (optional) `LevelCameraPitch` ("decoupled pitch"), `ApplyCameraLocalOffset(right*s, up*s,
  -forward*s)` (`s` = 6DOF position scale, default 1.0), then `ApplyHeadRotation(-yaw, pitch, 0)`. Roll is not
  written into the game camera; it is carried on the compositor pose.
- AER eye offset: `ApplyAerEyeOffset(matrix, ±half)` moves the eye along camera-right; `half` defaults to 0.0345 m
  (about 6.9 cm total). Sign is set by the parity of the mod's own build sequence (`[AERSEQ]`) and by "swap eyes".
- Native stereo keeps the head source as the left eye and adds the baseline at the right builder seat (5.4).

### 6.4 Pairing an image with the pose it was rendered with

- The game uploads the 768-byte camera buffer with `UpdateSubresource` (whole buffer) and, for other views,
  through mapped writes. The mod matches the uploaded view matrix to the history of committed head matrices and stamps the
  buffer, so each draw batch, and finally each copied image, carries the pose it was rendered with (`native_draw_pose.cpp`).
  Conflicts, stale generations and unknown writes fail closed; there is no fixed frame delay and no smoothing.
- In single-output modes the engine's rendered camera (`ctx+0xB804C0`) is compared with the last committed cameras by
  exact byte comparison; the drawn camera equals a committed camera on every frame, and its head sample is a fixed
  number of presents old (`[MONOTAG]`). The stereo bank route (`[base+0x45DF038]+0x300`) is not written in single-output modes.
- AER pairs (`EYEHOLD`): the first eye of a pair captures the game camera (and the head pose); the second eye
  replays that camera and head pose (`g_aerFreshHeadPose` is forced off; `[AERFRESH]` rejects a fresh second-eye
  pose). Stale holds older than 3 presents are dropped.

---

## 7. Lens and field of view

The engine has several layers; only the last two carry the picture, and they are the ones the mod pins.

| Layer | Address / offset | Finding |
|---|---|---|
| Camera parameter table | table `+0x4F01F30` (`kCamParamTableRva`), guard qword `+0x4CEDEE8`, `0x44F` (1103) 32-bit slots, filled by `SequenceActionInitCameraParameter` execute `+0xA382020` (`table[cameraParamType_ at +0x1B0] = cameraParamValue_ at +0x1B4`) | Slots hold **float bit patterns**. The gameplay FOV slot holds 48.0 (degrees) by default (slot 7 in the logged runs; the mod finds the slot by watching for the engine writing the reference value, or from `FovParamIndex`). Driving it works for third-person gameplay only; first person and cutscenes do not read it. |
| Camera object fields | `camera+0x300`/`+0x304`/`+0x308`, `source+0x40` | Dead: writes have no visual effect (downstream copies). `camera+0x300` is also a dword `setView` writes. |
| FOV channels | 9 channels of `0x30` bytes at `mgr+0x2C68`; winning channel pointer at `mgr+0x2E18` (`= 0x2C68 + 9*0x30`); the consumed value is a float (radians) at channel `+0x14`; additive lens channel pointer at `mgr+0x2FD8` | The channel table lives on the **manager passed to the setter**, not on the camera-job object. Each camera mode writes its own channel; higher-priority channels win. |
| FOV setter | `+0x91923D0` (`kFovSetterRva`): manager `RCX`, lens (radians) `XMM1`, channel `R8D`, mode `R9D`, duration at `[entry rsp+0x28]`, explicit start lens at `[entry rsp+0x38]` (-1 = current) | Every mode's FOV write (all 17 callers) passes here. DR1 execute breakpoint (`[FOVSET]`): while VR is on, `XMM1` is replaced by the pinned lens (default 110 clamped to 87 degrees), and for transitions with `duration > 0`, `mode != 6` and a valid start, the start stack argument is pinned as well so an authored narrow lens cannot flash (`[FOVSTART]`, `fov_transition_fix.h`; native mode 6 and zero duration bypass the interpolation start path, `+0x9192443` reads the start, `+0x9192493` calls `+0x918AA70` which writes channel `+0x14`). |
| Lens getter | `+0x9193380` (`kLensGetterRva`): `float(mgr) = [[mgr+0x2FD8]+0x14] + [[mgr+0x2E18]+0x14]`, 12 callers including the projection builders at `+0x92D92CB` and `+0x92D92FA` | Replaced by an absolute jump to `LensGetterDetour` (`[LENSPIN]`). Returns the pinned lens for every camera manager while VR is on; the unmodified sum otherwise. |
| Lens sibling | `+0x91934A0` (`kLensSiblingRva`): same sum inline, then virtual call `[[mgr+0x2AE0]]+0x78`, then tail jump to the lens convert `+0x92D5D50` (`kLensConvertRva`, `float(fov, v)`) | Replaced by `LensSiblingDetour`, which calls the convert function itself. |

Why the getter is pinned: the setter hook alone lets a camera that never calls the setter, a new camera manager,
or the additive zoom term through as a small picture with black around it. The declared frustum must equal the
rendered one, so the pin is applied at the point that all cameras read.

Related:

- `[LENSWATCH]` reports every episode where the rendered vertical FOV (from P11 in the projection at `source+0x40`)
  is more than 2 degrees off the pin.
- `[LENS]`/`[LENSHIST]` derive the lens the presenter should claim per frame from projection uploads of the main
  band (`P11` in 1.05..4.0; the most frequently uploaded lens of the frame wins).
- The title asset `level/title/title_envsys` can carry a `BCPT_FOV` node that drives the same table slot. That
  route is not used by the proxy.
- `SequenceActionSetCameraFov` at `+0x17AD980` (metadata `+0xAC0BBF0`) was identified as a first-person FOV lead; the mod does not hook it.

---

## 8. Game clock and task system

### 8.1 Frame update and world step

| Address | Ghidra name | Description |
|---|---|---|
| `+0x4D04B0` | `FUN_1404d04b0(game, int* rawDt)` | Frame update. Time is in **1/300000 s** units. It clamps dt (0.1 s = 30000), sets dt to 0 itself when the game is paused, and calls the world step twice, for world 0 and world 1. Not hooked. |
| `+0x2DBEC90` (`kWorldStepRva`) | `FUN_142dbec90(world 0\|1, int* dt, uint* unpausedDt, uint passes)` | `passes` = 2 when `display+0x244` (stereo gate) is set, otherwise `display+0x218` (output count). Hooked with MinHook for `[AERSYNC]`. |
| `+0x2DECB50` | `FUN_142decb50` | Stores `world+0x204 = dt`, `world+0x208 = unpausedDt`, then runs the task loop. Not hooked. |

The two clocks: `world+0x204` is the scaled (paused-aware) clock, `world+0x208` the unpaused clock (menus, UI).

### 8.2 Task runner and invoker

| Address | Name | Role |
|---|---|---|
| `+0xEE69FC0` | `FUN_14ee69fc0(runner, task)` | Task runner. Sets `task+0x38 \|= 0x10` and hands the invoker `&world+0x204` or `&world+0x208` depending on `task+0x38` bit 2. Calls the invoker unconditionally. |
| `+0x2DC1D60` (`kTaskInvokerRva`) | `FUN_142dc1d60(task, const u32* dt)` | Invoker. **If `*dt == 0` the callback is skipped** (`+0x2DC1D6D` jumps to `+0x176DCE0` with `rcx = task+8`). Otherwise adds banked time `task+0x4C` when `task+0x39 & 1` is clear, then dispatches by kind (`task+0x30`, kinds 2..10 through a jump table at `+0x2DC1E2C`, default `call [task+8](task+0x28, &dt)`), and zeroes `+0x4C`. |
| `+0x2DED010` | `FUN_142ded010(queue, threadIndex)` | Queue pop; passes `dl = (scheduler+0x1FC != 0)` (called from `+0x2DED080`); `scheduler+0x1F8` selects an alternate pop `+0x2DED300`. |
| `+0x2DCCFE0` (`kPopWrap`) / `+0x2DCD110` (`kPopLoop`) | `FUN_142dccfe0` / `FUN_142dcd110` | Pop a task from a batch and call the skip rule. The wrapper can tail-call the loop copy. |
| `+0xEE35030` (`kSkipRule`, thunk `+0x2DBBEB0`) | `FUN_14ee35030(task)` | Frame-skip rule. Returns `al = 0` RUN, `1` SKIP. |
| `+0x2DCD08A` / `+0x2DCD172` | restricted-pass gate | `test bpl,bpl` (wrapper) and `test sil,sil` (loop): if the pop is restricted and the batch lacks flag `0x10`, the task is dropped through `+0x2DCD230` / `+0xEE95B50` (`FUN_14ee95b50`, marks the task complete without running it). |
| `+0x2DCD079` / `+0x2DCD169` | return addresses of the rule calls inside the pop wrapper / loop | Used by the mod to recognise a legitimate pop context. |

Task structure (`task`):

| Offset | Field |
|---|---|
| `+0x08` | callback |
| `+0x10` | offset |
| `+0x28` | object (the actor for actor tasks) |
| `+0x30` | kind |
| `+0x38` | flags word: `0x40` run-now, `0x80` skip-once, bit 2 picks `world+0x208`, `0x10` set by the runner, bit 5 selects post handling |
| `+0x39` | byte; bit 0 = no banked time |
| `+0x40` | next |
| `+0x49` / `+0x4A` | interval / counter |
| `+0x4C` | banked dt |
| `+0x50` | parent |

Constructors: `FUN_14ee33ec0` (create), `FUN_14ee34210` (copy).

Skip rule: if `counter >= interval` or run-now, and skip-once is clear, the counter is reset and the task RUNs;
otherwise the counter is incremented, the frame dt is added to `+0x4C`, skip-once is cleared and the task is skipped.

Batch (`batch`): `+0x00` flags (`0x10` = allowed in restricted pass, `0x08` = lock at `+0x40`), `+0x06` (u16, atomic next index), `+0x08` done count,
`+0x68` task array, `+0x70` count.

### 8.3 Why native stereo starved some scenes ([TASKFIX])

In native stereo every actor batch is popped twice per present: once with the scheduler restricted
(`scheduler+0x1FC != 0`) and once unrestricted. With `passes = 2` in the world step this is consistent, but the exact
origin of the restricted second pass is not established. Ordinary actor tasks have skip interval 0, so the rule says RUN on both pops; the restricted one is
dropped by the gate and the normal one runs. Scripted-scene actors carry **interval 1**, where the rule
alternates RUN/SKIP. With exactly two calls per present, the RUN falls on the same pop every time; if that pop is
the restricted one the gate drops every RUN, the actor never updates, and the scene waits forever. Turning stereo
off restores one unrestricted pass per present.

Repair (`stereo_task_fix.cpp`, installed by `StereoTaskFix::Install`; MinHook on the two pops and the rule):

- Scope: every task whose callback is `+0xF3A20` (motion driving), `+0xF3B50` (actor update) or `+0xF3C40`
  (timed/attached state), with interval > 0, while native stereo is active, at the two known pop call sites, on a
  restricted pop of a batch without flag `0x10`.
- Action: reject that ineligible pass **before** the stateful skip rule runs and return SKIP through the native
  completion path. The counter (`+0x4A`), one-shot flags and banked dt are untouched, so the following
  unrestricted pass runs the original rule at native cadence. Mono, interval-0 tasks and unrelated callbacks are
  untouched. No scene, actor or id filtering.
- Callbacks are 5-byte `jmp` thunks: `+0xF3B50` -> `+0x5DC7060` (actor update job, dt x time scale `+0x5C00250`,
  or per-actor clock `+0x5C00450` for type `0x1025F8A`) -> actor `vcall +0x1048` -> `FUN_1400f3750` (`+0xF3750`);
  `+0xF3A20` -> `+0x5DC6760` -> component at actor `+0xA60` -> `+0x1274D0`; `+0xF3C40` -> `+0x5DC7160` -> actor `vcall +0x1050`
  (for the actor vtable `+0x30912C0` this is `+0x5DC62E0`, which ticks timed state objects at actor `+0x17A0/+0x17D0/+0x1800`
  and components `+0xDA0`, `+0xFF8`, `+0x1000`, `+0x1008`).
- Production installs only the two pops and the rule. The other detours in the file (invoker `+0x2DC1D60`, job
  `+0x5DC7060`, update `+0xF3750`, request `+0x10EFA0`, list `+0x5EB9410`, AI `+0x5E67ED0`, holder `+0x11D620`, tray
  `+0x89B7580`, motion setter `+0x5C163E0`, target resolver `+0xA157420`) are byte-checked but only installed in diagnostic builds.

**Constraints.** Re-arming the skip counter (instead of rejecting the pass before the rule) banks extra time on subsequent restricted SKIPs and did
not complete the scene. Forcing `+0x39 = 1` alone did not change scene behaviour either. A test with none of
the mod's gameplay patches and only the stereo gate flipped froze identically, so the starvation belongs
to the engine's own two-output path.

### 8.4 Scripted-scene classes involved in the diagnosis (engine map)

| Address | Description |
|---|---|
| `+0x7BE0EE0` (`kSequenceTickRva`) | sequence tick: actions array at `+0x78`, count at `+0x80`; skips an action if `+0x40 == 3` or `+0x45`; a single indirect call at `+0x7BE0F22` (`call [rax+0x130]`) runs each action. Execute returns true = still running. |
| `+0x36035E0` | vtable of `SequenceActionInteraction`; execute slot `+0x130` (`kSeqActionInteractionVtableRva`, `kSeqExecuteSlotOffset`) |
| `+0x354B820` / `+0x354BE80` | vtables of `SequenceActionExecAIModePlayMotion` / `...ExecAIModeWait`; shared execute `+0xA155FB0`; status at `+0x480` (0 = wait, 3/6/7 = fire output and finish); target vector pointer `+0x488`, count `+0x490`, 16-byte entries `{entity id, handle}`, `[handle+8]` is the live actor; resolver `+0xA157420`; start `+0xA15F0B0` -> `+0xA155E20` |
| `+0x34FDA80` | vtable of `SequenceActionTimeLineBlack`; execute `+0xA881BB0` -> base `+0xA0F910` |
| `+0x5C163E0` | ActorCharacter motion setter (writes `actor+0x7E4`, the current motion id) |
| `+0xE054B0` / `+0xE04E50` | motion request apply (actor `vcall +0xFF0`) / motion-request processor; gates at `+0xE04F13` (thunk `+0x1D3440` -> `+0x61078E0`) and `+0xE04F52` (thunk `+0x11E770` -> `+0x5E73120`) |
| `+0x8AE5680` | AI behaviour-graph leaf `BodyLeafNodeRequestAnimation` (vtable `+0x335BCD8`, slot `+0x158`); issues the motion request |
| `+0x5E3A040` -> `+0x5E3C450` | `requestAIMode(comp, mode, sub, ...)`: mode at `comp+0x94`, sub-mode `+0x98`, pending flag `+0x9C`; consumer `FUN_14010ff30`, per-frame driver `FUN_14010efa0` (`+0x10EFA0`), canApply `+0x10FD30` -> `+0x5E3CBD0` |
| mode ids | `0x1004A71` idle, `0x1015A6D` PlayMotion, `0x1015A6E`, `0x1015A6B`, `0x101F412`, `0x1019C6D`; sub-mode `0x1016B0E` default |
| `+0x5E67ED0` | AI component update, ticks four graph holders (`comp+0x68/+0x58/+0x60/+0x70`) through `+0x11D620`; state at `+0x5E67F2C`, gate at `+0x5E67F58` |
| `+0x3349098` / `+0x3349538` | vtables of `AIGraphTrayAIModeFSM` (update `+0x89B7580`, constructor `+0x89B70A0`) / `AIGraphTrayMindTaskFSM` (update `+0x89BB630`); graph update `+0x78072B0` -> `+0x776A750` -> transition walk `+0x776CC50` -> condition `+0x77FC5C0` |
| `+0xF3750` | actor update `FUN_1400f3750`; gates: `actor+0xF1 == 0` skip, `actor+0xF2 != 0` skip, control object `vcall +0xAB0` tested by thunk `+0x1D3440` (-> `+0x61079F0`); components via `vcall +0x9F0` -> list `+0x5EB9410` |

Interactive scenes can also run `SequenceActionInteraction::execute` in short bursts during ordinary play
(ambient interaction points), so a conversation is only recognised after continuous polling; the mono
fall-back that used this signal is a mode-switch aid, not a fix.

### 8.5 Actor LOD work path (bounded fallback)

In native stereo the actor LOD system demotes every actor (mono keeps about a quarter of bounded work entries at the finest tier; stereo none).

| Address | Function | Notes |
|---|---|---|
| `+0x170560` | actor LOD budget `(table, group, currentLevel, inputLevel, char* adjustment, short priority, float distance)` | hooked (17 bytes stolen); passive counters plus an optional near-actor tier restore (`g_nearLodRestoreEnabled`, off) |
| `+0x5FCF8A0` | LOD manager run | hooked (20 bytes), passive |
| `+0x5FD0920` | point-LOD kernel `(manager, entry, index)` | hooked (20 bytes), passive |
| `+0x5FD0BC0` | **bounded-LOD kernel** `(manager, entry, index)` | hooked (15 bytes). In stereo, when `entry+0x39 == 0` the mod sets it to 1 before the kernel runs and leaves it set through downstream consumption (`kHoldBoundedFallbackThroughDownstream`); restoring it right after the kernel breaks skeletal animation. `g_boundedFallbackRuntimeEnabled` gates it. |
| `+0x5FD0CF4` | return address of the call from the bounded kernel into `+0x170560` (`kActorBoundedBudgetReturnRva`) | used to identify the caller |

Work entry (bounded kernel): `+0x28` float distance (stored inline; it reads 0 before the kernel runs this frame),
`+0x33` level, `+0x34`, `+0x35` flag, `+0x36` mode, `+0x38` requested, `+0x39` acknowledged (the kernel enqueues downstream animation
work when `+0x38 != +0x39` among other conditions), `+0x20` pointer whose target object has a dword at `+0x7C`. The `+0x39`
forcing restored NPC animation and collision in native stereo. It is not the cause of scripted-scene stalls
(section 8.3).

### 8.6 AER pair synchronisation ([AERSYNC])

Problem: in AER the second eye of a pair renders one game frame after the first. `EYEHOLD` makes the static world
line up by giving the second eye the first eye's camera, but the simulation still advances on that frame, so cars,
party members and NPCs are one frame further along than the camera that looks at them. At driving speed this is tens
of centimetres, in one eye only. No camera-side change can fix it; releasing the hold merely moves the error to the world.

Mechanism (`WorldStepDetour`, hook on `+0x2DBEC90`): the decision is taken once per frame, on the call for world 0.
The frame whose camera build will be the **second** eye of a pair (parity of `PeekAerBuildEye()` XOR `g_aerSyncPhase`, phase fixed at 0) gets `dt = 0`
and its elapsed time is added to a carry (capped at 30000 = 0.1 s). The next frame, the first eye of the next pair,
receives `dt = real + carry`. The invoker (section 8.2) skips callbacks whose dt is 0, and a zero-dt frame is a
state the engine produces itself when paused. Rules: never two zero frames in a row, the unpaused clock (menus, UI)
is not touched, and the carry is handed back once when the feature turns off. Active only while AER is on with
`EYEHOLD`, not AFW, not Mono, not a flat scene, and head tracking in the `Active` phase.

The camera-build handler measures how far the game camera moved between pairs and how far the second eye's game
camera is from the first's; `[AERSYNC] pair check` logs these. On foot the camera also moves on the unpaused clock,
so the check reads "half off" in either phase and only reports; it does not flip the phase.

---

## 9. Per-output rendering state

### 9.1 Quality-mask apply ([QMASK])

| Address | Description |
|---|---|
| `+0x67A1890` (`FUN_1467a1890`) | multi-output settings caller; runs only when `mgr+0x244` or `mgr+0x218 > 1`; sole caller of `+0x679E050` at `+0x67A1901`; per-output blob at `base + output*0x13 + 0x40` |
| `+0x679E050` (`kQualityMaskApplyRva`, `FUN_14679e050(blob, ctx)`) | applies a 0x13-byte per-output settings blob. Hooked (15 bytes stolen, position independent). |
| `+0x679E5B0` | the matching state **saver** (reads current state into a blob) |
| `+0x679E7C0` | the state **restore** (see 9.2) |

Blob semantics: `blob[1] != 0` selects the "reduced quality" preset (kills the probe relight master byte, clears view-flag
bits 0 and 2, ORs disable bits into the feature mask). This branch never fired at runtime. Separately, in multi-output every frame
four feature flags are rewritten as `saved & gate` (`gate = blob[3]` for three of them, `blob[4]` for the fourth):

| Setter | Writes |
|---|---|
| `+0xE9616E0` | `obj(+0x58)+0x1E1` |
| `+0xE960F00` | `obj(+0x58)+0x1E0` |
| `+0xE962A30` | `subsystem+0xB9` |
| `+0xE9622A0` | `subsystem+0xB8` |

The mod passes the engine a local copy of the blob with `blob[1] = 0` and `blob[3] = blob[4] = 0xFF` so the AND is
a no-op (`g_qualityKeepEnabled`, default on).

### 9.2 View-quality restore ([VRESTORE], [BLOBCHECK])

`+0x679E7C0` (`kViewRestoreRva`, reached only through vtable data at `+0x542A614` and thunk `+0x47F9B0`; its prologue is not
trampoline safe: RIP-relative `mov` at byte 13) restores the view quality state from a saved 0x13-byte blob. The mod
replaces it entirely (absolute jump to `ViewRestoreReplacement`), reimplementing the writes and then, once stereo has ever
been on, forcing the measured mono baseline:

| Blob byte | Restored to |
|---|---|
| `blob[6]` | relight master byte at `[base+0x4F29430]+0xB827D0` |
| `blob[5]` | `viewctx+0x1DF8` |
| `blob[7]` | feature mask (`viewctx+0x6E0`) bit `0x400000` |
| `blob[8]` / `blob[9]` | view flags (`viewctx+0x1B20`) bit 2 / bit 0 |
| `blob[10]` | mask bit `0x400000000` |
| `blob[11]` | mask bit `0x8000000000` |
| `blob[12]` | mask bit `0x10000000000` |
| `blob[13]` | mask bit `0x80000000` |
| `blob[14]` | mask bit `0x200000000` |
| `blob[15..18]` | four setters (`+0xE9616E0`, `+0xE960F00`, `+0xE962A30`, `+0xE9622A0`) on the object returned by `+0xE8E2EF0(deferredBase)` |

Enforced baseline (when stereo is on or has ever been on): `viewFlags |= 5`, `mask &= ~0x80000000`, relight master byte = 1.
Here `viewctx` is `ctx+0xB80250`. In stereo the engine also calls the restore with a pointer into unrelated
memory (observed: the text "cuda_"); a real saved state contains only bytes 0..2, so any restore whose 0x13 bytes are not all `<= 2`
is skipped (`[BLOBCHECK]`).

### 9.3 Probe-weight producer and the interior lighting family ([PROBEW], [PROBEG])

- `+0xEBF3830` (`kProbeWeightsRva`, `FUN_14ebf3830`, source string "Light\Probe\LightProbeManagerBase.cpp(380): lightProbeWeightData_"):
  probe-weight producer. Object `+0x60` = probe count, `+0x64` = computed-empty latch (recomputes only when `count != 0 || !valid`).
  Hooked (16 bytes stolen; 12 bytes of pushes, `sub rsp,0x50`, 4-byte `cmp`).
- Two callers: `+0xEAE27A0` (producer B, the interior probe family) runs only when `viewFlags` bit 2 is set and `obj+0x2D8 == 0 && obj+0x2D9 == 0`; `+0xEAE27F0` (producer A) is gated on object bytes `obj+0x6D0`/`+0x6D1`. `DAT_1444f1f30` (`+0x44F1F30`, `kProbeForceGlobalRva`) is the engine's own override: non-zero makes producer B run regardless (the caller resets it to 0 each run; the mod's pulse of it, `[PROBEF]`, is off).
- In multi-output the engine reduces the view feature mask every frame (dword at `viewctx+0x1B20`: mono `0x1F`, stereo `0x1A`; relight byte `1` -> `0`; qword at `viewctx+0x6E0` bit 31 set). The reducer's writer is not identified. The mod
  corrects the three values **inside the producer hook** (`RetailProbeWeightsPolicy`), which runs after the reducer and before producer B's caller tests the gate: `viewFlags |= 5`, relight `= 1`, `mask6e0 &= ~0x80000000`. A correction at Present time lands too late (probes flash on alternate frames).
- Light manager: `ctx+0xB826E0` (`kLightManagerCtxOffset`). The specular stage `FUN_14ebf64f0` (`+0xEBF64F0`) picks its light-shape draw
  object from it: flag byte `+0x168 == 0` -> pointer at `+0x160`, else embedded object at `+0xB0`; the object's vtable method `+0x30` records per-light shape draws. Census only.

### 9.4 TAA history around output begin/end ([HISTFIX])

In multi-output the engine keeps each eye's TAA history in that eye's view state. Output END saves it (`+0x4816E0` -> `FUN_14e8e30c0` at `+0xE8E30C0`, which zeroes the shared slot) and output BEGIN restores it
(`+0x67A19A0` = `FUN_1467a19a0(outputMgr)` -> `FUN_14e8e3580(ctx, viewState)` at `+0xE8E3580`, which also allocates a fresh history when none was saved). Both are gated on the display stereo byte (`display+0x244`).

| Address / offset | Meaning |
|---|---|
| `+0x67A19A0` (`kOutputBeginRva`) | output begin. Hooked with MinHook (`OutputBeginDetour`). |
| `+0xE8E3580` (`kViewHistoryRestoreRva`) | `FUN_14e8e3580(ctx, viewState)` |
| `+0xEDB7160` | `FUN_14edb7160(display)` = current output index (same address as the output getter) |
| `[base+0x4F29430]` (`kDeferredCtxRva`) | ctx |
| `[base+0x4F294D8]` (`kSharedTargetsRva`, `DAT_144f294d8`) | shared renderer targets/pool object |
| `shared+0x620` (`kTaaHistorySlot`) | `temporalAALightingTexture` |
| output manager `+0x300 + out*8` (`kViewStateArrayOffset`) | per-output view-state pointer |

If the gate goes 1 -> 0 (stereo to single output, by any writer: menu, key, or the video/cutscene mono fall-back), the
first output that begins with the byte off receives the one restore the engine skipped, when `shared+0x620` is null. Without it, switching from
stereo to AER left the last eye's history parked, the shared slot null, and the screen-space-reflection pass
(`ScreenSpaceReflectionManager`, "reprojectedPreviousFrame") read a null texture and crashed at `+0xEBC2514`.

### 9.5 Renderer pool and lifecycle points

`[base+0x4F294D8]` (`kPoolGlobalRva`) is the renderer pool. The two teardown functions `FUN_14e8d98e0` (`+0xE8D98E0`, DrawPhaseEnd) and
`FUN_14e8dac40` (`+0xE8DAC40`, post-End) release these slots (offsets from the pool base, `kPoolSlotOffsets`):

`0x378, 0x3F0, 0x418, 0x440, 0x4B8, 0x4E0, 0x508, 0x530, 0x558, 0x6C0, 0x760, 0x788, 0x7B0, 0x7D8, 0x800, 0x828, 0x850, 0x8F0, 0x990, 0xAD0, 0xAF8, 0xB20, 0xC88, 0xCB0, 0xCD8, 0xD00, 0xD28, 0xD50`

The subset released only after the **last eye** is `0x8F0`, `0xC88`, `0xCB0`, `0xCD8`, `0xD00`, `0xD28`, `0xD50` (positions 17 and 22..27 of the array).
Measured: in mono only slot `0x990` is populated; in stereo many slots cycle non-null; after leaving stereo the pattern returns byte-identical to clean mono, so
nothing is "released and never rebuilt" at the pool level.

### 9.6 Post-effect chain and the bank getter

- `+0xEAD4120` (`FUN_14ead4120`, post-effect bank getter) returns the bindings slot `0x378` (param3 = 0) or `0x530` (param3 = 1) for output 0 and a hard **null for any output != 0**. That is how retail strips the second output's post banks (and why the engine's temporal-AA resolve does not run for the second eye; section 12).
- Six callers, hooked with trampolines (`kPostChainRvas`, stolen sizes 14, 15, 20, 17, 18, 14): `+0x67A2880` (bank-consuming state applier, indirectly called), `+0xE8D4BB0` (3-arg getter with out-pointer parameters; **must always execute**, skipping it crashed its caller at `+0xEBF67E2`), `+0xE8D8590` (phase dispatcher stage A, 3.7 KB), `+0xE8D94F0` (stage B), `+0xE8DA370` (phase stage), `+0xE8DA8B0` (`ForwardOnPostEffect`).
  Skipping their eye-1 duplicates (`[POSTSKIP]`, `g_postChainSkipEnabled`) is **off**: eye 1 needs those stages; skipping them unbinds per-output state and skin renders white on the right eye. The hooks only count.
- Frame-end bank rotator `+0x2BBBE90` (`kBankRotatorRva`, `FUN_142bbbe90`, source string "DrawManager.cpp(4456): ldrTex", 14 bytes stolen; only caller is the phase dispatcher's param_6 branch): runs once **per output**. Slots in the bindings object (`[[ctx+0xB820A8]+8]`): `0x378` current bank, `0x3A0`, `0x3C8`, `0x468` final-image handoff target, `0x490` source, `0x508` fresh ldrTex, `0x530` previous-frame temporal slot. Its handoff `bank[0x468] <- bank[0x490]` is gated on `!multiOutput || output == 0` through the getter call at `+0x2BBBF1D`; the mod returns the true output there (section 4).
- Named passes registered through `+0xEABA5A0` include `uploadMatrices`, `ShadowDepth` (x4 per frame in mono), `drawDirectionalDepthMapOne` (x8), `Opaque`, `ForwardOnOpaque`, `DeferredEmissive`, `Water`, `TransparentWithExtraDepthRT`, `ForwardOnPostEffect`, `AfterPostEffect`. The post filters run unannounced between `ForwardOnPostEffect` and `AfterPostEffect`.
- `+0x5BC3870` (`kUpdateDispatchRva`): per-frame update dispatcher; walks a list at `manager+0xBBF8` and calls virtual slot `+0x140` on each node. Hooked (15 bytes) for counters only.

### 9.6a Wireframe frames

A corrupt recorded rasterizer description (fill mode wireframe) can be replayed by the command executor after a stereo toggle. Chain: executor `+0x2DB009D` -> `+0x2DB0200`, state apply `+0x2DB4E20`, desc hash `+0x2DB41B0`, `RSSetState` call at `+0x2DB5065`. Retail never draws gameplay in wireframe, so `ui_hook.cpp` (`[WIREFIX]`) replaces every wireframe rasterizer state with a solid twin of identical desc inside the `RSSetState` hook. The root writer of the corrupt data is not identified.

### 9.7 Command-buffer replay call sites

The executor replays recorded draw state through fixed call sites, which the mod patches through relays for census and pose association:

| Site | Bytes | Call |
|---|---|---|
| `+0x2DB0D98` (`kReplayMapCallRva`) | `FF 50 70 48 8B 4C 24 50` | `Map` (`call [rax+0x70]`) -> `RetailReplayMapPolicy` |
| `+0x2DB1C1A`, `+0x2DB20E2` | `FF 50 68 E9 ...` | `Draw` (`call [rax+0x68]` then a jump to `+0x2DB3925`, `kReplayDrawContinueRva`) -> `RetailReplayDrawPolicy` |
| `+0x2DB290F` | `FF 90 48 01 00 00` | `Dispatch` (`call [rax+0x148]`) -> `RetailReplayDispatchPolicy` |
| IAT slot `+0x3067370` (`kReplayCopyIatRva`) | pointer | `memcpy` used by the executor; return address `+0x2DB0DB6` identifies the constant-buffer copy (`HookReplayCopy`) |

---

## 10. Volumetric light ([VOLGUARD])

| Address | Ghidra name | Role |
|---|---|---|
| `+0xE8D68C0` | view task `FUN_14e8d68c0` | per-view work; calls the entry below with the manager stored at `[ctx+0xB826F0]`, `ctx`, and `view = ctx+0xB80250` |
| `+0x2C7B290` (`kVolumetricEntryRva`) | `FUN_142c7b290(manager, ctx, view)` | entry of the volumetric light pass. Hooked with MinHook (`VolumetricEntryDetour`). |
| `+0x2C7EB50` | `FUN_142c7eb50` | `VolumetricLightManager` body |
| `+0x2C7F80A` | inside the body | crash site: reads `[texture+8]` where `texture` is null |
| `+0x2C86640` | thunk | -> `FUN_14ebb6770`, the test `(view+0x1DFF & 6) == 6 && [[view+0x708]+0x18] == 0` |

The pass's source target is `[[[view+0x1E58]+8]+0x8F0]` when `(view+0x1DFF & 6) == 6` and `[[view+0x708]+0x18] == 0`;
otherwise `[[view+0x1E48]+0x10]+0x750`, which is never null. In native stereo the first form was null on the view being
drawn during a large set piece, and the process crashed at `+0x2C7F80A`.

The guard reads exactly that condition (`VolumetricTargetMissing`): if the test conditions hold and `view+0x1E58`,
`[view+0x1E58]+8` or the `+0x8F0` slot is null (or any read faults), the volumetric entry returns without running the pass.
Fog and light shafts drop out for that view; everything else is unchanged.

What `view+0x1E58` holds (static analysis): each render job slot embeds a resource-binding set. The job setup
`FUN_14e973280` (`+0xE973280`) initialises it with `FUN_14eb3faf0` (`+0xEB3FAF0`: `set+8` = the slot's own entry array,
`0xDA` entries of `0x28` bytes), copies the source view's set into it with `FUN_14eb411d0` (`+0xEB411D0`; the source set
is read by `FUN_14e96b9c0`, a plain getter of `+0x1E58`), and stores it with `FUN_14e96bd10` (`+0xE96BD10`, a plain
setter of `+0x1E58`). So `[[view+0x1E58]+8]+0x8F0` is field `+8` (the resource pointer) of binding entry 57
(`57 * 0x28 = 0x8E8`) in the view's own copy of its parent view's bindings.

It is **not** the renderer pool (`[base+0x4F294D8]`): the pool slot `0x8F0` released after the last eye (section 9.5)
has the same offset by coincidence only. The missing resource is binding entry 57, already empty in the set the view
copies from. Which pass fills entry 57, and why it is empty in native stereo, is not established.

---

## 11. Actor and lighting facts not tied to a hook

- The 34 reader sites of the stereo gate byte are all renderer or display code (section 3). The engine has no gameplay-side stereo check.
- Light probes: mono baseline `viewFlags = 0x1F`, `relight = 1`, `mask6e0 bit31 = 0` (section 9.3).
- Lightning and battle-effect corruption (left eye floods with light) comes from malformed geometry already in CPU uploads: transform matrices consumed as 36-byte trail vertices.
  `lightning_geometry_fix.h` tags the four lightning shaders by the FNV-1a-64 hash of their bytecode (`0x47E10DBD23AB11A1`, `0xFA4539217706DFF8`, `0x04EAEC979EDD5535`, `0x0B8CCCC7247B9460`, tags 1..4), seeds a CPU-side shadow of the upload buffers, and rejects non-indexed strip draws with non-finite/extreme positions, negative colours or invalid opacity. Data that cannot be validated is left untouched. The underlying allocation/draw mismatch is not repaired.

---

## 12. DLSS integration (`dlss_upscaler.cpp`, `dlss_probe.cpp`, `mv_fix.h`)

### 12.1 What the game ships and how the mod calls NGX

- The game ships DLSS 1.0 (a different feature contract); it cannot be upgraded by swapping the DLL, and only one NGX initialiser can run per process. The mod initialises NGX itself on the game's D3D11 device (`NVSDK_NGX_D3D11_*`) and needs a DLSS 310.x `nvngx_dlss.dll` beside the executable. Preset K (transformer), DLAA quality value, flags `IsHDR | MVLowRes | AutoExposure`. One NGX feature (one history) per eye family so eyes never share a history. Any NGX or D3D failure turns the module off for the session and the engine's own TAA keeps running. AER keeps the engine's own path (`SetAerBypass`).
- Graphics config value for anti-aliasing: `Antialias` 0 = off, 1 = FXAA, 2 = TAA. Only TAA runs a temporal resolve that reads velocity.

### 12.2 The temporal resolve draw (fingerprint)

One pixel shader inside the `AfterPostEffect` pass. The mod identifies it at runtime (it is not a fixed hash) as the reader of a tracked velocity resource with this fingerprint (`DlssProbe::TemporalResolveShader`):

| Slot | Content |
|---|---|
| PS SRV 0 | scene colour, `R11G11B10_FLOAT` |
| PS SRV 3 | depth, `R32_FLOAT_X8X24_TYPELESS` view of `R32G8X24` (32-bit float depth + 8-bit stencil; standard Z, far = 1.0, near 0.2 m; stencil = material class) |
| PS SRV 6 | velocity, `RG16F` |
| PS CB 0 | 256 bytes, `cbTemporalAA` |
| RTV 0 | `R11G11B10_FLOAT` |

Other post effects (motion blur / depth of field: velocity slot 1, RGBA16F colour, 512-byte CB) also read depth and velocity and must be skipped.
The engine binds all 16 PS SRV slots on post draws (stale ones included), so format co-occurrence on one draw proves nothing;
identification is by resource identity (a velocity or depth resource written as an output is later seen as an input).

### 12.3 Data formats

- **Velocity**: `RG16F`, MRT slots 1 and 4 of the deferred pass. Written pixels hold **total** screen motion (camera included) in UV units, y-down: `prevUV = uv + v`. Unwritten pixels (static world, sky) hold the sentinel `(0, 1.0)`.
- **`cbTemporalAA`** (256 B; the mod shadows the first 28 floats by content: `c0 = (W, H, 1/W, 1/H)` at Map/Unmap/UpdateSubresource because the engine draws it from a pool of buffers, consumed once at the draw): `c0` screen size, `c1` frame bits, `c2.xy` (floats 8 and 9) UV jitter, `c3..c6` (floats 12..27) the 4x4 motion matrix (current UV + depth -> previous UV; column-major as bound; the compute pass multiplies `x_row(u, v, depth, 1) * motion`).
- **Camera constant buffer** (`IView_Combined_cbView`, section 5.5) supplies view, projection, `PreviousView`, `PreviousProj`.
- Jitter is applied in the projection's row 2 (columns 0 and 1); `ViewProjNoJitter` clears them (floats 8 and 9 of the projection).

### 12.4 Per-frame flow (resolved eye)

1. A compute pass (`kPrepareShader`) converts velocity to DLSS pixel motion vectors (`prev - cur`, scaled by target size; sentinel pixels rebuilt from depth through the motion matrix) and copies depth into a plain `R32F` texture.
2. NGX evaluates DLSS into the mod's UAV texture.
3. `CopyResource` into the engine's resolve target; the engine draw is skipped.
It only acts on the immediate context; if the resolve is recorded on a deferred context the engine's TAA keeps running.

### 12.5 Second eye

The engine never resolves the second eye (section 9.6). Both eyes render into **one shared scene-colour target**, the unresolved eye renders **first** in a frame, and both run the same post chain, the second minus the resolve. The mod learns the **anchor**: the first draw that reads the scene colour after the resolve, in the resolved eye. In the unresolved eye DLSS runs in place on the scene colour just before that same draw, with that eye's own velocity and depth family (learnt from `OMSetRenderTargets`: `RG16F` MRT plus DSV).
Because the unresolved eye renders before the frame's `cbTemporalAA` exists, its jitter is predicted: the resolved eye's real jitter is recorded per frame, the repeat period is found, and the value one period back is used (99.5 % hits). A Halton single-index model does not fit (x and y sit at different sequence positions). Its static-pixel motion uses the previous frame's motion matrix (one frame stale).

### 12.6 Motion-vector correction ([MVFIX])

The "previous" matrices in the view constant buffer are not the eye's own previous camera: they equal the same frame's camera shifted about 0.098 sideways (roughly the eye baseline), so the engine's TAA matrix and its written velocities hold a fixed fake parallax and no head motion. DLSS trusts motion vectors and smears vegetation and water reflections.
`mv_fix.h` observes every upload of the 768-byte view buffer, flags it main-camera when it is a perspective main-lens view with a full-size viewport, and keeps the last upload per buffer. `dlss_upscaler.cpp` keeps a short ring of distinct view uploads per eye family and per VS constant slot and rebuilds the camera part of that eye's motion from its real current and previous views (`ownMotion`, `engineCam` in `cbuffer MotionFix`): sentinel pixels use `ownMotion`; written pixels get `v += ownMotion*x - engineCam*x`. The engine's data is never modified. The second eye's pass uploads the same view twice per frame with different jitter, so for that family a same-view upload within 4 ms refreshes the entry instead of creating a new one.

### 12.7 Known limits

- The second eye's exposure/glare chain in TAA mode is not reproduced: the left eye can wash out toward bright sources. Filling null post inputs from the resolved eye (`[GLAREFIX]`, `[DECALFILL]`) did not change it and is off.
- In native stereo the GPU frame is not pixel-bound (a 4x pixel reduction saves only about 2.5 to 3 ms of about 17.5 ms), so DLSS is an image-quality feature there, not a stereo performance fix.
- Memory budget check `[DLSSMEM]`; oversized pictures are tiled into overlapping columns (`kTileOverlap = 64`, up to four columns) if NGX refuses the whole size.

---

## 13. Per-eye full-size capture ([FULLRES], `full_eye_capture.h`)

Native stereo renders **each eye at the full render resolution** and downsamples it into its half of the backbuffer (a `W/2 x H/2` picture centred vertically, so an eye is a quarter of the backbuffer's pixels). The mod
replays the engine's final packing draw into private eye images at full size:

- Recognised draw: 6 vertices, into the backbuffer, no depth, a single-sample 2D texture on PS SRV 0 whose shape matches the backbuffer (within 1 %) and whose width exceeds the packed half (`PlanFor`). The picture format is free: the game's own shader converts it.
- Two consecutive qualifying draws with the same source and shader define the pair; conflicts invalidate the frame. Each is re-drawn with an enlarged viewport around that eye's rectangle: `scale = 2*plan.w / backbuffer.w`, viewport origin `scale * (vp.TopLeftX - (eye ? W/2 : 0))`, `scale * (vp.TopLeftY - H/4)`. Shaders, textures, constants and blend stay the game's own.
- `CopyPair` then substitutes the two images for the two halves at the image-copy boundary.
- The engine's render resolution equals the backbuffer only at 100 % resolution scaling, which is why the full-size path engages only then (`RenderingResolutionRatio`).
- `[VIDEOFIT]`: viewports wider than 60 % of the backbuffer mean a full-screen video or loading screen; a picture in the left half means a per-eye composite. The layout is published each frame for the presenter.

---

## 14. Interface capture (`ui_hook.cpp`)

- **Hook layer.** The engine keeps its own cached copies of the D3D11 context method tables (slots `+0x38` `VSSetConstantBuffers`, `+0x60` `DrawIndexed`, `+0xA0`, `+0xA8`) and bypasses the context's public vtable, and it re-writes those caches periodically. Vtable patches therefore see only a handful of draws. The mod
  places MinHook **inline hooks on the d3d11.dll implementations** of Draw, DrawIndexed, DrawInstanced, DrawIndexedInstanced, DrawAuto and the two indirect variants, plus `Map`, `Unmap`, `UpdateSubresource(1)`, `CopyResource`, `CopySubresourceRegion(1)`, `Dispatch(Indirect)`, `OMSetRenderTargets(AndUnorderedAccessViews)`, `ClearDepthStencilView`, `RSSetState`, `ExecuteCommandList` and `FinishCommandList` (`[D3DHOOK]`, installed before any vtable patch touches the same contexts). Deferred contexts have their own vtable variant.
- **Interface draw rule (`[UIGATE]`).** Interface work is alpha-blended, binds no depth-stencil, and covers the display: `blend && !depth && IsLdrFormat(rt.fmt) && fullDisplay`. The engine's pipeline is HDR end to end: sprites (6-index quads) land in a display-sized `R16G16B16A16_FLOAT` target right before the backbuffer copy, so that format counts as an interface format together with the 8-bit and 10-bit ones.
- **Canvas.** Sprites are placed on a 1920 x 1080 canvas by the sprite vertex shader (bytecode at executable file offset `0x3076710`, not an RVA). `UiWorldRouting::ProjectSprite` reproduces it from the constant buffer: `v = local.x*cb[0..3] + local.y*cb[4..7] + cb[8..11] + cb[12..15]`; if `cb[134] > 0` `v.xy -= 0.001` else `v.xy = floor(v.xy + 0.4999)`; clip = `v * cb[32..47]` (floats 16..31 unused).
- **World-anchored icons** (objective ring, target lock, interaction prompts, labels): identified by same-pass association with an objective ring (`ObjectiveGroups`, centre tolerance 4 canvas pixels, size 4..96) or by texture; captured into their own textures (`ui_objective_world.h`: 512 px texture, 2 texels per canvas pixel, a standard 40-pixel ring = one metre billboard, 0.025 m per canvas pixel) and placed by the presenter as world-anchored quads. Native SBS draws the interface once per eye into half-width viewports; the layer keeps the left copy.
- **Layers.** Interface draws are redirected into a private texture (`[HUDLAYER]`, blend states derived so alpha carries coverage: `alpha = src.a + dst.a*(1-src.a)`) and shown on a compositor quad layer; the world-icon quad and the menu are separate layers.

---

## 15. Hardware breakpoints (debug register allocation)

| Register | Use in the shipping configuration | Diagnostic use |
|---|---|---|
| DR0 | head-writer breakpoint (execute at writer RIP; execute at the capture site during discovery; 4-byte write watch on `source+0x30` during writer discovery and recovery) | - |
| DR1 | FOV setter execute breakpoint at `+0x91923D0` (`[FOVSET]`); armed whenever native stereo or AER is on | - |
| DR2 | passive camera-getter observer at the capture site (`[CAMOBS]`, native stereo only) | sequence census call site, pop-gate/task probes |
| DR3 | not armed | character write watch, interaction execute, AI probes (armed only by the diagnostic builds; the interaction watch is retired) |

All registers are set on every thread by suspending it (`SetThreadContext`); handlers run in a vectored exception handler and always resume with the resume flag set for execute breakpoints. The hang-diagnostic set (`[POPGATE]`, `[TASKSKIP]`, `[ACTRATE]`, `[AIGRAPH]`, ...) is armed only when `kArmHangDiagnostics` is true; it is false in the shipping build.

---

## 16. Display and monitor spoof ([DISPQ], `display_spoof.cpp`)

Native stereo submits half of the desktop backbuffer per eye, so a `2560x1440` backbuffer gives each eye a `1280x720` picture; AER hands each eye the whole backbuffer. The engine sizes its backbuffer from the display it runs on, so the spoof makes every display-size query agree that the game's display is `[Display] BackbufferWH` (for example `5120x2880`, which is 16:9 and gives `2560x1440` per eye).
A 32:9 buffer would letterbox and fail the mod's 16:9 focal gates (`focalX > 0.3`, `IVFOV aspect 1.5..2.1`). The game manifest declares Per-Monitor DPI awareness, so every query in-process is in physical pixels.

Keyed on the **size** of the display the game uses (`SourceWH`, default = the largest attached display), not on "primary". Installed from `DllMain` (before the game's first query) with MinHook; every hook is a pass-through when inactive (target not larger than the source).

| Call | Behaviour |
|---|---|
| user32 `EnumDisplaySettingsW/A` | `ENUM_CURRENT_SETTINGS` and `ENUM_REGISTRY_SETTINGS` of a source-sized display return the target size; one extra mode is appended at the end of the indexed list |
| user32 `GetMonitorInfoW/A` | `rcMonitor` and `rcWork` of a source-sized monitor are rewritten to the target |
| user32 `GetSystemMetrics` | `SM_CX/CYSCREEN` (+ fullscreen) only if the primary monitor is the source size; the virtual-screen metrics are left alone |
| user32 `SetWindowPos` | observer only (logs the requested size) |
| gdi32 `GetDeviceCaps` | `HORZRES`/`VERTRES`/`DESKTOPHORZRES`/`DESKTOPVERTRES`, only when both axes of that DC are the source size |
| `IDXGIOutput::GetDesc` (vtable slot 7), `IDXGIOutput6::GetDesc1` (27) | `DesktopCoordinates` source -> target |
| `IDXGIOutput::GetDisplayModeList` (8), `IDXGIOutput1::GetDisplayModeList1` (19) | count + 1, target mode appended |
| `IDXGIOutput::FindClosestMatchingMode` (9), `IDXGIOutput1::FindClosestMatchingMode1` (20) | a request for the target resolves to it |
| `IDXGIFactory` swapchain creation | observed; the created size is logged (`[DISPQ] CreateSwapChainForHwnd desc=...`) |

The output vtable is patched once from the DXGI factory hook (`HookDxgiOutputs`); one vtable serves every output object of this DXGI implementation. The engine renders `min(configured resolution, display)`, so the game's graphics config (`DisplayResolutionWH`) must also name the target.

---

## 17. Swapchain, Present and runtime interaction

### 17.1 Proxy and factory

`dxgi.dll` is a proxy (`dxgi_proxy.def` exports `CreateDXGIFactory`, `CreateDXGIFactory1`, `CreateDXGIFactory2`, `DXGIDeclareAdapterRemovalSupport`, `DXGIDisableVBlankVirtualization`, `DXGIGetDebugInterface`, `DXGIGetDebugInterface1`, `DXGIReportAdapterConfiguration`), forwarding to the system `dxgi.dll`. Factory vtable patches: `IDXGIFactory` slot 10 (`CreateSwapChain`), `IDXGIFactory2` slots 15, 16, 24 (`CreateSwapChainForHwnd`, `...ForCoreWindow`, `...ForComposition`). Each newly created swapchain is hooked.

### 17.2 Present hooks

`SwapchainInline::Install` places **MinHook inline hooks on the function bodies** of `IDXGISwapChain::Present` (vtable slot 8), `Present1` (slot 22) and `ResizeBuffers` (slot 13) and leaves the public vtable untouched. An overlay that discovers the swapchain later (Steam or a capture tool) therefore never mistakes the mod's callback for DXGI's entry point; each detour calls its own executable trampoline, not the possibly re-patched DXGI address. `DXGI_PRESENT_TEST` calls and nested presents (an overlay calling back into Present) pass through untouched. The registry holds up to 8 targets per function.

### 17.3 Steam

Optional (`[Performance] SteamCallbacksHz`, 0 = off, 10..60 accepted): `SteamAPI_RunCallbacks` in the already loaded `steam_api64.dll` is limited to that rate; skipped pumps still call `SteamAPI_ReleaseCurrentThreadMemory`. No other Steam DLL is loaded or replaced.

### 17.4 D3D11 context vtables and Meta's runtime

Meta's runtime replaces D3D11 context vtables when its session starts (a copy of the vtable, or rewrites of slots). `[VTALIAS]` treats a vtable whose untouched slots 0-6 and 8-11 equal a hooked table as an alias of it; `[VTWATCH]` logs, every 240 presents, which patched slots (7 `VSSetConstantBuffers`, 12 `DrawIndexed`, 13 `Draw`, 14 `Map`, 15 `Unmap`, 47 `CopyResource`, 48 `UpdateSubresource`, 58 `ExecuteCommandList`) no longer point at the mod's code. Interception stays intact because the d3d11.dll implementations are inline hooked (section 14). Vtable patches are not durable against this runtime or an alt-tab.

### 17.5 OpenXR runtimes (`openxr_presenter.cpp`)

- Submission is `XR_KHR_D3D11_enable` on the **game's** device. Runtime name from `xrGetInstanceProperties` selects the paths below (`[XRRUNTIME]`).
- **SteamVR**: its OpenXR client creates a 1x1 keyed-mutex sync texture on the session device. A device made with an explicit adapter and `D3D_DRIVER_TYPE_UNKNOWN` cannot create one (`E_INVALIDARG`), which makes SteamVR silently reject every projection layer while `xrEndFrame` still succeeds. The XR device is re-created as a null-adapter hardware device when that resolves to the runtime's adapter (`[SVRDEVICE]`); a frame that would otherwise carry zero layers gets a tiny opaque-black projection layer (`[STEAMVRSCENE]`); a crop to the runtime's frustum exists (`[STEAMVRFOV]`) but is off because SteamVR accepts the uncropped view and cropping removed the spare edge its reprojection needs.
- **Meta (Quest Link)**: the runtime ignores the FOV declared on a projection view and maps the submitted rect onto its own per-eye frustum (a code comment gives about -54/+40 degrees horizontally and +-43.5 vertically for Quest 3; other notes in the repository quote different vertical figures). The game's lens is shorter than the frustum, so cropping cannot fix it. Each eye is shadow-copied while still acquired and redrawn into a canvas swapchain shaped like the runtime frustum at the same pixel density, black where the game drew nothing, and declared at the runtime FOV (`[METAFIT]`).
- The FOV declared to the compositor must equal what the game rendered (section 7).

---

## 18. Input

| Mechanism | Detail |
|---|---|
| Gamepad | The game reads the Xbox pad through `XInputGetState` in `xinput9_1_0.dll`. The mod hooks that export (loading the DLL if it is not yet loaded), and the same export in `xinput1_4.dll` and `xinput1_3.dll` when they are already loaded, with MinHook. While the mod's menu is open the hook returns success with an empty `XINPUT_GAMEPAD` (packet numbers keep counting) so the game sees a connected, idle pad (`[MENUBLOCK]`). |
| Mouse | `SetCursorPos` and `ClipCursor` are hooked and become no-ops while the menu is open (`[MENUINPUT]`). |
| Keyboard | Polled with `GetAsyncKeyState` from the Present hook. Keys: F9 native stereo toggle (rebindable), mono key (default End), menu key (default Insert). Developer keys (F2..F12, numpad) are off unless enabled in the ini. |
| DirectInput8 | The executable imports it, but nothing in the proxy hooks it; its role in the game's input is not confirmed. |

---

## 19. Patch and hook inventory (installed by default)

All sites are byte-verified first (section 1.3). "Kind": **jmp** = 14-byte absolute jump to the mod's function with a trampoline for the original; **jmp-nop** = same, replacing the whole function; **relay** = a call/jump site rewritten to a nearby executable relay; **iat** = pointer patch; **MinHook**; **nop** = instructions overwritten with `0x90`; **bp** = hardware breakpoint.

| RVA | Purpose | Kind | Tag |
|---|---|---|---|
| `+0xEDB7160` | current-output getter | jmp-nop (no original call) | `[OUTPOLICY]` |
| `+0x9207950` | stripped camera job -> full job | jmp-nop | `[CAMDEFER]`, `[CAMWRAP]` |
| `+0x10740F9`, `+0x1074182` | eye view/projection builder calls | relay | `[EYEPAIR]` |
| `+0x1074263`, `+0x1074277` | render-context projection/view commits | relay | `[EYEPAIR]` |
| `+0x10741EE`, `+0x1074203` | eye-0 shared-camera setter calls | nop | `[CAMFIX]` |
| `+0x92140F0` | matrix-cache writer guard (15 bytes) | jmp | matrix-cache skip logs |
| `+0xED942C0` | constant-buffer upload recorder (16) | jmp | pose association |
| `+0x6768390` | `uploadMatrices` (16) | jmp | pose capture |
| `+0xEABA5A0` | pass registrar (20 + 15, two chained hooks) | jmp | `[PASSQ]`, `[PASSCEN]` |
| `+0x2DB0D98`, `+0x2DB1C1A`, `+0x2DB20E2`, `+0x2DB290F` | replay Map/Draw/Dispatch | relay | `[REPLAY...]` |
| `+0x3067370` | replay memcpy | iat | pose association |
| `+0x170560`, `+0x5FCF8A0`, `+0x5FD0920`, `+0x5FD0BC0` | actor LOD budget/manager/point/bounded (17/20/20/15 bytes) | jmp | `[ACTORLOD]`, `[LODBOUND]` |
| `+0x67A2880`, `+0xE8D4BB0`, `+0xE8D8590`, `+0xE8D94F0`, `+0xE8DA370`, `+0xE8DA8B0` | post chain (14/15/20/17/18/14) | jmp | `[POSTSKIP]` |
| `+0x2BBBE90` | bank rotator (14) | jmp | `[BANKROT]` |
| `+0xED79434` | fiber output restore jump | relay | `[OUTSET]` |
| `+0xEBF3830` | probe-weight producer (16) | jmp | `[PROBEW]`, `[PROBEG]` |
| `+0x5BC3870` | update dispatcher (15) | jmp | `[UPDISP]` |
| `+0x679E050` | quality-mask apply (15) | jmp | `[QMASK]` |
| `+0x679E7C0` | view-quality restore | jmp-nop | `[VRESTORE]`, `[BLOBCHECK]` |
| `+0x67A19A0` | output begin | MinHook | `[HISTFIX]` |
| `+0x2DBEC90` | world step | MinHook | `[AERSYNC]` |
| `+0x2C7B290` | volumetric light entry | MinHook | `[VOLGUARD]` |
| `+0x9193380`, `+0x91934A0` | lens getter and sibling | jmp-nop | `[LENSPIN]` |
| `+0x2DCCFE0`, `+0x2DCD110`, `+0xEE35030` | task pops and skip rule | MinHook | `[TASKFIX]` |
| capture site `+0x1073C7E` (dynamic) | camera getter observer | bp (DR2) | `[CAMOBS]` |
| writer RIP (dynamic) | head pose | bp (DR0) | `[VRHEAD]` |
| `+0x91923D0` | FOV setter | bp (DR1) | `[FOVSET]` |

Defined but not installed: the hook at `+0x2D735B0` (`kUpdateRenderViewARva`, `TryInstallHeadTrackingHook`, tag `UPDATEVIEW_A`); that address is `0xCC` padding in the live executable. Compile-time-disabled options: `kInjectUploadMatrices`, `kConstructEye1MatrixCache`, `kNativeStereoOnly`, `kSkipCameraJobInStereo`, `kEnableIvFovPatchLane`, `kEnableCameraScalarLane`.
