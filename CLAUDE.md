# Project: DeviceControl_FullThreaded

Cerberus radar system control software, written in NI LabWindows/CVI 2020.
Portable Claude context — read this on every session start.

---

## System overview

The application controls a radar system comprising:

- **KMTronic** relay board (USB/serial)
- **Siglent SPD3303X** PSU (VISA/serial)
- **AD9914** DDS chirp generator (serial)
- **ADLINK PCI-9846H** simultaneous-sampling ADC (WD-DASK driver, `Wd-dask64.h`)

The PCI-9846H is a 2-channel, 16-bit, simultaneous-sampling ADC with up to
20 MS/s per channel and 256 MB of on-card DMA memory.

---

## Hard environment constraints

- **Compiler:** CVI 2020 ANSI C only — no C++ features (no `//` comments are fine,
  but no `bool`, `_Bool`, designated initialisers, mixed declarations and code, etc.).
- **Build:** CVI 2020 cannot be built from the command line. The user must build
  manually in the CVI IDE (Build → Build, or F7). Don't attempt CLI builds and don't
  block on build verification — describe what needs rebuilding and let the user do it.
- **`.uir` files are binary** (CVI User Interface Resource). Never hand-edit them —
  modify only via the CVI User Interface Editor inside the IDE.
- **64-bit build target** is the standard configuration. Several memory allocations
  rely on a 64-bit address space (e.g. multi-GB save queues, large BSS arrays).

---

## Threading model

- **UI thread (main):** CVI panel callbacks. `SetCtrlVal` and all UI calls must run
  here. Background threads post UI work via `PostDeferredCall`.
- **`HardwarePollThread`:** polls `WD_AI_AsyncDblBufferHalfReady`, calls
  `WD_AI_AsyncDblBufferHandled` to re-arm. Runs at `THREAD_PRIORITY_TIME_CRITICAL`
  to avoid user-mode preemption during the 116.5 ms half-buffer window at 36 MS/s.
- **`DiskSaveThread`:** drains the save TSQ and writes 16 MB blocks to disk.

`PostDeferredCall` is the only legal way to call UI functions from a background
thread in CVI 2020.

---

## DMA buffer layout (current)

| Constant | Value | Notes |
|---|---|---|
| `HALF_BUF_SAMPLES` | `8388608U` (2^23) | 16 MB per half-buffer (U16 samples) |
| `SCANS_PER_HALF` | `HALF_BUF_SAMPLES / ADC_NUM_CH` | Derived — 4 194 304 scans |
| `ALIGN_CRI_MAX` | `SCANS_PER_HALF / 4U` | Derived — sizes `powerProfile[]` (8 MB BSS) |
| `SAVE_QUEUE_CAP` | `64` | 64 × 16 MB = 1 GB TSQ. **CVI ceiling: max 127 slots at 16 MB/item** — `CmtNewTSQ` overflows 32-bit internal arithmetic at numItems × itemSize ≥ 2^32. |
| `TSQ_WRITE_MS` | `0` | Non-blocking save write — poll thread never stalls on full queue. |

---

## Save modes

| Mode | Constant | Mechanism | When to use |
|---|---|---|---|
| `SAVE_NONE` | 0 | No file output | Monitor / preview |
| `SAVE_THREAD` | 1 | User-space TSQ → `DiskSaveThread` → `fwrite` | **Default for all recording** — decouples disk from poll thread |
| `SAVE_TOFILE` | 2 | `WD_AI_AsyncDblBufferToFile` (driver) | **Avoid.** Synchronous in poll thread; causes OVR at high rates once OS write cache fills. |

`MasterAcqRecordCB` (master save button) and `AdcRecordCB` both use `SAVE_THREAD`.
The `AdcSaveCB` button still uses `SAVE_TOFILE` for legacy comparison only.

---

## Pinned: residual OVR at 36 MS/s

**Symptom:** With `SAVE_THREAD` + `TSQ_WRITE_MS=0` + `TIME_CRITICAL` poll thread,
OVR still first appears at HR ~96–105 at 36 MS/s scan rate. `Sd = 0` throughout
(disk keeping up; queue not filling).

**Suspected cause:** Kernel DPC/ISR preempting the 16 MB `CmtWriteTSQData` memcpy
inside the poll thread. User-mode priority can't defend against kernel-level
preemption.

**Workaround in use:** 18 MS/s scan rate (half), giving a 233 ms half-buffer fill
window — comfortable margin with the current architecture.

**Planned fix (not yet implemented):** Three-stage pipeline:

1. Poll thread posts buffer index (4 bytes) to a tiny index-queue, calls
   `WD_AI_AsyncDblBufferHandled` immediately. **No large copy.**
2. New copy thread reads index, memcpy 16 MB from `dmaBuffer[index]` into a free
   slot from a pre-allocated pool (64 × 16 MB = 1 GB). 233 ms window; copy takes
   ~2–4 ms.
3. Existing save thread unchanged — drains save queue, `fwrite` to disk.

Full investigation log: see `WORKING.md` § "DMA Buffer Overrun (OVR) Investigation".

---

## ADC calibration sequence

The PCI-9846H calibration must follow this exact order or `WD_AI_ContScanChannels`
returns `-201 ErrorConfigIoctl`:

1. **Register** card (`Register_Card`)
2. **Calibrate** (`WD_AD_Auto_Calibration_ALL`) — driver pins internal buffers
3. **Release** card (`Release_Card`) — clears driver buffer registrations
4. **Re-register** card
5. **Configure** (`WD_AI_Trig_Config`, `WD_AI_ContBufferSetup`, etc.)

`WD_AI_ContBufferReset` must be called **before** `WD_AI_Trig_Config` — calling it
after wipes the trigger configuration and produces immediate fStop.

---

## Key files

| File | Purpose |
|---|---|
| `DeviceControl_FullThreaded.c` | Main application — all callbacks, threads, ADC pipeline |
| `DeviceControl_FullThreaded.uir` | UI Editor resource (binary) |
| `DeviceControl_FullThreaded.h` | Generated UI header (constants, panel handles) |
| `DeviceControl_Full_Threaded.cws` | CVI project file (IDE state — diffs noisy) |
| `dds.h` / `dds.c` | AD9914 DDS register programming |
| `Wd-dask64.h` | ADLINK WD-DASK driver headers |
| `WORKING.md` | Full session-by-session investigation log |
| `TriggerAlignment_ImplementationNotes.md` | Chirp re-alignment algorithm notes |

---

## Reference systems

- **BB24 / Blunderbuss** (`Blunderbuss_code_DB1.c`) — prior radar system on the
  same PCI-9846H card. Authoritative reference for ADC calibration sequencing,
  FFT processing, and range-Doppler patterns. Cite as the canonical pattern when
  in doubt about ADLINK API usage on this card.

---

## Diagnostic status line

The 0.5 s status timer prints:

```text
Poll:NNNN HR:N FS:N | Sw:N Sd:N | PTrg:N PDn:N | OVR:N
```

| Field | Meaning |
|---|---|
| `Poll` | Poll-loop iterations (high = thread is healthy) |
| `HR` | Half-ready events (= half-buffers received from DMA) |
| `FS` | fStop count (should be 0 during normal acquisition) |
| `Sw` | Save-thread `fwrite` count |
| `Sd` | Save drops (queue was full at write attempt) |
| `PTrg` / `PDn` | Plot posted / plot deferred-callback completed |
| `OVR` | DMA overrun events (edge-detected, not latched-state count) |

Healthy long run: `HR` grows linearly, `Sw == HR` (or `Sw + Sd == HR`),
`OVR = 0`, `FS = 0`.

---

## When updating this file

This file IS the portable memory across machines. If you discover something
non-obvious about the project that future-you would need to know on day one
of a fresh checkout — add it here, not just in conversation. Keep it concise;
push detail to `WORKING.md` and link.
