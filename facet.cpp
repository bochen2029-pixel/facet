// facet — pivot filtering over Everything's index: the distribution of a result set (directory
// tree · extension · modified · size · write bursts) whose picks compile back into Everything
// syntax. Human views (report / --list / --gui) + agent surfaces (--json, --mcp). C/C++ only,
// OS APIs only, single exe; the index stays Everything's, reached over its WM_COPYDATA IPC.
//
// Build: build.bat (MSVC, /std:c++20 /W4 /permissive- /utf-8 /MT) -> facet.exe + facetw.exe
#include "app_util.h"
#include "es_client.h"
#include "everything_ipc.h"
#include "facets.h"
#include "query.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace facet {

int run_gui(const Opts& o);                    // facet_gui.cpp
int write_icon_file(const std::string& path);  // facet_gui.cpp: the app icon as a .ico (the .rc embeds it)
int make_shortcut(const std::string& where);   // facet_gui.cpp: Start Menu / desktop .lnk to the window

// ======================================================================
// JSON emit
// ======================================================================
static std::string jesc(std::string_view s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) o += ssprintf("\\u%04x", c);
                else o += (char)c;
        }
    }
    return o;
}
static std::string jstr(std::string_view s) { return "\"" + jesc(s) + "\""; }
static std::string jw(std::wstring_view w) { return jstr(narrow(w)); }
static std::string jn(uint64_t v) { return std::to_string(v); }
static std::string jopt(uint64_t v) { return v == kUnknown64 ? std::string("null") : std::to_string(v); }
static std::string jshare(uint64_t part, uint64_t whole) {
    return whole ? ssprintf("%.4f", (double)part / (double)whole) : "0";
}

// ======================================================================
// Mini JSON parse (MCP requests + selftest roundtrip). Tolerant, bounded.
// ======================================================================
struct JV {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double num = 0;
    std::string raw;
    std::string s;
    std::vector<JV> arr;
    std::vector<std::pair<std::string, JV>> obj;
    const JV* get(const char* k) const {
        for (const auto& kv : obj)
            if (kv.first == k) return &kv.second;
        return nullptr;
    }
    double as_num(double d) const { return t == Num ? num : d; }
    bool as_bool(bool d) const { return t == Bool ? b : d; }
    std::string as_str(const char* d) const { return t == Str ? s : d; }
};

static void jskip(const std::string& in, size_t& i) {
    while (i < in.size() && (in[i] == ' ' || in[i] == '\t' || in[i] == '\r' || in[i] == '\n')) ++i;
}
static bool jval(const std::string& in, size_t& i, JV& out, int depth);

static bool jstring(const std::string& in, size_t& i, std::string& out) {
    if (i >= in.size() || in[i] != '"') return false;
    ++i;
    out.clear();
    while (i < in.size()) {
        char c = in[i++];
        if (c == '"') return true;
        if (c != '\\') { out += c; continue; }
        if (i >= in.size()) return false;
        char e = in[i++];
        switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (i + 4 > in.size()) return false;
                unsigned cp = (unsigned)strtoul(in.substr(i, 4).c_str(), nullptr, 16);
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= in.size() && in[i] == '\\' && in[i + 1] == 'u') {
                    unsigned lo = (unsigned)strtoul(in.substr(i + 2, 4).c_str(), nullptr, 16);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                if (cp < 0x80) out += (char)cp;
                else if (cp < 0x800) {
                    out += (char)(0xC0 | (cp >> 6));
                    out += (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    out += (char)(0xE0 | (cp >> 12));
                    out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    out += (char)(0x80 | (cp & 0x3F));
                } else {
                    out += (char)(0xF0 | (cp >> 18));
                    out += (char)(0x80 | ((cp >> 12) & 0x3F));
                    out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    out += (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: return false;
        }
    }
    return false;
}

static bool jval(const std::string& in, size_t& i, JV& out, int depth) {
    if (depth > 48) return false;
    jskip(in, i);
    if (i >= in.size()) return false;
    const char c = in[i];
    if (c == '{') {
        out.t = JV::Obj;
        ++i;
        jskip(in, i);
        if (i < in.size() && in[i] == '}') { ++i; return true; }
        while (i < in.size()) {
            jskip(in, i);
            std::string key;
            if (!jstring(in, i, key)) return false;
            jskip(in, i);
            if (i >= in.size() || in[i] != ':') return false;
            ++i;
            JV v;
            if (!jval(in, i, v, depth + 1)) return false;
            out.obj.emplace_back(std::move(key), std::move(v));
            jskip(in, i);
            if (i < in.size() && in[i] == ',') { ++i; continue; }
            if (i < in.size() && in[i] == '}') { ++i; return true; }
            return false;
        }
        return false;
    }
    if (c == '[') {
        out.t = JV::Arr;
        ++i;
        jskip(in, i);
        if (i < in.size() && in[i] == ']') { ++i; return true; }
        while (i < in.size()) {
            JV v;
            if (!jval(in, i, v, depth + 1)) return false;
            out.arr.push_back(std::move(v));
            jskip(in, i);
            if (i < in.size() && in[i] == ',') { ++i; continue; }
            if (i < in.size() && in[i] == ']') { ++i; return true; }
            return false;
        }
        return false;
    }
    if (c == '"') { out.t = JV::Str; return jstring(in, i, out.s); }
    if (!strncmp(in.c_str() + i, "true", 4)) { out.t = JV::Bool; out.b = true; i += 4; return true; }
    if (!strncmp(in.c_str() + i, "false", 5)) { out.t = JV::Bool; out.b = false; i += 5; return true; }
    if (!strncmp(in.c_str() + i, "null", 4)) { out.t = JV::Null; i += 4; return true; }
    const size_t start = i;
    if (in[i] == '-') ++i;
    while (i < in.size() && (isdigit((unsigned char)in[i]) || in[i] == '.' || in[i] == 'e' ||
                             in[i] == 'E' || in[i] == '+' || in[i] == '-'))
        ++i;
    if (i == start) return false;
    out.t = JV::Num;
    out.raw = in.substr(start, i - start);
    out.num = strtod(out.raw.c_str(), nullptr);
    return true;
}

static bool jparse(const std::string& in, JV& out) {
    size_t i = 0;
    if (!jval(in, i, out, 0)) return false;
    jskip(in, i);
    return i == in.size();
}

// ======================================================================
// ANSI console rendering
// ======================================================================
namespace clr {
constexpr const char* reset = "\x1b[0m";
constexpr const char* bold = "\x1b[1m";
constexpr const char* dim = "\x1b[38;5;244m";
constexpr const char* track = "\x1b[38;5;238m";
constexpr const char* blu = "\x1b[38;2;92;164;238m";
constexpr const char* amb = "\x1b[38;2;245;185;66m";
constexpr const char* err = "\x1b[1;38;2;255;96;96m";
}  // namespace clr

static std::string bar(double frac, int width, const char* color, bool ansi) {
    frac = std::clamp(frac, 0.0, 1.0);
    std::string b;
    if (!ansi) {
        const int f = (int)(frac * width + 0.5);
        b.append((size_t)f, '#');
        b.append((size_t)(width - f), '.');
        return b;
    }
    static const char* eighth[] = { "\u258F", "\u258E", "\u258D", "\u258C", "\u258B", "\u258A", "\u2589" };
    const int fill8 = (int)(frac * width * 8 + 0.5);
    const int full = fill8 / 8, rem = fill8 % 8;
    b += color;
    for (int k = 0; k < full; ++k) b += "\u2588";
    int used = full;
    if (rem && full < width) { b += eighth[rem - 1]; used++; }
    b += clr::track;
    for (int k = used; k < width; ++k) b += "\u2591";
    b += clr::reset;
    return b;
}

// ======================================================================
// console plumbing
// ======================================================================
static bool g_vt_enabled = false;

static bool stdout_is_console() {
    DWORD m;
    return GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &m) != 0;
}
static bool stderr_is_console() {
    DWORD m;
    return GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &m) != 0;
}

// facetw.exe (windows subsystem) has no console of its own; for text modes borrow the
// parent's so `facetw --json` in a terminal still prints.
static void ensure_console_for_text() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$", "r", stdin);
}

static void console_setup(Opts& o) {
    if (!stdout_is_console()) { o.plain = true; return; }
    SetConsoleOutputCP(CP_UTF8);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD m = 0;
    if (GetConsoleMode(h, &m) && SetConsoleMode(h, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
        g_vt_enabled = true;
    else
        o.plain = true;
}

static int console_width() {
    CONSOLE_SCREEN_BUFFER_INFO bi{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &bi)) {
        const int w = bi.srWindow.Right - bi.srWindow.Left + 1;
        return std::clamp(w, 72, 180);
    }
    return 110;
}

// True only for a real double-click launch: our own fresh console, or no console AND no
// usable stdout (agent harnesses run us console-less with stdout piped — they must always
// get text, never a blocking window).
static bool fresh_own_console() {
    if (GetConsoleWindow() != nullptr) {
        DWORD pids[4];
        return GetConsoleProcessList(pids, 4) <= 1;
    }
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    const DWORD t = (h && h != INVALID_HANDLE_VALUE) ? GetFileType(h) : FILE_TYPE_UNKNOWN;
    return t != FILE_TYPE_PIPE && t != FILE_TYPE_DISK && t != FILE_TYPE_CHAR;
}

static void write_out(const std::string& s) { fwrite(s.data(), 1, s.size(), stdout); }

// ======================================================================
// filters → Everything terms
// ======================================================================
// --since: today | yesterday | thisweek ... (Everything's own constants) | 3d | 12h | 30m | 2w | YYYY-MM-DD
static std::wstring since_term(const std::wstring& s) {
    if (s.empty()) return L"";
    const std::string a = narrow(s);
    if (!a.empty() && isdigit((unsigned char)a[0])) {
        size_t i = 0;
        while (i < a.size() && isdigit((unsigned char)a[i])) ++i;
        if (i + 1 == a.size()) {
            const char u = (char)tolower((unsigned char)a.back());
            const std::string n = a.substr(0, i);
            // Everything 1.4 knows lastNdays / lastNmins; hours and weeks are spelled out in those
            const long v = strtol(n.c_str(), nullptr, 10);
            if (u == 'd') return widen("dm:last" + n + "days");
            if (u == 'h') return widen(ssprintf("dm:last%ldmins", v * 60));
            if (u == 'm') return widen("dm:last" + n + "mins");
            if (u == 'w') return widen(ssprintf("dm:last%lddays", v * 7));
        }
        if (a.size() == 10 && a[4] == '-' && a[7] == '-') return widen("dm:>=" + a);
    }
    return L"dm:" + s;
}

static std::vector<Filter> filters_from(const Opts& o) {
    std::vector<Filter> fs;
    for (const auto& d : o.include) fs.push_back({ Filter::Kind::DirIn, d });
    for (const auto& d : o.exclude) fs.push_back({ Filter::Kind::DirOut, d });
    for (const auto& e : o.ext) fs.push_back({ Filter::Kind::ExtIn, e });
    if (!o.since.empty()) fs.push_back({ Filter::Kind::Term, since_term(o.since) });
    if (o.files_only) fs.push_back({ Filter::Kind::Term, L"file:" });
    if (o.folders_only) fs.push_back({ Filter::Kind::Term, L"folder:" });
    return fs;
}

// Every mode reaches Everything the same way: start it when it is only not running (unless
// --no-start), point at a specific exe when told, and put the notes on stderr.
static void configure(Everything& es, const Opts& o) {
    es.launch.allow_start = !o.no_start;
    es.launch.exe_override = o.everything_exe;
    es.on_note = [](const std::string& n) { fprintf(stderr, "facet: %s\n", n.c_str()); };
}

// ======================================================================
// the pass: compile → stream → fold
// ======================================================================
struct Run {
    std::wstring compiled;
    EsInfo info;
    double ms = 0;
    std::string err;
    uint32_t total = 0;      // Everything's match count for the compiled query
};

static bool run_pass(const Opts& o, Everything& es, Facets& f, Run& r) {
    r.compiled = compile(o.query, filters_from(o));
    const double t0 = now_ms();
    const uint32_t req = ipc::kReqName | ipc::kReqPath | ipc::kReqSize | ipc::kReqModified;
    // rows retained → the caller's order matters; facets only → the index's own order is cheapest
    const uint32_t sort = f.config().keep_rows ? ipc_sort(o.sort, o.ascending) : (uint32_t)ipc::NameAsc;
    const bool progress = !o.quiet && stderr_is_console();
    ULONGLONG last = GetTickCount64();
    bool shown = false;
    auto sink = [&](const EsPage& pg, const EsItem* items, uint32_t n) -> bool {
        r.total = pg.total;
        for (uint32_t i = 0; i < n; ++i) f.add(items[i]);
        if (progress) {
            const ULONGLONG now = GetTickCount64();
            if (now - last > 700) {
                fprintf(stderr, "\r  facet: %s of %s items...", fmt_count(f.items).c_str(), fmt_count(pg.total).c_str());
                shown = true;
                last = now;
            }
        }
        return true;
    };
    const bool ok = es.query(r.compiled, sort, 0, o.max_rows, o.page, req, sink, &r.err);
    if (shown) fputs("\r                                                      \r", stderr);
    f.finish();
    r.info = es.info();
    r.ms = now_ms() - t0;
    return ok;
}

// ======================================================================
// report rendering
// ======================================================================
struct RenderCtx {
    bool ansi = true;
    int width = 110;
};

static std::string share_str(uint64_t part, uint64_t whole) {
    if (!whole) return "";
    const double p = 100.0 * (double)part / (double)whole;
    return p >= 99.95 ? "100%" : ssprintf("%.1f%%", p);
}

// (DirLine / dir_lines / flat_lines / collapsed_label / fold_threshold live in facets.h — shared with the window)

static void render_dirs(std::string& out, const Facets& f, const Opts& o, const RenderCtx& rc) {
    const bool ansi = rc.ansi;
    const char* D = ansi ? clr::dim : "";
    const char* R = ansi ? clr::reset : "";
    const char* B = ansi ? clr::bold : "";
    const int name_w = std::clamp(rc.width - 44, 24, 110);
    out += "\n";
    out += B;
    out += pad_display(o.flat ? ssprintf("DIRECTORIES  (flat, depth %d)", o.flat) : "DIRECTORIES", name_w + 2);
    out += R;
    out += D;
    out += ssprintf("%12s %9s %6s %10s", "", "items", "share", "bytes");
    out += R;
    out += "\n";
    const std::vector<DirLine> lines = o.flat ? flat_lines(f, o) : dir_lines(f, o);
    int printed = 0;
    for (const auto& dl : lines) {
        if (printed++ >= 120) { out += D; out += "  ...\n"; out += R; break; }
        const int ind = dl.level * 2;
        const std::string label = trunc_middle(dl.label, name_w - ind);
        std::string ln = " " + std::string((size_t)ind, ' ') + pad_display(label, name_w - ind) + " ";
        if (dl.note) {
            out += D;
            out += ln + std::string(12, ' ') + rpad_display(fmt_count(dl.count), 10);
            out += R;
            out += "\n";
            continue;
        }
        const double frac = f.items ? (double)dl.count / (double)f.items : 0.0;
        ln += bar(frac, 12, frac >= 0.5 ? clr::amb : clr::blu, ansi);
        ln += rpad_display(fmt_count(dl.count), 10) + " " + rpad_display(share_str(dl.count, f.items), 6) + " " +
              rpad_display(dl.has_bytes ? human_bytes(dl.bytes) : "", 10);
        out += ln + "\n";
    }
}

static std::vector<std::string> bucket_lines(const std::vector<BucketStat>& bs, uint64_t whole, int label_w) {
    std::vector<std::string> v;
    for (const auto& b : bs) {
        if (!b.count) continue;
        v.push_back(pad_display(b.label, label_w) + rpad_display(fmt_count(b.count), 9) + " " + rpad_display(share_str(b.count, whole), 6));
    }
    return v;
}

static std::vector<std::string> ext_lines(const Facets& f, const Opts& o, int label_w) {
    std::vector<std::string> v;
    int shown = 0, rest_n = 0;
    uint64_t rest = 0;
    for (const auto& e : f.exts) {
        if (shown >= o.top) { rest_n++; rest += e.count; continue; }
        shown++;
        const std::string label = e.ext.empty() ? "(no extension)" : "." + narrow(e.ext);
        v.push_back(pad_display(trunc_end(label, label_w - 1), label_w) + rpad_display(fmt_count(e.count), 9) + " " + rpad_display(share_str(e.count, f.files), 6));
    }
    if (rest_n) v.push_back(pad_display(ssprintf("+%d more", rest_n), label_w) + rpad_display(fmt_count(rest), 9));
    return v;
}

static void render_columns(std::string& out, const std::vector<std::string>& heads, const std::vector<std::vector<std::string>>& cols,
                           int col_w, const RenderCtx& rc) {
    const char* R = rc.ansi ? clr::reset : "";
    const char* B = rc.ansi ? clr::bold : "";
    size_t i = 0;
    while (i < cols.size()) {
        const size_t fit = std::max<size_t>(1, (size_t)((rc.width + 3) / (col_w + 3)));
        const size_t n = std::min(cols.size() - i, fit);
        out += "\n";
        for (size_t k = i; k < i + n; ++k) {
            out += " ";
            out += B;
            out += pad_display(heads[k], col_w);
            out += R;
            out += "  ";
        }
        out += "\n";
        size_t rows = 0;
        for (size_t k = i; k < i + n; ++k) rows = std::max(rows, cols[k].size());
        for (size_t r = 0; r < rows; ++r) {
            std::string line;
            for (size_t k = i; k < i + n; ++k) {
                line += " ";
                line += pad_display(r < cols[k].size() ? cols[k][r] : std::string(), col_w);
                line += "  ";
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            out += line + "\n";
        }
        i += n;
    }
}

static void render_bursts(std::string& out, const Facets& f, const Opts& o, const RenderCtx& rc) {
    const bool ansi = rc.ansi;
    const char* D = ansi ? clr::dim : "";
    const char* R = ansi ? clr::reset : "";
    const char* B = ansi ? clr::bold : "";
    out += "\n";
    out += B;
    out += "WRITE BURSTS";
    out += R;
    out += D;
    out += ssprintf("  files landing within %d s of each other — thousands in a minute is a clone or extract, "
                    "a dozen is an agent session, 1-2 is a hand", o.burst_gap_s);
    out += R;
    out += "\n";
    if (f.bursts.empty()) {
        out += D;
        out += "  (no dated items)\n";
        out += R;
        return;
    }
    const int where_w = std::clamp(rc.width - 44, 24, 120);
    for (const auto& b : f.bursts) {
        std::string when = fmt_filetime(b.start) + " -> ";
        when += fmt_local_date(b.end) == fmt_local_date(b.start) ? fmt_local_time(b.end) : fmt_filetime(b.end);
        std::string where;
        const std::wstring base = b.dir ? f.dir_path(b.dir) : std::wstring();
        if (!b.parts.empty()) {
            where = narrow(base);
            if (!where.empty()) where += "  >  ";
            for (size_t i = 0; i < b.parts.size(); ++i) {
                std::wstring rel = f.dir_path(b.parts[i].first);
                if (!base.empty() && rel.compare(0, base.size(), base) == 0) rel.erase(0, base.size());
                where += (i ? "  ·  " : "") + trunc_middle(narrow(rel), 44) + ssprintf(" %.0f%%", b.parts[i].second * 100.0);
            }
        } else {
            where = (b.dir ? narrow(base) : std::string("(scattered)")) + ssprintf(" %.0f%%", b.dir_share * 100.0);
        }
        out += rpad_display(fmt_count(b.count), 8) + "  " + pad_display(when, 30) + " " + trunc_end(where, where_w) + "\n";
    }
    out += D;
    out += ssprintf("  hand-paced: %s files in %s bursts of 1-2  ·  %s bursts in all  ·  last hour: %s items",
                    fmt_count(f.handpaced_files).c_str(), fmt_count(f.handpaced_bursts).c_str(),
                    fmt_count(f.burst_total).c_str(), fmt_count(f.last_hour).c_str());
    out += R;
    out += "\n";
}

static std::string render_report(const Facets& f, const Opts& o, const Run& r, const RenderCtx& rc) {
    const bool ansi = rc.ansi;
    const char* D = ansi ? clr::dim : "";
    const char* R = ansi ? clr::reset : "";
    const char* B = ansi ? clr::bold : "";
    std::string out;
    out.reserve(16384);
    out += B;
    out += "facet ";
    out += kVersion;
    out += R;
    out += "  ";
    out += o.query.empty() ? std::string("(everything)") : narrow(o.query);
    out += "\n";
    out += D;
    out += ssprintf("%s items · %s files · %s folders · %s", fmt_count(f.items).c_str(), fmt_count(f.files).c_str(),
                    fmt_count(f.folders).c_str(), human_bytes(f.bytes).c_str());
    out += ssprintf(" · Everything %s · %.0f ms", r.info.version().c_str(), r.ms);
    out += R;
    out += "\n";
    if (r.total > f.items) {
        out += ansi ? clr::amb : "";
        out += ssprintf("  facets cover the first %s of %s matches (raise --max)\n", fmt_count(f.items).c_str(), fmt_count(r.total).c_str());
        out += R;
    }
    if (f.items == 0) {
        out += "\n  no matches\n";
    } else {
        render_dirs(out, f, o, rc);
        const int label_w = 18, col_w = 34;
        std::vector<std::string> heads = { "MODIFIED", "SIZE", "EXTENSIONS" };
        std::vector<std::vector<std::string>> cols = {
            bucket_lines(f.modified, f.items, label_w), bucket_lines(f.sizes, f.items, label_w), ext_lines(f, o, label_w)
        };
        render_columns(out, heads, cols, col_w, rc);
        render_bursts(out, f, o, rc);
    }
    out += "\n";
    out += B;
    out += "QUERY";
    out += R;
    out += "  " + narrow(r.compiled) + "\n";
    out += D;
    out += "       -x DIR excludes a subtree · -i DIR drills in · -e md;txt · --since 3d · --flat 2 · -l rows · -j JSON";
    out += R;
    out += "\n";
    return out;
}

// ======================================================================
// JSON report
// ======================================================================
static void dir_children_json(std::string& j, const Facets& f, const Opts& o, uint32_t node, int level, uint64_t thr) {
    const DirNode& n = f.nodes[node];
    j += "\"children\":[";
    int shown = 0, rest_n = 0;
    uint64_t rest = 0;
    bool first = true;
    if (level <= o.depth) {
        for (uint32_t c : n.children) {
            const uint64_t cnt = f.nodes[c].count;
            if (cnt < thr || shown >= o.top) { rest_n++; rest += cnt; continue; }
            shown++;
            uint32_t cur = c;
            const std::wstring label = collapsed_label(f, cur, false);
            if (!first) j += ",";
            first = false;
            j += "{\"name\":" + jw(label) + ",\"path\":" + jw(f.dir_path(cur)) + ",\"count\":" + jn(f.nodes[cur].count) +
                 ",\"share\":" + jshare(f.nodes[cur].count, f.items) + ",\"bytes\":" + jn(f.nodes[cur].bytes) +
                 ",\"files_here\":" + jn(f.nodes[cur].self) + ",";
            dir_children_json(j, f, o, cur, level + 1, thr);
            j += "}";
        }
    } else {
        for (uint32_t c : n.children) { rest_n++; rest += f.nodes[c].count; }
    }
    j += "],\"more\":{\"directories\":" + jn((uint64_t)rest_n) + ",\"items\":" + jn(rest) + "}";
}

static std::string report_json(const Facets& f, const Opts& o, const Run& r) {
    std::string j;
    j.reserve(16384);
    j += "{\"tool\":\"facet\",\"version\":" + jstr(kVersion);
    j += ",\"query\":" + jw(o.query) + ",\"compiled\":" + jw(r.compiled);
    j += ",\"everything\":" + jstr(r.info.version());
    j += ",\"total\":" + jn(r.total) + ",\"scanned\":" + jn(f.items) + ",\"files\":" + jn(f.files) + ",\"folders\":" + jn(f.folders);
    j += ",\"bytes\":" + jn(f.bytes) + ",\"unknown_size\":" + jn(f.unknown_size) + ",\"last_hour\":" + jn(f.last_hour);
    j += ssprintf(",\"elapsed_ms\":%.1f", r.ms);
    j += ",\"error\":" + (r.err.empty() ? std::string("null") : jstr(r.err));
    const uint64_t thr = fold_threshold(o, f.items);
    j += ",\"directories\":[";
    if (o.flat) {
        const auto lines = flat_lines(f, o);
        bool first = true;
        for (const auto& l : lines) {
            if (l.note) continue;
            if (!first) j += ",";
            first = false;
            j += "{\"path\":" + jw(f.dir_path(l.node)) + ",\"count\":" + jn(l.count) + ",\"share\":" + jshare(l.count, f.items) +
                 ",\"bytes\":" + (l.has_bytes ? jn(l.bytes) : std::string("null")) + "}";
        }
    } else {
        struct Top { uint32_t node; uint64_t count; bool root_files; };
        std::vector<Top> tops;
        for (uint32_t drv : f.nodes[0].children) {
            const DirNode& dn = f.nodes[drv];
            if (dn.self > 0) tops.push_back({ drv, dn.self, true });
            for (uint32_t c : dn.children) tops.push_back({ c, f.nodes[c].count, false });
        }
        std::sort(tops.begin(), tops.end(), [](const Top& a, const Top& b) { return a.count > b.count; });
        bool first = true;
        int shown = 0;
        for (const auto& t : tops) {
            if (t.count < thr || shown >= o.top) continue;
            shown++;
            if (!first) j += ",";
            first = false;
            if (t.root_files) {
                j += "{\"path\":" + jw(f.dir_path(t.node)) + ",\"count\":" + jn(t.count) + ",\"share\":" + jshare(t.count, f.items) +
                     ",\"bytes\":null,\"files_here\":" + jn(t.count) + ",\"children\":[],\"more\":{\"directories\":0,\"items\":0}}";
                continue;
            }
            uint32_t cur = t.node;
            collapsed_label(f, cur, true);
            j += "{\"path\":" + jw(f.dir_path(cur)) + ",\"count\":" + jn(f.nodes[cur].count) + ",\"share\":" + jshare(f.nodes[cur].count, f.items) +
                 ",\"bytes\":" + jn(f.nodes[cur].bytes) + ",\"files_here\":" + jn(f.nodes[cur].self) + ",";
            dir_children_json(j, f, o, cur, 1, thr);
            j += "}";
        }
    }
    j += "],\"extensions\":[";
    {
        int shown = 0;
        for (const auto& e : f.exts) {
            if (shown >= o.top) break;
            if (shown++) j += ",";
            j += "{\"ext\":" + jw(e.ext) + ",\"count\":" + jn(e.count) + ",\"share\":" + jshare(e.count, f.files) + ",\"bytes\":" + jn(e.bytes) + "}";
        }
    }
    j += "],\"modified\":[";
    for (size_t i = 0; i < f.modified.size(); ++i) {
        const auto& b = f.modified[i];
        if (i) j += ",";
        j += "{\"bucket\":" + jstr(b.label) + ",\"query\":" + jw(b.query) + ",\"count\":" + jn(b.count) + ",\"bytes\":" + jn(b.bytes) + "}";
    }
    j += "],\"size\":[";
    for (size_t i = 0; i < f.sizes.size(); ++i) {
        const auto& b = f.sizes[i];
        if (i) j += ",";
        j += "{\"bucket\":" + jstr(b.label) + ",\"query\":" + jw(b.query) + ",\"count\":" + jn(b.count) + ",\"bytes\":" + jn(b.bytes) + "}";
    }
    j += "],\"bursts\":[";
    for (size_t i = 0; i < f.bursts.size(); ++i) {
        const auto& b = f.bursts[i];
        if (i) j += ",";
        j += "{\"start\":" + jstr(fmt_filetime_iso(b.start)) + ",\"end\":" + jstr(fmt_filetime_iso(b.end)) +
             ",\"seconds\":" + jn((b.end - b.start) / kTicksPerSec) + ",\"count\":" + jn(b.count) +
             ",\"dir\":" + (b.dir ? jw(f.dir_path(b.dir)) : std::string("null")) + ssprintf(",\"dir_share\":%.4f", b.dir_share);
        j += ",\"parts\":[";
        for (size_t k = 0; k < b.parts.size(); ++k) {
            if (k) j += ",";
            j += "{\"dir\":" + jw(f.dir_path(b.parts[k].first)) + ssprintf(",\"share\":%.4f}", b.parts[k].second);
        }
        j += "],\"query\":" + jw(b.query) + "}";
    }
    j += "],\"bursts_total\":" + jn(f.burst_total) + ",\"handpaced\":{\"bursts\":" + jn(f.handpaced_bursts) + ",\"files\":" + jn(f.handpaced_files) + "}";
    j += ssprintf(",\"burst_gap_s\":%d}", o.burst_gap_s);
    return j;
}

// ======================================================================
// modes
// ======================================================================
static int fail(const std::string& err, bool json, const Opts& o, const std::wstring& compiled) {
    if (json) {
        std::string j = "{\"tool\":\"facet\",\"version\":" + jstr(kVersion) + ",\"query\":" + jw(o.query) + ",\"compiled\":" + jw(compiled) + ",\"error\":" + jstr(err) + "}\n";
        write_out(j);
    } else {
        fprintf(stderr, "facet: %s\n", err.c_str());
    }
    return 2;
}

static int run_report(Opts o) {
    if (!o.json) console_setup(o);
    Everything es;
    configure(es, o);
    FacetConfig cfg;
    cfg.burst_gap_s = (uint32_t)o.burst_gap_s;
    cfg.top_bursts = (uint32_t)o.bursts;
    Facets f(cfg);
    Run r;
    if (!run_pass(o, es, f, r)) return fail(r.err, o.json, o, r.compiled);
    if (o.json) {
        write_out(report_json(f, o, r) + "\n");
        return 0;
    }
    RenderCtx rc;
    rc.ansi = !o.plain && g_vt_enabled;
    rc.width = console_width();
    write_out(render_report(f, o, r, rc));
    return 0;
}

static int run_list(Opts o) {
    if (!o.json) console_setup(o);
    const uint32_t cap = o.max_set ? o.max_rows : 200;
    o.max_rows = cap;
    Everything es;
    configure(es, o);
    FacetConfig cfg;
    cfg.keep_rows = cap ? cap : 0xFFFFFFFFu;
    cfg.top_bursts = 0;
    Facets f(cfg);
    Run r;
    if (!run_pass(o, es, f, r)) return fail(r.err, o.json, o, r.compiled);
    if (o.json) {
        std::string j = "{\"tool\":\"facet\",\"version\":" + jstr(kVersion) + ",\"query\":" + jw(o.query) + ",\"compiled\":" + jw(r.compiled);
        j += ",\"total\":" + jn(r.total) + ",\"shown\":" + jn(f.rows.size()) + ",\"sort\":" + jstr(sort_key_name(o.sort)) +
             std::string(",\"ascending\":") + (o.ascending ? "true" : "false") + ",\"rows\":[";
        for (size_t i = 0; i < f.rows.size(); ++i) {
            const Row& rw = f.rows[i];
            if (i) j += ",";
            j += "{\"path\":" + jw(f.row_path(rw)) + ",\"name\":" + jw(f.row_name(rw)) + ",\"dir\":" + jw(f.dir_path(rw.dir)) +
                 ",\"size\":" + jopt(rw.size) + ",\"modified\":" + (rw.mtime == kUnknown64 ? std::string("null") : jstr(fmt_filetime_iso(rw.mtime))) +
                 ",\"folder\":" + (rw.folder ? "true" : "false") + "}";
        }
        j += "]}\n";
        write_out(j);
        return 0;
    }
    std::string out;
    out.reserve(f.rows.size() * 96);
    for (const Row& rw : f.rows) {
        if (o.long_list)
            out += rpad_display(rw.folder ? "<dir>" : human_bytes(rw.size), 9) + "  " + pad_display(fmt_filetime(rw.mtime, false), 16) + "  ";
        out += narrow(f.row_path(rw)) + "\n";
    }
    write_out(out);
    if (r.total == 0) fputs("# no matches\n", stderr);
    else if (r.total > f.rows.size()) fprintf(stderr, "# showing %s of %s matches - refine the query or raise -n\n", fmt_count(f.rows.size()).c_str(), fmt_count(r.total).c_str());
    else fprintf(stderr, "# %s match%s\n", fmt_count(r.total).c_str(), r.total == 1 ? "" : "es");
    return 0;
}

static int run_count(const Opts& o) {
    Everything es;
    configure(es, o);
    const std::wstring compiled = compile(o.query, filters_from(o));
    uint32_t total = 0;
    std::string err;
    if (!es.count(compiled, 0, &total, &err)) return fail(err, o.json, o, compiled);
    if (o.json) write_out("{\"tool\":\"facet\",\"query\":" + jw(o.query) + ",\"compiled\":" + jw(compiled) + ",\"total\":" + jn(total) + "}\n");
    else write_out(std::to_string(total) + "\n");
    return 0;
}

// --where: what facet would talk to — the exe it found, whether an instance is up, what is indexed
static int run_where(const Opts& o) {
    const std::wstring exe = find_everything_exe(o.everything_exe);
    Everything es;
    es.launch.allow_start = false;
    es.launch.exe_override = o.everything_exe;
    std::string err;
    const bool up = es.connect(&err);
    const EsInfo& i = es.info();
    if (o.json) {
        std::string j = "{\"tool\":\"facet\",\"version\":" + jstr(kVersion) + ",\"everything_exe\":" + (exe.empty() ? std::string("null") : jw(exe)) +
                        std::string(",\"running\":") + (up ? "true" : "false");
        if (up)
            j += ",\"everything\":" + jstr(i.version()) + std::string(",\"db_loaded\":") + (i.db_loaded ? "true" : "false") +
                 ",\"size_indexed\":" + (i.size_indexed ? "true" : "false") + ",\"modified_indexed\":" + (i.modified_indexed ? "true" : "false") +
                 ",\"created_indexed\":" + (i.created_indexed ? "true" : "false");
        else
            j += ",\"error\":" + jstr(err);
        j += ",\"download\":\"https://www.voidtools.com/downloads/\"}\n";
        write_out(j);
        return up ? 0 : 2;
    }
    printf("Everything.exe   %s\n", exe.empty() ? "(none found)" : narrow(exe).c_str());
    if (up)
        printf("running          yes - %s, database %s, indexed: size %s, date-modified %s, date-created %s\n"
               "IPC              ok (WM_COPYDATA QUERY2 - the same channel es.exe uses)\n",
               i.version().c_str(), i.db_loaded ? "loaded" : "still loading", i.size_indexed ? "yes" : "no",
               i.modified_indexed ? "yes" : "no", i.created_indexed ? "yes" : "no (facet never asks for it)");
    else
        printf("running          no\n  %s\n", err.c_str());
    return up ? 0 : 2;
}

// ======================================================================
// --mcp: newline-delimited JSON-RPC 2.0 over stdio (MCP server, three tools)
// ======================================================================
static std::string jv_id(const JV& v) {
    if (v.t == JV::Str) return jstr(v.s);
    if (v.t == JV::Num) return v.raw;
    return "null";
}
static void mcp_send(const std::string& body) {
    fwrite(body.data(), 1, body.size(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}
static void mcp_result(const std::string& id, const std::string& result) {
    mcp_send("{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + result + "}");
}
static void mcp_error(const std::string& id, int code, const std::string& msg) {
    mcp_send("{\"jsonrpc\":\"2.0\",\"id\":" + id + ssprintf(",\"error\":{\"code\":%d,\"message\":", code) + jstr(msg) + "}}");
}
static std::string mcp_text(const std::string& text, bool is_error = false) {
    return "{\"content\":[{\"type\":\"text\",\"text\":" + jstr(text) + "}]" + (is_error ? ",\"isError\":true}" : "}");
}

static const char* kToolsList =
    "{\"tools\":[{"
    "\"name\":\"facet_query\","
    "\"description\":\"Distribution of an Everything search on this Windows box: which directories hold the "
    "matches (tree with counts), extensions, modified-date buckets, size buckets, and write bursts (files "
    "written within seconds of each other - thousands in a minute is a clone/extract, a dozen is an agent "
    "session, 1-2 is a human). Use it to find WHERE files went and what to exclude. query uses Everything "
    "syntax (ext:md dm:today, path:, size:, !term, a|b). Every filter compiles into the returned "
    "'compiled' query string.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"query\":{\"type\":\"string\",\"description\":\"Everything search query ('' = every indexed item)\"},"
    "\"exclude\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"directory subtrees to exclude\"},"
    "\"include\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"restrict to these subtrees (OR)\"},"
    "\"ext\":{\"type\":\"string\",\"description\":\"extensions, ';'-separated (md;txt)\"},"
    "\"since\":{\"type\":\"string\",\"description\":\"today | yesterday | 3d | 12h | 30m | 2w | YYYY-MM-DD | any Everything dm: constant\"},"
    "\"top\":{\"type\":\"integer\",\"default\":12,\"description\":\"rows per facet\"},"
    "\"depth\":{\"type\":\"integer\",\"default\":3,\"description\":\"directory tree levels below the top entries\"},"
    "\"flat\":{\"type\":\"integer\",\"default\":0,\"description\":\"instead of a tree, rank prefixes at this depth (2 = drive\\\\folder)\"},"
    "\"min\":{\"type\":\"integer\",\"default\":0,\"description\":\"fold directories with fewer items (0 = 1 % of the result set)\"},"
    "\"burst_gap_s\":{\"type\":\"integer\",\"default\":60},"
    "\"bursts\":{\"type\":\"integer\",\"default\":10},"
    "\"max\":{\"type\":\"integer\",\"default\":0,\"description\":\"scan at most this many items (0 = all)\"}"
    "},\"required\":[\"query\"]}},{"
    "\"name\":\"facet_list\","
    "\"description\":\"Rows of an Everything search (full path, size, modified, folder flag), sorted by "
    "modified desc unless told otherwise, with the same subtree/extension/since filters as facet_query. "
    "Returns total and the rows shown (default 100).\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"query\":{\"type\":\"string\"},"
    "\"exclude\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
    "\"include\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
    "\"ext\":{\"type\":\"string\"},"
    "\"since\":{\"type\":\"string\"},"
    "\"sort\":{\"type\":\"string\",\"enum\":[\"modified\",\"name\",\"path\",\"size\",\"ext\"],\"default\":\"modified\"},"
    "\"ascending\":{\"type\":\"boolean\",\"default\":false},"
    "\"max\":{\"type\":\"integer\",\"default\":100,\"maximum\":10000}"
    "},\"required\":[\"query\"]}},{"
    "\"name\":\"facet_count\","
    "\"description\":\"Match count of an Everything search with the same filters - the cheapest probe.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"query\":{\"type\":\"string\"},"
    "\"exclude\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
    "\"include\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
    "\"ext\":{\"type\":\"string\"},"
    "\"since\":{\"type\":\"string\"}"
    "},\"required\":[\"query\"]}}]}";

static void mcp_common_opts(const JV* a, Opts& o) {
    auto str = [&](const char* k) -> std::wstring { return a && a->get(k) ? widen(a->get(k)->as_str("")) : L""; };
    auto arr = [&](const char* k, std::vector<std::wstring>& out) {
        if (!a || !a->get(k) || a->get(k)->t != JV::Arr) return;
        for (const auto& v : a->get(k)->arr)
            if (v.t == JV::Str && !v.s.empty()) out.push_back(widen(v.s));
    };
    o.query = str("query");
    arr("exclude", o.exclude);
    arr("include", o.include);
    const std::wstring e = str("ext");
    if (!e.empty()) o.ext.push_back(e);
    o.since = str("since");
    o.quiet = true;
    o.json = true;
}

static int run_mcp(const Opts& base) {
    std::string line;
    Everything es;
    configure(es, base);
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        JV v;
        if (!jparse(line, v) || v.t != JV::Obj) {
            mcp_error("null", -32700, "parse error");
            continue;
        }
        const JV* mv = v.get("method");
        const std::string method = mv ? mv->as_str("") : "";
        const JV* idv = v.get("id");
        if (!idv || idv->t == JV::Null) continue;   // notification — no response
        const std::string id = jv_id(*idv);
        if (method == "initialize") {
            std::string proto = "2025-06-18";
            if (const JV* p = v.get("params"))
                if (const JV* pv = p->get("protocolVersion")) proto = pv->as_str(proto.c_str());
            mcp_result(id, "{\"protocolVersion\":" + jstr(proto) +
                               ",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"facet\","
                               "\"version\":" + jstr(kVersion) + "}}");
        } else if (method == "ping") {
            mcp_result(id, "{}");
        } else if (method == "tools/list") {
            mcp_result(id, kToolsList);
        } else if (method == "tools/call") {
            const JV* p = v.get("params");
            const std::string tool = p && p->get("name") ? p->get("name")->as_str("") : "";
            const JV* a = p ? p->get("arguments") : nullptr;
            auto num = [&](const char* k, double d) { return a && a->get(k) ? a->get(k)->as_num(d) : d; };
            auto bol = [&](const char* k, bool d) { return a && a->get(k) ? a->get(k)->as_bool(d) : d; };
            Opts o;
            mcp_common_opts(a, o);
            if (tool == "facet_query") {
                o.top = (int)std::clamp(num("top", 12), 1.0, 500.0);
                o.depth = (int)std::clamp(num("depth", 3), 0.0, 32.0);
                o.flat = (int)std::clamp(num("flat", 0), 0.0, 32.0);
                const int mn = (int)std::clamp(num("min", 0), 0.0, 1e9);
                if (mn > 0) { o.min_count = mn; o.min_set = true; }
                o.burst_gap_s = (int)std::clamp(num("burst_gap_s", 60), 1.0, 86400.0);
                o.bursts = (int)std::clamp(num("bursts", 10), 0.0, 200.0);
                o.max_rows = (uint32_t)std::clamp(num("max", 0), 0.0, 4e9);
                FacetConfig cfg;
                cfg.burst_gap_s = (uint32_t)o.burst_gap_s;
                cfg.top_bursts = (uint32_t)o.bursts;
                Facets f(cfg);
                Run r;
                const bool ok = run_pass(o, es, f, r);
                mcp_result(id, mcp_text(ok ? report_json(f, o, r) : r.err, !ok));
            } else if (tool == "facet_list") {
                std::string sk = a && a->get("sort") ? a->get("sort")->as_str("modified") : "modified";
                if (!parse_sort_key(sk, o.sort)) o.sort = SortKey::Modified;
                o.ascending = bol("ascending", false);
                o.max_rows = (uint32_t)std::clamp(num("max", 100), 1.0, 10000.0);
                FacetConfig cfg;
                cfg.keep_rows = o.max_rows;
                cfg.top_bursts = 0;
                Facets f(cfg);
                Run r;
                const bool ok = run_pass(o, es, f, r);
                if (!ok) { mcp_result(id, mcp_text(r.err, true)); continue; }
                std::string j = "{\"query\":" + jw(o.query) + ",\"compiled\":" + jw(r.compiled) + ",\"total\":" + jn(r.total) +
                                ",\"shown\":" + jn(f.rows.size()) + ",\"rows\":[";
                for (size_t i = 0; i < f.rows.size(); ++i) {
                    const Row& rw = f.rows[i];
                    if (i) j += ",";
                    j += "{\"path\":" + jw(f.row_path(rw)) + ",\"size\":" + jopt(rw.size) + ",\"modified\":" +
                         (rw.mtime == kUnknown64 ? std::string("null") : jstr(fmt_filetime_iso(rw.mtime))) +
                         ",\"folder\":" + (rw.folder ? "true" : "false") + "}";
                }
                j += "]}";
                mcp_result(id, mcp_text(j));
            } else if (tool == "facet_count") {
                const std::wstring compiled = compile(o.query, filters_from(o));
                uint32_t total = 0;
                std::string err;
                if (!es.count(compiled, 0, &total, &err)) mcp_result(id, mcp_text(err, true));
                else mcp_result(id, mcp_text("{\"query\":" + jw(o.query) + ",\"compiled\":" + jw(compiled) + ",\"total\":" + jn(total) + "}"));
            } else {
                mcp_error(id, -32602, "unknown tool: " + tool);
            }
        } else {
            mcp_error(id, -32601, "method not found: " + method);
        }
    }
    return 0;
}

// ======================================================================
// --selftest
// ======================================================================
static std::vector<uint8_t> fake_list2() {
    struct It { bool folder; const wchar_t* name; const wchar_t* path; uint64_t size; uint64_t mtime; };
    const It its[3] = {
        { true, L"docs", L"C:\\proj", 0xFFFFFFFFFFFFFFFFull, 133000000000000000ull },
        { false, L"a.md", L"C:\\proj\\docs", 1234, 133000000001000000ull },
        { false, L"\u65E5\u672C.txt", L"D:", 0, 0xFFFFFFFFFFFFFFFFull },
    };
    std::vector<uint8_t> b;
    auto u32 = [&](uint32_t v) { b.insert(b.end(), (const uint8_t*)&v, (const uint8_t*)&v + 4); };
    auto u64 = [&](uint64_t v) { b.insert(b.end(), (const uint8_t*)&v, (const uint8_t*)&v + 8); };
    auto str = [&](const wchar_t* s) {
        const uint32_t n = (uint32_t)wcslen(s);
        u32(n);
        b.insert(b.end(), (const uint8_t*)s, (const uint8_t*)s + ((size_t)n + 1) * 2);
    };
    u32(3); u32(3); u32(0);
    u32(ipc::kReqName | ipc::kReqPath | ipc::kReqSize | ipc::kReqModified);
    u32(ipc::NameAsc);
    const size_t items_at = b.size();
    for (int i = 0; i < 3; ++i) { u32(its[i].folder ? ipc::kItemFolder : 0); u32(0); }
    for (int i = 0; i < 3; ++i) {
        const uint32_t off = (uint32_t)b.size();
        memcpy(b.data() + items_at + (size_t)i * 8 + 4, &off, 4);
        str(its[i].name);
        str(its[i].path);
        u64(its[i].size);
        u64(its[i].mtime);
    }
    return b;
}

static int run_selftest(const Opts& opts) {
    int fails = 0;
    auto check = [&](bool ok, const std::string& what) {
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
        if (!ok) fails++;
    };
    printf("facet %s --selftest\n", kVersion);

    // ---- IPC layout + parser
    check(sizeof(ipc::Query2) == 28 && sizeof(ipc::List2) == 20 && sizeof(ipc::Item2) == 8, "IPC struct layout");
    {
        const std::vector<uint8_t> blob = fake_list2();
        std::vector<EsItem> items;
        EsPage pg;
        const bool ok = parse_list2(blob.data(), blob.size(), items, pg);
        check(ok && pg.total == 3 && items.size() == 3, "parse_list2 walks a synthetic LIST2");
        if (ok && items.size() == 3) {
            check(items[0].folder && std::wstring_view(items[0].name, items[0].name_len) == L"docs" &&
                      std::wstring_view(items[0].path, items[0].path_len) == L"C:\\proj" && items[0].size == kUnknown64,
                  "folder item: name/path/size-unknown");
            check(!items[1].folder && items[1].size == 1234 && items[1].mtime == 133000000001000000ull, "file item: size + mtime");
            check(std::wstring_view(items[2].name, items[2].name_len) == L"\u65E5\u672C.txt" && items[2].mtime == kUnknown64 && items[2].size == 0,
                  "unicode name, unknown mtime, zero size");
        }
        std::vector<uint8_t> cut(blob.begin(), blob.begin() + (long)(blob.size() - 5));
        check(!parse_list2(cut.data(), cut.size(), items, pg), "parse_list2 rejects a truncated reply");
    }

    // ---- the fold
    {
        FacetConfig c;
        c.now = now_filetime();
        c.keep_rows = 10;
        c.burst_gap_s = 60;
        c.top_bursts = 5;
        Facets f(c);
        auto add = [&](const wchar_t* path, const wchar_t* name, uint64_t size, uint64_t mtime, bool folder) {
            EsItem it;
            it.name = name;
            it.name_len = (uint32_t)wcslen(name);
            it.path = path;
            it.path_len = (uint32_t)wcslen(path);
            it.size = size;
            it.mtime = mtime;
            it.folder = folder;
            f.add(it);
        };
        const uint64_t t0 = c.now - 5 * 3600 * kTicksPerSec;
        add(L"C:\\a\\b", L"x.md", 100, t0, false);
        add(L"C:\\a", L"y.MD", 200, t0 + 10 * kTicksPerSec, false);
        add(L"C:\\a\\b\\c", L"z.txt", 300, t0 + 20 * kTicksPerSec, false);
        add(L"D:\\q", L"w.md", 400, t0 + 1000 * kTicksPerSec, false);
        add(L"C:", L"root.md", kUnknown64, t0 + 2000 * kTicksPerSec, false);
        add(L"C:\\a", L"sub", kUnknown64, t0 + 2001 * kTicksPerSec, true);
        f.finish();
        check(f.items == 6 && f.files == 5 && f.folders == 1 && f.bytes == 1000, "totals");
        uint32_t nC = 0, nA = 0, nB = 0;
        for (uint32_t d : f.nodes[0].children) if (f.nodes[d].name == L"C:") nC = d;
        if (nC) for (uint32_t x : f.nodes[nC].children) if (f.nodes[x].name == L"a") nA = x;
        if (nA) for (uint32_t x : f.nodes[nA].children) if (f.nodes[x].name == L"b") nB = x;
        check(nC && nA && nB, "directory trie built");
        check(f.nodes[0].count == 6 && f.nodes[nC].count == 5 && f.nodes[nA].count == 4 && f.nodes[nA].self == 2 && f.nodes[nB].count == 2, "subtree counts propagate");
        check(f.dir_path(nB) == L"C:\\a\\b\\", "dir_path");
        check(f.exts.size() == 2 && f.exts[0].ext == L"md" && f.exts[0].count == 4 && f.exts[1].ext == L"txt", "extensions (case-folded, ranked)");
        check(f.burst_total == 3 && f.handpaced_bursts == 2 && f.handpaced_files == 3, "bursts split on the gap");
        check(!f.bursts.empty() && f.bursts[0].count == 3 && f.bursts[0].dir == nA && f.bursts[0].dir_share > 0.99, "dominant burst directory");
        check(f.sizes[2].count == 4 && f.sizes[0].count == 2, "size buckets");
        check(f.rows.size() == 6 && f.row_path(f.rows[4]) == L"C:\\root.md" && f.rows[5].folder, "retained rows");
        uint64_t sum = 0;
        for (const auto& b : f.modified) sum += b.count;
        check(sum == 6, "every item lands in one modified bucket");
    }

    // ---- the compiler
    check(compile(L"ext:md", { { Filter::Kind::DirOut, L"C:/x y" } }) == L"ext:md !path:\"C:\\x y\\\"", "compile: exclude subtree");
    check(compile(L"", { { Filter::Kind::DirIn, L"C:\\a\\" }, { Filter::Kind::DirIn, L"D:\\b" } }) == L"<path:\"C:\\a\\\"|path:\"D:\\b\\\">",
          "compile: two includes OR");
    check(compile(L"q", { { Filter::Kind::ExtIn, L"md" }, { Filter::Kind::ExtIn, L"txt" }, { Filter::Kind::ExtOut, L"log" } }) == L"q ext:md;txt !ext:log",
          "compile: extensions");
    check(since_term(L"3d") == L"dm:last3days" && since_term(L"12h") == L"dm:last720mins" && since_term(L"2w") == L"dm:last14days" &&
              since_term(L"today") == L"dm:today" && since_term(L"2026-08-30") == L"dm:>=2026-08-30",
          "--since forms");

    // ---- formatting
    check(fmt_count(1234567) == "1,234,567" && fmt_count(999) == "999", "fmt_count");
    check(human_bytes(1536) == "1.5 KB" && human_bytes(100) == "100 B" && human_bytes(300ull << 20) == "300 MB", "human_bytes");
    check(display_width("chrome.exe") == 10 && display_width("\xE6\x97\xA5\xE6\x9C\xAC") == 4, "display_width ascii + CJK");
    {
        const std::string t = trunc_middle("C:\\Users\\user\\AppData\\Local\\Packages\\Claude\\x.md", 24);
        check(display_width(t) <= 24 && t.find("\xE2\x80\xA6") != std::string::npos && t.rfind("x.md") == t.size() - 4, "trunc_middle keeps the tail");
    }
    {
        JV v;
        check(jparse("{\"a\":[1,\"x\",{\"b\":null}],\"c\":true}", v) && v.get("a") && v.get("a")->arr.size() == 3, "mini JSON parser");
    }

    // ---- finding Everything
    {
        const std::wstring exe = find_everything_exe(opts.everything_exe);
        check(!exe.empty(), exe.empty() ? std::string("find_everything_exe: none found") : "find_everything_exe: " + narrow(exe));
        check(find_everything_exe(L"C:\\no\\such\\Everything.exe").empty(), "find_everything_exe honours a bad override (empty)");
        check(strstr(everything_install_hint(), "voidtools.com/downloads") != nullptr, "install hint carries the download link");
    }

    // ---- live
    {
        Everything es;
        configure(es, opts);
        std::string err;
        const bool up = es.connect(&err);
        check(up, up ? "Everything reachable: " + es.info().version() : "Everything: " + err);
        if (up) {
            printf("  info: db_loaded=%d size_indexed=%d modified_indexed=%d created_indexed=%d\n", es.info().db_loaded,
                   es.info().size_indexed, es.info().modified_indexed, es.info().created_indexed);
            uint32_t total = 0;
            const bool cnt_ok = es.count(L"", 0, &total, &err) && total > 0;
            check(cnt_ok, ssprintf("count(\"\") = %s items", fmt_count(total).c_str()));
            uint32_t n = 0, dated = 0, named = 0;
            uint32_t tot = 0;
            const bool q = es.query(L"ext:md", ipc::ModifiedDesc, 0, 50, 65536, ipc::kReqName | ipc::kReqPath | ipc::kReqSize | ipc::kReqModified,
                                    [&](const EsPage& pg, const EsItem* it, uint32_t k) {
                                        tot = pg.total;
                                        for (uint32_t i = 0; i < k; ++i) {
                                            n++;
                                            if (it[i].mtime != kUnknown64) dated++;
                                            if (it[i].name_len && it[i].path_len) named++;
                                        }
                                        return true;
                                    }, &err);
            check(q && n == 50 && named == 50, ssprintf("query ext:md max 50: %u rows, %u named, %u dated, %s total", n, named, dated, fmt_count(tot).c_str()));
            // paging: 3 pages of 7 must equal the first 21 of one page
            std::vector<std::wstring> paged, whole;
            es.query(L"ext:md", ipc::NameAsc, 0, 21, 7, ipc::kReqName | ipc::kReqPath, [&](const EsPage&, const EsItem* it, uint32_t k) {
                for (uint32_t i = 0; i < k; ++i) paged.push_back(std::wstring(it[i].path, it[i].path_len) + L"\\" + std::wstring(it[i].name, it[i].name_len));
                return true;
            }, &err);
            es.query(L"ext:md", ipc::NameAsc, 0, 21, 65536, ipc::kReqName | ipc::kReqPath, [&](const EsPage&, const EsItem* it, uint32_t k) {
                for (uint32_t i = 0; i < k; ++i) whole.push_back(std::wstring(it[i].path, it[i].path_len) + L"\\" + std::wstring(it[i].name, it[i].name_len));
                return true;
            }, &err);
            check(paged.size() == 21 && paged == whole, "paging is seamless (3 x 7 == 1 x 21)");
            // end to end: the biggest directory's count must equal what excluding it removes
            Opts o = opts;
            o.query = L"ext:md dm:last7days";
            o.quiet = true;
            FacetConfig cfg;
            cfg.top_bursts = 3;
            Facets f(cfg);
            Run r;
            const bool ok = run_pass(o, es, f, r);
            check(ok, ok ? ssprintf("facet pass over %s items in %.0f ms", fmt_count(f.items).c_str(), r.ms) : r.err);
            if (ok && f.items > 0) {
                check(f.items == r.total, "scanned every match");
                uint32_t best = 0;
                for (uint32_t drv : f.nodes[0].children)
                    for (uint32_t c : f.nodes[drv].children)
                        if (!best || f.nodes[c].count > f.nodes[best].count) best = c;
                if (best) {
                    Opts ox = o;
                    ox.exclude.push_back(f.dir_path(best));
                    const std::wstring cq = compile(ox.query, filters_from(ox));
                    uint32_t rest = 0;
                    const bool cok = es.count(cq, 0, &rest, &err);
                    check(cok && rest + f.nodes[best].count == r.total,
                          ssprintf("exclude compiles exactly: %s - %s = %s  (%s)", fmt_count(r.total).c_str(),
                                   fmt_count(f.nodes[best].count).c_str(), fmt_count(rest).c_str(), narrow(cq).c_str()));
                    Opts oi = o;
                    oi.include.push_back(f.dir_path(best));
                    uint32_t inc = 0;
                    check(es.count(compile(oi.query, filters_from(oi)), 0, &inc, &err) && inc == f.nodes[best].count, "include compiles exactly");
                }
                for (const auto& b : f.modified) {
                    if (!b.count || b.query.empty()) continue;
                    uint32_t c = 0;
                    const bool cok = es.count(r.compiled + L" " + b.query, 0, &c, &err);
                    check(cok && c == b.count, ssprintf("modified bucket '%s' compiles exactly (%s == %s)", b.label.c_str(), fmt_count(c).c_str(), fmt_count(b.count).c_str()));
                }
                for (const auto& b : f.sizes) {
                    if (!b.count || b.query.empty()) continue;
                    uint32_t c = 0;
                    const bool cok = es.count(r.compiled + L" " + b.query, 0, &c, &err);
                    check(cok && c == b.count, ssprintf("size bucket '%s' compiles exactly (%s == %s)", b.label.c_str(), fmt_count(c).c_str(), fmt_count(b.count).c_str()));
                }
                for (size_t bi = 0; bi < f.bursts.size() && bi < 2; ++bi) {
                    const Burst& b = f.bursts[bi];
                    uint32_t c = 0;
                    const bool cok = es.count(r.compiled + L" " + b.query, 0, &c, &err);
                    check(cok && c == b.count, ssprintf("burst compiles exactly (%s == %s)  %s", fmt_count(c).c_str(), fmt_count(b.count).c_str(), narrow(b.query).c_str()));
                }
                JV v;
                check(jparse(report_json(f, o, r), v) && v.get("directories") && v.get("bursts"), "report JSON parses back");
            }
        }
    }
    printf(fails ? "SELFTEST: %d FAILURE(S)\n" : "SELFTEST: ALL PASS\n", fails);
    return fails ? 3 : 0;
}

// ======================================================================
// args / help / main
// ======================================================================
static const char* kHelp = R"HELP(facet %s — pivot filtering over Everything's index

USAGE
  facet <query>            the distribution of a search: directories (tree) · extensions ·
                           modified · size · write bursts — then the compiled query to paste
  facet -l <query>         rows (full paths), newest first; -ll adds size + date; -n caps (200)
  facet -c <query>         match count only
  facet -j <query>         JSON for agents (with -l: JSON rows; with -c: JSON count)
  facet --gui [query]      the window (facetw.exe opens it with no console — pin it)
  facet --shortcut         put "facet" in the Start Menu (type facet in Start; pin from there) · --shortcut desktop
  facet --mcp              MCP stdio server — tools: facet_query, facet_list, facet_count
  facet --where            which Everything.exe facet found, whether it is running, what is indexed
  facet --selftest         parser, fold, compiler, formatting, and live IPC checks

<query> is Everything syntax, verbatim: ext:md dm:today · path:C:\NEW\ · size:>1mb · !draft ·
a|b · "exact phrase" · regex:^foo — anything Everything accepts. Empty = every indexed item.

FILTERS (each compiles into the query; the report prints the result)
  -x, --exclude DIR    drop a subtree            → !path:"DIR\"      (repeatable)
  -i, --include DIR    keep only a subtree       → path:"DIR\"       (several = OR)
  -e, --ext md;txt     only these extensions     → ext:md;txt
  --since S            today | yesterday | 3d | 12h | 30m | 2w | 2026-08-30 | any dm: constant
  --files / --folders  only files / only folders

SHAPE
  -t, --top N          rows per facet (default 12)
  -d, --depth N        directory tree levels below the top entries (default 3)
  --flat N             rank prefixes at depth N instead of a tree (2 = drive\folder)
  --min N              fold directories with fewer items (default: 1 %% of the result set)
  --burst-gap S        seconds of silence that close a write burst (default 60)
  --bursts N           bursts to show (default 10)
  -n, --max N          scan at most N items (0 = all; facets need all)
  -s, --sort K         list order: modified | name | path | size | ext   -a ascending
  --plain              no colors / ASCII bars     -q  no progress on stderr
  --no-start           never start Everything (by default facet finds Everything.exe and starts
                       it tray-only when no instance is running, then waits for the database)
  --everything-exe P   the Everything.exe to use (or FACET_EVERYTHING=P) — portable installs
  --ini P              the window's settings file: standing excludes + placement (default facet.ini
                       next to the exe) — a second profile, or a scratch one for tests
  -h, --help           this text                  -v  version

EVERYTHING
  facet needs the running Everything instance (1.4, or the 1.5 alpha's named instance). Not
  running but installed → facet starts it (-startup, tray only) and says so on stderr. Not
  installed at all → facet says what it is, where to get it (voidtools.com/downloads) and what
  to do; with -j the same text is in "error", so an agent can relay it. facet --where shows both.

READING THE REPORT
  DIRECTORIES  where the matches live, ranked; a chain like C:\Users\user\ collapses when it
               has nothing of its own; "(files right here)" are items directly in that folder.
  WRITE BURSTS files whose modified times sit within --burst-gap of each other: thousands in a
               minute is a clone/extract/sync, a dozen is an agent session, 1-2 is a hand.
  QUERY        the Everything query that produced exactly these numbers — paste it into
               Everything's search box or search.py; every -x / -i / -e / --since is in it.

AGENTS
  facet -j "ext:md dm:today"                         where did today's markdown go, by directory
  facet -j --flat 2 "dm:last1hour"                   what wrote to the disk in the last hour
  facet -l -j -n 50 -x C:\deepseek-harness-master "ext:md dm:today"
  MCP server:  claude mcp add facet -- C:/facet/facet.exe --mcp
  JSON shape: {query,compiled,everything,total,scanned,files,folders,bytes,last_hour,elapsed_ms,
    directories[{path,count,share,bytes,files_here,children[...],more{directories,items}}],
    extensions[{ext,count,share,bytes}], modified[{bucket,query,count,bytes}],
    size[{bucket,query,count,bytes}], bursts[{start,end,seconds,count,dir,dir_share}],
    bursts_total, handpaced{bursts,files}}   — every bucket's "query" is the term selecting it.

EXIT CODES   0 ok · 1 bad arguments · 2 Everything unreachable / IPC error · 3 selftest failed
)HELP";

static long need_num(int argc, wchar_t** argv, int& i, const char* flag) {
    if (i + 1 >= argc) {
        fprintf(stderr, "facet: %s needs a value\n", flag);
        exit(1);
    }
    return wcstol(argv[++i], nullptr, 10);
}
static std::wstring need_str(int argc, wchar_t** argv, int& i, const char* flag) {
    if (i + 1 >= argc) {
        fprintf(stderr, "facet: %s needs a value\n", flag);
        exit(1);
    }
    return argv[++i];
}

int app_main(int argc, wchar_t** argv) {
    Opts o;
    std::vector<std::wstring> pos;
    bool only_pos = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring w = argv[i];
        const std::string a = narrow(w);
        if (only_pos || a.size() < 2 || a[0] != '-') { pos.push_back(w); continue; }
        if (a == "--") { only_pos = true; continue; }
        if (a == "-l" || a == "--list") { if (o.mode == Opts::Mode::List) o.long_list = true; o.mode = Opts::Mode::List; }
        else if (a == "-ll" || a == "--long") { o.mode = Opts::Mode::List; o.long_list = true; }
        else if (a == "-c" || a == "--count") o.mode = Opts::Mode::Count;
        else if (a == "-j" || a == "--json") o.json = true;
        else if (a == "--gui") o.mode = Opts::Mode::Gui;
        else if (a == "--mcp") o.mode = Opts::Mode::Mcp;
        else if (a == "--selftest") o.mode = Opts::Mode::Selftest;
        else if (a == "--where") o.mode = Opts::Mode::Where;
        else if (a == "--make-icon") { o.mode = Opts::Mode::MakeIcon; o.out_file = narrow(need_str(argc, argv, i, "--make-icon")); }
        else if (a == "--shortcut") {
            o.mode = Opts::Mode::Shortcut;
            if (i + 1 < argc) {
                const std::string v = narrow(argv[i + 1]);
                if (v == "desktop" || v == "startmenu") { o.out_file = v; ++i; }
            }
        }
        else if (a == "--ini") o.ini = narrow(need_str(argc, argv, i, "--ini"));
        else if (a == "--no-start") o.no_start = true;
        else if (a == "--everything-exe") o.everything_exe = need_str(argc, argv, i, "--everything-exe");
        else if (a == "-h" || a == "--help" || a == "/?") o.mode = Opts::Mode::Help;
        else if (a == "-v" || a == "--version") o.mode = Opts::Mode::Version;
        else if (a == "-x" || a == "--exclude") o.exclude.push_back(need_str(argc, argv, i, "-x"));
        else if (a == "-i" || a == "--include" || a == "--in") o.include.push_back(need_str(argc, argv, i, "-i"));
        else if (a == "-e" || a == "--ext") o.ext.push_back(need_str(argc, argv, i, "-e"));
        else if (a == "--since") o.since = need_str(argc, argv, i, "--since");
        else if (a == "--files") o.files_only = true;
        else if (a == "--folders") o.folders_only = true;
        else if (a == "-t" || a == "--top") { o.top = (int)std::clamp(need_num(argc, argv, i, "--top"), 1L, 500L); o.top_set = true; }
        else if (a == "-d" || a == "--depth") { o.depth = (int)std::clamp(need_num(argc, argv, i, "--depth"), 0L, 32L); o.depth_set = true; }
        else if (a == "--flat") o.flat = (int)std::clamp(need_num(argc, argv, i, "--flat"), 1L, 32L);
        else if (a == "--min") { o.min_count = (int)std::clamp(need_num(argc, argv, i, "--min"), 1L, 1000000000L); o.min_set = true; }
        else if (a == "--burst-gap") o.burst_gap_s = (int)std::clamp(need_num(argc, argv, i, "--burst-gap"), 1L, 86400L);
        else if (a == "--bursts") o.bursts = (int)std::clamp(need_num(argc, argv, i, "--bursts"), 0L, 200L);
        else if (a == "-n" || a == "--max") { o.max_rows = (uint32_t)std::clamp(need_num(argc, argv, i, "--max"), 0L, 2000000000L); o.max_set = true; }
        else if (a == "-s" || a == "--sort") {
            const std::string k = narrow(need_str(argc, argv, i, "--sort"));
            if (!parse_sort_key(k, o.sort)) { fprintf(stderr, "facet: unknown sort '%s'\n", k.c_str()); return 1; }
        }
        else if (a == "-a" || a == "--asc") o.ascending = true;
        else if (a == "--desc") o.ascending = false;
        else if (a == "--plain") o.plain = true;
        else if (a == "-q" || a == "--quiet") o.quiet = true;
        else if (a == "--page") o.page = (uint32_t)std::clamp(need_num(argc, argv, i, "--page"), 64L, 1048576L);
        else if (a == "--shot") o.shot = narrow(need_str(argc, argv, i, "--shot"));
        else {
            fprintf(stderr, "facet: unknown option '%s' (try --help)\n", a.c_str());
            return 1;
        }
    }
    o.query = join(pos, L" ");

    const bool auto_gui = o.mode == Opts::Mode::Auto && !o.json && fresh_own_console();
    if (o.mode == Opts::Mode::Gui || auto_gui) {
        if (fresh_own_console()) FreeConsole();
        return run_gui(o);
    }
    ensure_console_for_text();

    switch (o.mode) {
        case Opts::Mode::Help:
            if (stdout_is_console()) SetConsoleOutputCP(CP_UTF8);
            printf(kHelp, kVersion);
            return 0;
        case Opts::Mode::Version: printf("facet %s\n", kVersion); return 0;
        case Opts::Mode::Mcp: return run_mcp(o);
        case Opts::Mode::Where: return run_where(o);
        case Opts::Mode::MakeIcon: return write_icon_file(o.out_file);
        case Opts::Mode::Shortcut: return make_shortcut(o.out_file);
        case Opts::Mode::Selftest: { console_setup(o); return run_selftest(o); }
        case Opts::Mode::List: return run_list(o);
        case Opts::Mode::Count: return run_count(o);
        default: return run_report(o);
    }
}

}  // namespace facet

int wmain(int argc, wchar_t** argv) { return facet::app_main(argc, argv); }
