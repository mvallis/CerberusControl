# Disk I/O Performance Optimization Guide

## Current Architecture Analysis

Your code **already implements producer-consumer threading**, which is excellent:
- ✅ Producer thread (HardwarePollThreadFunction): Polls DMA buffers and enqueues
- ✅ Consumer thread (DiskWriterThreadFunction): Dequeues and writes to disk
- ✅ Thread-safe queue with back-pressure management
- ✅ Adaptive queue depth monitoring

## Identified Performance Bottlenecks

### 1. **Blocking fflush() Every 10 Blocks**
**Problem**: `fflush()` is a synchronous blocking operation that forces disk I/O immediately.
- **Impact**: Consumer thread stalls, queue fills up, producer back-pressure triggers
- **Location**: Line 480 in DiskWriterThreadFunction
- **Current behavior**: Every 10 successful writes, the thread blocks until disk sync completes

### 2. **Single Sequential Consumer Thread**
**Problem**: Only one thread writing to disk means no parallelization of I/O operations.
- **Impact**: Limited to disk subsystem's single-file write throughput
- **Bottleneck**: Expensive seeks/flushes cause the single thread to pause

### 3. **Synchronous File I/O**
**Problem**: Using standard ANSI `fwrite()` doesn't allow true async disk operations in Windows.
- **Impact**: Thread must wait for disk I/O to complete before next dequeue
- **Missing**: Windows overlapped I/O (async writes) capability

---

## 4 Optimization Strategies (In Order of Impact)

### Strategy 1: **Deferred fflush() with Time-Based Batching** ⭐ EASY (5 min)
**Approach**: Instead of flushing every 10 blocks, accumulate writes and flush less frequently.

**Implementation**:
```c
/* In DiskWriterThreadFunction global context: */
static HANDLE flushTimerHandle = INVALID_HANDLE_VALUE;
static U32 blocksSinceLastFlush = 0;
static const U32 MAX_BLOCKS_BEFORE_FLUSH = 100;  /* Flush every ~100 blocks instead of 10 */
static const U32 MAX_FLUSH_INTERVAL_MS = 1000;   /* Force flush every 1 second max */

/* In main loop: */
if (writeCounter % MAX_BLOCKS_BEFORE_FLUSH == 0 || (time since last flush > 1000ms)) {
    fflush(recordFile);
}
```

**Expected Impact**: +20-40% write throughput (fewer I/O stalls)
**Risk**: Low (minimal code change)

---

### Strategy 2: **Double-Buffered Queue Writing** ⭐⭐ MODERATE (15 min)
**Approach**: Instead of writing each dequeued buffer immediately, accumulate multiple buffered chunks before writing.

**Implementation**:
```c
/* Add to globals: */
typedef struct {
    U16 *data;
    U32 size;
} QueuedBuffer;

#define BATCH_SIZE 4  /* Accumulate 4 dequeued buffers before one large write */

/* In DiskWriterThreadFunction: */
int CVICALLBACK DiskWriterThreadFunction (void *functionData)
{
    U16 **batchBuffers = malloc(BATCH_SIZE * sizeof(U16*));
    for (int i = 0; i < BATCH_SIZE; i++) {
        batchBuffers[i] = malloc(halfBufferSize * sizeof(U16));
    }
    
    U32 buffersInBatch = 0;
    
    while (isAcquiring) {
        int itemsRead = CmtReadTSQData(dmaQueue, batchBuffers[buffersInBatch], 1, 100, 0);
        
        if (itemsRead > 0) {
            currentQueueDepth--;
            buffersInBatch++;
            
            /* When batch is full, write all at once */
            if (buffersInBatch >= BATCH_SIZE) {
                if (recordFile) {
                    for (int i = 0; i < BATCH_SIZE; i++) {
                        fwrite(batchBuffers[i], sizeof(U16), halfBufferSize, recordFile);
                    }
                    recordedTrigs += (BATCH_SIZE * halfBufferSize / ADC_NUM_CHANNELS) / samplesPerChirp;
                    fflush(recordFile);  /* Single flush for entire batch */
                }
                buffersInBatch = 0;
            }
        }
    }
    
    /* Flush remaining partial batch */
    if (buffersInBatch > 0 && recordFile) {
        for (int i = 0; i < buffersInBatch; i++) {
            fwrite(batchBuffers[i], sizeof(U16), halfBufferSize, recordFile);
        }
        fflush(recordFile);
    }
    
    /* Cleanup */
    for (int i = 0; i < BATCH_SIZE; i++) free(batchBuffers[i]);
    free(batchBuffers);
    return 0;
}
```

**Expected Impact**: +30-50% write throughput (fewer flushes, larger sequential writes)
**Risk**: Low (isolated to consumer thread)

---

### Strategy 3: **Windows Overlapped (Async) I/O** ⭐⭐⭐ ADVANCED (30-45 min)
**Approach**: Use Windows async file I/O to completely decouple disk write from data dequeuing.

**Benefits**:
- Consumer thread can dequeue while disk write happens in background
- Multiple pending write operations improve throughput
- Significantly reduces stall time

**Implementation**:
```c
/* Add to globals: */
typedef struct {
    OVERLAPPED overlapped;
    U16 *buffer;
    U32 bufferSize;
} AsyncWriteContext;

#define PENDING_WRITES 4  /* Keep 4 write operations in flight */

/* Add to helper functions: */
static HANDLE asyncFile = INVALID_HANDLE_VALUE;
static AsyncWriteContext pendingWrites[PENDING_WRITES];
static U32 nextWriteIndex = 0;

/* Initialize in ADC_RecordStart(): */
TCHAR wideRecordPath[512];
MultiByteToWideChar(CP_ACP, 0, recordPath, -1, wideRecordPath, 512);
asyncFile = CreateFile(wideRecordPath, GENERIC_WRITE, 0, NULL,
                      CREATE_ALWAYS, FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING, NULL);

/* In DiskWriterThreadFunction: */
int CVICALLBACK DiskWriterThreadFunction (void *functionData)
{
    U16 *queueBuffer = malloc(halfBufferSize * sizeof(U16));
    if (!queueBuffer) return -1;
    
    /* Initialize async write contexts */
    for (int i = 0; i < PENDING_WRITES; i++) {
        pendingWrites[i].buffer = malloc(halfBufferSize * sizeof(U16));
        memset(&pendingWrites[i].overlapped, 0, sizeof(OVERLAPPED));
        pendingWrites[i].overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    }
    
    while (isAcquiring) {
        int itemsRead = CmtReadTSQData(dmaQueue, queueBuffer, 1, 100, 0);
        
        if (itemsRead > 0) {
            currentQueueDepth--;
            
            /* Check if current write slot is complete */
            AsyncWriteContext *ctx = &pendingWrites[nextWriteIndex];
            if (WaitForSingleObject(ctx->overlapped.hEvent, 0) == WAIT_OBJECT_0) {
                /* Previous write completed, ready for new one */
                memcpy(ctx->buffer, queueBuffer, halfBufferSize * sizeof(U16));
                
                ResetEvent(ctx->overlapped.hEvent);
                WriteFile(asyncFile, ctx->buffer, 
                         halfBufferSize * sizeof(U16), NULL, 
                         &ctx->overlapped);
                
                recordedTrigs += (halfBufferSize / ADC_NUM_CHANNELS) / samplesPerChirp;
                nextWriteIndex = (nextWriteIndex + 1) % PENDING_WRITES;
            } else {
                /* Write slot still busy - back-pressure will handle this */
                queueBackPressure = 1;
            }
        }
    }
    
    /* Wait for all pending writes to complete */
    for (int i = 0; i < PENDING_WRITES; i++) {
        WaitForSingleObject(pendingWrites[i].overlapped.hEvent, INFINITE);
        CloseHandle(pendingWrites[i].overlapped.hEvent);
        free(pendingWrites[i].buffer);
    }
    
    free(queueBuffer);
    return 0;
}

/* In ADC_RecordFinish(): */
static void ADC_RecordFinish (void)
{
    if (asyncFile == INVALID_HANDLE_VALUE) return;
    
    /* Flush all pending writes */
    FlushFileBuffers(asyncFile);
    CloseHandle(asyncFile);
    asyncFile = INVALID_HANDLE_VALUE;
}
```

**Expected Impact**: +50-100% write throughput (true async, no stalls)
**Risk**: Moderate (requires Windows async I/O understanding)
**Recommendation**: Use with Strategy 2 for best results

---

### Strategy 4: **Multiple Writer Threads (File Rotation)** ⭐⭐⭐⭐ COMPLEX (45-60 min)
**Approach**: Split data across multiple files with dedicated writer threads (e.g., rolling files).

**Use case**: Prevents single-file bottleneck, enables multi-core disk throughput
**Example**: File0.bin (0-file1.bin (1-file2.bin, rotating writes

```c
#define NUM_WRITER_THREADS 2
static CmtThreadFunctionID writerThreadPool[NUM_WRITER_THREADS];
static CmtTSQHandle writerQueues[NUM_WRITER_THREADS];
static FILE *writerFiles[NUM_WRITER_THREADS];

/* Each writer thread dequeues from its own queue and writes independently */
```

**Expected Impact**: +100-200% (scales with number of threads + disk parallelism)
**Risk**: High (complexity with synchronization, file format handling)

---

## Recommended Approach (Balanced)

### Phase 1: Quick Win (5 minutes)
✅ **Implement Strategy 1** (increase flush interval from 10 to 100 blocks)
- Change line 480: `if (writeCounter % 100 == 0) fflush()`

### Phase 2: Medium Term (15 minutes)
✅ **Implement Strategy 2** (batch 4 buffers before writing)
- Accumulate dequeued buffers, write in larger chunks
- Single `fflush()` per batch reduces I/O overhead dramatically

### Phase 3: Advanced (if needed)
⚡ **Implement Strategy 3** (async I/O) only if Phase 1+2 insufficient
- Complex but provides true non-blocking I/O
- Worth it if you're hitting single-threaded write limits

---

## Testing Strategy

1. **Baseline**: Measure current acquisition + save rate (blocks/second, queue depth, dropped samples)
   ```c
   /* Add diagnostic output every 5 seconds */
   if (heartbeatCounter % 50 == 0) {
       printf("Queue depth: %u, Peak: %u, Dropped: %u, Blocks written: %u\n",
              currentQueueDepth, maxQueueDepthObserved, 
              bufferCopiesDropped, writeCounter);
   }
   ```

2. **Phase 1 Test**: Run with `FLUSH_INTERVAL = 100`, measure queue behavior

3. **Phase 2 Test**: Run with batch size of 4, measure throughput improvement

4. **Monitor**:
   - Queue depth (should stay low mean, not spike high)
   - Dropped buffers (should be 0)
   - Write throughput (blocks/second)
   - File integrity (verify recorded data)

---

## Quick Implementation: Strategy 1 + 2 Combined

If you want the fastest improvement with minimal risk, here's a single focused change that combines batching + reduced flush:

1. Reduce flush frequency: `writeCounter % 100 == 0` (not 10)
2. Batch 2-4 dequeued buffers before each write
3. Monitor with queue depth output

Expected result: **+40% to +80% improvement** with under 20 minutes implementation time.
