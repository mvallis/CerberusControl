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
|---|---|
| `diagPollCount` | Times `AsyncDblBufferHalfReady` was called |
| `diagHalfReadyCount` | Times `halfReady == TRUE` was returned |
| `diagFStopCount` | Times `fStop == TRUE` was returned |
| `diagQueuedCount` | Times a half-buffer was successfully queued |
| `diagPlotTriggeredCount` | Times `PostDeferredCall` was posted for a plot |
| `diagPlotEnteredCount` | Times the deferred plot callback was entered |
| `diagPlotCompletedCount` | Times the deferred plot callback completed |
| `diagFirstRawSample` | First raw U16 value seen in DMA buffer |

These counters are displayed every 500 ms by `AdcPollTimerCB` in a single status line:
```
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
|---|---|
| Change trigger polarity to `WD_AI_TrgNegative` | Hardware provides a rectangular signal; both edges work. Positive-edge POST trigger is preferred. |
| Change `NumChans` in `WD_AI_ContScanChannels` from `1` to `2` | Per WD-DASK docs, this parameter is the *highest channel index* (0-based), not the count. `1` correctly scans CH0 and CH1. (The entire call was later replaced anyway.) |

---

## Current Status

Build is clean (no errors, no warnings). The application arms without error and the diagnostic timer fires. The next run will reveal which pipeline stage is failing based on the counter pattern in the status bar:

| Observed pattern | Implied failure |
|---|---|
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

```
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
                                  scanInterval  ReTrgCnt=1  ← STOP AFTER 1 TRIGGER
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
WD_AI_ContReadMultiChannels(..., adcBufId1, ...);                        // starts from buf1 ✓
```

### Expected diagnostic output after fixes

With `ReTrgCnt = 65535` and hardware confirmed working, the expected steady-state output is:

```
Poll:NNNN HR:N FS:0 Qd:N | PTrg:N PEntr:N PDone:N | Raw0:0x~~~~
```

- `FS` should remain near 0 throughout the run.
- `HR`, `Qd`, `PTrg`, `PEntr`, `PDone` should all climb together at the chirp trigger rate.
- `Raw0` should reflect actual ADC mid-scale (~`0x7FFF` for 0 V input).
