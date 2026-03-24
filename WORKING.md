# Working Log — DeviceControl_FullThreaded

**Date:** 2026-03-19
**Branch:** `buffOverun`
**Primary file:** `DeviceControl_FullThreaded.c`

---

## Overview

The application controls a radar system comprising:

- **KMTronic** relay board (USB/serial)
- **Siglent SPD3303X** PSU (VISA/serial)
- **AD9914** DDS chirp generator (serial)
- **ADLINK PCI-9846H** simultaneous-sampling ADC (WD-DASK driver)

The session focused entirely on the ADC acquisition pipeline. The hardware was confirmed working (a standalone example program `EXAMPLE_DBCODE_PostExtTrg.c` acquires data successfully), but the main application showed no plots and no observable data flow despite reporting "Acquisition Armed."

---

## Aims

1. Identify why continuous acquisition produced no plots.
2. Correct all runtime errors (crashes, double-frees).
3. Eliminate build warnings.
4. Replace incorrect ADC API usage with the appropriate call for simultaneous-sampling hardware.
5. Add comprehensive pipeline diagnostics so future test runs can pinpoint exactly which stage fails.

---

## Challenges

- **WD-DASK API ambiguity:** Two similar functions exist — `WD_AI_ContScanChannels` (designed for multiplexed/scanning ADCs) and `WD_AI_ContReadMultiChannels` (for simultaneous-sampling ADCs like the PCI-9846H). The original code used the wrong one. The working example uses `WD_AI_ContReadChannel` (single channel), which does not directly reveal the multi-channel equivalent.
- **CVI threading constraints:** `SetCtrlVal` and other UI calls are not re-entrant from background threads in CVI 2020. All UI updates from threads must go through `PostDeferredCall`. Several places in the original code violated this.
- **Silent consumer thread failure:** The disk-writer thread allocated a 6.7 MB buffer on every loop iteration and freed it at the end of each iteration. This caused rapid heap exhaustion and silent thread death, with no error visible to the user.
- **Double-free / GPF:** When the per-iteration malloc was moved to a single pre-allocated buffer, the `free()` call inside the loop was not removed, causing a General Protection Fault on the second iteration.
- **No diagnostic visibility:** With all failures silent, there was no way to distinguish between "DMA never fires", "data arrives but is dropped", "data is plotted but graph does not refresh", or "deferred callback never runs."

---

## Changes Made

### 1. Live Plot Triggering — `HardwarePollThreadFunction`

**Problem:** No `PostDeferredCall` existed anywhere in the data path. The UI was never asked to plot anything during continuous acquisition.

**Fix:** Added a throttled plot trigger inside the `halfReady` branch. Every 4th half-buffer that arrives (when no plot is already in flight), `plotBuffer` is populated and `PostDeferredCall(ADC_PlotFFT_Deferred, NULL)` is posted to the main UI thread. A `plotBusy` flag prevents concurrent deferred calls from stacking.

---

### 2. Correct ADC Start API — `AdcStartCB`

**Problem:** `WD_AI_ContScanChannels` was used. This function is designed for multiplexed (scanning) ADCs and does not reliably produce `halfReady` events on the simultaneous-sampling PCI-9846H.

**Fix:** Replaced with `WD_AI_ContReadMultiChannels`, which is the correct call for simultaneous-sampling hardware:

```c
chans[0] = 0;
chans[1] = 1;
err = WD_AI_ContReadMultiChannels(adcCard, ADC_NUM_CHANNELS, chans, adcBufId,
                                   totalScansPerCh, 1, 1, ASYNCH_OP);
```

---

### 3. Consumer Thread malloc Fix — `DiskWriterThreadFunction_Optimized`

**Problem:** A `U16 *queueBuffer` was allocated inside the `while (isAcquiring)` loop and freed at the end of each iteration. At 6.7M samples × 2 bytes = ~13 MB per iteration, this rapidly exhausted the heap and killed the thread silently.

**Fix:** Moved the `malloc` to before the loop (single allocation) and removed `free` from inside the loop. The single `free` is retained in the post-loop cleanup block.

---

### 4. Double-Free / General Protection Fault

**Problem:** After fix #3 moved `malloc` outside the loop, the `free(queueBuffer)` that remained inside the loop freed the buffer on the first iteration. On the second iteration `CmtReadTSQData` wrote into freed memory (GPF at line 484), then the same freed pointer was passed to `free` again (non-fatal error at line 533).

**Fix:** Confirmed removal of the orphaned `free(queueBuffer)` from inside the loop body.

---

### 5. Build Warnings

**Uninitialised struct array:**
`BufferSlot batch[BATCH_SIZE];` — if index 0 allocation fails and the function returns early, remaining `.buffer` fields are uninitialised, producing warnings on subsequent `if (batch[i].buffer)` checks.
Fix: `BufferSlot batch[BATCH_SIZE] = {0};`

**Implicit int-to-U16 narrowing:**
Loop variable `int i` passed to `WD_AI_CH_Config` and `WD_AI_CH_ChangeParam` which expect `U16`.
Fix: Explicit casts `(U16)i` at the call sites in `AdcConfigureCB`.

---

### 6. Thread-Safe UI Calls

**Problem:** `SetCtrlVal` calls existed inside `DiskWriterThreadFunction_Optimized` (a background thread), which is not safe in CVI 2020.

**Fix:** Removed all `SetCtrlVal` calls from the consumer thread. UI updates in the background poll thread are done exclusively through `PostDeferredCall`.

---

### 7. Error Checking — `WD_AI_ContBufferSetup`

**Problem:** The two `WD_AI_ContBufferSetup` calls in `AdcStartCB` had no error checking. A failure here would silently produce invalid buffer IDs, causing all subsequent DMA operations to fail with no reported cause.

**Fix:** Added error checks to both calls with status messages written to `TABPANEL_2_ADC_MSG_STATUS` on failure.

---

### 8. Pipeline Diagnostic Instrumentation

**Problem:** No visibility into which stage of the acquisition pipeline was failing.

**Fix:** Added seven volatile diagnostic counters, a deferred completion counter, and a first-raw-sample capture:

| Counter | Meaning |
| --- | --- |
| `diagPollCount` | Times `AsyncDblBufferHalfReady` was called |
| `diagHalfReadyCount` | Times `halfReady == TRUE` was returned |
| `diagFStopCount` | Times `fStop == TRUE` was returned |
| `diagQueuedCount` | Times a half-buffer was successfully queued |
| `diagPlotTriggeredCount` | Times `PostDeferredCall` was posted for a plot |
| `diagPlotEnteredCount` | Times the deferred plot callback was entered |
| `diagPlotCompletedCount` | Times the deferred plot callback completed |
| `diagFirstRawSample` | First raw U16 value seen in DMA buffer |

These counters are displayed every 500 ms by `AdcPollTimerCB` in a single status line:

```text
Poll:1250 HR:42 FS:0 Qd:42 | PTrg:10 PEntr:10 PDone:10 | Raw0:0x7FFF
```

The diagnostic timer is enabled in `AdcStartCB` on successful arm and disabled in `AdcStopCB` on stop.

---

### 9. Diagnostic Timer Enable/Disable

- **Enabled** in `AdcStartCB` after `WD_AI_ContReadMultiChannels` returns `NoError`:

  ```c
  SetCtrlAttribute(adcTabHandle, TABPANEL_2_ADC_TIMER_POLL, ATTR_INTERVAL, 0.5);
  SetCtrlAttribute(adcTabHandle, TABPANEL_2_ADC_TIMER_POLL, ATTR_ENABLED,  1);
  ```

- **Disabled** in `AdcStopCB` before button state is restored:

  ```c
  SetCtrlAttribute(adcTabHandle, TABPANEL_2_ADC_TIMER_POLL, ATTR_ENABLED, 0);
  ```

---

## Rejected Changes

| Proposed change | Reason rejected |
| --- | --- |
| Change trigger polarity to `WD_AI_TrgNegative` | Hardware provides a rectangular signal; both edges work. Positive-edge POST trigger is preferred. |
| Change `NumChans` in `WD_AI_ContScanChannels` from `1` to `2` | Per WD-DASK docs, this parameter is the *highest channel index* (0-based), not the count. `1` correctly scans CH0 and CH1. (The entire call was later replaced anyway.) |

---

## Current Status (2026-03-19)

Build is clean (no errors, no warnings). The application arms without error and the diagnostic timer fires. The next run will reveal which pipeline stage is failing based on the counter pattern in the status bar:

| Observed pattern | Implied failure |
| --- | --- |
| `Poll` climbing, `HR=0`, `FS=0` | `AsyncDblBufferHalfReady` never signals — DMA not running or wrong API |
| `HR` climbing, `Raw0=0x0000` | Half-ready fires but DMA buffer is empty — buffer setup issue |
| `HR` climbing, `Qd=0` | Data received but TSQ write failing (queue full or bad handle) |
| `Qd` climbing, `PTrg=0` | Consumer running but plot throttle never triggers — check `samplesPerChirp` |
| `PTrg` climbing, `PEntr=0` | `PostDeferredCall` posted but callback never runs — UI thread blocked |
| `PEntr` climbing, `PDone=0` | Crash inside `ADC_PlotFFT_Deferred` before completion counter |
| `PDone` climbing, no visible plots | `PlotY`/`PlotXY` called but graph not refreshing — graph handle or refresh attribute issue |

---

## 2026-03-20 — Diagnostic Results and Root Cause Identified

### Observed diagnostic output (first live test run)

```text
Poll:1898 HR:1 FS:1898 Qd:1 | PTrg:0 PEntr:0 PDone:0 | Raw0:0xFFFF
```

Interpretation:

- `Poll` climbing continuously — the poll thread loop is alive and running.
- `HR:1` — hardware delivered exactly **one** half-buffer ready event, then stopped.
- `FS:1898` — `fStop` is permanently asserted from that point onward (the value matches `Poll`, confirming it is set on every poll call after the hardware stopped).
- `Qd:1` — one buffer was successfully enqueued.
- `PTrg:0` — no plot was ever triggered (the `heartbeatCounter % 4` throttle prevented the single event from firing a plot).
- `Raw0:0xFFFF` — DMA transferred data (saturated ADC value, likely no RF signal connected at time of test, but DMA itself is working).

### Root cause 1 — `ReTrgCnt = 1` in `WD_AI_ContReadMultiChannels`

`WD_AI_ContReadMultiChannels` has a `ReTrgCnt` parameter that specifies the maximum number of trigger events the hardware will accept before it asserts `fStop` and halts DMA. The call in `AdcStartCB` was:

```c
WD_AI_ContReadMultiChannels(adcCard, ADC_NUM_CHANNELS, chans, adcBufId,
                             totalScansPerCh, 1, 1, ASYNCH_OP);
                                              ^  ^
                                  scanInterval  ReTrgCnt=1  <- STOP AFTER 1 TRIGGER
```

With `ReTrgCnt = 1`, the hardware accepted the first trigger, filled one half-buffer, then permanently stopped. This is why `FS` was permanently asserted and no further `halfReady` events occurred.

This is a **fundamental difference** from `WD_AI_ContReadChannel` (used in the working example `EXAMPLE_DBCODE_PostExtTrg.c`), which has no `ReTrgCnt` parameter and runs indefinitely until `WD_AI_AsyncClear` is called. The multi-channel variant requires this to be set explicitly for continuous operation.

**Fix:** Changed `ReTrgCnt` from `1` to `65535`:

```c
err = WD_AI_ContReadMultiChannels(adcCard, ADC_NUM_CHANNELS, chans, adcBufId1,
                                   totalScansPerCh, 1, 65535, ASYNCH_OP);
```

### Root cause 2 — `adcBufId` holds the second buffer's ID

Both `WD_AI_ContBufferSetup` calls wrote their result into the same `&adcBufId` variable:

```c
WD_AI_ContBufferSetup(adcCard, dmaBuffer1, halfBufferSize, &adcBufId);  // adcBufId = buf1 ID
WD_AI_ContBufferSetup(adcCard, dmaBuffer2, halfBufferSize, &adcBufId);  // adcBufId OVERWRITTEN = buf2 ID
WD_AI_ContReadMultiChannels(..., adcBufId, ...);                         // starts from buf2
```

The poll thread assumes `activeBuf == 0` means the hardware is filling `dmaBuffer1` first. If the hardware was told to start from `dmaBuffer2`'s ID, the first `halfReady` event would signal that `dmaBuffer2` was filled — but the software would read from `dmaBuffer1` (stale/uninitialised data).

**Fix:** Added a separate `adcBufId1` global to preserve the first buffer's ID:

```c
WD_AI_ContBufferSetup(adcCard, dmaBuffer1, halfBufferSize, &adcBufId1); // saved separately
WD_AI_ContBufferSetup(adcCard, dmaBuffer2, halfBufferSize, &adcBufId);
WD_AI_ContReadMultiChannels(..., adcBufId1, ...);                        // starts from buf1
```

### Expected diagnostic output after fixes

With `ReTrgCnt = 65535` and hardware confirmed working, the expected steady-state output is:

```text
Poll:NNNN HR:N FS:0 Qd:N | PTrg:N PEntr:N PDone:N | Raw0:0x~~~~
```

- `FS` should remain near 0 throughout the run.
- `HR`, `Qd`, `PTrg`, `PEntr`, `PDone` should all climb together at the chirp trigger rate.
- `Raw0` should reflect actual ADC mid-scale (~`0x7FFF` for 0 V input).

---

## 2026-03-20 — MAPsUpdate Branch: ADC Clean Slate

**Branch:** `MAPsUpdate`

The decision was made to rebuild the ADC acquisition pipeline from scratch using the updated MAPs drivers and example code for the PCI-9846H, rather than continuing to patch the existing implementation.

### What was removed from `DeviceControl_FullThreaded.c`

- **Threading:** `HardwarePollThreadFunction`, `DiskWriterThreadFunction_Optimized`, `Overrun_Deferred`, all `CmtXxx`/`CmtTSQ` variables and queue management state (`dmaQueue`, `producerThreadID`, `consumerThreadID`, `isAcquiring`, `queueMaxCapacity`, `BufferSlot`, etc.)
- **ADC acquisition:** `AdcRegisterCB`, `AdcConfigureCB`, `AdcStartCB`, `AdcStopCB`, `AdcSingleShotCB`, `AdcReleaseCB`, `AdcPollTimerCB` full implementations; all DMA buffer globals (`dmaBuffer1`, `dmaBuffer2`, `halfBufferSize`, `samplesPerChirp`, etc.); all diagnostic counters
- **FFT / plotting:** `ADC_PlotFFT_Deferred`, `ADC_ComputeFFT`, `FFT_Radix2`, `NextPow2`; all associated scratch arrays (`fftReal`, `fftImag`, `fftMagCH0/1`, `timeDataCH0/1`, `plotBuffer`, `rangeAxis`)
- **File saving/recording:** `ADC_SaveRawFile`, `ADC_RecordWriteHeader`, `ADC_RecordFinish`, `RadarFileHeader` struct, recording state globals
- **Includes:** `Wd-dask.h`, `<math.h>` (no longer required)
- **ADC cleanup** from `main()`

### What was kept

DDS, PSU, and Relay control code is fully intact and unchanged.

### Stub callbacks

All nine ADC tab callbacks required by the `.uir`/`.h` are present as minimal stubs that display `"ADC: Not yet implemented"` in the status field.

### Clean slate result

File reduced from 2111 lines to 829 lines. Clean compile baseline ready for new ADC implementation.

---

## 2026-03-20 — MAPsUpdate Branch: ADC Rebuild (double-buffer)

**Branch:** `MAPsUpdate`

### Architecture

Two save modes, selectable at runtime via different buttons:

| Button | Mode | Mechanism |
| --- | --- | --- |
| Start Acq | `SAVE_NONE` | Monitor only — acquire + plot, no file |
| Record | `SAVE_THREAD` | TSQ producer/consumer → raw U16 binary via `fwrite` thread |
| Save | `SAVE_TOFILE` | `WD_AI_ContScanChannelsToFile` + `WD_AI_AsyncDblBufferToFile` (driver-managed) |
| Single | `SAVE_NONE` | One trigger only (`reTrgCnt=1`), then hardware stops automatically |

### Key parameters

| Constant | Value | Meaning |
| --- | --- | --- |
| `HALF_BUF_SAMPLES` | 262144 (2^18) | U16 samples per half-buffer |
| `SCANS_PER_HALF` | 131072 | Simultaneous scan groups (2 ch) per half |
| `SAVE_QUEUE_CAP` | 32 | TSQ depth (32 × 512 KB = 16 MB) |
| `RETRIG_CNT_INF` | 65535 | Continuous re-trigger count |

### WD-DASK arm sequence

1. `WD_AI_ContBufferReset`
2. `WD_AI_AsyncDblBufferMode(card, 1)`
3. `WD_AI_ContBufferSetup` × 2 — **firstBufId** saved separately
4. `WD_AI_Trig_Config`: POST, ExtD, Positive, reTrgCnt=65535
5. `WD_AI_ContScanChannels(card, 1, firstBufId, SCANS_PER_HALF, 1, 1, ASYNCH_OP)`

Poll thread re-arms with `WD_AI_AsyncDblBufferHandled` (or `AsyncDblBufferToFile`).
`activeBuf ^= 1` tracks which half the hardware just filled.

### Diagnostic status line (every 0.5 s)

```text
Poll:NNNN HR:N FS:N | Sw:N Sd:N | PTrg:N PDn:N | OVR:N
```

`Poll` = poll loop iterations, `HR` = half-ready events, `FS` = fStop count,
`Sw` = buffers saved, `Sd` = save drops, `PTrg`/`PDn` = plot posted/done, `OVR` = DMA overrun.

### fStop handling

If `fStop` fires in the poll thread (reTrgCnt exhausted or hardware error), the thread sets
`isAcquiring = 0` and posts `ADC_StopDeferred` to the UI thread for cleanup.
User-initiated stop (`AdcStopCB`) calls `ADC_StopAcquisition` directly.
Both paths are idempotent (guarded by `pollThreadID` / `saveThreadID` checks).

### File naming

Auto-generated on Record/Save: `CerberusData_YYYYMMDD_HHMMSS.dat` (raw U16, no header).

### Rebuild result

File grows from 829 lines to 1478 lines. All nine ADC callbacks implemented.

---

## 2026-03-23 — Session 1: reTrgCnt confirmed; buffer reset attempts

### reTrgCnt = 1 is correct for all modes

Testing the Single Shot button (which uses `RETRIG_CNT_ONE = 1` in `WD_AI_Trig_Config`) produced
**continuous** data acquisition rather than stopping after one trigger. This confirms that
`reTrgCnt = 1` in `WD_AI_Trig_Config` does **not** limit the card to a single trigger event
when used with `WD_AI_ContScanChannels` in double-buffer mode.

This is a key distinction from the previous `WD_AI_ContReadMultiChannels` implementation, where
`reTrgCnt = 1` in that function's parameter list caused the hardware to assert `fStop` after one
event (confirmed by the `HR:1 FS:1898` diagnostic pattern). The parameter has different semantics
across these two API paths.

`RETRIG_CNT_INF` has been corrected from `65535` to `1`. Both continuous and single-shot constants
are now `1` — the distinction is retained in code for clarity only.

### Buffer reset attempts for successive runs

The `-201 ErrorConfigIoctl` on stop/restart was caused by lingering DMA buffer registrations from
the previous run. Two `WD_AI_ContBufferReset` calls were added:

1. **In `AdcConfigureCB`** — after freeing DMA buffers, before calibration and `AI_Config`.
   Clears any registrations that survived a previous session.

2. **In `ADC_StartCommon`** — at the very top, preceded by `WD_AI_AsyncDblBufferMode(card, 0)`
   to disable double-buffer mode before resetting. This is the safe sequence: disable → reset →
   re-enable (`AsyncDblBufferMode(1)`) → `ContBufferSetup × 2`.

These buffer reset calls were later found to break SingleShot and were reverted — see Session 2.

### Auto-calibration added to Configure (later removed — see Session 2)

`WD_AD_Auto_Calibration_ALL(adcCard)` was called at the start of `AdcConfigureCB`, after
the buffer reset and before `WD_AI_Config`. Caused a **FATAL RUN-TIME ERROR** — see Session 2.

---

## 2026-03-23 — Session 2: Buffer allocation root cause; DDS ring; UI fixes

### State when SingleShot worked correctly

**This is the reference "last known good" state.** SingleShot produced correct continuous real-time
plots (no saving, as intended) on the **first run after a fresh Configure**, with the following
conditions:

| Property | Value at working point |
| --- | --- |
| Buffer allocator | `WD_Buffer_Alloc` (not yet replaced) |
| Buffer resets | **None** — `WD_AI_ContBufferReset` had been removed entirely |
| `WD_AI_Trig_Config` order | Called **before** `WD_AI_AsyncDblBufferMode` |
| Channel config | Per-channel loop `WD_AI_CH_Config(card, i, range)` |
| `reTrgCnt` in `WD_AI_Trig_Config` | `1` — confirmed to give continuous acquisition |
| Auto-calibration | None at this point |
| Run count since Configure | **First run** — buffers were freshly allocated, never previously registered |

**Why it only worked once:** `WD_AI_ContBufferSetup` causes the WD-DASK driver to pin (lock) the
buffer pages for DMA at the kernel level. `WD_AI_AsyncClear` clears acquisition state but does
**not** unpin those pages. On the second run, `ContBufferSetup` was called again with the same
buffer addresses, which were still registered in the driver — returning `-201 ErrorConfigIoctl`.
Calling `free` (or `WD_Buffer_Free`) on still-pinned pages then caused a crash on release/exit.

---

### -201 ErrorConfigIoctl — root cause identified and fixed

**Previous attempted fixes that did not work:**

- `WD_AI_ContBufferReset` — broke SingleShot, reverted
- `WD_AI_AsyncDblBufferMode(0)` before reset — same result
- `WD_Buffer_Alloc` → `malloc` swap — crash on `free` of pinned pages worsened

**Actual root cause (from P98x6 `DbfTrig.c` reference):**

The P98x6 sample never uses `WD_Buffer_Alloc`. It uses:

```c
hMem = GlobalAlloc(GMEM_FIXED | GMEM_ZEROINIT, data_size * 2);
ai_buf = (U16 *)GlobalLock(hMem);
GlobalFix(hMem);
```

And critically, it **frees and reallocates both buffers on every IDC_START**:

```c
GlobalUnfix(hMem);  GlobalUnlock(hMem);  GlobalFree(hMem);
/* ... then GlobalAlloc + GlobalFix fresh ... */
```

This means `ContBufferSetup` always receives virgin (never-previously-registered) addresses, so
the driver never sees a double-registration.

**Fix applied:**

- `ADC_AllocBuffer(HGLOBAL*, U16**)` helper: `GlobalAlloc(GMEM_FIXED|GMEM_ZEROINIT) + GlobalLock + GlobalFix`
- `ADC_FreeBuffer(HGLOBAL*, U16**)` helper: `GlobalUnfix + GlobalUnlock + GlobalFree`
- `HGLOBAL hDmaBuffer1, hDmaBuffer2` globals added to store handles
- Buffer allocation **moved out of `AdcConfigureCB`** entirely
- `ADC_StartCommon` now calls `ADC_FreeBuffer` then `ADC_AllocBuffer` on **every start**,
  guaranteeing fresh unregistered memory for each `ContBufferSetup` call
- `AdcReleaseCB` and `ADC_Cleanup` updated to use `ADC_FreeBuffer`

---

### `WD_AI_CH_Config` changed to `All_Channels`

Per-channel loop replaced with a single call:

```c
WD_AI_CH_Config(adcCard, (U16)All_Channels, adcRangeTable[range]);
```

This matches the P98x6 `DbfTrig.c` reference exactly. The impedance `CH_ChangeParam` loop is
retained (still per-channel) since `All_Channels` is not documented for that function.

---

### Auto-calibration (`WD_AD_Auto_Calibration_ALL`) — permanently removed

History of attempts:

1. Added to `AdcConfigureCB` → **FATAL RUN-TIME ERROR** (unknown fault, immediate crash)
2. Moved to `AdcRegisterCB` → **UI thread freeze**, window could not close

Root cause: the function blocks the CVI UI thread indefinitely on this card/driver combination.
Removed entirely. Can be re-introduced only via a dedicated background thread with a separate
button — never on the UI thread.

---

### Release button fix

`AdcRegisterCB` now undims `TABPANEL_2_ADC_BTN_RELEASE` after successful registration, alongside
`TABPANEL_2_ADC_BTN_CONFIGURE`. Previously the Release button had no path to become accessible.

---

### DDS ring control (partial — UIR edit still required)

**Code changes:**

- `ScanVISAResources`: labels `ASRL19::INSTR` as `AD9914 DDS Controller`; populates DDS ring
  (guarded by `#if TABPANEL_DDS_RING_RESOURCE != TABPANEL_DDS_STR_COM_PORT`)
- `DdsConnectCB`: reads ring selection → looks up `resourceList[idx]` → validates ASRL prefix →
  parses COM number via `strtol` → builds `\\.\COMxx` path. No longer reads typed text.
- `#ifndef TABPANEL_DDS_RING_RESOURCE` fallback maps to `TABPANEL_DDS_STR_COM_PORT` (ID 2) so
  code compiles before UIR is updated.

**UIR edit still required:**
In CVI UIR Editor on the DDS tab panel: delete the string control (`DDS_STR_COM_PORT`, ID 2),
add a Ring control named `DDS_RING_RESOURCE`. CVI will regenerate the `.h` with the new constant.
Once done the `#ifndef` fallback is inactive and the ring is live.

**Important:** `ClearListCtrl` / `InsertListItem` on a string control is undefined in CVI and can
corrupt internal UI state. The calls are wrapped in a compile-time guard so they are suppressed
until the UIR is actually updated.

---

### PSU current meters

Two scale (meter) controls `PSU_TAB_PSU_CH1_METER` (ID 32) and `PSU_TAB_PSU_CH2_METER` (ID 31)
added to the PSU tab via UIR editor. Code updated:

- `PSU_ReadMeasurements`: each meter is driven by its channel's `MEASure:CURRent?` result
- Disconnect path: both meters zeroed alongside the other readback numerics

---

### Current ADC arm sequence (confirmed working architecture)

```c
/* AdcConfigureCB: */
WD_AI_Config(card, WD_ExtTimeBase, 0, 0, 0, 0)
WD_AI_CH_Config(card, All_Channels, range)
adcConfigured = 1

/* ADC_StartCommon (every start): */
WD_AI_Trig_Config(card, POST, ExtD, Positive, 0,0,0,0,0, reTrgCnt=1)
WD_AI_AsyncDblBufferMode(card, 1)
ADC_FreeBuffer + ADC_AllocBuffer   /* fresh GlobalAlloc+GlobalFix each run */
WD_AI_ContBufferSetup(buf1)  ->  firstBufId
WD_AI_ContBufferSetup(buf2)  ->  secondBufId
WD_AI_ContScanChannels(card, 1, firstBufId, SCANS_PER_HALF, 1, 1, ASYNCH_OP)

/* Poll thread: */
WD_AI_AsyncDblBufferHalfReady  ->  halfReady / fStop
WD_AI_AsyncDblBufferHandled (or AsyncDblBufferToFile)
activeBuf ^= 1

/* Stop: */
isAcquiring = 0
WD_AI_AsyncClear
ADC_FreeBuffer   /* buffers released — fresh ones allocated at next Start */
```

---

## 2026-03-23 — Session 3: Data saving, sidecar headers, buffer scaling, FFT

### Confirmed: acquisition and saving functional

Both `SAVE_THREAD` (TSQ → fwrite) and `SAVE_TOFILE` (driver-managed) paths successfully record
binary data to disk. Saved `.dat` files verified by loading in MATLAB — data is 16-bit signed
(confirmed: PCI-9846H is a **16-bit** ADC, not 14-bit as some documentation suggested).

### Bug fix: `.dat.dat` filename in SAVE_TOFILE mode

The ADLINK driver `WD_AI_ContScanChannelsToFile` **automatically appends `.dat`** to the filename
it receives (confirmed by examining the ADLINK sample code `CAIDbfFileDiv/CAIDbF.c`, which passes
`file_name="dbfai"` and prints the result as `'dbfai.dat'`).

`ADC_GenerateFilename` was producing `CerberusData_YYYYMMDD_HHMMSS.dat`, so the driver created
files named `CerberusData_YYYYMMDD_HHMMSS.dat.dat`.

**Fix:** `ADC_GenerateFilename` now produces the base name without extension
(`CerberusData_YYYYMMDD_HHMMSS`). The `SAVE_THREAD` path in `AdcRecordCB` explicitly appends
`.dat` for its `fopen` call. The `SAVE_TOFILE` path passes the base name to the driver, which
appends `.dat` itself.

### Sidecar header file (`.hdr`)

New function `ADC_WriteSidecarHeader(baseName, mode)` writes a plain-text INI-style `.hdr` file
alongside each `.dat` recording. Called from both `AdcRecordCB` and `AdcSaveCB`.

Sections and fields captured:

| Section | Fields |
| --- | --- |
| `[Timestamp]` | Date, Time |
| `[SaveMode]` | SAVE_THREAD or SAVE_TOFILE |
| `[ADC]` | NumChannels, HalfBufSamples, ScansPerHalf, SampsPerTrig, RangeIndex, VoltageRange_V, TimebaseIndex, ImpedanceIndex, DataType |
| `[DDS_Settings]` | ChirpMode, ClockMHz, StartFreqMHz, StopFreqMHz, RequestedPeriod_us, CW_FreqMHz |
| `[DDS_Actual]` | ActualStartMHz, ActualStopMHz, ActualPeriod_us |
| `[Timing]` | HMC432_Divider, ProgDivider, SampsPerChirp, SyncClkMHz, ADC_ClkMHz, TrigFreqHz, DRCTRL_Period_us, ChirpSteps, CalcChirpPeriod_us, DeadTime_us, DeadSamples, MinProgDivider |

### Half-buffer size increased to 2^20 (1,048,576 samples)

**Motivation:** At ~4.56 MS/s per channel (2 channels), the previous 2^18 buffer filled in only
28.8 ms — very tight for the poll thread to service before overrun. The new 2^20 buffer fills
in ~115 ms, giving comfortable headroom.

| Constant | Old value | New value |
| --- | --- | --- |
| `HALF_BUF_SAMPLES` | 262,144 (2^18, 512 KB) | 1,048,576 (2^20, 2 MB) |
| `SCANS_PER_HALF` | 131,072 | 524,288 |
| `SAVE_QUEUE_CAP` | 32 (16 MB total) | 32 (64 MB total) |

`PLOT_SCANS_MAX` remains at 131,072 — decoupled from the DMA buffer size. Only one chirp
(typically 128–1024 samples) is copied for plotting; the rest of the half-buffer is saved only.

**Memory budget** (at 2^20, 256 MB system limit):

| Resource | Size |
| --- | --- |
| DMA buffers × 2 | 4 MB |
| Save queue (32 × 2 MB) | 64 MB |
| Plot + FFT static arrays | ~7 MB |
| DiskSaveThread slot buffer | 2 MB |
| **Total** | **~77 MB** |

### Data-drop mitigations

1. **`TSQ_WRITE_MS` reduced from 200 to 50 ms** — if the save queue is full, the poll thread
   blocks for at most 50 ms before dropping the buffer and continuing. This prevents a stalled
   disk writer from causing the poll thread to miss the next DMA half-ready event (which would
   trigger a DMA overrun).

2. **`Sleep(0)` yield added to poll thread** — when `halfReady` is false, the thread now yields
   the CPU via `Sleep(0)` instead of busy-spinning. This prevents the poll thread from consuming
   100% of one core and starving the disk-writer thread of CPU time.

### FFT with Hann window added to `ADC_PlotDeferred`

**Implementation:**
- Added `#include <analysis.h>` — uses CVI's built-in `HanWin()` and `FFT()` (from `advanlys.h`)
- After deinterleaving ch0/ch1 voltage data, the FFT is computed for each channel:
  1. Copy voltage data to `fftReal[]`, zero `fftImag[]`
  2. Apply `HanWin(fftReal, N)` (Hann window, in-place)
  3. `FFT(fftReal, fftImag, N)` — in-place, separate real/imaginary arrays
  4. Compute magnitude in dB: `20 · log10(√(re² + im²) + 1e-20)`
  5. Plot positive-frequency half (N/2 bins) on `TABPANEL_2_ADC_GRAPH_FFT`
- Both channels plotted: CH0 in red, CH1 in blue (same as time-domain graph)
- FFT only runs when `plotScans` is a power of 2 (≥ 4); silently skips otherwise
- No zero-padding — FFT length equals `plotScans` (one chirp)
- The CVI analysis library is included automatically via "Full Runtime Support" in the
  project settings — no `.lib` file addition required

### Default voltage range corrected

The out-of-bounds range clamp in `AdcConfigureCB` previously defaulted to index 1 (±1 V).
Changed to default to index 0 (±5 V), which is the standard operating range for this system.

---

## 2026-03-23 — Session 4: Calibration, dimming, PSU timer, master view plan

### ADC auto-calibration — background thread implementation

`WD_AD_Auto_Calibration_ALL` previously caused fatal errors / UI hangs when called on the UI
thread. Now implemented as a background thread:

- `AdcCalThread` — scheduled via `CmtScheduleThreadPoolFunction`, calls
  `WD_AD_Auto_Calibration_ALL(adcCard)`, posts result via `PostDeferredCall`
- `ADC_CalDoneDeferred` — runs on UI thread, reports success/failure, restores button states
- `AdcCalibrateCB` — UI callback, dims Configure/Release/Calibrate while running, guards
  against concurrent calibration or acquisition

**UIR change required:** Add a command button to the ADC tab panel named `ADC_BTN_CALIBRATE`
with callback `AdcCalibrateCB`. The code uses `#ifdef TABPANEL_2_ADC_BTN_CALIBRATE` guards so
it compiles with or without the button present. Position it near Register/Configure.

### Improved button dimming state machine

**During acquisition** (`ADC_StartCommon` success):
- All start/record/save/single buttons → dimmed
- Stop → enabled
- **Configure → dimmed** (new)
- **Release → dimmed** (new)
- **Calibrate → dimmed** (new, via `#ifdef`)

**On stop** (`ADC_StopAcquisition`):
- Stop → dimmed
- All start/record/save/single buttons → enabled
- **Configure → enabled** (new)
- **Release → enabled** (new)
- **Calibrate → enabled** (new, via `#ifdef`)

**On release** (`AdcReleaseCB`):
- Previously would auto-stop if acquiring; now **returns with an error message** instead,
  since the Release button is dimmed during acquisition. The `isAcquiring` guard is retained
  as a safety net.

### PSU readback timer — disabled during acquisition

The PSU `ReadbackTimerCB` calls `PSU_ReadMeasurements` which performs **6 blocking VISA I/O
operations** (viWrite + viRead for voltage, current, and power on each of 2 channels) on the
UI thread. While these calls are blocking, the CVI event loop cannot process `PostDeferredCall`
requests from the ADC poll/plot threads. This could delay plot updates or cause the deferred
call queue to back up.

**Fix:**
- `ADC_StartCommon`: disables `PSU_TAB_PSU_TIMER_READBACK` before returning
- `ADC_StopAcquisition`: re-enables the timer if `psuSession != VI_NULL`

This means PSU voltage/current readback pauses while the ADC is acquiring, and resumes
automatically when acquisition stops.

---

### Master View Tab — Design Plan

A new tab (index 0, shifting existing tabs to 1–4) providing a single-pane operational overview
of the entire radar system. All controls delegate to existing subsystem functions — no duplicate
logic.

#### UIR Changes Required (CVI UIR Editor)

1. Add a new tab page at index 0 titled "Master" to `MAIN_PANEL_MAIN_PANEL_TAB`
2. Shift existing tab indices: PSU→1, Relay→2, DDS→3, ADC→4
3. Update `GetPanelHandleFromTabPage` calls in `main()` accordingly
4. Add the following controls to the Master tab panel:

#### Proposed Control Layout

```
+----------------------------------------------------------------------+
|  MASTER VIEW                                                          |
+----------------------------------------------------------------------+
|                                                                       |
|  [PSU STATUS]                          [SYSTEM CONTROLS]              |
|  CH1: ####V  ####A  [meter]           [  ALL PSU ON/OFF  ]           |
|  CH2: ####V  ####A  [meter]           [  TX RELAY (RLY1)  ] [LED]   |
|                                        [  DDS START/STOP   ] [LED]   |
|                                                                       |
|  [STATUS MESSAGES]                                                    |
|  DDS: Triggered sweep 500-1000 MHz                                   |
|  ADC: Acquiring (monitor)                                             |
|                                                                       |
|  [ADC CONTROLS]                                                       |
|  [ START ACQ ]  [ START RECORD ]  [  STOP  ]                         |
|                                                                       |
|  [FFT / RANGE PLOT]                                                   |
|  +----------------------------------------------------------------+  |
|  |                                                                |  |
|  |  (graph — same FFT magnitude data as ADC tab, replotted here) |  |
|  |                                                                |  |
|  +----------------------------------------------------------------+  |
+----------------------------------------------------------------------+
```

#### Controls to Add in UIR Editor

| Control type | Suggested constant name | Purpose |
| --- | --- | --- |
| Numeric (indicator) | `MASTER_NUM_CH1_VOLT` | CH1 voltage readback |
| Numeric (indicator) | `MASTER_NUM_CH1_CURR` | CH1 current readback |
| Numeric (indicator) | `MASTER_NUM_CH2_VOLT` | CH2 voltage readback |
| Numeric (indicator) | `MASTER_NUM_CH2_CURR` | CH2 current readback |
| Scale (meter) | `MASTER_CH1_METER` | CH1 current meter (visual) |
| Scale (meter) | `MASTER_CH2_METER` | CH2 current meter (visual) |
| Command button | `MASTER_BTN_PSU_ALL` | Toggle all PSU outputs, callback: `MasterPsuAllCB` |
| Command button | `MASTER_BTN_TX_RELAY` | Toggle TX relay (relay 1), callback: `MasterTxRelayCB` |
| LED | `MASTER_LED_TX_RELAY` | TX relay state indicator |
| Command button | `MASTER_BTN_DDS` | Start/stop DDS chirp, callback: `MasterDdsCB` |
| LED | `MASTER_LED_DDS` | DDS active indicator |
| Command button | `MASTER_BTN_ACQ_START` | Start ADC monitor, callback: `MasterAcqStartCB` |
| Command button | `MASTER_BTN_ACQ_RECORD` | Start ADC recording, callback: `MasterAcqRecordCB` |
| Command button | `MASTER_BTN_ACQ_STOP` | Stop ADC, callback: `MasterAcqStopCB` |
| Graph | `MASTER_GRAPH_FFT` | Range-domain FFT plot (mirrors ADC FFT graph) |
| Text message | `MASTER_MSG_DDS_STATUS` | DDS status string |
| Text message | `MASTER_MSG_ADC_STATUS` | ADC status string |
| Timer | `MASTER_TIMER_UPDATE` | Periodic refresh (~500 ms), callback: `MasterUpdateTimerCB` |

#### Code Architecture

- **`MasterUpdateTimerCB`**: Reads PSU values from the existing readback numerics on the PSU tab
  (via `GetCtrlVal` on `PSU_TAB_PSU_NUM_CH1_VOLT_READ` etc.) and mirrors them to the Master tab
  numerics and meters. Also reads relay/DDS state from existing globals (`relay1State`,
  `ddsSweepActive`) and updates LEDs. This avoids duplicate VISA calls — the master tab is a
  **consumer** of data already acquired by the subsystem tabs.

- **`MasterPsuAllCB`**: Calls the same logic as `PsuAllOutCB` — toggles all outputs.

- **`MasterTxRelayCB`**: Calls the same logic as `Rl1CB` — toggles relay 1 (TX).

- **`MasterDdsCB`**: If `ddsSweepActive`, calls `dds_powerdown()` (like `DdsStopCB`).
  Otherwise calls the DDS start sequence (like `DdsStartCB`), reading parameters from the
  DDS tab controls.

- **`MasterAcqStartCB` / `MasterAcqRecordCB` / `MasterAcqStopCB`**: Thin wrappers that call
  `ADC_StartCommon` / file setup / `ADC_StopAcquisition` respectively, mirroring the ADC tab
  callbacks. Button dimming on the Master tab must be synchronized with the ADC tab.

- **FFT plot mirroring**: In `ADC_PlotDeferred`, after plotting to `TABPANEL_2_ADC_GRAPH_FFT`,
  also plot the same data to `MASTER_GRAPH_FFT` (guarded by `#ifdef MASTER_GRAPH_FFT`). This
  ensures both tabs show identical data with zero extra computation.

#### Key Design Principles

1. **No duplicate hardware I/O** — master tab reads from existing globals / UI controls
2. **Single source of truth** — all state changes go through the existing subsystem functions
3. **Synchronized dimming** — master buttons mirror the ADC tab dimming state machine
4. **Timer gated** — `MASTER_TIMER_UPDATE` disabled when PSU is not connected (same pattern
   as `PSU_TIMER_READBACK`)

---

## 2026-03-23 — Session 5: -201 fix, UIR rename cleanup

### UIR rename confirmed — .c file already updated

The user renamed all tab panel controls via the CVI UIR Editor:
- `TABPANEL_2_ADC_*` → `ADC_TAB_ADC_*`
- `TABPANEL_DDS_*` → `DDS_TAB_DDS_*`
- New control: `ADC_TAB_ADC_BTN_CALIBRATE` (ID 22) with callback `AdcCalibrateCB`
- `DDS_TAB_DDS_RING_RESOURCE` (ID 31) now defined natively in the `.h`

The `.c` file had already been updated to use the new names in a prior session. Verified no
remaining `TABPANEL_2_ADC_*` or `TABPANEL_DDS_*` references exist.

### -201 ErrorConfigIoctl — root cause and fix

**Problem:** Second acquisition start after stopping (Register→Configure→Acquire→Stop→Start)
returned `-201 ErrorConfigIoctl` from `WD_AI_ContBufferSetup`. Same error occurred after
calibration (Register→Configure→Calibrate→Configure→Start).

**Root cause:** `WD_AI_ContBufferReset` was **never called** anywhere in the code. After
`WD_AI_AsyncClear` stops acquisition, the driver's internal buffer registration table retains
the old entries. Freeing the user-space memory via `GlobalFree` does not release the driver-side
page-pin registrations. When `ADC_StartCommon` allocates new buffers and calls
`WD_AI_ContBufferSetup`, the driver rejects the call because it believes buffers are already
registered.

For calibration, `WD_AD_Auto_Calibration_ALL` may internally register DMA buffers for its own
use. Without a subsequent `ContBufferReset`, those registrations persist and block the next
`ContBufferSetup`.

**Fix — three `WD_AI_ContBufferReset` calls added:**

1. **`ADC_StopAcquisition`** — after `WD_AI_AsyncClear`, disable double-buffer mode with
   `WD_AI_AsyncDblBufferMode(0)`, then `WD_AI_ContBufferReset` to release all driver-side
   buffer registrations. User-space buffers freed afterwards via `ADC_FreeBuffer`.

2. **`ADC_StartCommon`** — safety `WD_AI_ContBufferReset` before `AsyncDblBufferMode(1)` and
   buffer allocation. Catches any stale registrations from calibration or incomplete teardowns.

3. **`AdcCalThread`** — `WD_AI_ContBufferReset` immediately after
   `WD_AD_Auto_Calibration_ALL` returns. Cleans up any internal buffer registrations the
   calibration function may have created.

**Updated teardown sequence:**
```
isAcquiring = 0
Wait for poll/save threads
WD_AI_AsyncClear
WD_AI_AsyncDblBufferMode(card, 0)    /* NEW */
WD_AI_ContBufferReset(card)          /* NEW */
ADC_FreeBuffer × 2                   /* MOVED from StartCommon to here */
```

**Updated startup sequence:**
```
WD_AI_Trig_Config
WD_AI_ContBufferReset(card)          /* NEW — safety net */
WD_AI_AsyncDblBufferMode(card, 1)
ADC_FreeBuffer × 2                   /* no-op if already freed by Stop */
ADC_AllocBuffer × 2
WD_AI_ContBufferSetup × 2
WD_AI_ContScanChannels
```

### Obsolete preprocessor guards removed

- Removed `#ifndef DDS_TAB_DDS_RING_RESOURCE` / `DDS_TAB_DDS_STR_COM_PORT` fallback
  (now defined natively in `.h`)
- Removed all `#if DDS_TAB_DDS_RING_RESOURCE != DDS_TAB_DDS_STR_COM_PORT` guards around
  `ClearListCtrl`/`InsertListItem` calls — DDS ring is always populated now
- Removed all four `#ifdef ADC_TAB_ADC_BTN_CALIBRATE` guards — the constant is now always
  defined in the `.h` file
- Added Calibrate button dimming to `ADC_StartCommon` and `ADC_StopAcquisition` (was only
  in calibrate-specific callbacks before)
