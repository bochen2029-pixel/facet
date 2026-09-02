# What comes after facet — a third tool for this box

*Brainstorm, 2026-09-02. vramtop answered "who holds the VRAM"; facet answers "where did the
files go". Both are the same move: the OS already has the data, the stock UI refuses to pivot
it, so write the view — one C++ exe, OS APIs only, headless first, a window second, JSON/MCP
for the agents. The candidates below are ranked by how well they fit that move on Windows 11.*

## 1. iowho — who is writing what, right now  (recommended)

> **Became everywho.** Designed in full the same day as a blueprint ready for implementation:
> `C:\Intellect_AI_tools\everywho` (public: https://github.com/bochen2029-pixel/everywho) —
> architecture, the ETW spec, CLI/JSON contracts, GUI and test plans, decision records, contract
> headers. Everything below was reviewed against it; the manifest providers, the treemap,
> files-per-minute, the handle finder (as everywho's `--open` and its open-file rundown) and
> the process-tree provenance are folded in there. That folder is the canonical design.

facet infers write bursts after the fact from modified times: "2,797 files in one minute under
X". It cannot say *which process* did it. Windows can: the kernel's file and disk ETW providers
(`Microsoft-Windows-Kernel-File`, `Microsoft-Windows-Kernel-Disk`) attribute every create,
write, rename and delete to a PID, live. Nothing in the box shows that as a picture; Resource
Monitor buries it, Process Monitor floods it.

- **Live**: process × directory × bytes/s and files/min, as a treemap and a table; the burst
  facet's live twin. "What just wrote 2,000 files" is answered while it happens, with a PID.
- **Headless**: `iowho --json` snapshots, `--watch` NDJSON, `--spool` for TOWER's vitals lane
  like vramtop, `--mcp` for agents, and `--paths PID|NAME` emits the files a process touched
  as a **tape** — straight into `facet --files-from -` for the shape, or `everywhere` for the
  contents. That is the fourth organ joining the trilogy through the same contract.
- **Provenance**: which agent session wrote where (a Claude Code process tree → its cwd →
  its session tape); which backup job runs at 03:00; which installer sprayed AppData.
- **Cost**: kernel ETW needs elevation (or the Performance Log Users group); document it like
  Process Monitor does. ~2 weeks in the vramtop rhythm: collector organ (TDH event parsing),
  fold (process/dir trie, the same trie as facet), console, treemap, selftest with a planted
  write storm as the oracle.

## 2. portwho — who owns which connection

The network twin: `GetExtendedTcpTable` / `GetExtendedUdpTable` for owner PIDs, ETW
`Microsoft-Windows-TCPIP` or the per-process network counters for bytes/s, reverse DNS with a
cache. A treemap of processes × remote hosts, a table with country/ASN off a local table,
`--json`, `--spool`, `--mcp`. Useful for an agent fleet ("which agent is talking to which
endpoint, how much") and for the "why is my link saturated" moment. Smaller than iowho; no
elevation for the tables, elevation for per-process bytes.

## 3. handlewho — why can't I delete this

`NtQuerySystemInformation(SystemExtendedHandleInformation)` + `NtQueryObject` names: which
process holds a file, directory or mutex open. Process Explorer's "find handle" as a one-liner
(`handlewho C:\some\dir`), JSON for agents, and a rail by process. Pairs with facet's context
menu ("who has this open?"). Small; elevation only for other users' processes.

## 4. fleet — one pane for the agent swarm

Not a new collector: a board that reads vramtop, iowho, portwho and everywhen and shows the
running coding-harness sessions (process tree → cwd → session tape → last message time) with
their GPU, disk, network and last activity. This is what TOWER seems to be reaching for; the
tools above are its instruments, so build them first and let the board be the thin part.

## What I would do next, in order

1. Use facet daily for a week; pin the standing excludes; note every "I wish it also…" in the
   devlog. Tools get their shape from use, not from planning.
2. Wire the agents: `claude mcp add facet -- C:/facet/facet.exe --mcp`, then give everywhere
   and everywhen an `--mcp` (shape B, two afternoons) so a subagent can chain names → contents →
   sessions without a shell.
3. Build iowho as the fourth organ, tape-native from day one.
4. Publish everywhen and everywhere the way vramtop and facet are published, once their READMEs
   read like contracts; the family is worth more together than apart.
5. Then the fleet board — thin, over the four instruments and the concordance.
