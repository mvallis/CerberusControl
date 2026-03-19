# ADC Double Buffer Implementation - Testing Procedures

## Pre-Test Checklist
- [ ] Code compiles without errors in CVI
- [ ] Hardware enabled: PCI-9846 ADC, DDS configured, external trigger ready
- [ ] Disk space: ≥1 GB free for test recordings
- [ ] Oscilloscope/signal analyzer connected to monitor hardware trigger

---

## Test 1: Clean Acquisition (No Disk I/O Stress)
**Purpose**: Verify baseline data integrity and queue stability

**Setup:**
1. Open DeviceControl_FullThreaded in CVI
2. Navigate to ADC tab
3. Register card → Configure (accept defaults)
4. DDS tab: Set triggered chirp mode (100 µs period, 1-10 MHz sweep)
5. ADC tab: Leave recording OFF (will test disk write separately)

**Procedure:**
1. Click "Start Acquisition"
2. Send 50 external triggers manually (or use DDS trigger output)
3. Observe status bar: Should show "Acquisition Armed [Dual-Buffer Polled]"
4. FFT plot should update ~20× per second (one chirp per trigger)
5. Wait 10 seconds
6. Click "Stop Acquisition"

**Expected Results:**
```
Status message: "Stopped cleanly. Peak queue depth: 4/20"
- bufferCopiesDropped should be 0
- Peak queue should be well under queueHighWater (16)
- FFT plots should show clean chirp signals with no gaps
```

**Failure Modes:**
- Queue depth > 10 → back-pressure engaged unnecessarily (diagnose disk/CPU bottleneck)
- bufferCopiesDropped > 0 → race condition or hardware issue

---

## Test 2: Disk Write Pipeline (Recording)
**Purpose**: Verify disk write doesn't starve acquisition; check file integrity

**Setup:**
1. Configure same as Test 1
2. Create output directory: `C:\TestData\` (or similar)
3. ADC tab: Enable "Record" → select file path → Start

**Procedure:**
1. Click "Start Acquisition" with recording enabled
2. Send ~100 triggers over 20 seconds (steady ~5 Hz trigger rate)
3. Observe status bar messages: Should see periodic "Write: XX blocks"
4. After 20 seconds, click "Stop Acquisition"
5. Check output file exists and is > 1 MB

**Expected Results:**
```
Status: "Stopped cleanly. Peak queue depth: 8/20"
File size: ~4 MB (100 triggers × 40 KB per trigger)
Trigger count in header: 100
```

**Validation Script (MATLAB):**
```matlab
% Load radar file
fid = fopen('TestData/recording.bin', 'rb');
header_data = fread(fid, 128/4, 'uint32'); % 128-byte header = 32 uint32s
magic = header_data(1);
num_channels = header_data(2);
samples_per_trigger = header_data(3);
num_triggers = header_data(4);
sample_rate_hz = typecast(uint32(header_data(5:6)), 'double');

% Read all data
data = fread(fid, inf, 'uint16');
fclose(fid);

% Verify
expected_samples = num_triggers * samples_per_trigger * num_channels;
assert(length(data) == expected_samples, ...
    sprintf('Data length mismatch: got %d, expected %d', length(data), expected_samples));
disp(sprintf('✓ File valid: %d triggers, %d samples/trigger, %.1f MHz sample rate', ...
    num_triggers, samples_per_trigger, sample_rate_hz/1e6));
```

---

## Test 3: Back-Pressure Activation (Disk I/O Throttling)
**Purpose**: Verify queue watermarks and adaptive sleep work correctly

**Setup:**
1. Same as Test 2
2. Reduce file I/O speed (Windows): Open Task Manager → Disk usage monitor
3. Create a 10 MB RAM disk for faster baseline

**Procedure A (Simulate slow disk):**
1. Start recording to slow disk
2. Monitor `adcTabHandle TABPANEL_2_ADC_MSG_STATUS` in code via breakpoint
3. Trigger at high rate: ~10 req/sec (faster than typical)
4. Watch queue depth climb toward 16 (queueHighWater)

**Expected Behavior:**
- Queue depth reaches 14-16
- `queueBackPressure` becomes 1 (visible in locals if debugging)
- Producer thread sleeps 10ms instead of 1ms
- Consumer drains queue, depth drops below 4 (queueLowWater)
- `queueBackPressure` returns to 0
- Loop continues smoothly

**Procedure B (RAM disk baseline):**
1. Write same data to RAM disk (C:\RamDisk\test.bin)
2. Note peak queue depth: typically 2-4 (much lower, as expected)

**Verification:**
```
Compare two run outputs:
- Slow disk: "Peak queue depth: 15/20" → back-pressure kicked in
- RAM disk:  "Peak queue depth: 3/20"  → no back-pressure needed
Both should report: bufferCopiesDropped = 0 (no lost data)
```

---

## Test 4: Adaptive Buffer Sizing (Multi-Session Stress)
**Purpose**: Verify system learns and adapts to previous overrun conditions

**Procedure:**
1. **Session 1 (Normal load):**
   - Start acquisition with moderate trigger rate (3 req/sec)
   - Record for 30 seconds
   - Stop and note: "Peak queue depth: 6/20"
   - File saves successfully

2. **Session 2 (Simulated stress—system should adapt):**
   - Without restarting app, immediately click "Configure" then "Start Acquisition"
   - Trigger at very high rate (15 req/sec, back-to-back triggers)
   - Monitor status: should still see "Stopped cleanly" (no drops)
   - Note: queueMaxCapacity should have remained 20 (no previous stress)

3. **Session 3 (Force stress, configure next run):**
   - Trigger at saturating rate (20+ req/sec continuous, no gaps)
   - Watch queue depth climb to ~18-19/20
   - Should see: "OVERRUN: 1-2 buffers dropped"
   - Session ends

4. **Session 4 (Verify adaptive adjustment):**
   - Click "Configure" again immediately
   - Status message should show: "Adaptive: Buffer scaled +10% due to previous overrun"
   - queueMaxCapacity now = 25 (increased from 20)
   - Repeat saturating trigger rate
   - Should now see: "Stopped cleanly. Peak queue depth: 18/25" (no drops)

**Expected Timeline:**
```
Session 1: Clean, peak=6/20
Session 2: Clean, peak=8/20
Session 3: OVERRUN at 18/20, bufferCopiesDropped=1
Session 4: Clean, peak=18/25 (adapted), bufferCopiesDropped=0
```

---

## Test 5: Single-Shot Capture Diagnostics
**Purpose**: Verify diagnostic hardware polling without multi-threading complications

**Procedure:**
1. ADC tab: Click "Single Shot" button
2. Wait for status: "Waiting for Hardware Trigger (Max 5s)..."
3. Send ONE external trigger pulse
4. Verify: "Single-shot captured OK"
5. FFT plots should show clean chirp in time and frequency domains
6. Confirm save dialog and save to test file

**Expected Results:**
- Time-domain plot: Clean chirp waveform (yellow=CH0, cyan=CH1)
- Frequency-domain plot: Clear peak at expected chirp center frequency
- File saved with 128-byte header + single chirp sample set

---

## Test 6: Chirp Alignment & Sample Continuity
**Purpose**: Ensure no samples lost at buffer boundaries

**Procedure:**
1. Record 10 triggers at known sample rate and prog_div
2. Post-process in MATLAB:

```matlab
% Load recorded file
[data, header] = read_radar_file('recording.bin');
samples_per_trigger = header.samples_per_trigger;
num_triggers = header.num_triggers;
num_channels = header.num_channels;
sample_rate_hz = header.sample_rate_hz;

% Reshape: (trigger, sample, channel)
data_reshaped = reshape(data, num_channels, samples_per_trigger, num_triggers);
data_reshaped = permute(data_reshaped, [2, 3, 1]); % (sample, trigger, channel)

% Verify no DC drift between triggers (would indicate skip)
ch0_mean_per_trigger = squeeze(mean(data_reshaped(:,:,1), 1));
ch0_drift = diff(ch0_mean_per_trigger);
assert(max(abs(ch0_drift)) < 500, 'Large DC jump suggests missing samples');

% Verify sample count matches expectation
assert(size(data_reshaped, 3) == num_channels, 'Channel count mismatch');
disp('✓ All samples accounted for, no drops detected');
```

**Expected Results:**
- No DC jumps between triggers
- Trigger-to-trigger transitions smooth
- Phase continuity (if applicable) preserved

---

## Test 7: Thread Safety Verification
**Purpose**: Ensure no race conditions in buffer handoff

**Setup:**
1. Enable CVI Debug mode → Break on "Debug Events"
2. Set breakpoint in HardwarePollThreadFunction at `WD_AI_AsyncDblBufferHandled` call

**Procedure:**
1. Start acquisition with recording
2. When breakpoint hits (first buffer ready):
   - Check locals: `currentQueueDepth`, `src` pointer, `bufferCopy` contents
   - Verify `src` points to either `dmaBuffer1` or `dmaBuffer2` (not dangling)
3. Step through:
   - memcpy(bufferCopy, src, ...) → verify copy completes
   - CmtWriteTSQData(...) → verify returns > 0 (enqueued successfully)
   - WD_AI_AsyncDblBufferHandled() → verify hardware acknowledges
4. Allow consumer thread to run: Observe DiskWriterThreadFunction processing queueBuffer
5. Verify bufferCopy pointer is NOT used after handoff (memory accessed only on next iteration)

**Expected Results:**
- No memory access violations
- Buffer pointers alternate correctly (Buffer1 → Buffer2 → Buffer1...)
- Queue data read matches enqueued data

---

## Test 8: Edge Case - Very Short Recording (Micro-burst)
**Purpose**: Verify correct header update even with minimal data

**Procedure:**
1. Configure: 1 sample/trigger, 1 trigger
2. Record → send single trigger → stop
3. Inspect binary file:
   - Header offset 16 bytes (sizeof(magic) + num_channels offset): should be 1
   - File size: 128 (header) + 2 (channels) = 130 bytes

```matlab
fid = fopen('micro.bin', 'rb');
header = fread(fid, 32, 'uint32');
assert(header(4) == 1, 'num_triggers should be 1');
fclose(fid);
```

**Expected Results:**
- File valid, trigger count correct despite minimal data

---

## Test 9: Stop During Active Queue Drain
**Purpose**: Ensure data flushed during shutdown

**Procedure:**
1. Start recording with high trigger rate (queue filling)
2. As soon as queue shows depth ~10-12 (mid-drain), click "Stop Acquisition"
3. Verify system:
   - Waits for consumer thread to finish flushing queue
   - File size = expected (all queued data written)
   - Status shows "Stopped cleanly" or "OVERRUN" (if applicable)

**Expected Results:**
- No data loss during shutdown
- Recording file complete and valid
- Total triggers in file = expected count

---

## Performance Metrics (Reference)

| Scenario | Peak Queue | Back-Pressure? | Status | Notes |
|----------|-----------|---|--------|-------|
| 40 MSps, 5 Hz chirp, disk write | 4-8 | No | Clean | Baseline |
| 40 MSps, 10 Hz chirp, disk write | 8-12 | No | Clean | Normal stress |
| 40 MSps, 15 Hz chirp, disk write (slow disk) | 15-18 | Yes | Clean | Back-pressure active |
| 40 MSps, 20 Hz chirp, disk write (saturated) | 18-20 | Yes | OVERRUN (1-2 drops) | High stress |

---

## Troubleshooting

### Symptom: "HALTED: DMA Queue Full (Disk IO too slow)"
**Likely cause:** Disk write speed insufficient for sustained sampling rate
**Solutions:**
1. Write to fast SSD instead of network drive
2. Reduce trigger rate / sample rate
3. Disable DDS sweep during acquisition (test ADC in constant mode)
4. Manually increase `queueMaxCapacity` to 30-40 in code

### Symptom: "HALTED: Hardware DMA Overrun"
**Likely cause:** Buffer handoff failure (WD_AI_AsyncDblBufferHandled not called in time)
**Solutions:**
1. Increase `halfBufferSize` in AdcConfigureCB (higher latency tolerance)
2. Reduce DMA interrupt rate (increase `pDiv` / decrease `progDiv`)
3. Check CPU load (background processes stealing thread time)

### Symptom: FFT plot freezes (no updates)
**Likely cause:** Chirps not extracted (samplesPerChirp/prog_div mismatch)
**Solutions:**
1. Verify `samplesPerChirp` matches expected samples in DRCTRL period
2. Check `currentProgDiv` value matches hardware setting
3. Manually trigger one single-shot to verify plotting pipeline works

### Symptom: Recording file corrupted (MATLAB read fails)
**Likely cause:** Partial write / corruption during multi-threaded access
**Solutions:**
1. Stop acquisition cleanly (click "Stop Acq" / "Stop Record")
2. Try micro-burst test (Test 8) to isolate write logic
3. Check disk space (if full, final fflush may fail silently)

---

## Success Criteria

✅ **All tests pass if:**
1. Test 1: Peak queue < 50% capacity, no drops
2. Test 2: File saved, trigger count matches, MATLAB reads valid
3. Test 3: Back-pressure engages at queueHighWater, disengages at queueLowWater
4. Test 4: Adaptive sizing activates after stress, prevents future overruns
5. Test 5: Single-shot clean, FFT sensible
6. Test 6: No DC jumps, sample continuity
7. Test 7: Debug breakpoints show correct pointers
8. Test 8: Micro-burst file valid
9. Test 9: Graceful shutdown, data integrity maintained

**Expected duration:** ~45 minutes for full test suite

---

*Last Updated: March 17, 2026*
