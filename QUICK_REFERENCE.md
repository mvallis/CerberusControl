# ADC Double Buffer Implementation - Quick Reference

## Files Modified
- **[DeviceControl_FullThreaded.c](DeviceControl_FullThreaded.c)** — Core implementation

## Files Created (Documentation)
- **[IMPLEMENTATION_NOTES.md](IMPLEMENTATION_NOTES.md)** — Technical architecture & design decisions
- **[TESTING_PROCEDURES.md](TESTING_PROCEDURES.md)** — 9-part test suite with validation scripts

---

## Line-by-Line Changes

### Global Variables (Line ~101-114)
```c
/* ---- Queue Management (Data-Copy Strategy) ---- */
static U32    queueMaxCapacity        = 20;   /* Adaptively adjusted 20-25 */
static U32    queueHighWater          = 16;   /* Back-pressure trigger (80%) */
static U32    queueLowWater           = 4;    /* Resume trigger (20%) */
static volatile int queueBackPressure = 0;    /* Producer pause flag */
static U32    bufferCopiesDropped     = 0;    /* Overrun diagnostic counter */
static U32    maxQueueDepthObserved   = 0;    /* Peak depth this session */
static U32    lastSessionMaxDepth     = 0;    /* Previous session peak (for adaptive) */
```

### HardwarePollThreadFunction (Line ~525-600)
**Before:** Queued buffer pointers; acknowledged hardware immediately
**After:**
- Allocates temporary `bufferCopy` on startup
- Monitors `CmtGetTSQAttribute(dmaQueue, ATTR_TSQ_COUNT)`
- Implements back-pressure: activates if depth ≥ queueHighWater, deactivates if ≤ queueLowWater
- Copies buffer data: `memcpy(bufferCopy, src, halfBufferSize * sizeof(U16))`
- Enqueues copy: `CmtWriteTSQData(dmaQueue, bufferCopy, 1, 0, NULL)`
- **Only then** acknowledges hardware: `WD_AI_AsyncDblBufferHandled(adcCard)`
- Adaptive sleep: 1ms normal, 10ms when back-pressure active
- Tracks diagnostics: `maxQueueDepthObserved`, `bufferCopiesDropped`

### DiskWriterThreadFunction (Line ~460-515)
**Before:** Dereferenced buffer pointers from queue
**After:**
- Receives full buffer data from queue (not pointers)
- No pointer dereferencing needed
- Periodic `fflush` every 10 write blocks (prevents I/O spike at end)
- Plotting pipeline unchanged (extracts last complete chirp)
- Robust queue drain on shutdown

### AdcStartCB (Line ~1545-1580)
**Before:** Fixed queue initialization with size 20
**After:**
- Adaptive capacity based on `lastSessionMaxDepth`:
  - If previous peak > 12: capacity = 25, watermarks 20/5
  - Else: capacity = 20, watermarks 16/4
- Initialize `CmtNewTSQ(queueMaxCapacity, halfBufferSize * sizeof(U16), ...)`
- Reset diagnostics: `bufferCopiesDropped = 0`, `maxQueueDepthObserved = 0`, `queueBackPressure = 0`

### AdcStopCB (Line ~1712-1760)
**Before:** Simple shutdown message
**After:**
- Save session peak: `lastSessionMaxDepth = maxQueueDepthObserved`
- Report detailed status:
  - Clean: "Stopped cleanly. Peak queue depth: X/Y"
  - Overrun: "Stopped. OVERRUN: N buffers dropped, peak queue: X/Y"

### AdcConfigureCB (Line ~1495-1513)
**Before:** Fixed 50ms buffer target
**After:**
- Check if previous session had overruns: `if (bufferCopiesDropped > 0 || lastSessionMaxDepth > queueHighWater)`
- If true: scale target buffer by 1.1× (10% increase)
- Log adaptive action to status bar

---

## Architecture Summary

### Old (Pointer-Based) Flow
```
Hardware → dmaBuffer1/2 → HardwarePollThread queues pointer → DiskWriterThread dereferences pointer → Disk
                                                               (RACE CONDITION: hardware may overwrite while dereferencing)
```

### New (Data-Copy) Flow
```
Hardware → dmaBuffer1/2 → HardwarePollThread copies data → queues buffer copy → DiskWriterThread reads pre-copied data → Disk
                              ↓                                                        (NO RACE: hardware can't touch data)
                          WD_AI_AsyncDblBufferHandled (only after successful copy+enqueue)
                              ↓
                          Hardware can now overwrite buffer for next sample
```

---

## Key Features

✅ **Data-Copy Queue Strategy**
- Eliminates buffer access races
- Queue stores full `halfBufferSize * sizeof(U16)` bytes
- Producer copies data before telling hardware buffer is free

✅ **Back-Pressure Management**
- Queue depth monitored continuously
- Triggers at 80% (queueHighWater), releases at 20% (queueLowWater)
- Producer sleeps 1ms → 10ms during pressure to allow consumer drain

✅ **Adaptive Buffer Sizing**
- If previous session had `bufferCopiesDropped > 0` OR high queue depth:
  - Increases queueMaxCapacity: 20 → 25
  - Adjusts watermarks: 16/4 → 20/5
  - Scales halfBufferSize +10% in AdcConfigureCB
- Self-learning without user intervention

✅ **Diagnostic Counters**
- `bufferCopiesDropped`: How many enqueues failed (expected = 0)
- `maxQueueDepthObserved`: Peak throughout session
- `lastSessionMaxDepth`: Saved for next session's adaptive decisions
- `queueBackPressure`: Real-time pause flag

✅ **Safe Shutdown**
- Gracefully waits for both threads to complete
- Flushes entire queue to disk (no data loss)
- Reports final diagnostics including overrun status

---

## Critical Fixed Behavior

### BEFORE
```c
// Producer thread
U16 *src = (activeBuf == 0) ? dmaBuffer1 : dmaBuffer2;
queueStatus = CmtWriteTSQData(dmaQueue, src, 1, ...);  // Queue POINTER
if (queueStatus < 0) { /* handle error */ }
WD_AI_AsyncDblBufferHandled(adcCard);  // Release buffer immediately
activeBuf = 1 - activeBuf;

// Consumer thread
int itemsRead = CmtReadTSQData(dmaQueue, localBuf, ...);
// localBuf now points to dmaBuffer1 or dmaBuffer2
// BUT dmaBuffer might already be overwritten if hardware filled it!
```

### AFTER
```c
// Producer thread
U16 *src = (activeBuf == 0) ? dmaBuffer1 : dmaBuffer2;
memcpy(bufferCopy, src, halfBufferSize * sizeof(U16));  // COPY DATA
queueStatus = CmtWriteTSQData(dmaQueue, bufferCopy, 1, ...);  // Queue COPY
if (queueStatus < 0) { /* handle error */ }
WD_AI_AsyncDblBufferHandled(adcCard);  // Release buffer AFTER copy confirmed
activeBuf = 1 - activeBuf;

// Consumer thread
int itemsRead = CmtReadTSQData(dmaQueue, queueBuffer, ...);
// queueBuffer contains SAFE COPY of data
// Hardware cannot affect it
```

---

## Verification Checklist

Before declaring success:

- [ ] Code compiles without errors in CVI
- [ ] Test 1 passes: Clean acquisition, peak queue < 10/20
- [ ] Test 2 passes: Recording file valid, disc write loop working
- [ ] Test 3 passes: Back-pressure engages/disengages at thresholds
- [ ] Test 4 passes: Adaptive sizing increases capacity after overrun
- [ ] Test 6 passes: MATLAB validation shows zero sample drops
- [ ] Live plots update smoothly during acquisition
- [ ] Single-shot capture produces clean FFT

---

## Global State Management

| Variable | Init Value | When Changed | By Whom | Visible In |
|---|---|---|---|---|
| `queueMaxCapacity` | 20 | AdcStartCB | Based on `lastSessionMaxDepth` | — |
| `queueHighWater` | 16 | AdcStartCB | Adaptive adjust | — |
| `queueLowWater` | 4 | AdcStartCB | Adaptive adjust | — |
| `queueBackPressure` | 0 | HardwarePollThreadFunction | When queue depth crosses threshold | (Debug only) |
| `bufferCopiesDropped` | 0 | AdcStartCB, HardwarePollThreadFunction | On failed enqueue | AdcStopCB status msg |
| `maxQueueDepthObserved` | 0 | AdcStartCB, HardwarePollThreadFunction | Continuously updated | AdcStopCB status msg |
| `lastSessionMaxDepth` | 0 | AdcStopCB | At shutdown | AdcStartCB (for adaptive) |

---

## Testing Command Summary

**Minimal smoke test (5 min):**
```
1. Register card → Configure → Start (no disk write)
2. Watch FFT update smoothly for 10 seconds
3. Stop → verify "Stopped cleanly. Peak queue depth: <10/20"
```

**Full validation (45 min):**
```
See TESTING_PROCEDURES.md for 9-part comprehensive test suite
```

---

## Rollback Path

If issues arise, revert to file version prior to these changes. Core dependencies:
- `dmaBuffer1`, `dmaBuffer2` (unchanged API)
- `WD_AI_Async*` calls (CVI compatibility unchanged)
- `CmtTSQ*` calls (compatible with any queue size)

No breaking changes to callbacks or UI.

---

**Status**: ✅ Implementation Complete  
**Date**: March 17, 2026  
**Ready for**: CVI compilation + testing
