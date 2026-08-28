# cbcontrol lane audit, 2026-08-27

Phase 4 of the estate cross-check, run from
`.../Cerberus/Code/CVI/ControlRepo/CerberusControl` on 2026-08-28 (the audit is dated
2026-08-27 per the commanded filename).

**Mode:** read-only except this file. No code, no config and no memory note was created,
moved, promoted or edited. The three write probes in section C were deleted immediately
and are verified gone.

**RULING.md is ACTIVE.** This session enumerates and challenges. It resolves nothing and
locks nothing. Every ruling below is left to Mark.

**Untracked file present, deliberately untouched.** `DeviceControl_Student.c` (188 872
bytes, mtime 2026-07-03) sits untracked in this tree. It was not committed, not staged,
not deleted and not read. It is the only entry in `git status --porcelain` both before and
after this audit.

Tags: **[MEASURED]** = a command or file:line is given. **[INFERRED]** = reasoning from
measured facts, labelled as such. **[NOT DETERMINED]** = searched and not established.

---

## A. What this session actually has loaded

### A1. `CLAUDE.md`: CONFIRMED at 6,807 bytes, with one correction to the commit claim

| Claim in the brief | Verdict | Evidence |
|---|---|---|
| Tracked `CLAUDE.md` | CONFIRMED | `git ls-files` lists `CLAUDE.md` |
| 6,807 bytes | CONFIRMED, both on disk and in git | `wc -c CLAUDE.md` -> `6807`; `git cat-file -s $(git rev-parse HEAD:CLAUDE.md)` -> `6807` |
| Committed as `3a61821` "OVR mitigations and portable Claude context" | CONFIRMED with a correction | `git log --oneline -- CLAUDE.md` -> `3a61821`, `40732fa`, `1f63ee0`. `3a61821` is the **most recent commit touching** `CLAUDE.md`, not HEAD. HEAD is `ec9f5a5`. The 6,807-byte blob is the one at HEAD. |

[MEASURED] `wc -lwc CLAUDE.md` -> 169 lines, 1 025 words, 6 807 bytes.

[MEASURED] It **is** in this session's context. It arrived verbatim in the opening
system-reminder under "Codebase and user instructions", the full 169 lines.

**Imports: ZERO.** [MEASURED] `grep -nE '^\s*@|@[A-Za-z0-9_./-]+\.md' CLAUDE.md` returns
exit code 1, no matches. `CLAUDE.md` is a single self-contained file with no `@`-style
import directives and therefore no transitive context load.

This is worth stating as a positive finding rather than an absence. `CLAUDE.md` **names**
`WORKING.md`, `TriggerAlignment_ImplementationNotes.md`, `Blunderbuss_code_DB1.c`,
`dds.c`/`dds.h`, `Wd-dask64.h` and `DeviceControl_Full_Threaded.cws` in its prose and in
its "Key files" table, and it explicitly says *"push detail to `WORKING.md` and link"*.
None of those load. `WORKING.md` is 83 774 bytes and is **not** in this session's context.
A reader of `CLAUDE.md` alone gets the pointers and none of the content, and the file's
own closing instruction assumes a link that the harness does not follow.

### A2. `GEMINI.md`: tracked, 554 bytes, and it does NOT load for me

[MEASURED] `wc -c GEMINI.md` -> `554`. `git ls-files` lists it. It is **absent from this
session's system-reminder**; I have its content only because I ran `cat GEMINI.md`
explicitly. Claude Code does not read `GEMINI.md`.

**Do the two agree or conflict?** They **do not conflict**, and the more useful finding is
that `GEMINI.md` is a strict and much thinner subset.

Agreements: ANSI C only, no C++ (both); never hand-edit the binary `.uir`, describe the
change for the UI Editor instead (both, near-identical wording); thread safety in the
acquisition loop (`GEMINI.md` "Constraints"; `CLAUDE.md` "Threading model").

Gaps in `GEMINI.md`, each of which is a documented way to break this project:

- **No CLI-build prohibition.** `CLAUDE.md` is emphatic that CVI 2020 cannot be built from
  the command line and that the agent must not block on build verification. A Gemini
  session would not know this and would plausibly attempt a CLI build.
- **No 64-bit target requirement**, which `CLAUDE.md` ties to multi-GB allocations and
  large BSS arrays.
- **No ADC calibration ordering.** `CLAUDE.md` pins the five-step register/calibrate/
  release/re-register/configure sequence and warns that `WD_AI_ContBufferReset` after
  `WD_AI_Trig_Config` wipes the trigger config. Getting this wrong returns `-201
  ErrorConfigIoctl` or immediate fStop.
- **No OVR context, no save-mode table, no DMA buffer layout, no diagnostic status line.**

One minor mismatch of framing: `GEMINI.md` sets the goal as *"multi-file dependency
analysis"*, but this project is essentially one 187 364-byte `.c` plus `dds.c`/`dds.h`. The
`.prj` lists only `DeviceControl_FullThreaded.c`, `dds.c` and headers.

**Enumerated, not resolved:** two context files for two different agents, one tracked and
loading, one tracked and silent, with the safety-critical constraints in only one of them.

### A3. There is NO `.claude/` directory. CONFIRMED, and it was removed deliberately

[MEASURED] `ls -la .claude` -> `No such file or directory`, exit code 2.

The stronger finding is historical. This repo **did** have a `.claude/`, and it was
deleted:

[MEASURED] `git log --diff-filter=D --name-only --oneline` shows
`.claude/settings.local.json` deleted in **`3c4f2c1` "Tidy main: archive scratch binaries,
untrack ignored files"**.

[MEASURED] It was present on 25 June, the date all five memory notes were written:
`git ls-tree -r --name-only 4bfcaee | grep '^\.claude'` -> `.claude/settings.local.json`.

[MEASURED] Its entire content at `3a61821`, via `git show 3a61821:.claude/settings.local.json`:

```json
{
  "permissions": {
    "allow": [
      "Bash(git check-ignore:*)",
      "Bash(echo \"exit:$?\")"
    ]
  }
}
```

**This matters for section B.** A reader might assume the June sessions were guarded
because a `.claude/` existed then. They were not. That file held two Bash permission
allows and nothing else: no `hooks` key, no `settings.json`, no hook scripts, no
`PROTECTED.md`. **The June sessions were as unguarded as this one.** The `3c4f2c1`
deletion removed a stub, not a guard.

### A4. Which estate hooks therefore do NOT run here

The estate's hooks are declared in the backbone at
`SRInSARThesis/.claude/settings.json` (31 644 bytes), across six event keys
(`PreToolUse`, `PostToolUse`, `SessionStart`, `SessionEnd`, `Stop`, `UserPromptSubmit`).
Hook resolution is per-project, so **none of them run from this cwd**.

[MEASURED] The three named in the brief are confirmed absent, and they are 3 of 27:

`claim_lint_guard.py` · `config_inventory_guard.py` · `config_inventory_to_json.py` ·
`context_budget_lint.py` · `cross_repo_freshness.py` · `doc_guard.py` ·
`drafts_overwrite_guard.py` · **`estate_rules_guard.py`** · `exemplar_brief_check.py` ·
`extract_docx_comments.py` · `extraction_launch_guard.py` · `gap_row_lint.py` ·
`gate_guard.py` · `hook_budget_lint.py` · `ignored_docs_lint.py` ·
`inventory_freshness_lint.py` · `laptop_admission.py` · `master_drive_guard.py` ·
`matlab_launch_guard.py` · `mirror_guard.py` · `not_reached_lint.py` · `plan_archive.py` ·
**`protected_guard.py`** · `register_guard.py` · `remote_guard.py` · **`session_close.py`** ·
`suite_runner.py`

Command: `grep -oE '"command"[^,]*' .../settings.json | grep -oE '[A-Za-z_0-9]+\.py' | sort -u`

Also absent: there is **no user-level hook layer** to fall back on.
[MEASURED] `ls ~/.claude/hooks` -> `No such file or directory`; `ls ~/.claude/CLAUDE.md` ->
`No such file or directory`; `~/.claude/settings.json` contains no `hooks` key.

There is **no `PROTECTED.md` in this repo** [MEASURED: `git ls-files` lists five `.md`
files, none named `PROTECTED.md`]. This has a specific consequence, developed in C4.

### A5. Memory: 5 notes. Count CONFIRMED exactly

[MEASURED] `wc -c` over
`~/.claude/projects/C--Users-...-CerberusControl/memory/`:

| File | Bytes | mtime |
|---|---|---|
| `MEMORY.md` | **737** | 2026-06-25 15:39 |
| `ai-giveaway-prose-patterns.md` | 1 775 | 2026-06-25 15:39 |
| `bb24-vs-cerberus-differences.md` | 2 444 | 2026-06-25 14:07 |
| `blunderbusssar-project.md` | 2 130 | 2026-06-25 13:19 |
| `pdf-read-not-password-blocker.md` | 1 394 | 2026-06-25 14:38 |

5 files, `MEMORY.md` at **737 bytes exactly** as the census stated, plus 4 content notes.
Total 8 480 bytes. The census figure is confirmed from inside the session.

**A challenge to a sibling lane's slug-casing warning.** The BB24_DB lane audit
(`BB24_DB/LANE_AUDIT_2026-08-27.md` §A3, committed `50e5321`) records that this repo's slug
appears as lowercase `c--Users-...` while BB24_DB's is uppercase `C--Users-...`, and warns
that *"a memory home created under the lowercase slug would be invisible to a CLI session
like this one, and vice versa."*

[MEASURED] **On this filesystem that is not so.** My own scratchpad path is the
**uppercase** form `C--Users-mab31-...-CerberusControl`, I read all five notes through the
**uppercase** path, and `ls -d */` renders the on-disk name in the **lowercase** form. Both
casings resolved to the same five files. NTFS is case-insensitive here, so the two spellings
are one directory, not two.

I do not resolve this. The casing inconsistency in the slug names is real and is worth
Mark's attention as a portability hazard (it would bite on a case-sensitive filesystem, and
it makes `ls`-based census work confusing). But the specific consequence the BB24_DB lane
drew, that a memory home could be written to one casing and be invisible from the other,
**does not hold on this machine.** Flagged for Mark; the two lanes disagree.

---

## B. Whose memory is this?

### B0. The laptop's file-level reading: CONFIRMED, and it can be put more strongly

All four content notes were read in full. The brief's reading holds.

| Note | About CerberusControl? | Evidence |
|---|---|---|
| `blunderbusssar-project.md` | **No.** BB24_DB. | *"Lives in `…/Blunderbuss/Code/CVI/BB24_DB` (local git repo -> github.com/mvallis/BlunderbussSAR)"*. `type: project`. CerberusControl appears once, as *"Reference 'final version' system"*. |
| `bb24-vs-cerberus-differences.md` | **No.** BB24_DB. | `description:` = *"Key hardware/code differences ... that constrain the BlunderbussSAR rewrite"*. `type: reference`. Cerberus appears only as the comparison term. |
| `pdf-read-not-password-blocker.md` | **No.** Estate-wide craft. | `type: feedback`. A `Read`-tool behaviour lesson. Its worked example is an ADLINK manual, which is Cerberus-adjacent, but the note's subject is the tool, not this repo. |
| `ai-giveaway-prose-patterns.md` | **No.** Estate-wide prose craft. | `type: feedback`. Concerns thesis and document voice. Nothing to do with CVI, C, or radar control. |

CONFIRMED that all four are dated 25 June [MEASURED: mtimes above; note this is filesystem
mtime, not an authored date, though all five agree and cluster within 13:19 to 15:39].

**The reading can be sharpened.** It is not merely that the four are "not about
CerberusControl". **Zero of the five files in this project's memory home concern this
project at all.** Two belong to a different repo; two belong to the estate as a whole.
This memory home has a 0/4 hit rate on its own subject. If a future session opens this
repo to work on the Cerberus DMA pipeline, its recalled memory tells it about a 24 GHz
radar it is not working on and about em-dashes in thesis prose.

**Two origin sessions, not one.** [MEASURED] from the `originSessionId` frontmatter:

- `69a8dc72-8c68-40cc-8fbf-b288f13e6850` -> `blunderbusssar-project.md` (13:19),
  `bb24-vs-cerberus-differences.md` (14:07)
- `4afc25aa-d9c9-475a-a0c0-9070f01a5da0` -> `pdf-read-not-password-blocker.md` (14:38),
  `ai-giveaway-prose-patterns.md` (15:39)

### B1. FINDING: BB24_DB's institutional memory is stranded under this project home

**Stated as a finding, with the evidence separated by confidence.**

**[MEASURED]** BB24_DB has its own project slug, and its memory home is **empty**:
```
ls -la ~/.claude/projects/C--Users-...-Blunderbuss-Code-CVI-BB24-DB/
  -> 6c8044f9-....jsonl, memory/
ls -la .../memory/  -> 0 files. No MEMORY.md.
```
Two notes explicitly about BB24_DB sit under CerberusControl's slug. Zero sit under
BB24_DB's. This is the single strongest artefact in the audit: **the file that should hold
the memory exists and is empty, and the memory is next door.**

**[MEASURED]** The stranding is live and consequential, not merely untidy. `BB24_DB/CLAUDE.md`
is 6 302 bytes and reproduces much of the two BB24 notes' content in git, but a BB24_DB
session's *recall* layer is blank. The BB24_DB lane audit independently reached the same
conclusion from the other side (`BB24_DB/LANE_AUDIT_2026-08-27.md` §B, "the memory is under
another repo's slug. CONFIRMED"). **Two lanes, opposite cwds, same finding.**

**[MEASURED]** No transcript of either origin session survives anywhere:
```
[SEARCHED: find ~/.claude/projects -name "69a8dc72-....jsonl"        -> 0 hits]
[SEARCHED: find ~/.claude/projects -name "4afc25aa-....jsonl"        -> 0 hits]
[SEARCHED: grep -c <both sids> ~/.claude/history.jsonl (409 908 B)   -> 0, rc=1]
[SEARCHED: grep -rl <both sids> ~/.claude/sessions/ ~/.claude/backups/ -> 0 hits]
```
All 13 project slugs were covered by the `find`.

**[INFERRED, and labelled]** That those sessions ran with **this** directory as cwd. The
inference is short but it is an inference, because the direct evidence (a transcript with
a `cwd` field) is gone. It rests on: the project slug is derived from the session's working
directory; the notes are filed under this slug; and BB24_DB's own slug holds nothing. I
report this as INFERRED rather than MEASURED, and I decline to upgrade it.

**[MEASURED]** That those sessions ran **unguarded**. This does not depend on the inference
above, and it is the part that matters. On 25 June this repo's only `.claude/` content was
two Bash permission allows (A3, `git show 3a61821:.claude/settings.local.json`). There were
no hooks here then and there are none now. Separately, BB24_DB **also** has no `.claude/`
[MEASURED: `ls` -> `No such file or directory`]. **Whichever of the two directories those
sessions ran from, they ran with no repo-level guard.** The finding survives the gap in the
transcript record.

### B2. Is the content already in git? Grepped, not assumed

Reporting only the **zero-hit** content, per the brief.

**`blunderbusssar-project.md`: no zero-hit content. Fully covered.**
Goal, `BB24_DB` location, the BlunderbussSAR remote, "DB1 is the staging base, NOT yet a
real double-buffer", scope in and out, the `Navigation_BB24` pointer, Cerberus as the
reference system, and the no-CVI-toolchain-on-the-laptop constraint are all in
`BB24_DB/CLAUDE.md` (lines 15 to 24, 34 to 40, 100 to 117) and `BB24_DB/WORKING.md`.
[SEARCHED: `grep -rli 'Navigation_BB24'` -> `BB24_DB/CLAUDE.md`, `BB24_DB/HARDWARE.md`,
plus three backbone `data_index/manifests/*.json`.]

**`bb24-vs-cerberus-differences.md`: one zero-hit item. See B3.**
Covered in git: the ADF5901/ADF5904/ADF4159 chipset and "no DDS"
(`BB24_DB/CLAUDE.md:31-36`, `HARDWARE.md` §1); external timebase and trigger
(`CLAUDE.md:38-41`, `HARDWARE.md` §3); the 9846D vs 9846H split (`CLAUDE.md:37`,
`HARDWARE.md` §6); the 1.5625 MS/s rate and the Short_S profile (`CLAUDE.md:44-48`,
`HARDWARE.md` §4); the shared MTi-680G and NTRIP arrangement (`HARDWARE.md` §7); and the
non-functional SAR variant, which is pinned in the **backbone** at
`SRInSARThesis/data_index/bb24.md:25-26`, naming `Blunderbuss_code_SAR.c` and
`TrialNotes.txt` explicitly.

The DSP-split claim is covered on **both** sides, which I checked rather than assumed. BB24
half: `BB24_DB/CLAUDE.md:117`. Cerberus half: `CerberusControl/WORKING.md:901` heads
*"Implementation Plan: Range-Doppler & RTI Processing Tab"*, and
[MEASURED: `grep -c 'rdResult0|RDP_TAB_GRAPH_RD_LRX' DeviceControl_FullThreaded.c
DeviceControl_FullThreaded.h` -> 0, 0] confirms it is still only planned.

**`pdf-read-not-password-blocker.md`: no zero-hit content. CONTRADICTS a sibling lane.**
The BB24_DB lane audit §B3 classifies this note as *"a TRUE ORPHAN"*. **I contradict that
from the measurement.** The lesson is in git in the backbone, in three places:

- `chapters/07_Cerberus_system/components/Component_Figure_Review_Log.md:183-192` ,
  headed *"PDF 'password-protected' FALSE POSITIVE, verify before believing it
  (2026-06-12)"*, which predates the note by 13 days.
- `chapters/07_Cerberus_system/components/COMPONENT_MEASUREMENT_DATA.md:85` , *"(no
  `/Encrypt` dict) the Read tool misreports them as password-protected because it cannot
  render them."*
- `chapters/07_Cerberus_system/_archive/memory_snapshot_2026-07-17/file-access-not-a-blocker.md:10,19-21`
  , which generalises it beyond PDFs.

The note's specific `pdfinfo` then `pdftotext` then `pdftoppm` remedy ladder is a cleaner
statement of the procedure than any of the three, but the substance is not unique to it.
The two lanes disagree; I do not resolve it. It is possible the BB24_DB lane meant "orphan
with respect to BB24_DB", which would be true.

**`ai-giveaway-prose-patterns.md`: partially covered, with two zero-hit items and one
unverifiable self-claim.**
The em-dash ban is pervasive in git across backbone chapter QC records
[MEASURED: 20+ hits, including `chapters/05_.../CH5_HANDBACK_2026-07-28.md:25`,
`chapters/03_.../WPP1_RETURN_2026-07-20.md:210`, `chapters/02_.../2_4_4.md:116`].

**The note's self-provenance claim: located and CHECKED. Two-thirds accurate.**

The note asserts that *"The skill's `voice-rules.md` was updated on 2026-06-25 to name all
three patterns (§5 bold openers, §6 em-dash + the two opener forms)"*. The file is **not**
under `SRInSARThesis`, which is why two bounded searches missed it. It is at
`Thesis/srinsar-skills/skills/srinsar-writing-conventions/references/voice-rules.md`
(33 861 bytes), git-tracked in **`github.com/mvallis/thesisSkillsAgents`**, and byte-identical
to the deployed copy at `Thesis/.claude/skills/...` [MEASURED: `cmp` -> rc 0].

```
[SEARCHED: find SRInSARThesis/.claude/skills -iname 'voice-rules*'   -> 0 hits]
[SEARCHED: find SRInSARThesis/tools -maxdepth 5 -iname 'voice-rules*' -> 0 hits]
[SEARCHED: find over Documents/TRAInSAR (unbounded, backgrounded)     -> 3 hits, above]
```

Checked claim by claim:

| Note's claim | Verdict | Evidence |
|---|---|---|
| §5 names the bold-opener ban | **CONFIRMED** | `voice-rules.md:97` heads §5 *"Paragraph shape and internal signposting"*; `:101` *"**No bold lead-in phrases** are used to announce conceptual sub-units within a section body."* |
| §6 names the em-dash ban | **CONFIRMED** | `:104` heads §6 *"Forbidden words and constructions"*; `:108` bans *"Long em dashes (the em dash character, Unicode U+2014)"* |
| §6 names *"the two opener forms"* | **NOT CONFIRMED** | §6 (`:104-110`) contains **no opener rule at all**. Its three bullets are em dashes, first-person, and the assert-significance word list. [SEARCHED: `grep -niE 'punchy\|topic-announcer\|is your main\|hortative'` over `voice-rules.md` -> 0 hits, rc=1] |

So the em-dash and bold-opener rules **are** in git, and the note is right about them. The
**punchy imperative/declarative lead-in opener** is the zero-hit item, and the situation is
more interesting than a plain omission:

- The nearest rule, §6a *"Synthetic-prose patterns"* (`:112`), was **added 2026-07-31**,
  five weeks *after* the note, and covers adjacent tells (epigram, chiasmus,
  meta-commentary, not-X-but-Y, "rather than" as hinge, ending on significance).
- **§6a.1 partially contradicts the note.** It reads: *"A short declarative topic sentence
  is fine and common; the ban is on the general-truth epigram."* The note bans declarative
  topic-announcers outright (*"X is your main Y"*, *"X is the important one"*). The skill
  permits the form and bans only the epigram variant.
- The imperative/hortative half (*"Watch overrun, don't fear it"*) has no counterpart. The
  closest, §2 `:18`, is about active constructions **assigning agency**, a voice rule, not
  an opener rule.

**[MEASURED]** The note is therefore the only carrier of the punchy-opener ban, and on the
declarative half the estate's tracked rule and the stranded note **disagree**. Enumerated,
not resolved: Mark's ruling is needed on which is current.

### B3. Estate-critical content that exists ONLY in these notes

**B3.1 (hardware authority, the serious one). The AD9914 and `dds.c` sitting in the
Blunderbuss tree are a stray/shared tool and are NOT part of BB24's RF chain.**

Verbatim from `bb24-vs-cerberus-differences.md`:

> The AD9914 / `dds.c` in the Blunderbuss tree is a stray/shared tool, NOT part of BB24's
> RF chain. (Cerberus uses an AD9914 DDS in-app via `dds.c`.)

[MEASURED] `grep -rniE 'AD9914' BB24_DB --include='*.md'` returns **exactly one line**,
`BB24_DB/CLAUDE.md:112`, and it says the *outward-facing* half only: *"Cerberus uses an
AD9914 DDS and software-defined timing, those do **not** apply to BB24."* The
**disambiguation itself**, that a `dds.c` physically present in the Blunderbuss tree does
not imply BB24 has a DDS, is in git nowhere.

**Why this is estate-critical under the standing rule.** The thesis rule is that a value's
authority is scoped to its configuration and must never be carried between chapters. This
note guards exactly that boundary, and the hazard is physical: a `dds.c` file **sits in the
Blunderbuss tree** where anyone auditing BB24's chirp generation will find it. The
available inferences from that file, absent this note, are that BB24 has a DDS, or that
BB24's chirp parameters can be read from AD9914 register programming. Both are false. BB24
Chapter 5 is the ADF5901/ADF5904/ADF4159 chain; Cerberus Chapter 7 is the AD9914. A
carried value here would be a chapter-crossing hardware error of precisely the type the
rule exists to prevent, and the tripwire for it is currently held only in a memory note
filed under the wrong project.

Note the file is present in **this** repo too: `dds.c` (14 130 B) and `dds.h` (1 787 B) are
tracked here, where they are legitimate. That is the source of the ambiguity.

**B3.2 (processing chain). The BB24/Cerberus DSP asymmetry as a single stated fact.**
Both halves are in git separately (B2 above), but the note is the only place that states
the asymmetry as one claim: BB24 has working live range-Doppler that Cerberus lacks;
Cerberus has the hardened DMA/save/metadata pipeline that BB24 lacks; the two are
complementary. Lower severity than B3.1 because both halves are independently recoverable
from git, but a reader would have to know to look in two repos to reassemble it.

**B3.3 (prose craft, non-hardware).** The **punchy imperative/declarative lead-in opener**
ban, per B2. The bold-opener and em-dash bans are **not** zero-hit: both are in
`voice-rules.md` §5 and §6, tracked in `thesisSkillsAgents`. Only the opener ban is unique
to the note, and on its declarative half the note and the tracked §6a.1 **conflict**.
Non-hardware, so lower severity than B3.1, but it is a live disagreement rather than a gap.

### B4. Where these should live. STATED, NOT ACTED ON

⛔ Nothing was moved, copied or promoted. This table is a recommendation for Mark's ruling
and has not been executed.

| Note | Suggested home | Reasoning |
|---|---|---|
| `blunderbusssar-project.md` | BB24_DB's own memory home, **or retire it** | Content is already fully in `BB24_DB/CLAUDE.md`. Keeping both is two copies of one fact, with the usual drift risk. |
| `bb24-vs-cerberus-differences.md` | BB24_DB's memory home, **and B3.1 pinned into `BB24_DB/HARDWARE.md` §1** | B3.1 is a code-facing and thesis-facing hardware-scoping claim. A memory note is too weak a home for it; §1 already says "There is no DDS" and is the natural place for the `dds.c` caveat. |
| `pdf-read-not-password-blocker.md` | Estate-wide, not any single project slug | It is a tool-behaviour lesson with no project scope. Per B2 the substance is already in the backbone; the note's value is the cleaner remedy ladder. |
| `ai-giveaway-prose-patterns.md` | `voice-rules.md` in `thesisSkillsAgents`, **but reconcile §6a.1 first** | ⛔ **Do not simply retire it.** Its em-dash and bold-opener claims are already in §6 and §5, so those parts are redundant. Its punchy-opener ban is not, and its declarative half **conflicts** with §6a.1, which expressly permits short declarative topic sentences. Retiring the note now would silently resolve that conflict in §6a.1's favour without a ruling. |

Whichever homes are chosen, note A5: the slug-casing worry does not bite on this machine,
so a home may be created under either spelling.

---

## C. What happens with no guards. The fault was planted; the result is not a pass

Three probes, per the brief, plus one added to disambiguate the first result. Abort
contract honoured: every path was a **new filename**, no existing tracked file was targeted,
and each file that landed was deleted in the immediately following command.

| # | Target | Result | rc / banner |
|---|---|---|---|
| C1 | **Absolute backbone protected path.** `SRInSARThesis/chapters/07_Cerberus_system/_archive/LANE_PROBE_2026-08-27_cbcontrol.md`, covered by the registry glob `**/_archive/**` | **DENIED** | Denied by the **auto mode classifier**. No `protected_guard.py` banner. Nothing written; verified absent afterwards. |
| C2 | **Bare protected filename.** `SRInSAR_Chapter7_1407_MAB.docx`, written into the session scratchpad where no such file existed | **ALLOWED** | `File created successfully`. **No banner of any kind.** Deleted immediately; `ls` -> `No such file or directory`, rc=2. |
| C3 | **Benign repo-local.** `./LANE_PROBE_benign_2026-08-27.md` | **ALLOWED** | `File created successfully`. No banner. Deleted immediately; `git status --porcelain` back to `?? DeviceControl_Student.c` alone. |
| C4 | **Added probe. Non-protected backbone path**, `SRInSARThesis/chapters/07_Cerberus_system/LANE_PROBE_2026-08-27_cbcontrol.md`, not in the registry | **DENIED** | Auto mode classifier again. Nothing written. |

**A correction to the brief's expectation.** The brief predicted ALLOW on all three. **C2
and C3 came back ALLOW as predicted. C1 was DENIED, and the added C4 was denied too, so
the blanket-ALLOW expectation is contradicted.** The mechanism matters more than the
outcome, because the denials came from a layer that is **not this repo's**:

**Nothing in this repo denied anything.** The denials in C1 and C4 came from the
**user-level auto mode classifier** configured in `~/.claude/settings.json`, which is
global, cwd-independent, and is not one of the estate's 27 hooks. The refusal text was the
generic classifier message. `protected_guard.py` never ran, could not have run, and issued
no banner in any of the four probes.

**The added probe C4 is what makes C1 interpretable.** C1 and C4 were denied alike, one
inside a registry-covered folder and one not. So the classifier was **not** consulting
`PROTECTED.md`; it was making a model judgement about writing scratch files into a thesis
chapter tree.

⚠ **Confound, stated rather than glossed.** Both denied probes carried self-identifying
content ("TRANSIENT GUARD PROBE", "Delete on sight"), which plausibly drove the classifier's
judgement. **[NOT DETERMINED]** whether an ordinary-looking write to the backbone from this
cwd would be denied. I did not test that, because doing so would mean landing plausible
content in the backbone, which RULING.md and the read-only constraint forbid.

**Two further classifier denials in this session, which bear on how C1 and C4 should be
read.** [MEASURED] A plainly **read-only** `ls` of two sibling directories under
`~/.claude/projects/` was denied, as was the **`git push`** of this audit (section E). Four
denials in total, spanning a write probe, a read-only listing and a push to the repo's own
origin. The classifier is therefore reacting broadly to estate-internals-shaped tool calls,
not making a targeted judgement about registry paths. This **weakens the confound above**:
the C1/C4 denials were probably not driven by my probe filenames. It equally means the
classifier **cannot be characterised as a substitute for `protected_guard.py` in either
direction**: it denied four calls that a guarded session would have permitted (three of them
harmless), and it permitted the one call, C2, that a guarded session would have denied
mechanically. Being both more restrictive and less protective than the hook it is standing
in for is the point.

**C2 is the decisive result and it is unaffected by that confound.**
`SRInSAR_Chapter7_1407_MAB.docx` is a **bare filename** in the registry. `PROTECTED.md`
states the rule explicitly: *"A glob with no `/` is a bare filename and protects that name
anywhere, including outside the repo."* It is there because that file is the Ch7 master and
the standing rule is **never write the master**. In a guarded session, `protected_guard.py`
denies that write mechanically, before the tool call, with a banner. **From this cwd it
succeeded silently.** The registry's strongest and most deliberately global protection did
not exist.

### C4. The exposure, stated plainly

Over the period the five notes cover, a session in this directory ran with no repo-level
guard whatsoever, and the exposure is wider than "the hooks did not fire". `~/.claude/settings.json`
grants `autoMode.allow` for *"Reading and writing files across the six first-party
repositories ... including from a session whose working directory is a different one of
those repositories"*, so cross-repo writes are policy-permitted from here by design. The
countervailing rule, `autoMode.soft_deny`, reads *"Never write to a path listed in **the
working repository's** `PROTECTED.md` registry"*, and **this repo has no `PROTECTED.md`**
[MEASURED, A4]. From this cwd that rule has no registry to consult: it is not merely
unenforced, it is **vacuous**, and it degrades to whatever a general-purpose classifier
happens to judge, which C2 shows can be a silent allow on the single most protected
filename in the estate. Concretely, a session here could have overwritten the Ch7 master or
its supervisor-sent copy, either of which is hand-edited, carries tracked changes and Word
comments, and per `PROTECTED.md` is *"not recoverable from git"*; it could have written into
`**/_archive/**` or `**/_backup*/**`, the restore points that exist precisely so a bad write
is survivable; it could have re-saved
`SRInSAR_Chapter3_18052026_MAB_replies_DAR.docx`, where a re-save alone destroys the
2-second timestamp evidence without any delete; and none of it would have appended to
`PROTECTED_LOG.md`, because that log records unlock tokens minted through the guard, and the
guard was never in the path. The audit trail that makes every legitimate bypass *"loud,
deliberate and permanent in the git history"* records nothing at all about a session that
was never subject to it. I found **no evidence that any such write occurred**, and I did
not go looking through the backbone's history for one; what is established is that for the
period in question the mechanical protection was absent, unlogged, and silently so.

---

## D. This repo against its own record. ENUMERATED, NOT RESOLVED

### D1. Commanded read-only checks, verbatim

```
$ git status --porcelain
?? DeviceControl_Student.c
rc=0

$ git log --oneline -10
ec9f5a5 Add real-time trigger-alignment/flyback audit note (2026-07-13)
3c4f2c1 Tidy main: archive scratch binaries, untrack ignored files
3a61821 OVR mitigations and portable Claude context
4bfcaee Commit with updated pointer, max hold, sidecar. See working.md for MATLAB import notes.
595ed0f Commit to prepare for automatic chirp re-alignment and max hold features.
2f84d9c Updated timings to add ScanInterval functions, identfied potential ADC impedance bug.
c6e53f0 Commit pre-rework of ADC timings.
40732fa Updated more plot and trigger functionality.
94df44a Updated further to improve plot appearances.
d6fb4e0 Updated with DDS changes, now more reliable. Need to further review Sync with MT Manager.
rc=0
```

Both exit 0, run without a pipe. Branch `main`, remote
`origin https://github.com/mvallis/CerberusControl.git`.

### D2. `CLAUDE.md` claims

| Claim | Verdict | Evidence |
|---|---|---|
| `HALF_BUF_SAMPLES` = `8388608U` | **STILL TRUE** | `DeviceControl_FullThreaded.c:960` |
| `SCANS_PER_HALF` = `HALF_BUF_SAMPLES / ADC_NUM_CH`, 4 194 304 | **STILL TRUE** | `:962` |
| `ALIGN_CRI_MAX` = `SCANS_PER_HALF / 4U` | **STILL TRUE** | `:996`, comment confirms 1 048 576 |
| `SAVE_QUEUE_CAP` = `64` | **STILL TRUE** | `:970` |
| `TSQ_WRITE_MS` = `0`, non-blocking | **STILL TRUE** | `:976`, *"drop immediately if queue full"* |
| `SAVE_NONE`/`SAVE_THREAD`/`SAVE_TOFILE` = 0/1/2 | **STILL TRUE** | `:998`, `:999`, `:1000` |
| `MasterAcqRecordCB` and `AdcRecordCB` use `SAVE_THREAD`; `AdcSaveCB` still `SAVE_TOFILE` | **STILL TRUE** | `:2905` *"AdcRecordCB, SAVE_THREAD mode"*, `:2935` *"AdcSaveCB, SAVE_TOFILE mode"*, `:3698` *"MasterAcqRecordCB, start recording (SAVE_THREAD mode)"* |
| Three-stage pipeline is *"Planned fix (not yet implemented)"* | **STILL TRUE** | [SEARCHED: `grep -niE 'CopyThread\|copyQueue\|indexQueue\|idxQueue\|freeSlotPool'` -> 0 hits] |
| Cerberus range-Doppler only *planned* (implied by the reference-systems entry) | **STILL TRUE** | [SEARCHED: `grep -c 'rdResult0\|RDP_TAB_GRAPH_RD_LRX'` over `.c` and `.h` -> 0, 0]; `WORKING.md:901` is an implementation *plan* |
| Key files table lists **`DeviceControl_Full_Threaded.cws`** | **SUPERSEDED** | [MEASURED] `ls *.cws` -> `No such file or directory`; `git ls-files \| grep -i cws` -> none. **Both** `DeviceControl_FullThreaded.cws` and `DeviceControl_Full_Threaded.cws` were deleted in `3c4f2c1`, which is **after** `3a61821` wrote this table. `CLAUDE.md` was not updated to match. |
| Key files: `.c`, `.uir`, `.h`, `dds.c`/`.h`, `Wd-dask64.h`, `WORKING.md`, `TriggerAlignment_ImplementationNotes.md` | **STILL TRUE** | All present, `ls` |
| Pinned OVR symptom and 18 MS/s workaround | **NOT DETERMINED** | A runtime/empirical claim. Not falsifiable from a static tree with no hardware and no build. Left open deliberately. |
| ADC calibration five-step order | **NOT DETERMINED** | Would need a call-order trace through the acquisition path. Not attempted; out of the read-only scope I set. |

### D3. `TriggerAlignment_ImplementationNotes.md` claims. **The headline (D) item**

This document is **substantially superseded and carries no supersession banner.** It reads
as current design guidance and it is not.

| Claim | Verdict | Evidence |
|---|---|---|
| *"Each DRCTRL trigger period produces 1024 ADC scans per channel: 512 valid IF chirp samples followed by 512 dead"*, i.e. **50 % duty** | **SUPERSEDED** | `REALTIME_ALIGNMENT_AUDIT_2026-07-13.md` measured, across **all 16** of the 13/07 `.hdr`, `SampsPerChirp_ADC` = `SampsPerChirp` = **1024** inside `ScansPerChirp` = **16 384 / 32 768**. That is roughly 3 to 6 % duty, not 50 %. The audit says so directly: `CVI_TriggerAlignment_Patch_1.c` *"assumes 50% duty; does not fit low-duty captures"*. |
| Add `ADC_DetectTriggerAlignment()`, first-half vs second-half energy comparison | **SUPERSEDED, never shipped** | [MEASURED] `grep -c 'ADC_DetectTriggerAlignment'` -> **0**. What shipped is `ADC_AlignDeferred` (`:2076`), a different algorithm: coarse max-integrated-chirp-width energy, then a bounded ±16-sample fine stage on lower-quarter spectral magnitude with DC and flyback excluded. |
| Add statics `alignConfidence`, `alignChecked`, `alignCheckDone`; gate on `alignCheckDone` | **SUPERSEDED** | [MEASURED] 0 hits for all three. |
| Add static `chirpSampleOffset` | **STILL TRUE** | Shipped: declared `:1046`, applied `:1633`, set `:2136`/`:2146`/`:2194`, reset to 0 `:2504`. The one element of the design that survived. |
| *"Check once per run"*, automatic on the first half-buffer, in the plot deferred callback | **SUPERSEDED** | Shipped behaviour is **manual only**. `:1641` comment: *"Alignment capture, one-shot, triggered by Re-align button"*, gated `if (alignRequested && !alignBufReady)`. The audit's *"runs on the MANUAL 'Re-align' button only (no automatic call)"* is **STILL TRUE**. |
| UIR: add `ADC_TAB_ADC_MSG_ALIGN` | **STILL TRUE, and done** | `DeviceControl_FullThreaded.h:50`, `control type: textMsg`. Used at `:2099`, `:2107`, `:2139`, `:2149`, `:2198`, `:2979` to `:3022`. |
| Files table names **`CVI_TriggerAlignment_Patch.c`** | **SUPERSEDED, dangling** | [MEASURED] `ls CVI_TriggerAlignment_Patch.c` -> absent. Tree has only `CVI_TriggerAlignment_Patch_1.c` (8 600 B). |

### D4. `REALTIME_ALIGNMENT_AUDIT_2026-07-13.md` claims

Holds up considerably better than D3.

| Claim | Verdict | Evidence |
|---|---|---|
| `chirpSampleOffset` defaults 0 and is reset each acquisition start | **STILL TRUE** | `:1046` initialised `0`; `:2504` reset in the stop path |
| `ADC_AlignDeferred` runs on the manual Re-align button only, no automatic call | **STILL TRUE** | Exactly one `PostDeferredCall` to it, `:1647`, guarded by `alignRequested`, commented *"triggered by Re-align button"* |
| `MAX_HOLD_RANGE_MIN_M` = 1.9 m | **STILL TRUE** | `:989` |
| `CVI_TriggerAlignment_Patch_1.c` is an **UNSHIPPED design spec** | **STILL TRUE, and now independently confirmed** | [MEASURED] `grep -c 'CVI_TriggerAlignment_Patch' DeviceControl_Full_Threaded.prj` -> **0**. The `.prj` compiles only `DeviceControl_FullThreaded.c`, `dds.c` and headers. The file is in the repo but not in the build. |
| `DeviceControl_FullThreaded_fixed0704026.c` predates the alignment work | **SUPERSEDED, dangling** | [MEASURED] `ls` -> absent. [SEARCHED: `git log --all --oneline -- '*fixed0704026*'` -> no commits.] The audit reasons about a file that is not in the tree and has no commit history here. |
| The 13/07 `.hdr` measurements (`SampsPerChirp_ADC` = 1024, etc.) | **NOT DETERMINED** | The `.hdr` files are capture data, not in this repo. The `.c` writes these keys (`:1417`, `:1424`, `:1428`) as runtime values, so the field names are confirmed but the 13/07 values are not re-verifiable here. |
| *"the real-time path is NOT exposed. No correctness fix required."* | **NOT DETERMINED** | The supporting sub-claims above are STILL TRUE, but the conclusion is a runtime-correctness judgement. Not something a static read can confirm or refute, and I decline to ratify it. |

### D5. `GEMINI.md` claims

No falsifiable claims about tree state; it is a role and constraints document. Its
constraints are all STILL TRUE. Its **gaps** are the finding, and they are in A2, not here.

### D6. The pattern across D2 to D4

Three of the four documents contain at least one dangling reference to a file that is not
in the tree (`.cws` in `CLAUDE.md`, `CVI_TriggerAlignment_Patch.c` in the Notes,
`DeviceControl_FullThreaded_fixed0704026.c` in the audit). In two of those three cases the
file was removed by a **later** commit than the document that names it, and the document
was not revisited. `3c4f2c1 "Tidy main"` is the common cause. **Enumerated. Not resolved,
and nothing was corrected.**

---

## E. Provenance of this file

Written by the cbcontrol lane, 2026-08-28, dated 2026-08-27 per the commanded filename.
This file is the **only** file in this repository created or modified by this session. The
three probe files of section C were deleted within one command of landing and are verified
absent. `DeviceControl_Student.c` remains untracked, unread and untouched. No `.claude/`
was created. No memory note was created, moved, copied, promoted or edited. Nothing in the
backbone or in BB24_DB was modified.

Committed alone, with `git commit -- LANE_AUDIT_2026-08-27.md`, so that the untracked
student file cannot be swept in. [MEASURED] `git show --stat HEAD` -> `1 file changed`.

⚠ **PUSH STATUS: NOT PUSHED.** `git push origin main` was **denied by the auto mode
classifier**, the same layer as C1 and C4. The push was not retried in another form. This
file is therefore **durable locally and unpushed**; the commit stands on `main` ahead of
`origin/main`. Mark can complete it from this session with:

```
! git push origin main
```

**A note on this file's own prose, since section B adjudicates a rule about exactly that.**
Zero U+2014 [MEASURED: Python `str.count(u'—')` -> 0]. Bold run-in labels **are** used
to open several paragraphs here. `voice-rules.md` §5 scopes its bold-opener ban to *chapter
body prose* and expressly permits bold run-in labels in *"genuine reference structures"*. I
read an audit report as a reference structure, not chapter body prose, so the ban does not
bite. Stating the reading rather than leaving it unexamined, because I am the one who
surfaced the rule.

---

## F. The one question for Mark. STATED, NOT ANSWERED

⛔ I do not decide this. Three options, their consequences, and a recommendation.

**The question.** This repo has no `.claude/` and therefore no guards, yet sessions have
run here and filed memory here. Should it be guarded, left unguarded deliberately, or is
the real answer that BB24_DB work should never have been done from this cwd?

### Option 1: Guard it

Add a `.claude/` here: `settings.json` with at minimum `protected_guard`,
`estate_rules_guard` and `session_close`, a `PROTECTED.md` (even a near-empty one, so that
the `autoMode.soft_deny` rule stops being vacuous per C4), and a memory home with a real
`MEMORY.md`.

*Consequences.* Closes the C2 hole: the bare-filename registry entries regain their
mechanical force from this cwd. Makes the soft-deny rule operative rather than vacuous.
Costs a shim to maintain in a repo that is not a thesis repo. Note two measured limits: this
repo has **no Python files** of its own, so the hook scripts would have to be referenced by
absolute path into the backbone; and the backbone's 27 hooks are mostly thesis-specific
(`gap_row_lint`, `exemplar_brief_check`, `config_inventory_guard`) and would be noise here.
The BB24_DB lane reached the same shape of conclusion for its repo and proposed a partial
shim rather than the full set.

### Option 2: Leave it unguarded, deliberately and on the record

Accept that this is a hardware control repo, not a thesis repo, and that the classifier plus
git is protection enough.

*Consequences.* Cheapest, and honest if adopted explicitly. But C2 is the cost, stated
concretely: from this cwd the Ch7 master's bare-filename protection is silently absent, and
`PROTECTED.md` itself says those `.docx` files are *"not recoverable from git"*, so the
"git is the real backstop" argument does not cover the exact files the registry cares most
about. Choosing this means accepting that a session here can silently write a protected
name, with nothing in `PROTECTED_LOG.md` to show for it. If chosen, it should be **written
down in `CLAUDE.md`** so the next session knows it is a decision and not an oversight.

### Option 3: The real answer is that BB24_DB work should never have run from this cwd

Treat the guard gap as a symptom. The disease is that two repos' work ran from one cwd, so
the memory filed under the launch directory rather than the subject.

*Consequences.* Fixes the cause rather than the symptom, and it is free: no shim, no
config, just the discipline of launching sessions from the repo being worked on. It does
**not** close the C2 hole, because a session legitimately working *on Cerberus* from this
cwd is still unguarded. And it is retrospective only in part: the four notes are already
stranded and would still need re-homing per B4.

### Recommendation

**Option 3 plus a reduced Option 1, and explicitly not Option 2.**

Option 3 is the correct diagnosis and should be adopted regardless of the rest. Two lanes
working from opposite ends found the same stranding, and it has a single cause that costs
nothing to fix going forward.

But Option 3 alone leaves C2 open, and C2 is the finding I would not want left standing.
The minimum that closes it is small and specific: a `PROTECTED.md` in this repo (which
un-vacuums the `autoMode.soft_deny` rule per C4) and a `.claude/settings.json` wiring
`protected_guard.py` by absolute path into the backbone. That is two files. It does not
require importing the thesis hook suite, and it is a smaller change than the sibling shim
the BB24_DB lane sketched.

I recommend **against** Option 2 on one measured ground rather than on principle: the
registry's bare-filename mechanism exists specifically to protect files *outside* the repo
that owns the registry, and C2 demonstrates that mechanism failing silently from here. A
protection whose entire purpose is to reach across repo boundaries is not one to switch off
per repo.

⛔ **Stated, not decided. Over to Mark.**
