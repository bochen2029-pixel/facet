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
