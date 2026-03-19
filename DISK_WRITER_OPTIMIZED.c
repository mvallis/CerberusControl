#include <windows.h>
#include <time.h>

/*===========================================================================
 * QUICK IMPLEMENTATION: Batch-Based Async-Friendly Disk Writer
 * 
 * Combines:
 * - Strategy 1: Reduced fflush() frequency
 * - Strategy 2: Batched buffer accumulation
 * 
 * Replace the DiskWriterThreadFunction in DeviceControl_FullThreaded.c
 * with this improved version.
 * 
 * Expected improvement: +40% to +80% write throughput
 *===========================================================================*/

/* Add these to the globals section of DeviceControl_FullThreaded.c */
#define BATCH_SIZE 4                    /* Accumulate 4 dequeued buffers before write */
#define FLUSH_INTERVAL_BLOCKS 100       /* Flush every 100 accumulated blocks */
#define FLUSH_INTERVAL_MS 2000          /* Or every 2 seconds, whichever comes first */

typedef struct {
    U16 *buffer;
    U32 size;
} BufferSlot;

/*===========================================================================
 * IMPROVED: Batch-Aware Disk Writer Thread
 *===========================================================================*/
int CVICALLBACK DiskWriterThreadFunction_Optimized (void *functionData)
{
    /* Allocate batch storage */
    BufferSlot batch[BATCH_SIZE];
    for (int i = 0; i < BATCH_SIZE; i++) {
        batch[i].buffer = (U16*)malloc(halfBufferSize * sizeof(U16));
        if (!batch[i].buffer) return -1;
        batch[i].size = halfBufferSize;
    }
    
    U32 buffersInBatch = 0;
    U32 totalWriteCount = 0;
    time_t lastFlushTime = time(NULL);
    
    while (isAcquiring) {
        /* Attempt to read from queue (100ms timeout) */
        U16 *queueBuffer = (U16*)malloc(halfBufferSize * sizeof(U16));
        if (!queueBuffer) break;
        
        int itemsRead = CmtReadTSQData(dmaQueue, queueBuffer, 1, 100, 0);
        
        if (itemsRead > 0) {
            /* Successfully dequeued a buffer */
            if (currentQueueDepth > 0) {
                currentQueueDepth--;
            }
            
            /* Add to batch */
            memcpy(batch[buffersInBatch].buffer, queueBuffer, halfBufferSize * sizeof(U16));
            buffersInBatch++;
            
            /* Check if batch is full or flush interval exceeded */
            int shouldFlush = 0;
            if (buffersInBatch >= BATCH_SIZE) {
                shouldFlush = 1;
            } else if (buffersInBatch > 0) {
                time_t currentTime = time(NULL);
                if (difftime(currentTime, lastFlushTime) >= (FLUSH_INTERVAL_MS / 1000.0)) {
                    shouldFlush = 1;  /* Time-based flush */
                }
            }
            
            /* Write entire batch to disk */
            if (shouldFlush && recordFile != NULL) {
                for (U32 j = 0; j < buffersInBatch; j++) {
                    size_t written = fwrite(batch[j].buffer, sizeof(U16), 
                                           halfBufferSize, recordFile);
                    if (written != halfBufferSize) {
                        /* Disk write error - log and continue */
                        fprintf(stderr, "DISK_WRITE_ERROR: Expected %u items, wrote %zu\n",
                               halfBufferSize, written);
                    }
                }
                
                /* Single fflush for entire batch */
                fflush(recordFile);
                lastFlushTime = time(NULL);
                totalWriteCount++;
                
                /* Update UI status every 10 batches (every ~40 buffers) */
                if (totalWriteCount % 10 == 0) {
                    char msg[64];
                    sprintf(msg, "Write: %u batches (%u buf)", totalWriteCount, 
                           totalWriteCount * buffersInBatch);
                    SetCtrlVal(adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, msg);
                }
                
                /* Update trigger count */
                recordedTrigs += (buffersInBatch * halfBufferSize / ADC_NUM_CHANNELS) / samplesPerChirp;
                
                /* Reset batch */
                buffersInBatch = 0;
            }
        }
        
        free(queueBuffer);
    }
    
    /* Flush any remaining partial batch on shutdown */
    if (buffersInBatch > 0 && recordFile != NULL) {
        for (U32 j = 0; j < buffersInBatch; j++) {
            fwrite(batch[j].buffer, sizeof(U16), halfBufferSize, recordFile);
        }
        fflush(recordFile);
    }
    
    /* Final stats before exit */
    char finalMsg[128];
    sprintf(finalMsg, "Write complete: %u batches, %u total samples",
           totalWriteCount, totalWriteCount * BATCH_SIZE * (halfBufferSize / ADC_NUM_CHANNELS));
    SetCtrlVal(adcTabHandle, TABPANEL_2_ADC_MSG_STATUS, finalMsg);
    
    /* Cleanup */
    for (int i = 0; i < BATCH_SIZE; i++) {
        if (batch[i].buffer) free(batch[i].buffer);
    }
    
    return 0;
}


/*===========================================================================
 * ALTERNATIVE: Async I/O Windows Implementation (Advanced)
 * 
 * Use this version if you implement Strategy 3 (Overlapped I/O)
 * REQUIRES: Windows.h, asyncfile handle setup in ADC_RecordStart
 *===========================================================================*/

typedef struct {
    OVERLAPPED overlapped;
    U16 *buffer;
    U32 size;
    DWORD bytesWritten;
} AsyncWriteOp;

/*
 * Advanced implementation requires:
 * 1. Create file with FILE_FLAG_OVERLAPPED in ADC_RecordStart
 * 2. Maintain pool of PENDING_WRITES async operations
 * 3. Wait for completion before reusing slot
 * 4. Flush all pending on shutdown
 * 
 * Example skeleton (full implementation 30-40 lines additional):
 * 
 *   static const U32 PENDING_WRITES = 4;
 *   static AsyncWriteOp asyncOps[PENDING_WRITES];
 *   
 *   while (isAcquiring) {
 *       itemsRead = CmtReadTSQData(dmaQueue, queueBuffer, 1, 100, 0);
 *       if (itemsRead > 0) {
 *           AsyncWriteOp *slot = &asyncOps[nextSlot];
 *           WaitForSingleObject(slot->overlapped.hEvent, INFINITE);
 *           
 *           memcpy(slot->buffer, queueBuffer, halfBufferSize * sizeof(U16));
 *           
 *           WriteFile(asyncFile, slot->buffer,
 *                     halfBufferSize * sizeof(U16), NULL,
 *                     &slot->overlapped);
 *           
 *           nextSlot = (nextSlot + 1) % PENDING_WRITES;
 *       }
 *   }
 */


/*===========================================================================
 * INTEGRATION INSTRUCTIONS
 *===========================================================================
 * 
 * 1. Copy DiskWriterThreadFunction_Optimized() to DeviceControl_FullThreaded.c
 * 
 * 2. Add defines and typedef to globals section:
 *    #define BATCH_SIZE 4
 *    #define FLUSH_INTERVAL_BLOCKS 100
 *    #define FLUSH_INTERVAL_MS 2000
 *    typedef struct { U16 *buffer; U32 size; } BufferSlot;
 * 
 * 3. Replace the scheduling call at line ~1607:
 *    OLD: CmtScheduleThreadPoolFunction(DEFAULT_THREAD_POOL_HANDLE,
 *         DiskWriterThreadFunction, NULL, &consumerThreadID);
 *    NEW: CmtScheduleThreadPoolFunction(DEFAULT_THREAD_POOL_HANDLE,
 *         DiskWriterThreadFunction_Optimized, NULL, &consumerThreadID);
 * 
 * 4. Recompile and test:
 *    - Monitor queue depth (should stay low)
 *    - Verify no dropped buffers
 *    - Check file integrity (read and validate header/data)
 * 
 * 5. Tune BATCH_SIZE and FLUSH_INTERVAL based on your system:
 *    - Faster disk: larger BATCH_SIZE (8) + longer intervals
 *    - Slower disk: smaller BATCH_SIZE (2) + shorter intervals
 *    - Watch queue depth in diagnostics panel
 * 
 *===========================================================================*/
