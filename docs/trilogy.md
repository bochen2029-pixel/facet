# The trilogy — everything · everywhere · everywhen, one pipe

*Brainstorm 2026-09-02, morning. **Shape A is built the same day** (facet 0.3.0, everywhen
with `--paths` / `--json` / `locate`; everywhere already spoke the contract): pipelines 1, 2 and
3 below run as written, with measured numbers in the facet README. B, C and D remain proposals.*

## The three axes

| organ | answers | engine | already has |
|---|---|---|---|
| `C:\Everything` + **facet** | *which* files, by name / path / date / size — and *where they went* | Everything's live MFT index | `facet -l -j` rows · `-c` · `--mcp` · the compiled query |
| `C:\everywhere` | *which files contain* a phrase, set of literals, or regex | stateless GPU scan at drive speed; pattern count is free | `--files-from FILE\|-` · `--jsonl` · `-l` · `--patterns GROUPFILE --group-tags` |
| `C:\everywhen` | *which sessions* (Claude Code, DSH) said it — message-grain, fork-deduped by uuid | SQLite + FTS5 concordance over the tapes | `search --hours N --query Q` · rows carry file + line + uuid + session |

Each is strongest exactly where the others are weakest: names are free but shallow, contents are
true but cost I/O, sessions are structured but only cover the harness. The point is not a
fourth engine. It is a **contract** that lets a result set flow from one to the next.

## The contract: a path tape

One stream, two encodings, every tool reads and writes it:

- **plain** — one path per line (LF), or NUL-separated with `-0` for paths with newlines
- **JSONL** — one object per line, a superset schema; consumers ignore fields they do not know

```
{"path": "C:\\x\\y.md", "size": 1234, "modified": "2026-09-01T21:41:28",       // from facet / Everything
 "line": 88, "offset": 5120, "match": "titanic", "group": "ships",              // from everywhere
 "session": "6ebfe793-…", "uuid": "b1c2…", "ts": "2026-09-02T02:07:08Z", "role": "assistant"}   // from everywhen
```

Paths are normalized once, at the boundary: case-folded for comparison, `\\?\` prefix stripped,
trailing separator stripped. That is all the plumbing a shell needs to compose the three.

## What each tool adds to speak it (small, all C++)

- **facet**: `--paths` (emit paths only, LF or `-0`) and `--files-from FILE|-` (fold *a given
  list* instead of an Everything query: size and mtime via one `GetFileAttributesEx` per path —
  ~100 k files/s warm — so the rail works over any tape: "here is where the content hits live").
- **everywhere**: already there — `--files-from -` in, `--jsonl` / `-l` out. Add `-0` and, later,
  `--mcp` so agents get it structured.
- **everywhen**: `--paths` (the unique session tapes behind a hit, forks collapsed through the
  uuid dedup it already does), `--json` rows, and `locate FILE:LINE` (file + line → uuid, role,
  ts, session, branches) so a content hit found by everywhere in a tape resolves back to the
  message it lives in. Later `--family`: every session sharing any uuid with a hit (connected
  components over the fork graph) — "all the forks that touched this".

## The pipelines these unlock

1. **Names → contents → where.** *"Which of last week's markdown, minus the vendored repo,
   mentions the join, and where do those live?"*
   ```
   facet --paths -x C:\deepseek-harness-master ext:md dm:last7days
     | everywhere --files-from - -e "join" -l
     | facet --files-from -
   ```
   Everything narrows for free, everywhere reads only those bytes, facet shows the shape of
   the hits. The scan budget is visible before it runs: facet's tally is the byte count.

2. **Sessions → tapes → contents.** *"Which sessions that talked about facet also mention
   vramtop — counting each fork family once?"*
   ```
   everywhen search --hours 720 --query facet --paths
     | everywhere --files-from - -e vramtop -l
     | everywhen locate -
   ```
   everywhen finds and dedups, everywhere scans the raw tapes (true even for the last five
   minutes the index has not folded yet), everywhen resolves the hits back to messages.

3. **Contents → where (provenance).** *"Where do files containing an API key live, and when did
   they land?"* — `everywhere -l "API_KEY" C:\Data | facet --files-from -` gives the directory
   tree and the write bursts of the hits; a burst of 2,000 in a minute is a clone that brought
   the secret in, one file at 03:00 is the backup job.

4. **Set algebra across tools.** Everything's own syntax handles AND / OR / NOT at the name layer;
   across tools a tiny `tape` verb (or facet subcommands) does `and | or | not` over two path
   tapes with the normalization above. That is the "intersection or union" in one place.

5. **The harness-first future.** More of the day happens inside a coding harness than in a
   browser, so the *session* axis grows: everywhen gains adapters per tape format (Claude Code
   jsonl and DSH today; Codex, Cursor, Gemini CLI, opencode tomorrow — each is a few hundred
   lines of "what is a message, what is its uuid, what is the tool call"). facet over
   `~/.claude/projects` already shows session activity by project and burst; everywhere makes any
   tape searchable at drive speed before it is indexed. The concordance becomes the map of where
   the time went.

## Four shapes, in order of cost

- **A. Shell pipes on the tape contract** — cheapest, honest, each tool stays single-purpose.
  A day of work across the three. *Do this first regardless of what follows.*
- **B. A `--mcp` on all three** — facet has it; everywhere and everywhen get one. The agent is
  the orchestrator; paths stay data, nothing new to maintain.
- **C. A `tri` / `every` front verb** — sugar over A: `every files "ext:md dm:7d" grep "join"
  sessions "vramtop"`. Nice for humans, not necessary for agents.
- **D. One window, three engines** — facet's window grows a **contents** box: type a phrase and
  everywhere runs over the *current result set's paths*; hits become a CONTENT facet (per
  directory, per burst) and a match-count column, and the rail re-ranks by where the phrase
  lives. Then a **sessions** mode: the everywhen index as the source, rows are messages, facets
  are project / session family / role / day / tool. Same rail, same chips, same compiled query
  line — the query just gains a second and a third clause.

Recommendation: A now, then D one engine at a time, everywhere first: "of these 316 titanic
files, which mention *iceberg*, and where are they" is the most natural next click in the
window that exists today, and everywhere's cost is exactly the result set's byte count, which
facet already shows.

## Things to get right

- **Budget before scan.** everywhere is stateless, so cost is bytes: facet's tally (files,
  bytes) is the estimate; refuse or confirm above a threshold (say 20 GB) unless `--force`.
- **Truth ordering.** Everything's index is live to the USN journal; everywhen's index trails
  live sessions; everywhere is true at the moment it reads. Pipelines that end in everywhere
  are the ones to trust for "right now".
- **Dedup semantics.** everywhen collapses fork copies by message uuid; a "hit" should count a
  fork family once, and `--paths` must emit one tape per family (the canonical one) unless asked
  for all.
- **Paths with spaces and unicode** survive only through LF/NUL tapes and JSONL — never through
  shell argument lists.
- **Exit codes** stay rg-style (0 hit · 1 none · 2 error) so a pipe can short-circuit.
