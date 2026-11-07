# CerberusControl

> **START HERE** (an agent reads this block as its briefing; a person can read the last line)
> - **What:** LabWindows/CVI 2020 (ANSI C) control software for Cerberus, the X-band FMCW radar: it drives the relay board, the PSU, the AD9914 chirp generator and the ADLINK PCI-9846H digitiser, and records the raw captures that `CerberusSignalProcessing` reads. A satellite repo (S5); the backbone wins every value and notation conflict.
> - **Read first:** `CLAUDE.md` (system, threading, buffers, save modes, calibration order), then the pinned overrun note in it; `WORKING.md` is the long session log, read by heading only.
> - **Basis docs:** backbone `basis/BASIS_Ch8_Cerberus.md` (architecture and implementation); index `basis/README.md`.
> - **Plan rows:** backbone `handoffs/FINAL_PUSH_PLAN_2026-10-19.md` section 000P and `handoffs/TASK_LISTS_INDEX.md` (entry S5).
> - **Binds:** CVI cannot build from the command line, so the user builds in the IDE (F7); never hand-edit the binary `.uir` (use the CVI User Interface Editor); the ADC calibration order in `CLAUDE.md` is fixed; the 36 MS/s overrun is open (18 MS/s is the workaround); a Cerberus capture is not analysis-ready until its state is verified from the data (skill `capture-state-verification`); the BB24 channel map is never carried to Cerberus.
> - **In plain words:** this repository is the program that runs the Cerberus radar and saves what it records; everything downstream starts from those files.

Key files: `DeviceControl_FullThreaded.c`, `.h` and `.uir` (the application), `dds.c` and `dds.h` (AD9914 registers), `Wd-dask64.h` and `WD-Dask64.lib` (ADLINK driver), `Blunderbuss_code_DB1.c` (BB24 reference on the same card).
Recording uses `SAVE_THREAD` (default); `SAVE_TOFILE` is kept for comparison only.
Notes: `TriggerAlignment_ImplementationNotes.md` (chirp re-alignment), `REALTIME_ALIGNMENT_AUDIT_2026-07-13.md`, `LANE_AUDIT_2026-08-27.md`, `SWEEP_CBCONTROL_2026-08-28.md`.
