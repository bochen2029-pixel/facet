// facet · es_client.h — the collector organ: stream an Everything query over IPC.
// One reply window per thread; results arrive as borrowed views inside the sink callback.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "app_util.h"

namespace facet {

struct EsItem {                   // views into the IPC reply — valid only inside the sink
    const wchar_t* name = L"";
    uint32_t name_len = 0;
    const wchar_t* path = L"";    // parent directory as Everything spells it ("C:\Users\me"; "C:" at a root)
    uint32_t path_len = 0;
    uint64_t size = kUnknown64;   // bytes; kUnknown64 when not reported (folders)
    uint64_t mtime = kUnknown64;  // FILETIME ticks, UTC; kUnknown64 when none
    bool folder = false;
    bool drive = false;
    uint32_t matches = 0;         // content hits in this file (a scan's count), 0 = none / no scan
};

struct EsPage {
    uint32_t total = 0;           // matches for the whole query
    uint32_t offset = 0;          // index of items[0]
    uint32_t count = 0;           // items in this page
};

// return false to stop streaming
using EsSink = std::function<bool(const EsPage&, const EsItem* items, uint32_t n)>;

struct EsInfo {
    int major = 0, minor = 0, revision = 0, build = 0;
    bool db_loaded = false;
    bool size_indexed = false, modified_indexed = false, created_indexed = false;
    std::string version() const;   // "1.4.1.1026"
};

uint32_t ipc_sort(SortKey k, bool ascending);

struct EsLaunch {                 // how connect() may bring Everything up when it is not running
    bool allow_start = true;      // find Everything.exe and start it "-startup" (tray only, no window)
    std::wstring exe_override;    // --everything-exe PATH / FACET_EVERYTHING
    uint32_t wait_ms = 30000;     // for the IPC window, then for the database
};

// Where an Everything.exe lives: the override, FACET_EVERYTHING, the registry (App Paths and the
// Uninstall keys, both views), the usual install folders, scoop / chocolatey, then PATH.
// Empty when none exists.
std::wstring find_everything_exe(const std::wstring& override_path = L"");
// The text printed when no Everything.exe exists anywhere: what it is, where to get it, what to do.
const char* everything_install_hint();

// Talks to the running Everything instance. The reply window is created on the thread that
// first queries, so keep one Everything per thread.
class Everything {
public:
    struct Impl;
    Everything();
    ~Everything();
    Everything(const Everything&) = delete;
    Everything& operator=(const Everything&) = delete;

    EsLaunch launch;                                    // knobs for connect()
    std::function<void(const std::string&)> on_note;    // "starting Everything…" lines (stderr / status)

    bool connect(std::string* err = nullptr);   // find the IPC window (starting Everything if allowed), read version + index flags
    const EsInfo& info() const;
    bool fast_sort(uint32_t ipc_sort_type) const;
    bool running() const;                       // an IPC window exists right now

    // Stream q in pages of page_size, sorted by sort_type. Stops after max_items (0 = all) or
    // when the sink returns false. False (with err) only on IPC failure.
    bool query(const std::wstring& q, uint32_t sort_type, uint32_t search_flags, uint32_t max_items,
               uint32_t page_size, uint32_t request_flags, const EsSink& sink, std::string* err);
    bool count(const std::wstring& q, uint32_t search_flags, uint32_t* total, std::string* err);
    uint32_t timeout_ms = 120000;

private:
    Impl* p_;
};

// Reply parser, exposed for --selftest: walks a List2 blob into items (views into data).
bool parse_list2(const void* data, size_t cb, std::vector<EsItem>& out, EsPage& page);

// OS glue (Windows-correct), defined in es_client.cpp
std::string narrow(std::wstring_view w);
std::wstring widen(std::string_view s);
uint64_t now_filetime();
double now_ms();                                                   // monotonic, for timing
std::string fmt_filetime(uint64_t ft, bool with_seconds = true);   // local "2026-08-30 08:37:12"
std::string fmt_filetime_iso(uint64_t ft);                         // local "2026-08-30T08:37:12"
std::string fmt_local_date(uint64_t ft);                           // local "2026-08-30"
std::string fmt_local_time(uint64_t ft);                           // local "08:37:12"
uint64_t local_midnight(uint64_t ft);                              // FILETIME of local 00:00 on ft's local day

}  // namespace facet
