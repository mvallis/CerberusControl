# Cerberus CVI — Trigger Alignment Detection: Implementation Notes

## Background

Each DRCTRL trigger period produces 1024 ADC scans per channel: 512 valid IF chirp samples followed by 512 dead (noise-only) samples. If the ADC latches on the wrong edge at acquisition start, the layout is inverted — dead window first, chirp second. Because the DMA double-buffer runs continuously from a single trigger event, **alignment is fixed for the entire acquisition** and only needs to be determined once per run.

---

## Detection Method

Energy comparison across the first N triggers of the first half-buffer:

- Compute sum-of-squares for samples `[0 .. 511]` (first half) and `[512 .. 1023]` (second half) per trigger row
- If majority of checked triggers have `energy(first) >= energy(second)` → **aligned**, offset = 0
- Otherwise → **inverted**, offset = 512
- Confidence = fraction of triggers agreeing with the majority decision

Implemented in `ADC_DetectTriggerAlignment()` — pure integer arithmetic, no malloc, runs in microseconds on 32 triggers.

---

## Decision: Check Once Per Run, Not Every Half-Buffer

**Rationale:** Alignment cannot change mid-run. Checking every half-buffer wastes CPU and is unnecessary.

**Implementation:** Gate the detection behind `static int alignCheckDone = 0`. Set to 1 after the first check. Reset to 0 in the acquisition stop path so the next run re-checks.

---

## Static Variables to Add

Insert after the existing `adcVoltScale` / `adcConfiguredRange` block (~line 1003):

```c
static int    chirpSampleOffset = 0;   /* 0=normal, 512=inverted */
static double alignConfidence   = 1.0; /* fraction of triggers agreeing */
static int    alignChecked      = 0;   /* 1 after first detection */
static int    alignCheckDone    = 0;   /* 1 = skip further checks this run */
```

```c
#define N_ALIGN_CHECK   32     /* triggers to sample for detection */
#define ALIGN_THRESH    0.70   /* confidence below this = warn */
```

---

## Function to Add: `ADC_DetectTriggerAlignment()`

Add before `ADC_StartCommon()`. Signature:

```c
static void ADC_DetectTriggerAlignment(
    const U16 *buf,        /* de-interleaved CH0 U16 data, row-major        */
    int scansPerTrig,      /* SampsPerChirp + DeadSamples = 1024            */
    int validN,            /* SampsPerChirp = 512                           */
    int deadN,             /* DeadSamples = 512                             */
    int nTriggers,         /* number of triggers to check                   */
    int    *outOffset,     /* output: 0 or validN                           */
    double *outConfidence  /* output: fraction agreeing, always >= 0.5      */
);
```

Full implementation in `CVI_TriggerAlignment_Patch.c`.

---

## Call Site — Display/Plot Deferred Callback

Insert **after de-interleave, before FFT and PlotY calls**:

```c
if (!alignCheckDone)
{
    int scansPerTrig   = (int)SampsPerChirp + (int)DeadSamples;
    int trigsInHalfBuf = (int)(plotScans / scansPerTrig);
    int trigsToCheck   = (trigsInHalfBuf < N_ALIGN_CHECK)
                           ? trigsInHalfBuf : N_ALIGN_CHECK;
    char alignMsg[256];
    int    newOffset;
    double newConfidence;

    ADC_DetectTriggerAlignment(ch0, scansPerTrig,
        (int)SampsPerChirp, (int)DeadSamples,
        trigsToCheck, &newOffset, &newConfidence);

    chirpSampleOffset = newOffset;
    alignConfidence   = newConfidence;
    alignChecked      = 1;
    alignCheckDone    = 1;

    if (newOffset == 0)
        snprintf(alignMsg, sizeof(alignMsg),
                 "Trigger: ALIGNED (conf=%.0f%%) [checked once]",
                 newConfidence * 100.0);
    else
        snprintf(alignMsg, sizeof(alignMsg),
                 "*** TRIGGER INVERTED — offset=%d (conf=%.0f%%) ***",
                 newOffset, newConfidence * 100.0);

    SetCtrlVal(adcTabHandle, ADC_TAB_ADC_MSG_ALIGN, alignMsg);
    SetCtrlAttribute(adcTabHandle, ADC_TAB_ADC_MSG_ALIGN, ATTR_TEXT_COLOR,
                     (newOffset != 0) ? MakeColor(220,30,30) : MakeColor(0,180,0));
}
```

Then modify the existing sample extraction index from:
```c
fftInput[i] = ch0[triggerRow * scansPerTrig + i];
```
to:
```c
fftInput[i] = ch0[triggerRow * scansPerTrig + chirpSampleOffset + i];
```
Apply to both `ch0` and `ch1` extraction.

---

## Reset in Stop Path

Add alongside `WD_AI_AsyncClear`, `WD_AI_AsyncDblBufferMode(adcCard, 0)`, and `WD_AI_ContBufferReset`:

```c
alignCheckDone    = 0;
chirpSampleOffset = 0;
alignConfidence   = 1.0;
alignChecked      = 0;
```

---

## UIR Change Required

Add a text label control `ADC_TAB_ADC_MSG_ALIGN` to the ADC tab in the `.uir` file to display the alignment status string. Colour is set programmatically: **green** = aligned, **red** = inverted.

---

## N_ALIGN_CHECK Recommendation

32 is conservative and sufficient. Since the check runs only once per run, raising to **64 or 128** costs nothing ongoing and gives a more robust single decision. Update the `#define` accordingly.

---

## Files

| File | Purpose |
|---|---|
| `CVI_TriggerAlignment_Patch.c` | Full C implementation (statics, function, call site) |
| `cerberus_lib/cb_detect_trigger_alignment.m` | MATLAB equivalent (library) |
| `cerberus_prep_data.m` | MATLAB — calls detector, applies offset, returns alignment struct |
| `cerberus_check_trigger.m` | MATLAB — diagnostic figures + user confirmation prompt |
