// facet · pass.h — the pass, shared by the console modes and the window: a query (or a tape)
// streamed through Everything, optionally through everywhere ("contains"), folded into Facets.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "app_util.h"
#include "es_client.h"
#include "facets.h"

namespace facet {

struct Tape {
    std::vector<std::wstring> paths;   // unique, backslashed, no trailing separator
    size_t records = 0, dups = 0, bad = 0;
    bool nul = false;
};

struct Run {
    std::wstring compiled;               // in: a precompiled query (the window); out: what ran
    EsInfo info;
    double ms = 0;
    std::string err;
    uint32_t total = 0;                  // Everything's match count (after tape ops or a scan: the fold's count)
    std::string source = "everything";   // or "tape"
    std::string spec;                    // the tape's name
    size_t tape_paths = 0, tape_missing = 0, tape_records = 0, tape_dups = 0, tape_bad = 0;
    bool ops_active = false;
    // the content scan ("contains"), when o.grep is set
    std::wstring grep;
    std::string everywhere_exe;
    uint64_t scanned_files = 0, hit_files = 0, matches = 0;
    double scan_ms = 0;
    bool scan_capped = false;
};

struct PassHooks {
    std::function<bool()> cancelled;                                                   // polled between pages and while the scan runs
    std::function<void(const char* phase, uint64_t done, uint64_t total)> progress;    // "everything" | "everywhere" | "fold"
};

void configure(Everything& es, const Opts& o);   // start-if-needed, exe override, notes to stderr
bool run_pass(const Opts& o, Everything& es, Facets& f, Run& r, const Tape* inline_tape = nullptr, const PassHooks* hooks = nullptr);

// the shell form of a pass with a scan — what Ctrl+Shift+C copies from the window
std::wstring pipeline_text(const std::wstring& compiled, const std::wstring& grep, bool literal, bool icase);

}  // namespace facet
