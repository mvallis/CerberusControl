# DOC SWEEP - CerberusControl - 2026-08-28

> STATUS: DONE 2026-08-28 | owner: code | closes-when: superseded by the next sweep of this repo | verified: 2026-08-28
>
> Run under [`DOC_SWEEP_PROTOCOL.md`](../../Thesis/ThesisRemote/results/2026-08-26--estate-review/DOC_SWEEP_PROTOCOL.md)
> (ZENITH, 2026-08-26), commissioned by Mark: *"a full picture of the repo and fileset
> (and I mean complete, not just until claude thinks its done sufficent)"*.
> Manifest: `ThesisRemote/results/2026-08-28--laptop-doc-sweep/sweep_cbcontrol.json`

**Verify this report rather than believing it:**

```
python ThesisRemote/src/doc-lifecycle/testing/sweep_manifest.py check \
       --manifest ThesisRemote/results/2026-08-28--laptop-doc-sweep/sweep_cbcontrol.json
```

⛔ This report deliberately does **not** state its own exit code. A document that certifies
its own check is the pattern this protocol exists to distrust - `INVENTORY.md` said
*"Tracked files: 125"* in a repo holding 772 and nobody looked again. Run the command.
The measured results for all seven repos are quoted once, in the ZENITH mail note.

⚠ **Deviation from `DOC_SWEEP_PROTOCOL` section 5, stated openly:** the protocol says to put
the manifest *beside* the report, inside the repo. **That cannot work.** `set_digest` covers
paths **and byte sizes**, so a manifest living in the repo it describes changes its own
digest the moment it is written, and `check` can never pass. MEASURED on BB24_DB:
`digest then 34a3e10d8744d814` vs `digest now 103126f6ae1aacf1`. Manifests are therefore
held together in ThesisRemote - which also lets ZENITH read all seven without pulling
seven repos.

---

## 1 - The file set, classified by how each file is REACHED

`37` files enumerated (tracked **and** untracked), `7` of them `.md`.

| class | files | what it means here |
|---|---|---|
| `ALWAYS_LOADED` | 1 | read every session - the loading mechanism is named in section 2 |
| `HANDOVER_DATED` | 2 | dated artefact - the archiving population |
| `REFERENCE` | 3 | durable knowledge read on purpose |
| `SOURCE` | 30 | code, config, data, or build input - not prose |
| `ARCHIVED` | 1 | already retired, or self-declared DONE/SUPERSEDED |

⭐ **The live surface of this repo is 1 files (3%)** - `ALWAYS_LOADED` + `TRIGGER_LOADED`
+ `LEDGER` + `INVENTORY`. Everything else is reference, source, dated or retired.

## 2 - What is genuinely ALWAYS_LOADED, with the mechanism named

⛔ A markdown **link is not an import**. `AGENTS.md` sat marked BINDING and was loaded by
no session for ~2 months on exactly this confusion, so this section admits a file only
when the loading mechanism can be pointed at.

| file | mechanism |
|---|---|
| `CLAUDE.md` | session-root context file |

## 3 - MACHINE_CHECKED: is the checker actually wired?

⛔ *"An unnamed checker is an assertion."* 0 rows are `MACHINE_CHECKED`; every one names
its checker, and **0 of 0** of those checkers appear in this estate's hook or settings
wiring (grepped, not asserted).

## 4 - INVENTORY files: does the stated count match reality today?

None in this repo.

## 5 - ORPHAN candidates

⛔ **Propose, never dispose** (Mark's ruling, 2026-07-29). *What earns a file* is his call.
An `ORPHAN` here means only: no inbound reference was found and no load mechanism applies.

**None.**

## 6 - Dated-artefact share

**2 of 7 `.md` are `HANDOVER_DATED` (29%).** The estate-wide comparator quoted in the
protocol was 43%. This is the population an archiving pass would act on.

## 7 - What this sweep did NOT cover, and why

⛔ A silent omission is the failure this protocol exists to prevent, so:

- **Classification is by MECHANISM, not by reading every file.** Path rules, extension rules,
  header signal and a grepped reference index decided 37 rows; the residue was read by an
  agent. Which rule fired is recorded in each row's `notes`.
- **`referenced_by` is grepped, never asserted** - but a file consumed by a **directory glob**
  is invisible to a name-based index. That produced false orphans until path rules were added
  for it. Any `ORPHAN` remains a candidate, not a verdict.
- **Binary files** are classified `SOURCE` by extension and were not opened.
- **The classifier's own error rate is measured, not assumed** - see section 8.

## 8 - How far this can be trusted

`RC=0` means **complete**, not **correct**. Correctness was measured separately: a random
stratified sample was handed to an independent agent told to **refute** the classifications.

| round | sample | disagreement | outcome |
|---|---|---|---|
| 1 | 60 | **22%** | 4 systematic faults found and fixed |
| 2 | 60 + all orphans | **16.7%** | 5 more faults; 6 of 11 orphans proved FALSE |
| 3 | 60 | see `gateB3_result.json` | final |

The round-2 fixes collapsed the orphan list 11 -> 5, and the survivors then agreed **file for
file** with the independent auditor's own verdicts. That agreement, not the exit code, is the
reason these classifications can be relied on.

