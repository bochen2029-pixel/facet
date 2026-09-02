// facet · app_util.h — shared app-side helpers: options, formatting, Unicode-width columns.
// Pure std; no windows.h here. Formatting follows C:\GPUz\app_util.h so the tools read alike.
#pragma once
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace facet {

constexpr const char* kVersion = "0.2.0";
constexpr uint64_t kUnknown64 = ~0ull;             // "not reported" — never confuse with 0
constexpr uint64_t kTicksPerSec = 10000000ull;     // FILETIME resolution
constexpr uint64_t kTicksPerDay = 864000000000ull;

enum class SortKey { Modified, Name, Path, Size, Ext, Created, Recent };

struct Opts {
    enum class Mode { Auto, Report, List, Count, Gui, Mcp, Selftest, Where, MakeIcon, Shortcut, Help, Version };
    Mode mode = Mode::Auto;
    bool json = false;
    std::wstring query;                    // the Everything query, verbatim
    std::vector<std::wstring> exclude;     // -x DIR      -> !path:"DIR\"
    std::vector<std::wstring> include;     // -i DIR      -> path:"DIR\"   (several = OR)
    std::vector<std::wstring> ext;         // -e md;txt   -> ext:md;txt
    std::wstring since;                    // --since     -> dm: term
    bool files_only = false, folders_only = false;
    int top = 12;                          // rows per facet
    int depth = 3;                         // directory tree: levels expanded below the top entries
    bool top_set = false, depth_set = false;   // given explicitly (the window picks tighter defaults otherwise)
    int flat = 0;                          // --flat N: flat prefix list at depth N instead of a tree
    int min_count = 1;                     // hide facet rows with fewer items
    bool min_set = false;                  // --min given: use it verbatim instead of the 1 % rule
    int burst_gap_s = 60;                  // seconds of silence that close a write burst
    int bursts = 10;                       // bursts to show
    uint32_t max_rows = 0;                 // rows to fetch (0 = all)
    bool max_set = false;
    bool long_list = false;                // list: add size + date columns
    SortKey sort = SortKey::Modified;
    bool ascending = false;
    bool plain = false;                    // no ANSI
    bool quiet = false;                    // no progress on stderr
    uint32_t page = 65536;                 // IPC page size
    std::string shot;                      // --gui --shot FILE.png: render once, save, exit
    bool no_start = false;                 // never launch Everything ourselves
    std::wstring everything_exe;           // --everything-exe PATH (or FACET_EVERYTHING)
    std::string out_file;                  // --make-icon FILE.ico · --shortcut [startmenu|desktop]
    std::string ini;                       // --ini PATH: the window's settings file (default facet.ini next to the exe)
};

inline const char* sort_key_name(SortKey k) {
    switch (k) {
        case SortKey::Name: return "name";
        case SortKey::Path: return "path";
        case SortKey::Size: return "size";
        case SortKey::Ext: return "ext";
        case SortKey::Created: return "created";
        case SortKey::Recent: return "recent";
        default: return "modified";
    }
}
inline bool parse_sort_key(const std::string& s, SortKey& out) {
    if (s == "modified" || s == "date" || s == "dm") out = SortKey::Modified;
    else if (s == "name") out = SortKey::Name;
    else if (s == "path") out = SortKey::Path;
    else if (s == "size") out = SortKey::Size;
    else if (s == "ext" || s == "extension") out = SortKey::Ext;
    else if (s == "created" || s == "dc") out = SortKey::Created;
    else if (s == "recent") out = SortKey::Recent;
    else return false;
    return true;
}

inline std::string ssprintf(const char* f, ...) {
    va_list ap;
    va_start(ap, f);
    char buf[2048];
    const int n = vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return std::string(buf, n > 0 ? (size_t)std::min<int>(n, sizeof(buf) - 1) : 0);
}

inline std::string fmt_count(uint64_t v) {
    const std::string s = std::to_string(v);
    std::string o;
    const size_t n = s.size();
    for (size_t i = 0; i < n; ++i) {
        if (i && (n - i) % 3 == 0) o += ',';
        o += s[i];
    }
    return o;
}

inline std::string human_bytes(uint64_t b) {
    if (b == kUnknown64) return "-";
    if (b < 1024) return ssprintf("%llu B", (unsigned long long)b);
    double v = (double)b / 1024.0;
    static const char* u[] = { "KB", "MB", "GB", "TB", "PB" };
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    return ssprintf(v < 10.0 ? "%.1f %s" : "%.0f %s", v, u[i]);
}

// ---- UTF-8 display width (terminal columns), so CJK names align ----
inline uint32_t utf8_next(std::string_view s, size_t& i) {
    const unsigned char c = (unsigned char)s[i++];
    if (c < 0x80) return c;
    const int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    uint32_t cp = (n == 3) ? (c & 0x07u) : (n == 2) ? (c & 0x0Fu) : (n == 1) ? (c & 0x1Fu) : c;
    for (int k = 0; k < n && i < s.size() && (((unsigned char)s[i]) & 0xC0) == 0x80; ++k, ++i)
        cp = (cp << 6) | (((unsigned char)s[i]) & 0x3Fu);
    return cp;
}
inline int cp_width(uint32_t cp) {
    if (cp == 0 || cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;
    if ((cp >= 0x300 && cp <= 0x36F) || (cp >= 0x200B && cp <= 0x200F) ||
        (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF)
        return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1FAFF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD))
        return 2;
    return 1;
}
inline int display_width(std::string_view s) {
    int w = 0;
    for (size_t i = 0; i < s.size();) w += cp_width(utf8_next(s, i));
    return w;
}
inline std::string pad_display(const std::string& s, int cols) {
    const int w = display_width(s);
    return w >= cols ? s : s + std::string((size_t)(cols - w), ' ');
}
inline std::string rpad_display(const std::string& s, int cols) {
    const int w = display_width(s);
    return w >= cols ? s : std::string((size_t)(cols - w), ' ') + s;
}
// Truncate to cols keeping head and tail — for paths the tail is the informative part.
inline std::string trunc_middle(const std::string& s, int cols) {
    if (cols < 4 || display_width(s) <= cols) return s;
    std::vector<std::pair<size_t, int>> cps;   // (byte offset, width)
    for (size_t i = 0; i < s.size();) {
        const size_t at = i;
        cps.emplace_back(at, cp_width(utf8_next(s, i)));
    }
    const int head_cols = std::max(1, (cols - 1) * 2 / 5);
    const int tail_cols = cols - 1 - head_cols;
    size_t h = 0;
    int w = 0;
    while (h < cps.size() && w + cps[h].second <= head_cols) w += cps[h++].second;
    size_t t = cps.size();
    w = 0;
    while (t > h && w + cps[t - 1].second <= tail_cols) w += cps[--t].second;
    const size_t hb = h < cps.size() ? cps[h].first : s.size();
    const size_t tb = t < cps.size() ? cps[t].first : s.size();
    return s.substr(0, hb) + "\xE2\x80\xA6" + s.substr(tb);
}
inline std::string trunc_end(const std::string& s, int cols) {
    if (display_width(s) <= cols) return s;
    std::string out;
    int w = 0;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = i;
        const int cw = cp_width(utf8_next(s, j));
        if (w + cw > cols - 1) break;
        out.append(s, i, j - i);
        w += cw;
        i = j;
    }
    return out + "\xE2\x80\xA6";
}

}  // namespace facet
