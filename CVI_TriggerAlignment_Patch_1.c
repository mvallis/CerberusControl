/*=============================================================================
 * Trigger Alignment Detection and Display Correction
 * DeviceControl_FullThreaded.c — patch additions
 *
 * INSERT LOCATION SUMMARY:
 *   [A] Static variable declarations — after existing static ADC globals (~line 985)
 *   [B] ADC_DetectTriggerAlignment() function — before ADC_StartCommon (~line 1971)
 *   [C] Call site in display callback — inside the half-buffer plot block,
 *       before FFT and plot calls, typically inside ADC_PlotDeferred or the
 *       equivalent display update function where plotBuffer is processed.
 *
 * BEHAVIOUR:
 *   On each half-buffer that arrives, the first N_ALIGN_CHECK trigger rows
 *   are checked.  If the dead window consistently has more energy than the
 *   valid chirp window, chirpSampleOffset is set to SampsPerChirp (512).
 *   The plot and FFT extraction then reads from [offset ... offset+SampsPerChirp-1]
 *   instead of [0 ... SampsPerChirp-1], correcting the display in real time.
 *
 *   A status label is updated each half-buffer with the current alignment state.
 *   No data is discarded — the offset only affects which samples are sent to
 *   the display path.  Saved .dat files are always the complete raw buffer.
 *=============================================================================*/


/*=============================================================================
 * [A] STATIC VARIABLE DECLARATIONS
 * Add after the existing adcVoltScale / adcConfiguredRange block
 *=============================================================================*/

/* Trigger alignment state — updated each half-buffer by ADC_DetectTriggerAlignment.
   chirpSampleOffset = 0     -> first SampsPerChirp samples are the valid chirp (normal)
   chirpSampleOffset = 512   -> second SampsPerChirp samples are the valid chirp (inverted)
   alignConfidence   = fraction of checked triggers agreeing with the current state (0..1) */
static int    chirpSampleOffset = 0;
static double alignConfidence   = 1.0;
static int    alignChecked      = 0;   /* set to 1 after first successful check */

#define N_ALIGN_CHECK   32    /* triggers sampled per half-buffer for alignment detection */
#define ALIGN_THRESH    0.70  /* confidence below this triggers a warning label */


/*=============================================================================
 * [B] ADC_DetectTriggerAlignment FUNCTION
 * Add before ADC_StartCommon or near the other ADC_ helper functions
 *=============================================================================*/

/*---------------------------------------------------------------------------
 * ADC_DetectTriggerAlignment — energy-based trigger edge detector
 *
 *   buf           Pointer to start of one channel's de-interleaved half-buffer
 *                 (U16, already de-interleaved, ScansPerTrigger samples per row)
 *   scansPerTrig  SampsPerChirp + DeadSamples (e.g. 1024)
 *   validN        SampsPerChirp (e.g. 512)
 *   deadN         DeadSamples (e.g. 512)
 *   nTriggers     Number of complete triggers in buf to examine
 *   outOffset     Output: 0 (normal) or validN (inverted)
 *   outConfidence Output: fraction of triggers agreeing (0..1)
 *---------------------------------------------------------------------------*/
static void ADC_DetectTriggerAlignment (const U16 *buf,
                                         int scansPerTrig, int validN, int deadN,
                                         int nTriggers,
                                         int *outOffset, double *outConfidence)
{
    int    t, i;
    double sumSqFirst, sumSqLast, val;
    int    votesAligned = 0;   /* first half > last half */
    int    checked      = 0;

    for (t = 0; t < nTriggers; t++)
    {
        const U16 *row = buf + t * scansPerTrig;

        sumSqFirst = 0.0;
        sumSqLast  = 0.0;

        for (i = 0; i < validN; i++)
        {
            val = (double)row[i] - 32768.0;
            sumSqFirst += val * val;
        }
        for (i = 0; i < deadN; i++)
        {
            val = (double)row[validN + i] - 32768.0;
            sumSqLast += val * val;
        }

        if (sumSqFirst >= sumSqLast)
            votesAligned++;

        checked++;
    }

    if (checked == 0)
    {
        *outOffset     = 0;
        *outConfidence = 1.0;
        return;
    }

    *outConfidence = (double)votesAligned / (double)checked;

    if (*outConfidence >= 0.5)
    {
        /* Majority say first half > last half: normal alignment */
        *outOffset = 0;
    }
    else
    {
        /* Majority say last half > first half: inverted — offset by validN */
        *outOffset     = validN;
        *outConfidence = 1.0 - *outConfidence;   /* reflect to > 0.5 for display */
    }
}


/*=============================================================================
 * [C] CALL SITE — insert at the TOP of the display/plot deferred callback,
 *     before the existing FFT and PlotY calls.
 *
 *     The variable names below match the conventions in the existing code:
 *       plotBuffer         — the U16 half-buffer array
 *       plotScans          — SCANS_PER_HALF / ADC_NUM_CH (scans per channel)
 *       SampsPerChirp      — replace with your #define or variable
 *       DeadSamples        — replace with your #define or variable
 *       ScansPerTrigger    — SampsPerChirp + DeadSamples
 *       ADC_NUM_CH         — 2
 *
 *     The existing code extracts ch0[] and ch1[] as de-interleaved arrays;
 *     insert this block AFTER de-interleaving and BEFORE the FFT/plot calls.
 *=============================================================================*/

    /* --- Trigger alignment detection (insert after de-interleave, before FFT) --- */
    {
        int    newOffset;
        double newConfidence;
        int    scansPerTrig    = (int)SampsPerChirp + (int)DeadSamples;
        int    trigsInHalfBuf  = (int)(plotScans / scansPerTrig);
        int    trigsToCheck    = (trigsInHalfBuf < N_ALIGN_CHECK)
                                   ? trigsInHalfBuf : N_ALIGN_CHECK;
        char   alignMsg[256];

        /* ch0 is already de-interleaved; use it for the detection.
           Pass the raw U16 buffer slice for the first trigsToCheck triggers. */
        ADC_DetectTriggerAlignment (
            ch0,              /* de-interleaved CH0 U16 data, row-major */
            scansPerTrig,     /* samples per trigger period               */
            (int)SampsPerChirp,
            (int)DeadSamples,
            trigsToCheck,
            &newOffset, &newConfidence);

        /* Update global state */
        chirpSampleOffset = newOffset;
        alignConfidence   = newConfidence;
        alignChecked      = 1;

        if (newOffset == 0)
        {
            snprintf (alignMsg, sizeof(alignMsg),
                      "Trigger: ALIGNED  (conf=%.0f%%)",
                      newConfidence * 100.0);
        }
        else
        {
            snprintf (alignMsg, sizeof(alignMsg),
                      "*** TRIGGER INVERTED — offset=%d  (conf=%.0f%%) ***",
                      newOffset, newConfidence * 100.0);
        }

        /* Update the ADC status label — replace ADC_TAB_ADC_MSG_STATUS with
           whichever label control is appropriate for alignment status display.
           If a dedicated label exists, use that instead. */
        SetCtrlVal (adcTabHandle, ADC_TAB_ADC_MSG_ALIGN, alignMsg);

        /* Optional: change label text colour to red when inverted.
           Requires a TextMsg or similar control that supports ATTR_TEXT_COLOR. */
        if (newOffset != 0 && newConfidence > ALIGN_THRESH)
        {
            SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_MSG_ALIGN,
                              ATTR_TEXT_COLOR, MakeColor(220, 30, 30));
        }
        else
        {
            SetCtrlAttribute (adcTabHandle, ADC_TAB_ADC_MSG_ALIGN,
                              ATTR_TEXT_COLOR, MakeColor(0, 180, 0));
        }
    }

    /*
     * EXISTING FFT AND PLOT CODE — modify the sample extraction to use
     * chirpSampleOffset.  Typically the existing code does something like:
     *
     *   for (i = 0; i < SampsPerChirp; i++)
     *       fftInput[i] = ch0[triggerRow * scansPerTrig + i];
     *
     * Change the inner index to:
     *
     *   for (i = 0; i < SampsPerChirp; i++)
     *       fftInput[i] = ch0[triggerRow * scansPerTrig + chirpSampleOffset + i];
     *
     * This applies to both ch0 and ch1 extraction.
     * No other changes to the FFT, window, or plot calls are needed.
     */
