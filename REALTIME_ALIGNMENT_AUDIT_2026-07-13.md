# Real-time trigger-alignment / flyback audit — 2026-07-13

Read-only audit prompted by an OFFLINE processing bug (MATLAB pipeline): the offline
trigger-alignment detector `cb_detect_trigger_alignment` mis-locks on low-duty-cycle captures
(chirp << trigger period) — its PMR-vs-offset profile is a flat plateau over the chirp, so argmax
returns a large window offset (13/07 sky set 19-85) when the chirp starts at sample 0. The bad
window slides into the chirp-end / DDS-reset FLYBACK transient at ~sample N, smearing low-frequency
energy into the range profile and inflating the noise floor ~2-2.5 dB.
QUESTION: is the REAL-TIME (this firmware) path exposed?

## Conclusion: real-time path is NOT exposed. No correctness fix required.
- Live FFT window is **anchored at sample 0** (`chirpSampleOffset` defaults to 0, reset each
  acquisition start) and spans `SampsPerChirp_ADC` samples = the chirp length. The flyback at
  ~sample `SampsPerChirp` is OUTSIDE the processed window.
  Verified across ALL 16 of the 13/07 `.hdr`: `SampsPerChirp_ADC` = `SampsPerChirp` = 1024
  (< `ScansPerChirp` = 16384 / 32768, the full CRI).
- Peak / max-hold exclude near range (< `MAX_HOLD_RANGE_MIN_M` = 1.9 m) where the flyback's
  DC/low-freq energy lands -> `LastPeak_LRX/URX`, `MaxHold` are immune.
- The saved `.dat` is the complete raw buffer -> the offline bug is purely offline windowing;
  the raw 13/07 data is intact.
- The shipped auto-aligner `ADC_AlignDeferred` runs on the MANUAL "Re-align" button only (no
  automatic call). When invoked it is hardened: coarse = max integrated chirp-width energy;
  fine = bounded +/-16 samples, lower-quarter spectral magnitude, DC + flyback excluded. NOT
  exposed to the offline plateau-argmax flaw.
- `CVI_TriggerAlignment_Patch_1.c` is an UNSHIPPED design spec (assumes 50% duty; does not fit
  low-duty captures).
- `DeviceControl_FullThreaded_fixed0704026.c` predates the alignment work (a ScanInterval / ADC
  impedance fix); unrelated to this.

## Optional ergonomic nice-to-haves (NOT data-integrity)
1. Record `chirpSampleOffset` in the `.hdr` sidecar for traceability.
2. Optionally auto-run `ADC_AlignDeferred` once at acquisition start so the live display
   self-corrects for the (unrecorded, fixed) trigger -> DDS-ramp latency.

Full offline discovery + evidence: MATLAB repo `HANDOVER_2026-07-13_trigger-alignment.md`.
Audit 2026-07-13; read-only, no firmware changed.
