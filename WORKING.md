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

### ADC calibration lockup — diagnosed and fixed

**Problem:** `WD_AD_Auto_Calibration_ALL` in `AdcCalThread` caused UI lockup / memory corruption.

**Root cause (from reviewing `98X6CAL.CPP` in `Digitizer_common\CAutoCal`):**
All ADLINK calibration examples follow the same pattern:
```c
card = WD_Register_Card(card_type, card_num);
// NO AI_Config, NO CH_Config, NO buffers — nothing
err = WD_AD_Auto_Calibration_ALL(card);
WD_Release_Card(card);
```
Calibration is a **standalone operation** on a freshly registered card. The previous code called
`WD_AI_ContBufferReset` inside `AdcCalThread` immediately after calibration — but no buffers
were registered at that point. Calling `ContBufferReset` with no registered buffers likely
crashed the driver, corrupting memory and freezing the UI.

**Fixes applied:**

1. **Removed `WD_AI_ContBufferReset` from `AdcCalThread`** — no buffers exist at calibration
   time. The safety `ContBufferReset` at the top of `ADC_StartCommon` handles any stale state
   that calibration may leave behind.

2. **`ADC_CalDoneDeferred` now sets `adcConfigured = 0`** — calibration writes new ADC/DAC
   constants, invalidating any prior `AI_Config`/`CH_Config` state. User must re-Configure
   after calibration. Status message updated to say "please re-Configure".

3. **All acquisition buttons dimmed after calibration** — Start/Record/Save/Single are dimmed
   in both `AdcCalibrateCB` (while running) and `ADC_CalDoneDeferred` (after completion, since
   `adcConfigured` is now 0). Only Configure/Release/Calibrate are re-enabled.

**Correct workflow:** Register → Calibrate → Configure → Acquire

---

## Implementation Plan: Range-Doppler & RTI Processing Tab

**Date:** 2026-03-25
**Status:** Planned (not yet implemented)
**Reference:** `Blunderbuss_code_DB1.c` — BB24 24 GHz FMCW radar system

### Goal

Add a new tab page (tab index 5, after ADC) containing four 2D intensity plots:
1. **LRX Range-Doppler** — range bins × Doppler bins (one frame per CPI)
2. **URX Range-Doppler** — same for CH1
3. **LRX Range-Time Intensity (RTI)** — range bins × time (scrolling, one column per CPI)
4. **URX Range-Time Intensity (RTI)** — same for CH1

Plus UI controls: CPI chirp count ring, slow-time window selection, colour map, dB floor/ceiling.

---

### Background Theory

In FMCW radar processing, a **Coherent Processing Interval (CPI)** is a block of N consecutive
chirps processed together. Each chirp produces one fast-time FFT spectrum (range profile).
Stacking N chirps and performing a second FFT across the slow-time (chirp-to-chirp) dimension
for each range bin yields a **Range-Doppler map**, revealing target velocity alongside range.

The **RTI plot** is simpler: each CPI contributes a single column of range-bin powers, appended
to a scrolling 2D image that shows how the range profile evolves over time.

---

### Architecture — Data Flow

The current acquisition pipeline is:

```
DMA Half-Buffer (poll thread)
     ├── TSQ → Save thread (disk write)
     └── memcpy → plotBuffer → PostDeferredCall → ADC_PlotDeferred (single chirp FFT)
```

The new processing path adds a **CPI accumulator** that collects N chirps before triggering
a batch 2D processing step:

```
DMA Half-Buffer (poll thread)
     ├── TSQ → Save thread (disk write)
     ├── memcpy → plotBuffer → PostDeferredCall → ADC_PlotDeferred (existing single-chirp)
     └── memcpy → CPI accumulator (circular)
                    │
                    ▼  (when N chirps accumulated)
              PostDeferredCall → RDP_ProcessDeferred (UI thread)
                    │
                    ├── Fast-time FFT × N chirps → complex spectra [N][rangeBins]
                    ├── Transpose to [rangeBins][N]
                    ├── Slow-time FFT per range bin → Range-Doppler [rangeBins][N/2]
                    ├── Magnitude + dB conversion
                    ├── Append power column to RTI scrolling buffer
                    └── PlotScaledIntensity × 4 graphs
```

**Alternative (higher performance):** Use a dedicated processing thread (like BB24's
`Data_Processing_Thread`) with its own TSQ. The poll thread writes each half-buffer into
a CPI TSQ; the processing thread reads N items, processes, and posts plot results via
`PostDeferredCall`. This avoids blocking the UI thread during the compute-heavy FFT stage.

---

### Step-by-Step Implementation Plan

#### Step 1: UIR Tab Creation (CVI User Interface Editor)

Create a new tab page "Range-Doppler" (index 5) in the main tab control. Add:

| Control | Type | Constant Name | Notes |
|---|---|---|---|
| LRX Range-Doppler | Graph (2D) | `RDP_TAB_GRAPH_RD_LRX` | For `PlotScaledIntensity` |
| URX Range-Doppler | Graph (2D) | `RDP_TAB_GRAPH_RD_URX` | |
| LRX RTI | Graph (2D) | `RDP_TAB_GRAPH_RTI_LRX` | Scrolling range-time |
| URX RTI | Graph (2D) | `RDP_TAB_GRAPH_RTI_URX` | |
| CPI Chirps | Ring | `RDP_TAB_RING_CPI` | Items: 64, 128, 256, 512, 1024, 2048 |
| Slow-Time Window | Ring | `RDP_TAB_RING_SLOW_WIN` | Hann, Hamming, BH, Rectangular |
| dB Floor | Numeric | `RDP_TAB_NUM_DB_FLOOR` | Default: -80 |
| dB Ceiling | Numeric | `RDP_TAB_NUM_DB_CEIL` | Default: 0 |
| Colour Map | Ring | `RDP_TAB_RING_COLORMAP` | Jet, Hot, Grayscale |
| Enable toggle | LED button | `RDP_TAB_BTN_ENABLE` | Enables/disables CPI collection |
| Status | Text message | `RDP_TAB_MSG_STATUS` | |

Regenerate `DeviceControl_FullThreaded.h` after editing.

#### Step 2: Globals and Memory Management (~60 lines)

```c
/* --- Range-Doppler / RTI globals --- */
static int     rdpTabHandle;                    /* tab panel handle                */
static int     rdpEnabled       = 0;            /* collection active               */
static U32     rdpCpiSize       = 256;          /* chirps per CPI (from ring)      */
static U32     rdpRangeBins     = 0;            /* = plotScans / 2 (half-spectrum)  */
static U32     rdpChirpCount    = 0;            /* chirps accumulated so far        */

/* CPI accumulator — stores deinterleaved voltage for N chirps × 2 channels.
   Layout: cpiBuffer[chirp * plotScans * 2 + sample], same as DMA interleave.
   Allocated when CPI size or plotScans changes. */
static double *cpiAccum0        = NULL;         /* CH0 voltage, [cpiSize][plotScans] */
static double *cpiAccum1        = NULL;         /* CH1 voltage, [cpiSize][plotScans] */
static U32     cpiAccumSize     = 0;            /* current allocation               */

/* Processing output arrays */
static double *rdResult0        = NULL;         /* Range-Doppler CH0 [rangeBins][cpiSize/2] */
static double *rdResult1        = NULL;         /* Range-Doppler CH1                        */

/* RTI scrolling buffers — [rangeBins][RTI_HISTORY_LEN] */
#define RTI_HISTORY_LEN  256                    /* columns of history               */
static double *rtiBuffer0       = NULL;
static double *rtiBuffer1       = NULL;

/* Slow-time FFT resources */
static PFFTTable   rdpSlowTable    = NULL;
static U32         rdpSlowTableN   = 0;
static NIComplexNumber *rdpSlowWork = NULL;     /* [cpiSize/2] temp complex array   */
static NIComplexNumber *rdpSlowOut  = NULL;
```

Add `RDP_Cleanup()` to free all above, called from `ADC_Cleanup()`.

Add `RDP_EnsureBuffers(U32 rangeBins, U32 cpiSize)` to (re)allocate when parameters change.

#### Step 3: CPI Accumulation from Poll Thread (~40 lines)

In `HardwarePollThread`, after the existing plot trigger, add:

```c
if (rdpEnabled && rdpChirpCount < rdpCpiSize)
{
    /* Deinterleave one chirp into the CPI accumulator */
    U32 s, off = rdpChirpCount * plotScans;
    double sc = adcVoltScale / 32768.0;
    for (s = 0; s < plotScans; s++)
    {
        cpiAccum0[off + s] = ((double)src[s * 2    ] - 32768.0) * sc;
        cpiAccum1[off + s] = ((double)src[s * 2 + 1] - 32768.0) * sc;
    }
    rdpChirpCount++;

    if (rdpChirpCount >= rdpCpiSize)
    {
        rdpChirpCount = 0;
        PostDeferredCall ((DeferredCallbackPtr)RDP_ProcessDeferred, NULL);
    }
}
```

**Note:** This deinterleaves and converts in the poll thread, which adds some latency per
half-buffer. If this becomes a bottleneck, move to a separate thread with a TSQ. The
deinterleave loop for 256–2048 chirps × 256–4096 samples per chirp is ~0.5–4 M iterations
per CPI — fast enough for the ~10–100 ms CPI periods typical at 1–10 MHz ADC clock.

#### Step 4: 2D Processing (RDP_ProcessDeferred) (~150 lines)

This runs on the UI thread via `PostDeferredCall`.

```
For each channel (CH0, CH1):

    1. FAST-TIME FFT (per chirp)
       for chirp = 0 .. cpiSize-1:
           copy cpiAccum[chirp * plotScans .. (chirp+1)*plotScans - 1]
           apply fast-time window (same as existing ADC_ApplyWindow)
           zero-pad if desired (use existing padFactor)
           FFTEx → complex output [paddedN]
           store complex result: fastRe[chirp][bin], fastIm[chirp][bin]
               (only positive-frequency half: bins 0 .. rangeBins-1)

    2. TRANSPOSE + SLOW-TIME FFT (per range bin)
       for bin = 0 .. rangeBins-1:
           extract column: slowWork[k].real = fastRe[k][bin], .imaginary = fastIm[k][bin]
               for k = 0 .. cpiSize-1
           apply slow-time window (CxHanWin, CxBlkHarrisWin, etc.)
           CxFFTEx(slowWork, cpiSize, cpiSize, rdpSlowTable, TRUE, slowOut)
           for k = 0 .. cpiSize/2 - 1:
               re = slowOut[k].real; im = slowOut[k].imaginary
               rdResult[bin * (cpiSize/2) + k] = 20*log10(sqrt(re*re + im*im) + 1e-20)
                                                  + correction_dB

    3. RTI UPDATE
       shift rtiBuffer left by one column (memmove)
       for bin = 0 .. rangeBins-1:
           rtiBuffer[bin * RTI_HISTORY_LEN + (RTI_HISTORY_LEN-1)]
               = max power across Doppler bins for this range bin
               (or: zero-Doppler bin power, for stationary-target RTI)
```

**Key functions from CVI Analysis Library:**
- `FFTEx()` — real-to-complex fast-time FFT (already used)
- `CxFFTEx()` — complex-to-complex slow-time FFT (new — BB24 uses this)
- `CxHanWin()`, `CxBlkHarrisWin()` — complex window functions for slow-time

**Memory layout for `rdResult`:**
Row-major `[rangeBins][cpiSize/2]` — this is what `PlotScaledIntensity` expects
when called with `numColumns = cpiSize/2, numRows = rangeBins`.

#### Step 5: 2D Plotting (~80 lines)

After processing, plot all four graphs:

```c
/* Colour map setup (done once at init or when user changes selection) */
ColorMapEntry colourMap[] = {
    { 0.0,  { 0x00, 0x00, 0x80 } },   /* dark blue  = floor   */
    { 0.25, { 0x00, 0x00, 0xFF } },   /* blue                 */
    { 0.50, { 0x00, 0xFF, 0x00 } },   /* green                */
    { 0.75, { 0xFF, 0xFF, 0x00 } },   /* yellow               */
    { 1.0,  { 0xFF, 0x00, 0x00 } },   /* red        = ceiling */
};

/* Range-Doppler plots */
DeleteGraphPlot (rdpTabHandle, RDP_TAB_GRAPH_RD_LRX, -1, VAL_DELAYED_DRAW);
PlotScaledIntensity (rdpTabHandle, RDP_TAB_GRAPH_RD_LRX,
                     rdResult0,
                     cpiSize / 2,           /* numColumns (Doppler bins) */
                     rangeBins,             /* numRows (range bins)      */
                     VAL_DOUBLE,
                     rangePerBin, 0.0,      /* y scaling: range axis     */
                     dopplerPerBin, 0.0,    /* x scaling: velocity axis  */
                     colourMap, VAL_BLACK,
                     numColours, 1, 0);

/* RTI plots */
DeleteGraphPlot (rdpTabHandle, RDP_TAB_GRAPH_RTI_LRX, -1, VAL_DELAYED_DRAW);
PlotScaledIntensity (rdpTabHandle, RDP_TAB_GRAPH_RTI_LRX,
                     rtiBuffer0,
                     RTI_HISTORY_LEN,       /* numColumns (time)         */
                     rangeBins,             /* numRows (range bins)      */
                     VAL_DOUBLE,
                     rangePerBin, 0.0,
                     1.0, 0.0,              /* x = CPI index             */
                     colourMap, VAL_BLACK,
                     numColours, 1, 0);
```

**Axis scaling for Range-Doppler x-axis (Doppler/velocity):**
- Doppler frequency per bin = PRF / cpiSize (Hz)
- PRF = trigger frequency (from DDS tab: `DDS_TAB_DDS_NUM_TRIG_FREQ`)
- Velocity per bin = (Doppler per bin × c) / (2 × f_carrier)
- f_carrier = FREQ_MULTIPLIER × actStart (or centre frequency)
- Unambiguous velocity = ±PRF / 2

#### Step 6: UI Integration (~30 lines)

- Read `rdpTabHandle` via `GetPanelHandleFromTabPage` in `main()` (tab index 5)
- Add `RDP_TAB_RING_CPI` callback to update `rdpCpiSize` and reallocate buffers
- Add `RDP_TAB_BTN_ENABLE` callback to set/clear `rdpEnabled` and reset `rdpChirpCount`
- Dim the enable button when not acquiring; un-dim when acquisition starts

#### Step 7: Cleanup

- Add `RDP_Cleanup()` call to `ADC_Cleanup()`
- Free all dynamic arrays, destroy slow-time FFT table
- Reset `rdpEnabled` in `ADC_StopAcquisition()`

---

### Dependencies and Considerations

1. **UIR must be edited first** — all graph and control IDs are assigned by the CVI UI Editor.
   Use provisional `#ifndef` defines during development (same pattern as FFT window/zero-pad
   rings).

2. **Thread safety** — CPI accumulation writes from the poll thread; processing reads from
   the UI thread. The `PostDeferredCall` mechanism ensures no concurrent access (poll thread
   resets `rdpChirpCount = 0` and stops writing before the deferred call runs). If timing
   is tight, add a `rdpBusy` flag (same pattern as `plotBusy`).

3. **Memory** — Worst case: 2048 chirps × 4096 samples × 2 channels × 8 bytes = 128 MB for
   CPI accumulators. Practical values (256 chirps × 256 samples) need only ~1 MB. Allocate
   dynamically based on UI ring values.

4. **Performance** — Fast-time FFT of 2048 chirps × 256-point = ~0.5M complex operations.
   Slow-time FFT of 128 range bins × 2048-point = ~0.3M complex operations. Total ~1M
   operations per CPI per channel — well within real-time at typical CPI rates (1–10 Hz).
   If needed, move to the dedicated processing thread architecture.

5. **Existing FFT infrastructure** — Reuse `ADC_ApplyWindow`, `ADC_GetFFTTable` for fast-time.
   Need new `CxFFTEx` + complex window calls for slow-time (not yet in the codebase, but
   available in CVI's `analysis.h`).

6. **Colour maps** — BB24 uses custom `ColorMapEntry` arrays. CVI's `PlotScaledIntensity`
   accepts these directly. Provide 2-3 preset maps (Jet, Hot, Grayscale) selectable via ring.

---
---

# Code Review — 2026-03-30

**Branch:** `Release1`

Full review of `DeviceControl_FullThreaded.c` (~4000 lines), `dds.h`, and
`DeviceControl_FullThreaded.h`. Focus: instrument control quality, threading,
data integrity, maintainability.

---

## Issues Found

### 1. `SAVE_TOFILE` macro has an unterminated comment (BUG — trivial)

**Location:** `DeviceControl_FullThreaded.c` line ~959

```c
#define SAVE_TOFILE  2   /* WD_AI_AsyncDblBufferToFile (driver-managed)
// Define the Trig Digital threshold level here!?
```

The `/*` is never closed. Compiles only because `#define` ends at the physical line,
so the dangling `/*` is part of the replacement text (which is just `2`) and is never
substituted into code. Confusing to read.

**Fix:** Close the comment on the same line:

```c
#define SAVE_TOFILE  2   /* WD_AI_AsyncDblBufferToFile (driver-managed) */
```

Move the TODO to its own line if still needed.

---

### 2. DDS init/cal sequence duplicated 3 times (MEDIUM — maintainability)

**Locations:**
- `DdsInitCalCB` (~line 766)
- `MasterDdsCB` (~line 2981)
- `SeqStep_DdsInitCW` (~line 3475)

All three repeat the identical `powerdown → reset → powerup → reset → calibrate_dac`
sequence with identical error handling. Any bug fix or ordering change must be
applied in three places.

**Fix:** Extract a single static helper:

```c
/* Returns 1 on success, 0 on failure. Sets ddsInitDone on success. */
static int DDS_FullInitCal (void)
{
    dds_powerdown ();
    dds_reset ();
    if (!dds_powerup ())        return 0;
    if (!dds_reset ())          return 0;
    if (!ad9914_calibrate_dac ()) return 0;
    ddsInitDone = 1;
    return 1;
}
```

Replace all three sites with calls to `DDS_FullInitCal()`, keeping only the
site-specific status message and UI update around the call.

---

### 3. PSU readback blocks the UI thread (MEDIUM — responsiveness)

**Location:** `PSU_ReadMeasurements` (line ~359), called from `ReadbackTimerCB`
(line ~577) on the UI thread.

Six sequential VISA query-response pairs, each with a 3000 ms timeout. If the PSU
is slow or unresponsive, the UI freezes for up to 18 seconds. Correctly suspended
during ADC acquisition (line ~2205), but still causes jank during idle use.

**Fix:** Move readback to a background thread:

1. Create a `PSU_ReadbackThread` that runs the 6 queries, stores results in
   `static volatile double` globals.
2. When done, post a `PSU_ReadbackDeferred` to update the UI numerics/meters.
3. Change `ReadbackTimerCB` to schedule the thread (with a guard to prevent
   overlapping reads) instead of calling `PSU_ReadMeasurements` directly.

---

### 4. `MasterResetStartCB` blocks the UI thread for 1+ seconds (LOW — UX)

**Location:** `MasterResetStartCB` line ~3982 — `Delay(1.0)` on the UI thread.

Every other long operation (calibration, startup sequence, shutdown) correctly uses
a background thread. This one was left synchronous.

**Fix:** Refactor to the same background-thread + deferred-call pattern used by
`MasterSequenceThread`:

1. Create `MasterResetThread` with the same step-by-step structure.
2. Move the `Delay(1.0)` and DDS restart into the background thread.
3. Post deferred calls for each UI update and for the final status.

---

### 5. `fwrite` return value ignored in `DiskSaveThread` (LOW — data integrity)

**Location:** `DiskSaveThread` line ~1568

```c
fwrite (slotBuf, sizeof (U16), HALF_BUF_SAMPLES, recordFile);
```

A disk-full or I/O error silently produces a truncated recording with no user
notification.

**Fix:**

```c
size_t written = fwrite (slotBuf, sizeof (U16), HALF_BUF_SAMPLES, recordFile);
if (written < HALF_BUF_SAMPLES)
    diagSaveError++;
```

Add `static volatile U32 diagSaveError = 0;` to the diagnostic counters. Display
it in `AdcPollTimerCB` and in the stop status message. Optionally post a deferred
call to surface a warning immediately on first error.

---

### 6. `ADC_TAB_ADC_NUM_TRIGGERS` control is dead UI (LOW — clarity)

**Location:** Header defines `ADC_TAB_ADC_NUM_TRIGGERS 9` (numeric). Never read
anywhere in code. `reTrgCnt` is hardcoded to 1 in all call sites.

**Fix (choose one):**

- **Option A — Remove:** Delete the control from the UIR in the CVI editor.
- **Option B — Wire up:** Read it into `reTrgCnt` in `ADC_StartCommon` so the
  user can control re-trigger count from the UI.

Recommendation: Option A unless multi-trigger acquisition is needed in future.

---

### 7. Relay/DDS auto-connect hardcodes COM port numbers (LOW — portability)

**Locations:**
- `MasterTxRelayCB` line ~2893: `FindResourceBySubstring("ASRL18")`
- `MasterDdsCB` line ~2943: `FindResourceBySubstring("ASRL19")`
- `SeqStep_ConnectRelayDds` lines ~3411, ~3445: same hardcoded strings

If Windows reassigns COM numbers (USB hub change, driver reinstall), the master tab
auto-connect silently fails. The per-device tab connect still works because the
user manually selects the resource.

**Fix:** Add two string controls (or `#define` constants) for the relay and DDS
VISA substrings. Read them at auto-connect time. Alternatively, match by USB
VID/PID instead of COM number.

---

### 8. Thread safety of shared globals is fragile (LOW — correctness)

Multiple `volatile` globals (`isAcquiring`, `saveMode`, `plotBusy`, `plotScans`,
all `diagXxx` counters) are accessed from different threads without locks or
memory barriers. Works on x86/MSVC because aligned 32-bit reads/writes are
atomic in practice, but:

- `isAcquiring` is written by both the UI thread (`ADC_StopAcquisition`) and
  the poll thread (on `fStop`). Actual synchronisation comes from
  `CmtWaitForThreadPoolFunctionCompletion`, not from `volatile`.
- `plotScans` is set before acquisition starts, never during. Safe by sequencing.

**Fix (if desired):** Use CVI `CmtNewLock`/`CmtGetLock`/`CmtReleaseLock` for
critical sections, or at minimum document the threading contract in comments.

No immediate action needed — correct on the target platform today.

---

### 9. FFT dBm scaling comments are misleading (TRIVIAL — documentation)

**Location:** `ADC_PlotDeferred` lines ~1700–1712

The `+30` factor comment says "dBW → dBm" but the actual conversion is
`dBV_peak → dBm into 50Ω`. Dangling TODO: `// account for bits for voltage like BB24?`

**Fix:** Rewrite the comment to accurately describe the full chain:

```
Peak voltage → 20·log10(|X|)
  − 20·log10(N/2)             FFT normalisation
  + window coherent gain corr  from GetWinProperties()
  − 3 dB                      peak → RMS
  + 30 dB                     dBV → dBm (into 1 Ω)
  − 10·log10(50)              correct for 50 Ω load
  = dBm into 50 Ω
```

Remove or resolve the BB24 TODO.

---

## Priority Table

| # | Issue | Effort | Impact | When |
|---|-------|--------|--------|------|
| 1 | Unterminated `SAVE_TOFILE` comment | Trivial | Low | Next edit |
| 2 | Extract DDS init helper | Low | Medium | Next edit |
| 5 | `fwrite` error checking | Low | Medium | Next edit |
| 9 | FFT comment cleanup | Trivial | Low | Next edit |
| 6 | Remove dead `ADC_NUM_TRIGGERS` control | Low | Low | Next UIR edit |
| 3 | PSU readback to background thread | Medium | High | Next refactor |
| 4 | `MasterResetStartCB` to background thread | Low | Medium | Next refactor |
| 7 | Configurable COM port aliases | Medium | Medium | Next refactor |
| 8 | Thread safety docs/locks | Low | Low | When changing threading |

---

## ScanInterval & Clock Divider Expansion (2026-04-02)

### Summary

Expanded the clock divider / timing display to expose the full signal chain and add
user-configurable `ScanInterval` for the ADLINK `WD_AI_ContScanChannels` API.
Previously the `ScanIntrv` and `SampIntrv` parameters were hardcoded to `1`.

**Signal chain (unchanged):**

```
SYNC_CLK = DDS_CLOCK / 24
ADC_CLK  = SYNC_CLK / (hmcDiv × 2)      ← clock speed from two dividers only
Trigger  = ADC_CLK  / progDiv            ← trigger division
```

**New — ScanInterval affects only sampling rate:**

```
ADC Sampling Rate = ADC_CLK / scanInterval
```

The trigger period (`progDiv` ADC_CLK cycles) is unaffected by `scanInterval`.
However, `minProgDiv` and dead-sample calculations now account for the fact that
each ADC sample takes `scanInterval` clock cycles:

- `chirpSteps    = sampsPerChirp × scanInterval × adcClkDivTotal` (SYNC_CLK cycles)
- `deadSamples   = progDiv / scanInterval − sampsPerChirp`
- `minProgDiv    = sampsPerChirp × scanInterval + 1`

### Files Changed

| File | Change |
|------|--------|
| `DeviceControl_FullThreaded.h` | Added 4 placeholder `#define`s for new UI controls |
| `DeviceControl_FullThreaded.c` | Updated `DDS_UpdateTimingDisplay()`, `ADC_StartCommon()`, `ADC_WriteSidecarHeader()` |

### Code Details

#### `DDS_UpdateTimingDisplay()` (~line 597)

- Reads `scanInterval` from ADC tab (`ADC_TAB_ADC_NUM_SCAN_INTERVAL`).
- Clamps to minimum of 1.
- Calculates `adcSampRateMHz = adcClkMHz / scanInterval`.
- Updated `chirpSteps`, `deadSamples`, `minProgDiv` formulas (see above).
- Writes three new DDS-tab indicators: fixed `/2`, mirrored `scanInterval`, ADC sampling rate.
- Validation warning updated to `progDiv <= sampsPerChirp × scanInterval`.

#### `ADC_StartCommon()` (~line 2138)

- Reads `scanInterval` from ADC tab before arming.
- Passes `(U32)scanInterval` as both `ScanIntrv` and `SampIntrv` to
  `WD_AI_ContScanChannels` and `WD_AI_ContScanChannelsToFile`
  (previously hardcoded `1, 1`).

#### `ADC_WriteSidecarHeader()` (~line 1240, 1350)

- `[ADC]` section: added `ScanInterval` field.
- `[Timing]` section: added `FixedDiv2 = 2`, `ScanInterval`, `ADC_SampRateMHz`.

### New #defines (placeholder — regenerated by CVI when UIR is saved)

```c
#define ADC_TAB_ADC_NUM_SCAN_INTERVAL  28  /* numeric control on ADC tab */
#define DDS_TAB_DDS_NUM_FIXED_DIV2     34  /* numeric indicator on DDS tab */
#define DDS_TAB_DDS_NUM_SCAN_INTERVAL  35  /* numeric indicator on DDS tab */
#define DDS_TAB_DDS_NUM_ADC_SAMP_RATE  36  /* numeric indicator on DDS tab */
```

### UIR Editor Instructions

Add these controls in the CVI User Interface Editor. The **Constant Name** must
match exactly — CVI will assign its own control IDs and regenerate the `.h` on save.

#### ADC Tab — 1 new numeric control

| Label | Constant Name | Data Type | Default | Mode |
|-------|---------------|-----------|---------|------|
| Scan Interval | `ADC_TAB_ADC_NUM_SCAN_INTERVAL` | `int` | `1` | Hot (editable) |

#### DDS Tab — 3 new numeric indicators

| Label | Constant Name | Data Type | Mode |
|-------|---------------|-----------|------|
| Fixed /2 | `DDS_TAB_DDS_NUM_FIXED_DIV2` | `int` | Indicator (read-only) |
| Scan Interval | `DDS_TAB_DDS_NUM_SCAN_INTERVAL` | `int` | Indicator (read-only) |
| ADC Samp Rate (MHz) | `DDS_TAB_DDS_NUM_ADC_SAMP_RATE` | `double` | Indicator (read-only) |

After adding the controls and saving the `.uir`, CVI regenerates the `.h` automatically.
The placeholder `#define` lines will be overwritten with the correct IDs.

---

## Sidecar Header & Plot Axis Fixes (2026-04-05)

### Sidecar header — missing timing fields added

The `[Timing]` section of `ADC_WriteSidecarHeader` was missing three values that
`DDS_UpdateTimingDisplay` computes and displays on the DDS tab. These were present
in the original Session 3 field table but were lost during the ScanInterval expansion
refactor.

**Added fields:**

| Field | Type | Source control |
|-------|------|----------------|
| `ChirpSteps` | int | `DDS_TAB_DDS_NUM_CHIRP_STEPS` |
| `CalcChirpPd_us` | double | `DDS_TAB_DDS_NUM_CALC_PERIOD` |
| `MinProgDivider` | int | `DDS_TAB_DDS_NUM_MIN_PROG_DIV` |

**Updated `[Timing]` section now writes:**

```text
HMC432_Divider, FixedDiv2, ProgDivider, ScanInterval, SampsPerChirp,
SyncClkMHz, ADC_ClkMHz, ADC_SampRateMHz, PRF_Hz, DRCTRL_Period_us,
DeadTime_us, DeadSamples, ChirpSteps, CalcChirpPd_us, MinProgDivider
```

### Bug fix: plot axes ignored `scanInterval`

**Problem:** In `ADC_PlotDeferred`, both the time-domain x-axis and the FFT
beat-frequency x-axis read `DDS_TAB_DDS_NUM_ADC_CLK` (the raw ADC clock). The
actual ADC sampling rate is `ADC_CLK / scanInterval`. When `scanInterval > 1`,
both axes were incorrect — the time axis was compressed and the beat-frequency axis
was stretched by a factor of `scanInterval`.

The **range axis was unaffected** — the formula `c × scans / (2 × BW × paddedN)`
does not depend on the sampling rate (the chirp duration is implicitly encoded
in the number of samples).

**Fix:** Both locations now read `DDS_TAB_DDS_NUM_ADC_SAMP_RATE` (which already
contains `ADC_CLK / scanInterval` as computed by `DDS_UpdateTimingDisplay`):

- Time-domain: `timePerSample_us = 1.0 / adcSampRate` (was `1.0 / adcClk`)
- FFT: `fs_Hz = adcSampRate × 1e6` (was `adcClk × 1e6`)

When `scanInterval = 1` (the previous hardcoded default), the result is identical
to the old behaviour.

---

## 2026-04-23 — Peak/Hold Floor, Master Cursor, WD-DASK Save Path, Sidecar Metrics

**Branch:** `Release1`

### Overview

Four related changes in one session:
1. Minimum-range floor applied to all peak readouts (legend + max-hold)
2. Master FFT graph cursor 1 reads out dBm at the user-selected range
3. Master "Record" button switched from threaded save to WD-DASK driver save
4. File truncation on stop + peak/hold metrics appended to sidecar on stop

---

### 1. Minimum range floor on all peak readouts

**Problem:** The per-frame peak finder (which drives the legend labels "LRX x.xxm y.yydBm")
started searching at bin 2, regardless of TX bandwidth or zero-padding. Near-DC bins could
appear as the "peak" in cluttered or low-range scenes. The max-hold display already
applied the `MAX_HOLD_RANGE_MIN_M = 1.9 m` threshold correctly.

**Fix:** `minBin` is now computed once from `MAX_HOLD_RANGE_MIN_M / rangePerBin` immediately
after the x-axis scaling, before both the peak finder and the max-hold search:

```c
minBin = (rangePerBin > 0.0)
         ? (U32)(MAX_HOLD_RANGE_MIN_M / rangePerBin) + 1U : 2U;
if (minBin < 2U) minBin = 2U;
```

Both `peak0`/`peak1` (legend) and the max-hold loop now start at `minBin`. The formula
accounts automatically for TX bandwidth and zero-padding factor via `rangePerBin`.

| TX BW | Zero-pad | rangePerBin | minBin (~2 m) |
|-------|----------|-------------|---------------|
| 1.5 GHz | ×1 | ~0.10 m | ~20 |
| 1.5 GHz | ×4 | ~0.025 m | ~77 |

**Files changed:** `DeviceControl_FullThreaded.c` — `ADC_PlotDeferred` (~line 1869)

---

### 2. Master FFT graph cursor readout

**What was done in UIR:** Cursor 1 was added to `MASTER_TAB_MASTER_GRAPH_FFT` in the
CVI UIR Editor (snap-to-plot mode). No constant name is assigned — cursors are accessed
by number (1-based).

**Code:** After the master plot markers, each plot cycle reads cursor 1:

```c
GetGraphCursor (masterTabHandle, MASTER_TAB_MASTER_GRAPH_FFT, 1, &crsX, &crsY);
crsBin = (U32)(crsX + 0.5);   /* crsX is in data coordinates (bin index) */
```

The result is formatted as:

```
Cursor: 4.20 m  LRX -14.3 dBm  URX -17.1 dBm
```

and written to `MASTER_TAB_GRAPH_LABEL` (the text message control near the graph).
Both channels are sampled at the cursor bin simultaneously.

**Note on CVI cursor coordinates:** `GetGraphCursor` returns the cursor x position in
**data coordinates** (bin index, 0-based double), not in display coordinates (metres).
The axis `ATTR_XAXIS_GAIN = rangePerBin` affects axis tick labels only, not the value
returned by `GetGraphCursor`. Range is recovered as `(double)crsBin * rangePerBin`.

**Files changed:** `DeviceControl_FullThreaded.c` — `ADC_PlotDeferred` (~line 1987)

---

### 3. Master "Record" button switched to WD-DASK (SAVE_TOFILE)

**Previous state:** `MasterAcqRecordCB` called `ADC_StartCommon(SAVE_THREAD, ...)`,
opening the output file via `fopen`/`fwrite` through the user-space TSQ save thread.

**New state:** `MasterAcqRecordCB` now calls `ADC_StartCommon(SAVE_TOFILE, ...)`.
The ADLINK driver writes raw U16 data directly to disk via `WD_AI_ContScanChannelsToFile`
+ `WD_AI_AsyncDblBufferToFile`. No `fopen` on the application side.

**Save button mapping (current):**

| Button | Mode | Mechanism |
|--------|------|-----------|
| ADC tab — Start Acq | `SAVE_NONE` | Monitor only |
| ADC tab — Record | `SAVE_THREAD` | TSQ → fwrite thread |
| ADC tab — Save | `SAVE_TOFILE` | WD-DASK driver (unchanged) |
| Master tab — Start | `SAVE_NONE` | Monitor only |
| **Master tab — Record** | **`SAVE_TOFILE`** | **WD-DASK driver (changed)** |
| Master tab — Reset+Start | `SAVE_TOFILE` | WD-DASK driver (unchanged) |

**Files changed:** `DeviceControl_FullThreaded.c` — `MasterAcqRecordCB` (~line 3600)

---

### 4. WD-DASK file format — confirmed no binary header

The ADLINK sample code (`CAIDbfFile/CAIDbF.c`, `CAIDbfFileDiv/CAIDbF.c`) confirms:

- `WD_AI_ContScanChannelsToFile` writes **raw U16 interleaved samples with no binary
  file header**. Byte 0 of the `.dat` file is sample 0, CH0.
- `WD_AI_AsyncClear(card, &startPos, &accessCnt)` flushes the partially-filled
  current DMA half-buffer to the file and returns `accessCnt` = number of extra samples
  written. The ADLINK sample explicitly adds this to its running total count.

**Consequence:** Without intervention, the `.dat` file ends with an incomplete
half-buffer (variable size each run, depending on when Stop is pressed). This produced
the "random lingering samples at the end" seen in post-processing.

**Fix:** `ADC_StopAcquisition` now truncates the `.dat` file to exactly
`diagHalfReady × HALF_BUF_SAMPLES × 2` bytes immediately after `WD_AI_AsyncClear`,
using the Win32 `SetFilePointerEx` + `SetEndOfFile` API. This removes the partial
DMA flush and gives a file of exactly `diagHalfReady` complete half-buffers.

```c
if (saveMode == SAVE_TOFILE && recordPath[0] != '\0')
{
    HANDLE hFile = CreateFile (datPath, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, ...);
    li.QuadPart = (LONGLONG)diagHalfReady * HALF_BUF_SAMPLES * sizeof(U16);
    SetFilePointerEx (hFile, li, NULL, FILE_BEGIN);
    SetEndOfFile (hFile);
    CloseHandle (hFile);
}
```

**Files changed:** `DeviceControl_FullThreaded.c` — `ADC_StopAcquisition` (~line 2192)

---

### 5. Sidecar header additions and metrics on stop

#### New field in `[Timing]`

`ScansPerChirp = progDiv / scanInterval` is now written at the end of the `[Timing]`
section. This is the full CRI length in ADC scans (chirp + dead time). Previously
derivable from ProgDivider and ScanInterval but now explicit for MATLAB readability.

#### New section `[Peak_Metrics]` — appended on stop

`ADC_AppendSidecarMetrics(baseName)` is called from `ADC_StopAcquisition` when
a file was being saved (`saveMode != SAVE_NONE`). It opens the `.hdr` in append mode
and adds:

```ini
[Peak_Metrics]
MaxHold_Dbm           = -12.34
MaxHold_Range_m       = 5.678
LastPeak_LRX_Range_m  = 4.321
LastPeak_LRX_Dbm      = -14.56
LastPeak_URX_Range_m  = 5.100
LastPeak_URX_Dbm      = -15.20
CompleteHalfBufs      = 42
```

`CompleteHalfBufs` matches `diagHalfReady` at stop time — it gives the exact number
of full half-buffers in the truncated `.dat` file and is the ground truth for file
size calculations in post-processing.

---

## MATLAB Post-Processing Update Notes

### File structure (SAVE_TOFILE / WD-DASK mode)

```
CerberusData_YYYYMMDD_HHMMSS.dat   — raw binary
CerberusData_YYYYMMDD_HHMMSS.hdr   — plain-text INI sidecar
```

**`.dat` file:**
- **No binary header.** Data starts at byte 0.
- Format: interleaved U16, little-endian: `[CH0_s0, CH1_s0, CH0_s1, CH1_s1, ...]`
- Mid-code offset: 32768 (subtract to convert to signed; divide by 32768 for ±1 FS)
- Voltage: `(sample - 32768) / 32768 * VoltageRange_V` (VoltageRange_V from sidecar)
- File size: **exactly** `CompleteHalfBufs × HalfBufSamples × 2` bytes
  (no partial half-buffer at end — truncated on stop since 2026-04-23)
- Total scans: `CompleteHalfBufs × ScansPerHalf` (each scan = one CH0+CH1 pair)

**Existing MATLAB read-in to update:**

1. **Remove any header skip.** If the existing script skipped any bytes at the start
   of the file, remove that skip — the data starts at byte 0.

2. **Read using `CompleteHalfBufs` from sidecar** (not file size) as the authoritative
   sample count, to avoid off-by-one from rounding:
   ```matlab
   nHalfs  = hdr.Peak_Metrics.CompleteHalfBufs;   % from sidecar [Peak_Metrics]
   nScans  = nHalfs * hdr.Data_Format.ScansPerHalf;
   nSamps  = nScans * hdr.Data_Format.NumChannels; % = nHalfs * HalfBufSamples
   raw = fread(fid, nSamps, 'uint16');
   ```

3. **Reshape and deinterleave:**
   ```matlab
   raw  = reshape(raw, 2, []);          % 2 rows: [CH0; CH1]
   ch0  = double(raw(1,:)) - 32768;
   ch1  = double(raw(2,:)) - 32768;
   ```

4. **Convert to volts:**
   ```matlab
   Vrange = hdr.Data_Format.VoltageRange_V;
   ch0_V  = ch0 / 32768 * Vrange;
   ch1_V  = ch1 / 32768 * Vrange;
   ```

5. **Reshape into chirps** using `ScansPerChirp` from `[Timing]`:
   ```matlab
   Ns = hdr.Timing.ScansPerChirp;      % full CRI length (chirp + dead time)
   Nc = hdr.Data_Format.ScansPerHalf.ScansPerChirp;  % or floor(nScans / Ns)
   Nc = floor(nScans / Ns);
   ch0_chirps = reshape(ch0(1 : Nc*Ns), Ns, Nc);   % [samples_per_CRI × num_chirps]
   ch1_chirps = reshape(ch1(1 : Nc*Ns), Ns, Nc);
   ```
   The first `SampsPerChirp` rows of each column are the active chirp; the remaining
   rows (up to `ScansPerChirp`) are dead time (no TX signal). Discard or keep as needed.

6. **FMCW range FFT per chirp:**
   ```matlab
   Nchirp = hdr.Timing.SampsPerChirp;   % active chirp samples only
   chirp_data = ch0_chirps(1:Nchirp, :);
   padFactor  = 4;                       % match ZeroPadFactor from [FFT_Processing]
   Nfft       = Nchirp * padFactor;
   window     = hann(Nchirp);
   chirp_win  = chirp_data .* window;    % apply window column-wise
   R = fft(chirp_win, Nfft, 1);         % range FFT per chirp, zero-padded
   R = R(1:Nfft/2, :);                  % positive frequencies only
   ```

7. **Range axis:**
   ```matlab
   c   = 299792458;
   BW  = hdr.TX_Radar.TX_Bandwidth_GHz * 1e9;
   rangePerBin = c / (2 * BW * Nfft / Nchirp);  % matches C code formula
   r_axis = (0 : Nfft/2-1) * rangePerBin;
   ```
   Equivalently: `rangePerBin = c * Nchirp / (2 * BW * Nfft)`.

8. **Check for new sidecar section `[Peak_Metrics]`:**
   This section is appended at the end of the `.hdr` after recording stops. A sidecar
   written by an older build will not have it — add an `isfield` guard in MATLAB:
   ```matlab
   if isfield(hdr, 'Peak_Metrics')
       maxHold_dBm   = hdr.Peak_Metrics.MaxHold_Dbm;
       maxHold_range = hdr.Peak_Metrics.MaxHold_Range_m;
       nHalfs        = hdr.Peak_Metrics.CompleteHalfBufs;
   end
   ```

### Sidecar parsing reminder

Parse the `.hdr` as INI-style key=value pairs, grouped by `[Section]` headers.
All numeric fields are plain ASCII decimal. String fields (`SaveMode`, `Context`,
`Interleave`, etc.) are plain text. Section names are case-sensitive as written.

The `[Peak_Metrics]` section will be present in all files recorded from this build
onward (when a save mode was active). Files from earlier builds will have all sections
up to and including `[Run_Note]` only.

### Key sidecar fields for post-processing

| Section | Field | Use |
|---------|-------|-----|
| `[Data_Format]` | `HalfBufSamples` | Samples per half-buffer (U16 words, both channels) |
| `[Data_Format]` | `ScansPerHalf` | Sample pairs (scans) per half-buffer |
| `[Data_Format]` | `VoltageRange_V` | Full-scale voltage (±) for conversion |
| `[Timing]` | `SampsPerChirp` | Active chirp length in ADC samples (for FFT window) |
| `[Timing]` | `ScansPerChirp` | Full CRI length in scans (chirp + dead time) — **new** |
| `[Timing]` | `ProgDivider` | CRI length in ADC_CLK cycles |
| `[Timing]` | `ScanInterval` | ADC_CLK cycles per sample |
| `[TX_Radar]` | `TX_Bandwidth_GHz` | TX bandwidth for range scaling |
| `[FFT_Processing]` | `ZeroPadFactor` | Zero-padding multiplier used in live display |
| `[Peak_Metrics]` | `CompleteHalfBufs` | Authoritative half-buffer count (ground truth file size) — **new** |
| `[Peak_Metrics]` | `MaxHold_Dbm` / `MaxHold_Range_m` | Max-hold result at stop time — **new** |

---

## DMA Buffer Overrun (OVR) Investigation — 2026-04-24

### Status: partially resolved, residual OVR unresolved at 36 MS/s — pinned for later

### Background
`diagDmaOverrun` (shown as `OVR` in the status line) counts DMA FIFO overruns detected
via `WD_AI_ContStatus` bits `0x60000000` (`BUF_OVR_DET | FIFO_OVR_DET`).

At 36 MS/s scan rate (2 ch), the raw data rate is **144 MB/s** and each half-buffer
fills in **116.5 ms**.  The poll thread must call `WD_AI_AsyncDblBufferHandled` within
that window or the DMA overwrites unread data.

### Changes made this session

| # | Change | File | Effect |
|---|--------|------|--------|
| 1 | `HALF_BUF_SAMPLES` 1 MB → 8 MB (2^23) | constants | 8× draining headroom (~420 ms fill at 36 MS/s) |
| 2 | `SCANS_PER_HALF` derived from `HALF_BUF_SAMPLES/ADC_NUM_CH` | constants | Scales automatically |
| 3 | `ALIGN_CRI_MAX` derived from `SCANS_PER_HALF/4U` | constants | Scales automatically; sizes `powerProfile[]` (8 MB BSS) |
| 4 | `SAVE_QUEUE_CAP` 32 → 64 slots (1 GB TSQ) | constants | ~42 s headroom at 24 MB/s deficit; stays under CVI 32-bit TSQ ceiling (127 slots max at 16 MB/item) |
| 5 | `TSQ_WRITE_MS` 50 → 0 | constants | Poll thread never blocks on save queue full |
| 6 | OVR edge-detection (`prevOvrBits`) | poll thread | Counts only new hardware latch transitions, not persistent state |
| 7 | `MasterAcqRecordCB` SAVE_TOFILE → SAVE_THREAD | master save CB | Eliminates synchronous `WD_AI_AsyncDblBufferToFile` from poll thread |
| 8 | `HardwarePollThread` elevated to `THREAD_PRIORITY_TIME_CRITICAL` | poll thread | Prevents user-mode scheduler preemption during processing |

### Root causes resolved
- **SAVE_TOFILE in master save button**: `WD_AI_AsyncDblBufferToFile` is synchronous.
  Once the OS write cache fills (~100 events × 16 MB = 1.6 GB written), writes stall
  for ~133 ms > 116.5 ms window → OVR.  Fixed by switching to SAVE_THREAD.
- **`TSQ_WRITE_MS = 50`**: Queue-full condition blocked poll thread for 50 ms.  Fixed to 0.
- **Latched OVR status bits**: bits stay set after first overrun; edge detection now
  prevents `diagDmaOverrun` accumulating on every subsequent event.
- **`SAVE_QUEUE_CAP = 256`** (was attempted): 256 × 16 MB = 4 GB = 2^32 overflows
  CVI's internal 32-bit TSQ arithmetic → divide-by-zero in `CmtReadTSQData`.  Reverted to 64.

### Residual issue — PINNED
**Symptom:** At 36 MS/s, OVR first appears at ~HR 96–105 and accumulates (~44 OVR
over 404 HR in one run).  `Sd = 0` throughout (disk keeping up; queue never fills).

**Eliminated causes:**
- Save queue full (Sd = 0)
- Save queue blocking poll thread (TSQ_WRITE_MS = 0)
- Latched OVR bits inflating count (edge detection in place)
- User-mode thread preemption (THREAD_PRIORITY_TIME_CRITICAL set)

**Suspected remaining cause:**
Hardware-level DPC/interrupt preemption of the poll thread, or occasional latency
in `CmtWriteTSQData`'s internal mutex while the save thread holds it during a 16 MB
`CmtReadTSQData` copy.  The 16 MB `CmtWriteTSQData` copy in the poll thread is the
primary risk — if it is preempted mid-copy by a kernel DPC for even ~100 ms, the
116.5 ms window is exceeded.

**Planned fix (not yet implemented):**
Decouple the 16 MB copy from the poll thread entirely using a three-stage pipeline:
1. **Poll thread** (time-critical): post `activeBuf` index (4 bytes) to a tiny
   index-queue; call `WD_AI_AsyncDblBufferHandled` immediately.  No large copy.
2. **Copy thread** (new): read index, `memcpy` 16 MB from `dmaBuffer[index]` into a
   free slot from a pre-allocated pool (64 × 16 MB = 1 GB).  Has 233 ms window
   (2 × half-buffer fill) to complete the copy; takes ~2–4 ms.  Push pool-slot
   pointer to save queue.
3. **Save thread** (existing, unchanged): drain save queue, `fwrite` to disk.

This removes ALL large memory operations from the poll thread.  The poll-thread
processing time drops from ~10 ms (with 16 MB copy) to <1 ms, making OVR at 36 MS/s
essentially impossible regardless of DPC latency.

**Workaround in use:** 18 MS/s scan rate (half rate), which doubles the fill time to
~233 ms, providing adequate margin with the current architecture.

