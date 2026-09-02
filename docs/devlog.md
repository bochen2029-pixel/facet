# facet — development log

Running notes, newest at the bottom. Decisions, measurements, test results.

## 2026-09-01 · kickoff

- Problem: Everything answers "which files match" but never "how are the matches distributed",
  so you cannot discover what to exclude; you page past it. The pivot that is missing is a
  facet rail (directory tree / extension / modified / size / write-burst, with counts) whose
  clicks compile back into Everything syntax.
- Measured on this box (es.exe export + 40-line Python demo, see the session): 7,135,466 items
  indexed; 526,574 .md; 3,811 .md modified in 3 days; of those only 173 under AppData but 2,797
  written in ONE minute (2026-08-30 08:37) under C:\deepseek-harness-master. Static exclude
  lists cannot track that; a live facet can. Files-per-minute is a provenance signal Everything
  does not have.
- Decision: front end, not indexer. Ride Everything's index over its WM_COPYDATA IPC
  (QUERY2/LIST2 — the same channel es.exe uses), request NAME+PATH+SIZE+DATE_MODIFIED only
  (date created/accessed are NOT indexed here: asking for them makes Everything hit the disk
  per row). Never load a DLL. C++ only, OS APIs only, single exe — the vramtop pattern.
- Decision: every filter is compiled into the query and re-run through Everything (server-side),
  so facet counts are exact and the printed query is exactly what the user sees. No client-side
  filtering.
- Decision: streaming pages of 65,536 items; facets are aggregated per page so memory stays
  bounded (directory trie + extension map + fixed buckets + one (mtime,dir) event per row for
  burst clustering). Rows are kept verbatim only up to a cap (--list / the window).
- es.exe argv trap (logged so nobody repeats it): from Git Bash a single quoted argument
  becomes ONE literal phrase for es.exe and every count reads 0. Verify query syntax through
  facet's own IPC path (`facet -c`), never through es.exe from bash.

## 2026-09-01 · milestone 1 — headless CLI works end to end

- IPC layout (QUERY2 = 7 DWORDs + UTF-16 string; LIST2 = 5 DWORDs + ITEM2[] + data; strings as
  DWORD len + chars + NUL; sizes LARGE_INTEGER; dates FILETIME) verified empirically: counts
  equal es.exe (ext:md 526,575 vs 526,574 a minute earlier), paging 3x7 == 1x21, unicode names
  intact. Everything sends the reply re-entrantly during our SendMessage — SMTO_NORMAL + a
  message pump handle both orders.
- Query syntax facts, measured through facet -c (not es.exe from bash — see the argv trap):
  `!path:"C:\dir\"` excludes a subtree exactly (3,812 - 2,841 = 971); `<path:"A\"|path:"B\">` ORs;
  `dm:A..B` is inclusive at day and at second precision (`dm:...T08:37:53..T08:37:53` = 2,797);
  relative windows must be plural (`dm:last1hour` = 0, `dm:last60mins` = 9, `dm:last24hours` ok,
  `dm:last1day` = 0 but `dm:last1days` ok) → --since compiles hours to mins and weeks to days;
  `dm:>=DATE` and `dm:DATE..` both work; `size:1..4095` byte ranges work; `file:` / `folder:` work.
- --selftest verifies every modified bucket, every size bucket, the two biggest bursts and the
  biggest directory's include/exclude against Everything's own counts: all equal.
- Speed: -c 37 ms; 3,812 items 29 ms; 382,307 items 373 ms; whole disk 7,135,216 items 10.2 s
  (page 262144) / 10.7 s (65536) / 12.6 s (16384) — Everything's formatting dominates, not the
  fold. Peak working set for the whole disk ~460 MB (16-byte event per dated item + trie).
- Burst "where": descend while one child holds >= 90 %; on a split name the biggest pieces
  (each descended the same way) relative to the split point. Bursts compile to dm:START..END.
- Measurement trap (mine): a peak-memory probe that redirected stdout and only polled HasExited
  deadlocked — the child blocks on a full pipe. Read the pipe or don't redirect.

## 2026-09-01 · milestone 2 — the window

- Shape: query box (a subclassed EDIT) · chips bar · facet rail (custom-drawn, share bars behind
  labels, section headers fold) · virtual results table (custom-drawn, server-side sort by
  header click) · status line carrying the compiled query. Worker thread owns its own
  Everything (one IPC reply window per thread); the UI thread only paints; a newer request
  cancels the pass in flight at its next page (generation counter checked in the sink).
- Every pick is a Filter chip; chips compile through the same query.h as the CLI, and the query
  is re-run through Everything — no client-side filtering anywhere, so window counts, report
  counts and Everything's status bar agree by construction.
- Verified with real window messages (PostMessage clicks/keys from a DPI-aware pwsh driver,
  PrintWindow captures): drill 3,812 → 2,841 · Esc → 3,812 · exclude → 971 · sort by size ·
  keyboard selection · second exclude 971 → 798 · clean exit writing facet.ini.
- Traps hit and logged:
  · gdiplus.h needs <objidl.h> (WIN32_LEAN_AND_MEAN drops IStream/PROPID) and wants min/max as
    functions once NOMINMAX is on (`using std::min; using std::max;` before the include);
    `small` is a macro from rpcndr.h — never name a variable that.
  · The window is sized in PHYSICAL pixels under PerMonitorV2: on this 225 % display a
    1280×820 window is a postage stamp and the Path column collapsed to zero width. Scale the
    default / minimum size by GetDpiForSystem.
  · Without WS_CLIPCHILDREN the parent's double-buffered paint wipes the EDIT child; its text
    only reappeared while the caret blinked. Symptom: the query box goes blank on focus loss.
  · A DPI-unaware driver process sees a virtualized client rect (2762 → 1227) while the
    coordinates it posts are taken as physical by the receiver — clicks land on the wrong rows.
    SetProcessDpiAwarenessContext(-4) in the driver fixes both the hit-testing and the capture.
  · PrintWindow(PW_CLIENTONLY | PW_RENDERFULLCONTENT) captures child controls too; force
    RedrawWindow(RDW_ALLCHILDREN | RDW_UPDATENOW) first. Downscaling the PNG with bicubic made
    it 2.6× LARGER (ClearType noise compresses badly) — keep the native capture (~300 KB).
- Rail defaults in the window: top 8, depth 2, and a subtree only opens when it holds >= 5 % of
  the result set (also applied to the report), so the other facets stay reachable.
- Everything 1.4 `parent:"C:\NEW\"` selects the files directly inside a folder (4 of the 145 in
  the subtree here) and negates cleanly (`!parent:` = 3,806); `infolder:` and `nosubfolders:`
  give the same count. The window's "(files right here)" rows compile to it.
- Window tally shows "searching… N of T" from a per-page atomic while a pass runs; typing while
  the table has focus is forwarded to the query box; `-n` caps the window's pass like the CLI.
- C:\Everything\README.md (the AI-session locator doc) now points at facet for the "where did
  the matches go" question, so future sessions find it without being told.

## 2026-09-02 · Everything found / started / explained · the app icon · Start Menu entry

- Bo's first run from a fresh PowerShell hit "Everything is not running" (only the -svc service
  was up; the tray instance that owns the IPC window was not). Now connect() looks for
  Everything.exe (override / FACET_EVERYTHING → registry App Paths + Uninstall keys, both views
  → Program Files / LocalAppData\Programs / scoop / chocolatey → PATH), starts it with -startup
  (tray only), waits for the IPC window and for IsDbLoaded, notes it on stderr (or the window's
  tally) and carries on. --no-start turns it off. On this box the 1.4 x86 installer left
  InstallLocation + DisplayIcon under the WOW6432Node Uninstall key; that is what facet reads.
- Nothing installed anywhere → the error is a paragraph: what Everything is, the download page,
  run it once so it indexes, or point facet at a portable copy. -j puts the same text in "error".
- Everything 1.5 alpha runs as a named instance whose IPC window is
  EVERYTHING_TASKBAR_NOTIFICATION_(1.5a); connect() tries both classes.
- facet --make-icon writes the runtime icon as a .ico (ICONDIR + 32-bpp DIBs, opaque AND mask,
  16…256 px); facet.rc embeds it so Explorer and the Start Menu show it. facet --shortcut writes
  a .lnk (IShellLinkW) to facetw.exe --gui into the Start Menu (or the desktop), which is how
  the window becomes "type facet in Start" like Everything itself.
- Published: github.com/bochen2029-pixel/facet (public, MIT), release v0.1.0 with the two exes.

## 2026-09-02 · 0.2.0 — the cell menu: only / not by name, folder level, size, date

- Bo's ask: right-click any column and exclude or keep only "this". Tightened into: the column
  decides what "this" means, and the menu names the term it adds. Path → every folder level from
  the file's own folder up to the drive (only + not) plus the files directly in it (`parent:`);
  name → exact name (`wfn:"…"`) and extension; size → exact bytes, `>=`, `<=`, and the bucket;
  modified → the day, the minute, `>=`, `<=`. All verified as Everything 1.4 terms by `facet -c`
  (`wfn:` 1 of 316 titanic files; `size:302` 1; `dm:2026-07-12` 19; minute range 2; `!dm:` and
  `!size:a..b` negate cleanly).
- `column_picks()` is a pure function in facets.cpp; --selftest checks every column's terms and
  groups. Includes on name / size / date are single-valued (a new pick replaces the old chip);
  excludes accumulate. Chips wrap onto up to five rows; the bar grows.
- Verification without touching a modal menu: WM_APP+7 (column, index) applies a pick to the
  selected row; FACET_LOG=FILE makes the window log clicks, picks, submits and results. The
  driver read back: wfn → 1, size:>= → 2, dm:day → 300, and the log showed the earlier "drill
  did nothing" was the driver's stale geometry (the chip bar is 34 logical px empty, not 30).
- README hero is Bo's own capture (titanic, 316 items); docs/trilogy.md holds the brainstorm for
  wiring everything · everywhere · everywhen through one path tape.
