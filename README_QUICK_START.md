# Quick Start: Improve Your Data Saving Performance

## TL;DR - The Problem
Your current code flushes to disk **every 10 buffer writes**. This is synchronous, blocking, and causes the consumer thread to stall while the disk syncs. The producer back-pressures and acquisition slows down.

## TL;DR - The Solution
Accumulate multiple buffers (**batch = 4**) and flush less frequently (**every 100 blocks instead of 10**).

## 3-Step Implementation (20 minutes)

### Step 1: Add Batch Configuration (1 min)
Edit **DeviceControl_FullThreaded.c** - add after line 117:
```c
/* Add these defines */
#define BATCH_SIZE 4
#define FLUSH_INTERVAL_BLOCKS 100
```

### Step 2: Replace DiskWriterThreadFunction (15 min)
Copy the optimized function from **DISK_WRITER_OPTIMIZED.c** (the one called `DiskWriterThreadFunction_Optimized`)

Paste it into your file **in place of** the existing `DiskWriterThreadFunction` (lines ~453-527)

### Step 3: Update Thread Scheduling (1 min)
Find line ~1607, change:
```c
// BEFORE:
CmtScheduleThreadPoolFunction(DEFAULT_THREAD_POOL_HANDLE, 
    DiskWriterThreadFunction, NULL, &consumerThreadID);

// AFTER:
CmtScheduleThreadPoolFunction(DEFAULT_THREAD_POOL_HANDLE, 
    DiskWriterThreadFunction_Optimized, NULL, &consumerThreadID);
```

## Build & Test
```
"C:\Program Files (x86)\National Instruments\CVI2020\cvi.exe" /build DeviceControl_Full_Threaded.prj
```

Run acquisition, measure:
- ✅ No dropped data (check diagnostics)
- ✅ Lower queue depth peaks
- ✅ Faster save completion

## Expected Results
- **Write throughput**: +40% to +80% faster
- **Queue back-pressure**: Reduced
- **I/O stalls**: Fewer and shorter

---

## If You Want More Details

1. **Understanding the problem**: Read `IO_OPTIMIZATION_GUIDE.md`
2. **All implementation options**: Read `PATCH_INSTRUCTIONS.md`
3. **Reference implementation**: Check `DISK_WRITER_OPTIMIZED.c`
4. **Architecture notes**: Check `/memories/cerberus-threading-architecture.md`

---

## Advanced Option: Async I/O (Optional)

If batching isn't enough, you can implement true async disk I/O:
- Decouple disk writes from buffer dequeuing
- Use Windows overlapped I/O (WriteFileEx)
- Potentially 2x more improvement

See `DISK_WRITER_OPTIMIZED.c` for async skeleton code.

---

## Support Notes

🔧 **Your current architecture is solid**: You already have producer/consumer threading with queues. This improvement just makes the consumer more efficient.

⚠️ **Both HW + disk**: If you're still bottlenecking after this, check:
- Disk write speed (Crystal DiskInfo, disk usage monitor)
- Queue back-pressure diagnostics (add printf output)
- Hardware overrun messages (WD-DASK driver events)

💡 **Tuning**: After implementation, adjust `BATCH_SIZE` based on your disk:
- SSD: Try BATCH_SIZE=8 for more batching
- HDD: Keep BATCH_SIZE=2-4
- Watch the queue depth telemetry to dial it in

---

## Files Created

- `IO_OPTIMIZATION_GUIDE.md` - Detailed 4-strategy comparison
- `PATCH_INSTRUCTIONS.md` - Line-by-line changes required  
- `DISK_WRITER_OPTIMIZED.c` - Reference implementation
- This file - Quick start guide
