// facet · scan.h — the everywhere seam: a content scan over a stream of paths, in-process.
// everywhere.exe (C:\everywhere: the GPU content grep) reads paths on stdin, we read its JSONL
// count records on stdout. Nothing is retained but the hits, so the base set can be the disk.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace facet {

// Where an everywhere.exe lives: the override, FACET_EVERYWHERE, next to facet.exe,
// C:\everywhere\build\Release, C:\everywhere, Program Files, then PATH. Empty when none.
std::wstring find_everywhere_exe(const std::wstring& override_path = L"");
const char* everywhere_install_hint();
std::wstring quote_arg(const std::wstring& a);   // one argument, CreateProcess / MSVCRT rules

struct ScanHit {
    std::wstring path;    // as everywhere reported it, backslashed
    uint32_t count = 0;   // matching lines in that file
};
struct ScanResult {
    bool ok = false;
    std::string err;
    uint64_t fed = 0;              // paths handed to everywhere
    std::vector<ScanHit> hits;     // files that matched, in everywhere's order
    uint64_t matches = 0;          // sum of the counts
    double ms = 0;                 // start → exit
    int exit_code = -1;            // rg parity: 0 hit · 1 none · 2 error
};

class ContentScan {
public:
    ContentScan();
    ~ContentScan();
    ContentScan(const ContentScan&) = delete;
    ContentScan& operator=(const ContentScan&) = delete;

    // everywhere --files-from - [-F] [-i] -e PHRASE --jsonl -c --quiet
    bool start(const std::wstring& exe, const std::wstring& phrase, bool literal, bool icase, std::string* err);
    bool feed(std::wstring_view path);   // false once everywhere is gone (broken pipe)
    // Closes stdin, waits (polling cancelled() every 100 ms; true = terminate), parses the output.
    ScanResult finish(const std::function<bool()>& cancelled);
    void kill();
    uint64_t fed() const;
    std::wstring command() const;        // the command line, for the status line / logs

private:
    struct Impl;
    Impl* p_;
};

}  // namespace facet
