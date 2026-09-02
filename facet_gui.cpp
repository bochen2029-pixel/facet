// facet · facet_gui.cpp — the window: a query box, a facet rail you click, a results table.
// Left-click a facet row drills in (include), right-click excludes; every pick becomes a chip,
// the chips compile into the Everything query shown in the status line, and the query is re-run
// through Everything — so what you see is exactly what that query returns. Everything streams
// on a worker thread with its own IPC window; the UI thread only paints. Keys: Ctrl+L query ·
// Esc clear filters · F5 rerun · Enter open · Ctrl+C copy path · Ctrl+Shift+C copy query ·
// Ctrl+T pin on top. --shot FILE.png renders once and saves (the README screenshot, no hands).
#include "app_util.h"
#include "es_client.h"
#include "everything_ipc.h"
#include "facets.h"
#include "query.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <objidl.h>   // IStream / PROPID for gdiplus.h, which WIN32_LEAN_AND_MEAN leaves out
#include <shlobj.h>   // SHGetKnownFolderPath (the Start Menu / desktop folders)
#include <shobjidl.h> // IShellLinkW (the shortcut)

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <memory>
using std::max;   // gdiplus.h reaches for the min/max macros NOMINMAX removed; hand it the functions
using std::min;
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

namespace facet {

namespace {

// ---- theme (vramtop's palette, so the two tools sit together) ----
constexpr COLORREF kBg = RGB(15, 15, 19);
constexpr COLORREF kPanel = RGB(22, 22, 28);
constexpr COLORREF kPanel2 = RGB(27, 27, 34);
constexpr COLORREF kTrack = RGB(43, 43, 52);
constexpr COLORREF kText = RGB(236, 236, 240);
constexpr COLORREF kDim = RGB(148, 148, 160);
constexpr COLORREF kBlu = RGB(92, 164, 238);
constexpr COLORREF kAmb = RGB(244, 180, 66);
constexpr COLORREF kRed = RGB(238, 92, 92);
constexpr COLORREF kSel = RGB(38, 58, 92);
constexpr COLORREF kHover = RGB(33, 33, 42);
constexpr COLORREF kChipIn = RGB(38, 60, 92);
constexpr COLORREF kChipOut = RGB(92, 42, 50);
constexpr COLORREF kEditBg = RGB(30, 30, 38);
constexpr COLORREF kThumb = RGB(70, 70, 84);

COLORREF mix(COLORREF a, COLORREF b, double t) {
    auto ch = [&](int x, int y) { return (BYTE)std::lround(x + (y - x) * t); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}

enum class RowKind { Header, Dir, Ext, Mod, Size, Burst, Note };
struct RailRow {
    RowKind kind = RowKind::Note;
    int level = 0;
    std::wstring label;
    std::wstring right;         // the count
    double frac = 0.0;          // share of the result set → the bar behind the label
    bool actionable = false;
    Filter inc, exc;            // what a left / right click adds
    std::string group;          // "mod" / "size" / "burst" are single-valued: a new pick replaces
    std::wstring hint;          // status text on hover
    std::wstring section;       // the header this row sits under (headers: their own name)
    RECT rc{};
};

struct Chip {
    Filter f;
    std::string group;
    bool pinned = false;        // standing exclude, kept in facet.ini
    std::wstring label;
    RECT rc{}, rc_x{};
};

struct Result {                 // built by the worker, owned by the UI once delivered
    uint32_t gen = 0;
    std::unique_ptr<Facets> f;
    std::wstring compiled;
    uint32_t total = 0;
    double ms = 0;
    std::string err;
    EsInfo info;
};

struct Request {
    uint32_t gen = 0;
    std::wstring compiled;
    SortKey sort = SortKey::Modified;
    bool ascending = false;
};

struct Scroll {                 // a region's vertical scroll state + its drawn scrollbar
    int pos = 0, page = 0, total = 0;
    RECT track{}, thumb{};
    void clamp() { pos = std::clamp(pos, 0, std::max(0, total - page)); }
};

struct Gui {
    Opts opts;
    HWND hwnd = nullptr, edit = nullptr;
    WNDPROC edit_proc = nullptr;
    HBRUSH edit_brush = nullptr;
    int dpi = 96;
    HFONT fTitle = nullptr, fBold = nullptr, fNorm = nullptr, fSmall = nullptr;
    HICON icon_big = nullptr, icon_small = nullptr;
    // worker
    SRWLOCK lock = SRWLOCK_INIT;
    CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT;
    Request req;
    bool req_pending = false;
    bool quit = false;
    HANDLE thread = nullptr;
    std::atomic<uint32_t> gen{ 0 };
    std::atomic<uint64_t> progress_items{ 0 }, progress_total{ 0 };   // the pass in flight, for the tally
    // state
    std::wstring query;
    std::vector<Chip> chips;
    SortKey sort = SortKey::Modified;
    bool ascending = false;
    std::unique_ptr<Result> res;
    bool busy = false;
    std::wstring compiled;      // of the latest submit
    std::wstring note;          // from the worker ("starting Everything…"), guarded by lock
    std::wstring busy_note;     // the UI's copy, shown in the tally while busy
    // views
    std::vector<RailRow> rail;
    std::vector<std::wstring> collapsed;   // rail sections folded by clicking their header
    Scroll srail, stable;
    int sel = -1;
    int hover_rail = -1, hover_row = -1, hover_chip = -1, hover_col = -1;
    bool topmost = false;
    std::wstring status;
    RECT rcTop{}, rcChips{}, rcRail{}, rcHeader{}, rcTable{}, rcStatus{};
    int col_x[5]{};             // name | path | size | modified | end
    int drag = 0;               // 1 rail thumb · 2 table thumb
    int drag_off = 0;
    bool shot_pending = false;
    std::wstring ini;
};
Gui* g = nullptr;

constexpr UINT WM_APP_RESULT = WM_APP + 1;
constexpr UINT WM_APP_NOTE = WM_APP + 2;
constexpr UINT_PTR kDebounceTimer = 1, kShotTimer = 2, kProgressTimer = 3;
constexpr int kEditId = 100;
constexpr uint32_t kRowsCap = 200000;
enum { kMenuOpen = 1, kMenuOpenFolder, kMenuCopyPath, kMenuCopyName, kMenuDrill, kMenuExclude };

int px(const Gui& s, int v) { return MulDiv(v, s.dpi, 96); }

void make_fonts(Gui& s) {
    for (HFONT* f : { &s.fTitle, &s.fBold, &s.fNorm, &s.fSmall })
        if (*f) { DeleteObject(*f); *f = nullptr; }
    auto mk = [&](int pt, int weight) {
        return CreateFontW(-MulDiv(pt, s.dpi, 96), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    };
    s.fTitle = mk(15, FW_SEMIBOLD);
    s.fBold = mk(12, FW_SEMIBOLD);
    s.fNorm = mk(12, FW_NORMAL);
    s.fSmall = mk(11, FW_NORMAL);
    if (s.edit) SendMessageW(s.edit, WM_SETFONT, (WPARAM)s.fTitle, TRUE);
}

// The icon: a facet rail — three bars of falling length. Drawn at runtime for the window and
// exported by `facet --make-icon facet.ico` for the .rc, so Explorer and the Start Menu show it too.
void draw_icon_pixels(int sz, std::vector<uint32_t>& p) {
    p.assign((size_t)sz * (size_t)sz, 0xFF14141A);
    auto put = [&](int x0, int y0, int x1, int y1, uint32_t argb) {
        for (int y = std::max(0, y0); y < std::min(sz, y1); ++y)
            for (int x = std::max(0, x0); x < std::min(sz, x1); ++x) p[(size_t)y * sz + x] = argb;
    };
    const int gp = std::max(1, sz / 12), h = (sz - 4 * gp) / 3;
    put(gp, gp, sz - gp, gp + h, 0xFF5CA4EE);                                    // blue: the big one
    put(gp, 2 * gp + h, gp + (sz - 2 * gp) * 3 / 5, 2 * gp + 2 * h, 0xFFF4B442);   // amber
    put(gp, 3 * gp + 2 * h, gp + (sz - 2 * gp) / 3, 3 * gp + 3 * h, 0xFF40C76A);   // green
}

HICON make_icon(int sz) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sz;
    bi.bmiHeader.biHeight = -sz;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color || !bits) return nullptr;
    std::vector<uint32_t> px;
    draw_icon_pixels(sz, px);
    memcpy(bits, px.data(), px.size() * sizeof(uint32_t));
    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON h2 = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return h2;
}

// ---- gdi helpers ----
void fill(HDC dc, const RECT& rc, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &rc, b);
    DeleteObject(b);
}
void frame(HDC dc, const RECT& rc, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FrameRect(dc, &rc, b);
    DeleteObject(b);
}
void draw_text(HDC dc, HFONT f, COLORREF c, RECT rc, const std::wstring& t, UINT flags) {
    HGDIOBJ of = SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, t.c_str(), (int)t.size(), &rc, flags | DT_NOPREFIX | DT_SINGLELINE);
    SelectObject(dc, of);
}
int text_width(HDC dc, HFONT f, const std::wstring& t) {
    HGDIOBJ of = SelectObject(dc, f);
    SIZE sz{};
    GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz);
    SelectObject(dc, of);
    return sz.cx;
}
RECT rect(int l, int t, int r, int b) { return RECT{ l, t, r, b }; }

// ---- ini: standing excludes + window placement, next to the exe ----
std::wstring exe_dir() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    std::wstring p(buf, n);
    const size_t s = p.find_last_of(L'\\');
    return s == std::wstring::npos ? L"" : p.substr(0, s + 1);
}
void ini_load(Gui& s, int& x, int& y, int& w, int& h) {
    std::ifstream in(s.ini.c_str());
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "exclude" && !v.empty()) {
            Chip c;
            c.f = { Filter::Kind::DirOut, widen(v) };
            c.group = "dir";
            c.pinned = true;
            s.chips.push_back(c);
        } else if (k == "x") x = atoi(v.c_str());
        else if (k == "y") y = atoi(v.c_str());
        else if (k == "w") w = atoi(v.c_str());
        else if (k == "h") h = atoi(v.c_str());
        else if (k == "topmost") s.topmost = atoi(v.c_str()) != 0;
    }
}
void ini_save(const Gui& s) {
    std::ofstream out(s.ini.c_str(), std::ios::trunc);
    if (!out) return;
    out << "; facet — standing excludes (right-click a chip to pin / unpin) and window placement\n[standing]\n";
    for (const auto& c : s.chips)
        if (c.pinned && c.f.kind == Filter::Kind::DirOut) out << "exclude=" << narrow(dir_prefix(c.f.value)) << "\n";
    WINDOWPLACEMENT wp{ sizeof wp };
    if (s.hwnd && GetWindowPlacement(s.hwnd, &wp)) {
        const RECT& r = wp.rcNormalPosition;
        out << "[window]\nx=" << r.left << "\ny=" << r.top << "\nw=" << (r.right - r.left) << "\nh=" << (r.bottom - r.top) << "\n";
    }
    out << "topmost=" << (s.topmost ? 1 : 0) << "\n";
}

// ---- clipboard / shell ----
void copy_text(HWND h, const std::wstring& t) {
    if (!OpenClipboard(h)) return;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
    if (mem) {
        void* p = GlobalLock(mem);
        if (p) {
            memcpy(p, t.c_str(), (t.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
    }
    CloseClipboard();
}
void shell_open(HWND h, const std::wstring& path) { ShellExecuteW(h, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
void shell_reveal(HWND h, const std::wstring& path) {
    const std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(h, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

// ---- the worker: one Everything per thread, streams the compiled query into a Facets ----
DWORD WINAPI worker(LPVOID) {
    Gui& s = *g;
    Everything es;
    es.launch.allow_start = !s.opts.no_start;
    es.launch.exe_override = s.opts.everything_exe;
    es.on_note = [&s](const std::string& n) {
        AcquireSRWLockExclusive(&s.lock);
        s.note = widen(n);
        ReleaseSRWLockExclusive(&s.lock);
        if (s.hwnd) PostMessageW(s.hwnd, WM_APP_NOTE, 0, 0);
    };
    for (;;) {
        Request r;
        AcquireSRWLockExclusive(&s.lock);
        while (!s.req_pending && !s.quit) SleepConditionVariableSRW(&s.cv, &s.lock, INFINITE, 0);
        if (s.quit) { ReleaseSRWLockExclusive(&s.lock); return 0; }
        r = s.req;
        s.req_pending = false;
        ReleaseSRWLockExclusive(&s.lock);

        auto res = std::make_unique<Result>();
        res->gen = r.gen;
        res->compiled = r.compiled;
        FacetConfig cfg;
        cfg.keep_rows = kRowsCap;
        cfg.top_bursts = (uint32_t)std::max(1, s.opts.bursts);
        cfg.burst_gap_s = (uint32_t)s.opts.burst_gap_s;
        res->f = std::make_unique<Facets>(cfg);
        const double t0 = now_ms();
        bool stale = false;
        const uint32_t req = ipc::kReqName | ipc::kReqPath | ipc::kReqSize | ipc::kReqModified;
        s.progress_items = 0;
        s.progress_total = 0;
        const bool ok = es.query(r.compiled, ipc_sort(r.sort, r.ascending), 0, s.opts.max_rows, s.opts.page, req,
                                 [&](const EsPage& pg, const EsItem* it, uint32_t n) {
                                     res->total = pg.total;
                                     for (uint32_t i = 0; i < n; ++i) res->f->add(it[i]);
                                     s.progress_items = res->f->items;
                                     s.progress_total = pg.total;
                                     if (s.gen.load() != r.gen) { stale = true; return false; }
                                     return true;
                                 }, &res->err);
        if (stale || s.gen.load() != r.gen) continue;
        res->f->finish();
        res->info = es.info();
        res->ms = now_ms() - t0;
        if (!ok && res->err.empty()) res->err = "query failed";
        if (s.hwnd) PostMessageW(s.hwnd, WM_APP_RESULT, 0, (LPARAM)res.release());
    }
}

std::wstring chip_label(const Filter& f) {
    switch (f.kind) {
        case Filter::Kind::DirIn: return L"in " + dir_prefix(f.value);
        case Filter::Kind::DirOut: return L"not " + dir_prefix(f.value);
        case Filter::Kind::ExtIn: return L"ext:" + f.value;
        case Filter::Kind::ExtOut: return L"!ext:" + f.value;
        default: return f.value;
    }
}

void submit(Gui& s) {
    std::vector<Filter> fs;
    for (const auto& c : s.chips) fs.push_back(c.f);
    s.compiled = compile(s.query, fs);
    const uint32_t gn = ++s.gen;
    AcquireSRWLockExclusive(&s.lock);
    s.req = { gn, s.compiled, s.sort, s.ascending };
    s.req_pending = true;
    ReleaseSRWLockExclusive(&s.lock);
    WakeConditionVariable(&s.cv);
    s.busy = true;
    SetTimer(s.hwnd, kProgressTimer, 200, nullptr);   // repaint the tally while the pass runs
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void add_chip(Gui& s, const Filter& f, const std::string& group) {
    const bool single = group == "mod" || group == "size" || group == "burst";
    for (size_t i = 0; i < s.chips.size(); ++i) {
        const Chip& c = s.chips[i];
        if (c.f.kind == f.kind && c.f.value == f.value) return;              // already there
        if (single && c.group == group && !c.pinned) { s.chips.erase(s.chips.begin() + (long)i); break; }
    }
    Chip c;
    c.f = f;
    c.group = group;
    c.label = chip_label(f);
    s.chips.push_back(c);
    submit(s);
}

void remove_chip(Gui& s, size_t i) {
    if (i >= s.chips.size()) return;
    const bool was_pinned = s.chips[i].pinned;
    s.chips.erase(s.chips.begin() + (long)i);
    if (was_pinned) ini_save(s);
    submit(s);
}

void clear_chips(Gui& s) {
    std::vector<Chip> keep;
    for (const auto& c : s.chips)
        if (c.pinned) keep.push_back(c);
    if (keep.size() == s.chips.size()) return;
    s.chips = std::move(keep);
    submit(s);
}

// ---- the rail view model, from the latest result ----
void build_rail(Gui& s) {
    s.rail.clear();
    s.hover_rail = -1;
    if (!s.res || !s.res->f) return;
    const Facets& f = *s.res->f;
    Opts o = s.opts;
    std::wstring section;
    auto header = [&](const wchar_t* t, uint64_t n) {
        RailRow r;
        r.kind = RowKind::Header;
        r.label = t;
        r.right = widen(fmt_count(n));
        r.section = t;
        r.hint = L"click: fold / unfold this section";
        section = t;
        s.rail.push_back(r);
    };
    header(L"DIRECTORIES", f.items);
    const std::vector<DirLine> lines = o.flat ? flat_lines(f, o) : dir_lines(f, o);
    for (const DirLine& dl : lines) {
        RailRow r;
        r.kind = dl.note ? RowKind::Note : RowKind::Dir;
        r.level = dl.level;
        r.label = widen(dl.label);
        r.right = widen(fmt_count(dl.count));
        r.frac = f.items ? (double)dl.count / (double)f.items : 0.0;
        if (dl.here && dl.node) {
            const std::wstring p = f.dir_path(dl.node);   // the files directly inside: Everything's parent: function
            r.kind = RowKind::Dir;
            r.actionable = true;
            r.inc = { Filter::Kind::Term, L"parent:\"" + p + L"\"" };
            r.exc = { Filter::Kind::Term, L"!parent:\"" + p + L"\"" };
            r.group = "dir";
            r.hint = L"click: only the files directly in " + p + L"   ·   right-click: exclude them";
        } else if (!dl.note && dl.node) {
            const std::wstring p = f.dir_path(dl.node);
            r.actionable = true;
            r.inc = { Filter::Kind::DirIn, p };
            r.exc = { Filter::Kind::DirOut, p };
            r.group = "dir";
            r.hint = L"click: only " + p + L"   ·   right-click: exclude it   ·   " + widen(human_bytes(dl.bytes));
        }
        s.rail.push_back(r);
    }
    header(L"EXTENSIONS", f.files);
    int shown = 0;
    for (const ExtStat& e : f.exts) {
        if (shown++ >= o.top) break;
        RailRow r;
        r.kind = RowKind::Ext;
        r.label = e.ext.empty() ? L"(no extension)" : L"." + e.ext;
        r.right = widen(fmt_count(e.count));
        r.frac = f.files ? (double)e.count / (double)f.files : 0.0;
        if (!e.ext.empty()) {
            r.actionable = true;
            r.inc = { Filter::Kind::ExtIn, e.ext };
            r.exc = { Filter::Kind::ExtOut, e.ext };
            r.group = "ext";
            r.hint = L"click: only ." + e.ext + L"   ·   right-click: exclude it   ·   " + widen(human_bytes(e.bytes));
        }
        s.rail.push_back(r);
    }
    auto buckets = [&](const wchar_t* title, const std::vector<BucketStat>& bs, RowKind kind, const char* group) {
        header(title, f.items);
        for (const BucketStat& b : bs) {
            if (!b.count) continue;
            RailRow r;
            r.kind = kind;
            r.label = widen(b.label);
            r.right = widen(fmt_count(b.count));
            r.frac = f.items ? (double)b.count / (double)f.items : 0.0;
            if (!b.query.empty()) {
                r.actionable = true;
                r.inc = { Filter::Kind::Term, b.query };
                r.exc = { Filter::Kind::Term, L"!" + b.query };
                r.group = group;
                r.hint = L"click: " + b.query + L"   ·   right-click: !" + b.query + L"   ·   " + widen(human_bytes(b.bytes));
            }
            s.rail.push_back(r);
        }
    };
    buckets(L"MODIFIED", f.modified, RowKind::Mod, "mod");
    buckets(L"SIZE", f.sizes, RowKind::Size, "size");
    header(L"WRITE BURSTS", f.burst_total);
    for (const Burst& b : f.bursts) {
        RailRow r;
        r.kind = RowKind::Burst;
        std::wstring where;
        if (!b.parts.empty()) {
            const std::wstring base = b.dir ? f.dir_path(b.dir) : L"";
            for (size_t i = 0; i < b.parts.size() && i < 2; ++i) {
                std::wstring rel = f.dir_path(b.parts[i].first);
                if (!base.empty() && rel.compare(0, base.size(), base) == 0) rel.erase(0, base.size());
                where += (i ? L" · " : L"") + rel + widen(ssprintf(" %.0f%%", b.parts[i].second * 100.0));
            }
            if (b.dir) where = base + L" > " + where;
        } else {
            where = (b.dir ? f.dir_path(b.dir) : L"(scattered)") + widen(ssprintf(" %.0f%%", b.dir_share * 100.0));
        }
        r.label = widen(fmt_filetime(b.start, false)) + L"   " + where;
        r.right = widen(fmt_count(b.count));
        r.frac = f.items ? (double)b.count / (double)f.items : 0.0;
        r.actionable = true;
        r.inc = { Filter::Kind::Term, b.query };
        r.exc = { Filter::Kind::Term, L"!" + b.query };
        r.group = "burst";
        r.hint = widen(fmt_count(b.count)) + L" files " + widen(fmt_filetime(b.start)) + L" -> " + widen(fmt_local_time(b.end)) +
                 L"   ·   click: only this burst   ·   right-click: exclude it";
        s.rail.push_back(r);
    }
    if (!f.bursts.empty()) {
        RailRow r;
        r.kind = RowKind::Note;
        r.label = L"hand-paced: " + widen(fmt_count(f.handpaced_files)) + L" files in " + widen(fmt_count(f.handpaced_bursts)) + L" bursts of 1-2";
        s.rail.push_back(r);
    }
    // every non-header row belongs to the header above it
    std::wstring cur;
    for (auto& r : s.rail) {
        if (r.kind == RowKind::Header) cur = r.section;
        else r.section = cur;
    }
}

bool is_collapsed(const Gui& s, const std::wstring& section) {
    return std::find(s.collapsed.begin(), s.collapsed.end(), section) != s.collapsed.end();
}

// ---- layout ----
void layout(Gui& s, RECT client) {
    const int topH = px(s, 46), chipH = px(s, 30), statusH = px(s, 26), headH = px(s, 26);
    s.rcTop = rect(client.left, client.top, client.right, client.top + topH);
    s.rcChips = rect(client.left, s.rcTop.bottom, client.right, s.rcTop.bottom + chipH);
    s.rcStatus = rect(client.left, client.bottom - statusH, client.right, client.bottom);
    const int railW = std::clamp((int)(client.right - client.left) * 2 / 5, px(s, 280), px(s, 560));
    s.rcRail = rect(client.left, s.rcChips.bottom, client.left + railW, s.rcStatus.top);
    s.rcHeader = rect(s.rcRail.right, s.rcChips.bottom, client.right, s.rcChips.bottom + headH);
    s.rcTable = rect(s.rcRail.right, s.rcHeader.bottom, client.right, s.rcStatus.top);
    const int tw = s.rcTable.right - s.rcTable.left - px(s, 10);
    const int sizeW = px(s, 84), dateW = px(s, 128);
    const int nameW = std::clamp(tw * 3 / 10, px(s, 140), px(s, 420));
    s.col_x[0] = s.rcTable.left + px(s, 8);
    s.col_x[1] = s.col_x[0] + nameW;
    s.col_x[3] = s.rcTable.right - px(s, 10) - dateW;
    s.col_x[2] = s.col_x[3] - sizeW;
    s.col_x[4] = s.rcTable.right - px(s, 10);
    if (s.edit) {
        const int eh = px(s, 30);
        MoveWindow(s.edit, s.rcTop.left + px(s, 12), s.rcTop.top + (topH - eh) / 2 + px(s, 4), s.rcTop.right - px(s, 250) - px(s, 12),
                   eh - px(s, 8), TRUE);
    }
}

int rail_row_h(const Gui& s, const RailRow& r) { return r.kind == RowKind::Header ? px(s, 28) : px(s, 21); }
int table_row_h(const Gui& s) { return px(s, 22); }

void layout_rail(Gui& s) {
    int y = s.rcRail.top + px(s, 4) - s.srail.pos;
    for (auto& r : s.rail) {
        if (r.kind != RowKind::Header && is_collapsed(s, r.section)) { r.rc = RECT{}; continue; }   // folded away
        const int h = rail_row_h(s, r);
        r.rc = rect(s.rcRail.left, y, s.rcRail.right - px(s, 10), y + h);
        y += h;
    }
    s.srail.total = y + s.srail.pos - s.rcRail.top + px(s, 8);
    s.srail.page = s.rcRail.bottom - s.rcRail.top;
    s.srail.clamp();
}

void layout_table(Gui& s) {
    const int rows = s.res && s.res->f ? (int)s.res->f->rows.size() : 0;
    s.stable.total = rows * table_row_h(s);
    s.stable.page = s.rcTable.bottom - s.rcTable.top;
    s.stable.clamp();
}

void draw_scrollbar(HDC dc, Scroll& sc, RECT region, int w) {
    sc.track = rect(region.right - w, region.top, region.right, region.bottom);
    sc.thumb = RECT{};
    if (sc.total <= sc.page) return;
    fill(dc, sc.track, kPanel2);
    const int trackH = sc.track.bottom - sc.track.top;
    const int th = std::max(w * 2, (int)((long long)trackH * sc.page / sc.total));
    const int ty = sc.track.top + (int)((long long)(trackH - th) * sc.pos / std::max(1, sc.total - sc.page));
    sc.thumb = rect(sc.track.left + 2, ty, sc.track.right - 2, ty + th);
    fill(dc, sc.thumb, kThumb);
}

// ---- paint ----
void paint(Gui& s, HDC dc, RECT client) {
    fill(dc, client, kBg);
    SetBkMode(dc, TRANSPARENT);
    layout(s, client);
    const int m = px(s, 10);
    const Facets* f = (s.res && s.res->f) ? s.res->f.get() : nullptr;

    // top bar: the query box (a child control) + the tally
    fill(dc, s.rcTop, kPanel);
    if (s.edit) {
        RECT er;
        GetWindowRect(s.edit, &er);
        MapWindowPoints(nullptr, s.hwnd, (POINT*)&er, 2);
        InflateRect(&er, px(s, 6), px(s, 4));
        fill(dc, er, kEditBg);
        frame(dc, er, GetFocus() == s.edit ? kBlu : kTrack);
    }
    {
        std::wstring tally;
        if (s.busy) {
            const uint64_t done = s.progress_items.load(), tot = s.progress_total.load();
            if (!s.busy_note.empty() && !tot) tally = s.busy_note;
            else tally = tot ? L"searching…  " + widen(fmt_count(done)) + L" of " + widen(fmt_count(tot)) : L"searching…";
        } else if (s.res && !s.res->err.empty()) tally = L"! " + widen(s.res->err);
        else if (f) tally = widen(fmt_count(s.res->total)) + L" items · " + widen(ssprintf("%.0f ms", s.res->ms));
        RECT tr = rect(s.rcTop.right - px(s, 250), s.rcTop.top, s.rcTop.right - m, s.rcTop.bottom);
        draw_text(dc, s.fBold, (s.res && !s.res->err.empty()) ? kRed : kDim, tr, tally, DT_RIGHT | DT_VCENTER | DT_END_ELLIPSIS);
    }

    // chips
    fill(dc, s.rcChips, kPanel2);
    {
        int x = s.rcChips.left + m;
        const int ch = px(s, 20), cy = s.rcChips.top + (s.rcChips.bottom - s.rcChips.top - ch) / 2;
        for (size_t i = 0; i < s.chips.size(); ++i) {
            Chip& c = s.chips[i];
            const int tw = text_width(dc, s.fSmall, c.label) + px(s, 14) + px(s, 16) + (c.pinned ? px(s, 12) : 0);
            c.rc = rect(x, cy, x + tw, cy + ch);
            c.rc_x = rect(c.rc.right - px(s, 18), cy, c.rc.right, cy + ch);
            const bool out = c.f.kind == Filter::Kind::DirOut || c.f.kind == Filter::Kind::ExtOut ||
                             (c.f.kind == Filter::Kind::Term && !c.f.value.empty() && c.f.value[0] == L'!');
            COLORREF bg = out ? kChipOut : kChipIn;
            if ((int)i == s.hover_chip) bg = mix(bg, RGB(255, 255, 255), 0.12);
            fill(dc, c.rc, bg);
            RECT lr = rect(c.rc.left + px(s, 7), cy, c.rc_x.left, cy + ch);
            draw_text(dc, s.fSmall, kText, lr, (c.pinned ? L"● " : L"") + c.label, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
            draw_text(dc, s.fSmall, kDim, c.rc_x, L"×", DT_CENTER | DT_VCENTER);
            x = c.rc.right + px(s, 6);
            if (x > s.rcChips.right - px(s, 40)) break;
        }
        if (s.chips.empty()) {
            RECT hr = rect(x, s.rcChips.top, s.rcChips.right - m, s.rcChips.bottom);
            draw_text(dc, s.fSmall, kDim, hr,
                      L"no filters — click a facet to drill in, right-click to exclude; picks become chips here and compile into the query below",
                      DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }

    // rail
    fill(dc, s.rcRail, kPanel);
    layout_rail(s);
    {
        SaveDC(dc);
        IntersectClipRect(dc, s.rcRail.left, s.rcRail.top, s.rcRail.right, s.rcRail.bottom);
        for (size_t i = 0; i < s.rail.size(); ++i) {
            const RailRow& r = s.rail[i];
            if (r.rc.bottom < s.rcRail.top || r.rc.top > s.rcRail.bottom) continue;
            if (r.kind == RowKind::Header) {
                if ((int)i == s.hover_rail) fill(dc, r.rc, kHover);
                RECT hr = rect(r.rc.left + m, r.rc.top + px(s, 6), r.rc.right - m, r.rc.bottom);
                draw_text(dc, s.fBold, kDim, hr, (is_collapsed(s, r.section) ? L"▸  " : L"▾  ") + r.label, DT_LEFT | DT_VCENTER);
                draw_text(dc, s.fSmall, kDim, hr, r.right, DT_RIGHT | DT_VCENTER);
                continue;
            }
            const bool hov = (int)i == s.hover_rail && r.actionable;
            if (r.rc.bottom == 0) continue;   // folded
            if (hov) fill(dc, r.rc, kHover);
            const int ind = m + r.level * px(s, 12);
            RECT br = rect(r.rc.left + ind, r.rc.top + px(s, 2), r.rc.right - m, r.rc.bottom - px(s, 2));
            if (r.frac > 0 && r.kind != RowKind::Note) {
                RECT bar = br;
                bar.right = bar.left + (LONG)std::lround((br.right - br.left) * std::clamp(r.frac, 0.0, 1.0));
                fill(dc, bar, mix(hov ? kHover : kPanel, r.frac >= 0.5 ? kAmb : kBlu, 0.28));
            }
            RECT lr = rect(br.left + px(s, 4), r.rc.top, br.right - px(s, 64), r.rc.bottom);
            const COLORREF tc = r.kind == RowKind::Note ? kDim : (r.actionable ? kText : kDim);
            draw_text(dc, s.fNorm, tc, lr, r.label, DT_LEFT | DT_VCENTER | (r.kind == RowKind::Dir ? DT_PATH_ELLIPSIS : DT_END_ELLIPSIS));
            RECT cr = rect(br.right - px(s, 62), r.rc.top, br.right - px(s, 4), r.rc.bottom);
            draw_text(dc, s.fNorm, r.kind == RowKind::Note ? kDim : kText, cr, r.right, DT_RIGHT | DT_VCENTER);
        }
        RestoreDC(dc, -1);
        draw_scrollbar(dc, s.srail, s.rcRail, px(s, 10));
        if (!f && !s.busy) {
            draw_text(dc, s.fNorm, kDim, s.rcRail, L"type an Everything query above", DT_CENTER | DT_VCENTER);
        }
    }

    // table header
    fill(dc, s.rcHeader, kPanel2);
    {
        static const wchar_t* names[4] = { L"Name", L"Path", L"Size", L"Modified" };
        static const SortKey keys[4] = { SortKey::Name, SortKey::Path, SortKey::Size, SortKey::Modified };
        for (int c = 0; c < 4; ++c) {
            RECT hr = rect(s.col_x[c], s.rcHeader.top, s.col_x[c + 1] - px(s, 8), s.rcHeader.bottom);
            std::wstring t = names[c];
            if (s.sort == keys[c]) t += s.ascending ? L" ▲" : L" ▼";
            if (c == s.hover_col) fill(dc, rect(s.col_x[c] - px(s, 4), s.rcHeader.top, s.col_x[c + 1] - px(s, 4), s.rcHeader.bottom), kHover);
            draw_text(dc, s.fBold, s.sort == keys[c] ? kText : kDim, hr, t, (c >= 2 ? DT_RIGHT : DT_LEFT) | DT_VCENTER);
        }
    }

    // table
    layout_table(s);
    {
        SaveDC(dc);
        IntersectClipRect(dc, s.rcTable.left, s.rcTable.top, s.rcTable.right, s.rcTable.bottom);
        if (f) {
            const int rh = table_row_h(s);
            const int first = s.stable.pos / rh;
            const int last = std::min((int)f->rows.size(), (s.stable.pos + s.stable.page) / rh + 1);
            for (int i = first; i < last; ++i) {
                const Row& rw = f->rows[(size_t)i];
                RECT rr = rect(s.rcTable.left, s.rcTable.top + i * rh - s.stable.pos, s.rcTable.right - px(s, 10), s.rcTable.top + (i + 1) * rh - s.stable.pos);
                if (i == s.sel) fill(dc, rr, kSel);
                else if (i == s.hover_row) fill(dc, rr, kHover);
                draw_text(dc, s.fNorm, rw.folder ? kAmb : kText, rect(s.col_x[0], rr.top, s.col_x[1] - px(s, 8), rr.bottom),
                          std::wstring(f->row_name(rw)), DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
                draw_text(dc, s.fNorm, kDim, rect(s.col_x[1], rr.top, s.col_x[2] - px(s, 8), rr.bottom), f->dir_path(rw.dir),
                          DT_LEFT | DT_VCENTER | DT_PATH_ELLIPSIS);
                draw_text(dc, s.fNorm, kDim, rect(s.col_x[2], rr.top, s.col_x[3] - px(s, 8), rr.bottom),
                          rw.folder ? L"<dir>" : widen(human_bytes(rw.size)), DT_RIGHT | DT_VCENTER);
                draw_text(dc, s.fNorm, kDim, rect(s.col_x[3], rr.top, s.col_x[4], rr.bottom), widen(fmt_filetime(rw.mtime, false)),
                          DT_RIGHT | DT_VCENTER);
            }
            if (f->rows.empty()) draw_text(dc, s.fNorm, kDim, s.rcTable, s.busy ? L"" : L"no matches", DT_CENTER | DT_VCENTER);
        }
        RestoreDC(dc, -1);
        draw_scrollbar(dc, s.stable, s.rcTable, px(s, 10));
    }

    // status
    fill(dc, s.rcStatus, kPanel);
    {
        std::wstring st = s.status;
        if (st.empty()) {
            st = L"QUERY  " + (s.compiled.empty() ? L"(everything)" : s.compiled);
            if (f && f->rows.size() < s.res->total)
                st += L"     ·  showing the first " + widen(fmt_count(f->rows.size())) + L" of " + widen(fmt_count(s.res->total)) + L" rows";
            st += L"     ·  Ctrl+L query · Esc clear · Ctrl+Shift+C copy query · Ctrl+T pin";
        }
        RECT sr = rect(s.rcStatus.left + m, s.rcStatus.top, s.rcStatus.right - m, s.rcStatus.bottom);
        draw_text(dc, s.fSmall, kDim, sr, st, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    }
}

void update_title(Gui& s) {
    std::wstring t = L"facet";
    if (!s.query.empty()) t += L" — " + s.query;
    if (s.res && s.res->f) t += L" · " + widen(fmt_count(s.res->total));
    SetWindowTextW(s.hwnd, t.c_str());
}

// ---- screenshot: render the window (child controls included) and save a PNG through GDI+ ----
bool png_clsid(CLSID& out) {
    UINT n = 0, sz = 0;
    Gdiplus::GetImageEncodersSize(&n, &sz);
    if (!sz) return false;
    std::vector<uint8_t> buf(sz);
    auto* enc = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(n, sz, enc);
    for (UINT i = 0; i < n; ++i)
        if (wcscmp(enc[i].MimeType, L"image/png") == 0) { out = enc[i].Clsid; return true; }
    return false;
}
bool save_shot(HWND hwnd, const std::wstring& path) {
    Gdiplus::GdiplusStartupInput in;
    ULONG_PTR tok = 0;
    if (Gdiplus::GdiplusStartup(&tok, &in, nullptr) != Gdiplus::Ok) return false;
    RECT rc;
    GetClientRect(hwnd, &rc);
    // make sure every child (the query box) has painted before the capture
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    HDC wdc = GetDC(hwnd);
    HDC mdc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ ob = SelectObject(mdc, bmp);
    bool ok = PrintWindow(hwnd, mdc, PW_CLIENTONLY | 0x00000002 /* PW_RENDERFULLCONTENT */) != 0;
    SelectObject(mdc, ob);
    if (ok) {
        CLSID clsid;
        ok = png_clsid(clsid);
        if (ok) {
            Gdiplus::Bitmap gb(bmp, nullptr);
            ok = gb.Save(path.c_str(), &clsid, nullptr) == Gdiplus::Ok;
        }
    }
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(hwnd, wdc);
    Gdiplus::GdiplusShutdown(tok);
    return ok;
}

// ---- input ----
int hit_rail(const Gui& s, POINT p) {
    if (!PtInRect(&s.rcRail, p) || p.x >= s.rcRail.right - px(s, 10)) return -1;
    for (size_t i = 0; i < s.rail.size(); ++i)
        if (PtInRect(&s.rail[i].rc, p)) return (int)i;
    return -1;
}
int hit_row(const Gui& s, POINT p) {
    if (!s.res || !s.res->f || !PtInRect(&s.rcTable, p) || p.x >= s.rcTable.right - px(s, 10)) return -1;
    const int i = (p.y - s.rcTable.top + s.stable.pos) / table_row_h(s);
    return (i >= 0 && i < (int)s.res->f->rows.size()) ? i : -1;
}
int hit_chip(const Gui& s, POINT p) {
    for (size_t i = 0; i < s.chips.size(); ++i)
        if (PtInRect(&s.chips[i].rc, p)) return (int)i;
    return -1;
}
int hit_col(const Gui& s, POINT p) {
    if (!PtInRect(&s.rcHeader, p)) return -1;
    for (int c = 0; c < 4; ++c)
        if (p.x >= s.col_x[c] - px(s, 4) && p.x < s.col_x[c + 1] - px(s, 4)) return c;
    return -1;
}

void ensure_visible(Gui& s) {
    if (s.sel < 0) return;
    const int rh = table_row_h(s);
    const int top = s.sel * rh, bot = top + rh;
    if (top < s.stable.pos) s.stable.pos = top;
    else if (bot > s.stable.pos + s.stable.page) s.stable.pos = bot - s.stable.page;
    s.stable.clamp();
}

std::wstring sel_path(const Gui& s) {
    if (s.sel < 0 || !s.res || !s.res->f || s.sel >= (int)s.res->f->rows.size()) return L"";
    return s.res->f->row_path(s.res->f->rows[(size_t)s.sel]);
}
std::wstring sel_dir(const Gui& s) {
    if (s.sel < 0 || !s.res || !s.res->f || s.sel >= (int)s.res->f->rows.size()) return L"";
    const Row& r = s.res->f->rows[(size_t)s.sel];
    return r.folder ? s.res->f->row_path(r) + L"\\" : s.res->f->dir_path(r.dir);
}

void read_query(Gui& s) {
    const int n = GetWindowTextLengthW(s.edit);
    std::wstring t((size_t)n, L'\0');
    if (n > 0) GetWindowTextW(s.edit, t.data(), n + 1);
    s.query = t;
}

void set_sort(Gui& s, SortKey k) {
    if (s.sort == k) s.ascending = !s.ascending;
    else { s.sort = k; s.ascending = (k == SortKey::Name || k == SortKey::Path); }
    submit(s);
}

void context_menu(Gui& s, POINT screen) {
    if (s.sel < 0) return;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuOpen, L"Open\tEnter");
    AppendMenuW(menu, MF_STRING, kMenuOpenFolder, L"Open containing folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuCopyPath, L"Copy full path\tCtrl+C");
    AppendMenuW(menu, MF_STRING, kMenuCopyName, L"Copy name");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuDrill, (L"Only this folder:  " + sel_dir(s)).c_str());
    AppendMenuW(menu, MF_STRING, kMenuExclude, (L"Exclude this folder:  " + sel_dir(s)).c_str());
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, s.hwnd, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
        case kMenuOpen: shell_open(s.hwnd, sel_path(s)); break;
        case kMenuOpenFolder: shell_reveal(s.hwnd, sel_path(s)); break;
        case kMenuCopyPath: copy_text(s.hwnd, sel_path(s)); s.status = L"copied  " + sel_path(s); break;
        case kMenuCopyName: {
            const Row& r = s.res->f->rows[(size_t)s.sel];
            copy_text(s.hwnd, std::wstring(s.res->f->row_name(r)));
            break;
        }
        case kMenuDrill: add_chip(s, { Filter::Kind::DirIn, sel_dir(s) }, "dir"); break;
        case kMenuExclude: add_chip(s, { Filter::Kind::DirOut, sel_dir(s) }, "dir"); break;
        default: break;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void on_result(Gui& s, Result* raw) {
    std::unique_ptr<Result> r(raw);
    if (r->gen != s.gen.load()) return;   // a newer request is on its way
    s.res = std::move(r);
    s.busy = false;
    s.busy_note.clear();
    s.sel = -1;
    s.hover_row = -1;
    s.stable.pos = 0;
    s.srail.pos = 0;
    build_rail(s);
    update_title(s);
    if (!s.opts.shot.empty() && !s.shot_pending) {
        s.shot_pending = true;
        SetTimer(s.hwnd, kShotTimer, 700, nullptr);
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

LRESULT CALLBACK edit_sub(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    Gui& s = *g;
    if (m == WM_KEYDOWN) {
        const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        if (wp == VK_RETURN) { KillTimer(s.hwnd, kDebounceTimer); read_query(s); submit(s); update_title(s); return 0; }
        if (wp == VK_ESCAPE) { clear_chips(s); return 0; }
        if (wp == VK_DOWN) {
            SetFocus(s.hwnd);
            if (s.sel < 0 && s.res && s.res->f && !s.res->f->rows.empty()) s.sel = 0;
            InvalidateRect(s.hwnd, nullptr, FALSE);
            return 0;
        }
        if (wp == VK_F5) { submit(s); return 0; }
        if (ctrl && shift && wp == 'C') { copy_text(s.hwnd, s.compiled); s.status = L"copied the query"; InvalidateRect(s.hwnd, nullptr, FALSE); return 0; }
        if (ctrl && wp == 'A') { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
        if (ctrl && wp == 'T') { PostMessageW(s.hwnd, WM_KEYDOWN, 'T', 0); return 0; }
    }
    if (m == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE || wp == 1)) return 0;   // no ding
    if (m == WM_SETFOCUS || m == WM_KILLFOCUS) InvalidateRect(s.hwnd, &s.rcTop, FALSE);
    return CallWindowProcW(s.edit_proc, h, m, wp, lp);
}

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    Gui& s = *g;
    switch (m) {
        case WM_CREATE: {
            s.hwnd = h;
            s.edit = CreateWindowExW(0, L"EDIT", s.query.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 10, 10, h,
                                     (HMENU)(INT_PTR)kEditId, GetModuleHandleW(nullptr), nullptr);
            s.edit_proc = (WNDPROC)SetWindowLongPtrW(s.edit, GWLP_WNDPROC, (LONG_PTR)edit_sub);
            s.edit_brush = CreateSolidBrush(kEditBg);
            SendMessageW(s.edit, EM_SETLIMITTEXT, 4000, 0);
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, kText);
            SetBkColor(dc, kEditBg);
            return (LRESULT)s.edit_brush;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == kEditId && HIWORD(wp) == EN_CHANGE) SetTimer(h, kDebounceTimer, 260, nullptr);
            return 0;
        case WM_TIMER:
            if (wp == kDebounceTimer) {
                KillTimer(h, kDebounceTimer);
                const std::wstring before = s.query;
                read_query(s);
                if (before != s.query) { submit(s); update_title(s); }
            } else if (wp == kShotTimer) {
                KillTimer(h, kShotTimer);
                const bool ok = save_shot(h, widen(s.opts.shot));
                fprintf(stderr, "facet: %s %s\n", ok ? "saved" : "could not save", s.opts.shot.c_str());
                DestroyWindow(h);
            } else if (wp == kProgressTimer) {
                if (!s.busy) KillTimer(h, kProgressTimer);
                else InvalidateRect(h, &s.rcTop, FALSE);
            }
            return 0;
        case WM_CHAR:
            // typing while the table has focus goes to the query box
            if (wp >= 0x20 && wp != 0x7F && GetFocus() == h) {
                SetFocus(s.edit);
                const LRESULT len = SendMessageW(s.edit, WM_GETTEXTLENGTH, 0, 0);
                SendMessageW(s.edit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
                SendMessageW(s.edit, WM_CHAR, wp, lp);
            }
            return 0;
        case WM_APP_RESULT:
            on_result(s, (Result*)lp);
            return 0;
        case WM_APP_NOTE:
            AcquireSRWLockExclusive(&s.lock);
            s.busy_note = s.note;
            ReleaseSRWLockExclusive(&s.lock);
            InvalidateRect(h, &s.rcTop, FALSE);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, std::max<LONG>(rc.right, 1), std::max<LONG>(rc.bottom, 1));
            HGDIOBJ ob = SelectObject(mem, bmp);
            paint(s, mem, rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, ob);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE: {
            RECT rc;
            GetClientRect(h, &rc);
            layout(s, rc);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_SETFOCUS:
            return 0;
        case WM_MOUSEMOVE: {
            const POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (s.drag) {
                Scroll& sc = s.drag == 1 ? s.srail : s.stable;
                const int trackH = sc.track.bottom - sc.track.top, th = sc.thumb.bottom - sc.thumb.top;
                if (trackH > th)
                    sc.pos = (int)((long long)(p.y - s.drag_off - sc.track.top) * (sc.total - sc.page) / (trackH - th));
                sc.clamp();
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            const int hr = hit_rail(s, p), hrow = hit_row(s, p), hc = hit_chip(s, p), hcol = hit_col(s, p);
            std::wstring st;
            if (hr >= 0) st = s.rail[(size_t)hr].hint;
            else if (hrow >= 0) {
                const Row& r = s.res->f->rows[(size_t)hrow];
                st = s.res->f->row_path(r) + L"   ·   " + (r.folder ? L"folder" : widen(human_bytes(r.size))) + L"   ·   " + widen(fmt_filetime(r.mtime)) +
                     L"   ·   double-click opens · right-click for more";
            } else if (hc >= 0) st = L"× removes this filter   ·   right-click pins it as a standing exclude (kept in facet.ini)";
            else if (hcol >= 0) st = L"click to sort by this column (Everything sorts; the list is re-fetched)";
            if (hr != s.hover_rail || hrow != s.hover_row || hc != s.hover_chip || hcol != s.hover_col || st != s.status) {
                s.hover_rail = hr;
                s.hover_row = hrow;
                s.hover_chip = hc;
                s.hover_col = hcol;
                s.status = st;
                InvalidateRect(h, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (s.hover_rail != -1 || s.hover_row != -1 || s.hover_chip != -1 || s.hover_col != -1 || !s.status.empty()) {
                s.hover_rail = s.hover_row = s.hover_chip = s.hover_col = -1;
                s.status.clear();
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            const POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            SetFocus(h);
            if (PtInRect(&s.srail.thumb, p)) { s.drag = 1; s.drag_off = p.y - s.srail.thumb.top; SetCapture(h); return 0; }
            if (PtInRect(&s.stable.thumb, p)) { s.drag = 2; s.drag_off = p.y - s.stable.thumb.top; SetCapture(h); return 0; }
            if (PtInRect(&s.srail.track, p) && s.srail.total > s.srail.page) { s.srail.pos += (p.y < s.srail.thumb.top ? -1 : 1) * s.srail.page; s.srail.clamp(); InvalidateRect(h, nullptr, FALSE); return 0; }
            if (PtInRect(&s.stable.track, p) && s.stable.total > s.stable.page) { s.stable.pos += (p.y < s.stable.thumb.top ? -1 : 1) * s.stable.page; s.stable.clamp(); InvalidateRect(h, nullptr, FALSE); return 0; }
            const int hc = hit_chip(s, p);
            if (hc >= 0) { remove_chip(s, (size_t)hc); return 0; }
            const int hr = hit_rail(s, p);
            if (hr >= 0) {
                const RailRow& r = s.rail[(size_t)hr];
                if (r.kind == RowKind::Header) {
                    if (is_collapsed(s, r.section)) s.collapsed.erase(std::find(s.collapsed.begin(), s.collapsed.end(), r.section));
                    else s.collapsed.push_back(r.section);
                    InvalidateRect(h, nullptr, FALSE);
                } else if (r.actionable) {
                    add_chip(s, r.inc, r.group);
                }
                return 0;
            }
            const int hcol = hit_col(s, p);
            if (hcol >= 0) {
                static const SortKey keys[4] = { SortKey::Name, SortKey::Path, SortKey::Size, SortKey::Modified };
                set_sort(s, keys[hcol]);
                return 0;
            }
            const int hrow = hit_row(s, p);
            if (hrow >= 0) { s.sel = hrow; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        }
        case WM_LBUTTONUP:
            if (s.drag) { s.drag = 0; ReleaseCapture(); }
            return 0;
        case WM_LBUTTONDBLCLK: {
            const POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            const int hrow = hit_row(s, p);
            if (hrow >= 0) { s.sel = hrow; shell_open(h, sel_path(s)); }
            return 0;
        }
        case WM_RBUTTONDOWN: {
            const POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            SetFocus(h);
            const int hc = hit_chip(s, p);
            if (hc >= 0) {
                Chip& c = s.chips[(size_t)hc];
                if (c.f.kind == Filter::Kind::DirOut) {
                    c.pinned = !c.pinned;
                    ini_save(s);
                    s.status = c.pinned ? L"pinned — always excluded from now on (facet.ini)" : L"unpinned";
                    InvalidateRect(h, nullptr, FALSE);
                }
                return 0;
            }
            const int hr = hit_rail(s, p);
            if (hr >= 0) {
                const RailRow& r = s.rail[(size_t)hr];
                if (r.actionable) add_chip(s, r.exc, r.group + "-out");
                return 0;
            }
            const int hrow = hit_row(s, p);
            if (hrow >= 0) {
                s.sel = hrow;
                InvalidateRect(h, nullptr, FALSE);
                POINT sp = p;
                ClientToScreen(h, &sp);
                context_menu(s, sp);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(h, &p);
            const int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            if (PtInRect(&s.rcRail, p)) { s.srail.pos -= notches * 3 * px(s, 21); s.srail.clamp(); }
            else { s.stable.pos -= notches * 3 * table_row_h(s); s.stable.clamp(); }
            // hover follows the content under the cursor
            s.hover_rail = hit_rail(s, p);
            s.hover_row = hit_row(s, p);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN: {
            const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
            const int rows = (s.res && s.res->f) ? (int)s.res->f->rows.size() : 0;
            const int page = std::max(1, s.stable.page / table_row_h(s));
            switch (wp) {
                case VK_DOWN: if (rows) s.sel = std::min(rows - 1, s.sel + 1); break;
                case VK_UP: if (rows) s.sel = std::max(0, s.sel - 1); break;
                case VK_NEXT: if (rows) s.sel = std::min(rows - 1, std::max(0, s.sel) + page); break;
                case VK_PRIOR: if (rows) s.sel = std::max(0, s.sel - page); break;
                case VK_HOME: if (rows) s.sel = 0; break;
                case VK_END: if (rows) s.sel = rows - 1; break;
                case VK_RETURN: if (s.sel >= 0) shell_open(h, sel_path(s)); return 0;
                case VK_ESCAPE: clear_chips(s); return 0;
                case VK_F5: submit(s); return 0;
                case VK_DELETE: if (s.sel >= 0) add_chip(s, { Filter::Kind::DirOut, sel_dir(s) }, "dir"); return 0;
                case 'C':
                    if (ctrl && shift) { copy_text(h, s.compiled); s.status = L"copied the query"; }
                    else if (ctrl && s.sel >= 0) { copy_text(h, sel_path(s)); s.status = L"copied  " + sel_path(s); }
                    break;
                case 'L': case 'F':
                    if (ctrl) { SetFocus(s.edit); SendMessageW(s.edit, EM_SETSEL, 0, -1); }
                    break;
                case 'T':
                    if (ctrl || GetFocus() == h) {
                        s.topmost = !s.topmost;
                        SetWindowPos(h, s.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                        ini_save(s);
                    }
                    break;
                default: return 0;
            }
            ensure_visible(s);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mmi = (MINMAXINFO*)lp;
            mmi->ptMinTrackSize = { px(s, 720), px(s, 460) };
            return 0;
        }
        case WM_DPICHANGED: {
            s.dpi = HIWORD(wp);
            make_fonts(s);
            const RECT* r = (const RECT*)lp;
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_DESTROY:
            ini_save(s);
            AcquireSRWLockExclusive(&s.lock);
            s.quit = true;
            ReleaseSRWLockExclusive(&s.lock);
            WakeConditionVariable(&s.cv);
            ++s.gen;   // any pass in flight stops at its next page
            if (s.thread) WaitForSingleObject(s.thread, 5000);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(h, m, wp, lp);
    }
}

}  // namespace

int run_gui(const Opts& o) {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    using PFN_SetCtx = BOOL(WINAPI*)(HANDLE);
    if (auto p = (PFN_SetCtx)GetProcAddress(u32, "SetProcessDpiAwarenessContext")) p((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */);
    else SetProcessDPIAware();

    static Gui state;
    g = &state;
    state.opts = o;
    if (!o.depth_set) state.opts.depth = 2;   // the rail is one column: keep every facet above the fold
    if (!o.top_set) state.opts.top = 8;
    state.query = o.query;
    state.sort = o.sort;
    state.ascending = o.ascending;
    state.ini = o.ini.empty() ? exe_dir() + L"facet.ini" : widen(o.ini);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = 0, hgt = 0;
    ini_load(state, x, y, w, hgt);
    for (auto& d : o.exclude) { Chip c; c.f = { Filter::Kind::DirOut, d }; c.group = "dir"; c.label = chip_label(c.f); state.chips.push_back(c); }
    for (auto& d : o.include) { Chip c; c.f = { Filter::Kind::DirIn, d }; c.group = "dir"; c.label = chip_label(c.f); state.chips.push_back(c); }
    for (auto& e : o.ext) { Chip c; c.f = { Filter::Kind::ExtIn, e }; c.group = "ext"; c.label = chip_label(c.f); state.chips.push_back(c); }
    for (auto& c : state.chips) c.label = chip_label(c.f);
    const HINSTANCE hinst = GetModuleHandleW(nullptr);
    state.icon_big = (HICON)LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    state.icon_small = (HICON)LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    if (!state.icon_big) state.icon_big = make_icon(32);      // no icon resource: draw it
    if (!state.icon_small) state.icon_small = make_icon(16);

    WNDCLASSW wc{};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"facetGui";
    wc.hIcon = state.icon_big ? state.icon_big : LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    RegisterClassW(&wc);

    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int sdpi = 96;   // the window is sized in physical pixels: scale the defaults by the system DPI
    {
        using PFN_SysDpi = UINT(WINAPI*)();
        if (auto p = (PFN_SysDpi)GetProcAddress(u32, "GetDpiForSystem")) sdpi = (int)p();
    }
    const int defW = std::min(MulDiv(1240, sdpi, 96), sw - 80), defH = std::min(MulDiv(800, sdpi, 96), sh - 80);
    if (w < MulDiv(720, sdpi, 96) || hgt < MulDiv(460, sdpi, 96) || w > sw || hgt > sh) { w = defW; hgt = defH; x = (sw - w) / 2; y = (sh - hgt) / 2; }    if (x < -w + 100 || x > sw - 100 || y < 0 || y > sh - 100) { x = (sw - w) / 2; y = (sh - hgt) / 2; }
    // WS_CLIPCHILDREN: the parent's double-buffered paint must never wipe the query box
    state.hwnd = CreateWindowExW(state.topmost ? WS_EX_TOPMOST : 0, wc.lpszClassName, L"facet", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                 x, y, w, hgt, nullptr, nullptr, wc.hInstance, nullptr);
    if (!state.hwnd) return 1;
    if (state.icon_small) SendMessageW(state.hwnd, WM_SETICON, ICON_SMALL, (LPARAM)state.icon_small);
    if (state.icon_big) SendMessageW(state.hwnd, WM_SETICON, ICON_BIG, (LPARAM)state.icon_big);
    {
        using PFN_GetDpi = UINT(WINAPI*)(HWND);
        if (auto p = (PFN_GetDpi)GetProcAddress(u32, "GetDpiForWindow")) state.dpi = (int)p(state.hwnd);
    }
    make_fonts(state);
    {
        RECT rc;
        GetClientRect(state.hwnd, &rc);
        layout(state, rc);
    }
    ShowWindow(state.hwnd, SW_SHOW);
    SetFocus(state.edit);
    {
        const LRESULT len = SendMessageW(state.edit, WM_GETTEXTLENGTH, 0, 0);   // caret at the end, not at 0
        SendMessageW(state.edit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    }
    state.thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    submit(state);
    update_title(state);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (state.thread) CloseHandle(state.thread);
    if (state.edit_brush) DeleteObject(state.edit_brush);
    if (state.icon_big) DestroyIcon(state.icon_big);
    if (state.icon_small) DestroyIcon(state.icon_small);
    return 0;
}

// --make-icon FILE.ico: the runtime icon at every size Windows asks for, as a plain .ico
// (ICONDIR + one 32-bpp DIB per size with an all-opaque AND mask). The .rc embeds the file.
int write_icon_file(const std::string& path) {
    const int sizes[] = { 16, 20, 24, 32, 40, 48, 64, 256 };
    const uint16_t n = (uint16_t)std::size(sizes);
    std::vector<uint8_t> out;
    auto w16 = [&](uint16_t v) { out.push_back((uint8_t)(v & 0xFF)); out.push_back((uint8_t)(v >> 8)); };
    w16(0);   // reserved
    w16(1);   // type: icon
    w16(n);
    const size_t dir_at = out.size();
    out.resize(dir_at + 16u * n);
    for (uint16_t i = 0; i < n; ++i) {
        const int sz = sizes[i];
        std::vector<uint32_t> px;
        draw_icon_pixels(sz, px);
        const size_t start = out.size();
        const uint32_t mask_stride = (((uint32_t)sz + 31) / 32) * 4;
        BITMAPINFOHEADER bih{};
        bih.biSize = sizeof bih;
        bih.biWidth = sz;
        bih.biHeight = sz * 2;   // XOR + AND
        bih.biPlanes = 1;
        bih.biBitCount = 32;
        bih.biSizeImage = (DWORD)((uint32_t)sz * (uint32_t)sz * 4 + mask_stride * (uint32_t)sz);
        const auto* hb = reinterpret_cast<const uint8_t*>(&bih);
        out.insert(out.end(), hb, hb + sizeof bih);
        for (int y = sz - 1; y >= 0; --y) {   // XOR bitmap: bottom-up rows of BGRA
            const auto* row = reinterpret_cast<const uint8_t*>(px.data() + (size_t)y * (size_t)sz);
            out.insert(out.end(), row, row + (size_t)sz * 4);
        }
        out.insert(out.end(), (size_t)mask_stride * (size_t)sz, (uint8_t)0);   // AND mask: opaque
        const uint32_t bytes = (uint32_t)(out.size() - start), off = (uint32_t)start;
        uint8_t* e = out.data() + dir_at + 16u * i;
        e[0] = (uint8_t)(sz == 256 ? 0 : sz);
        e[1] = e[0];
        e[2] = 0;   // colours (0 = no palette)
        e[3] = 0;   // reserved
        e[4] = 1; e[5] = 0;    // planes
        e[6] = 32; e[7] = 0;   // bits per pixel
        memcpy(e + 8, &bytes, 4);
        memcpy(e + 12, &off, 4);
    }
    FILE* f = _wfopen(widen(path).c_str(), L"wb");
    if (!f) {
        fprintf(stderr, "facet: cannot write %s\n", path.c_str());
        return 1;
    }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    printf("facet: wrote %s (%zu bytes, %u sizes)\n", path.c_str(), out.size(), (unsigned)n);
    return 0;
}

// --shortcut [startmenu|desktop]: a .lnk to the window (facetw.exe --gui), so "facet" is one
// Start-Menu keystroke away and can be pinned to the taskbar from there.
int make_shortcut(const std::string& where) {
    wchar_t self_buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, self_buf, (DWORD)std::size(self_buf));
    const std::wstring self(self_buf, n);
    const std::wstring dir = exe_dir();
    std::wstring target = dir + L"facetw.exe";
    if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) target = self;
    const bool desktop = where == "desktop";
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(desktop ? FOLDERID_Desktop : FOLDERID_Programs, 0, nullptr, &folder))) {
        fprintf(stderr, "facet: cannot resolve the %s folder\n", desktop ? "desktop" : "Start Menu");
        return 1;
    }
    const std::wstring lnk = std::wstring(folder) + L"\\facet.lnk";
    CoTaskMemFree(folder);
    const HRESULT hi = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW* sl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl));
    if (SUCCEEDED(hr)) {
        sl->SetPath(target.c_str());
        sl->SetArguments(L"--gui");
        sl->SetWorkingDirectory(dir.c_str());
        sl->SetIconLocation(target.c_str(), 0);
        sl->SetDescription(L"facet - pivot filtering over Everything's index");
        IPersistFile* pf = nullptr;
        hr = sl->QueryInterface(IID_PPV_ARGS(&pf));
        if (SUCCEEDED(hr)) {
            hr = pf->Save(lnk.c_str(), TRUE);
            pf->Release();
        }
        sl->Release();
    }
    if (SUCCEEDED(hi)) CoUninitialize();
    if (FAILED(hr)) {
        fprintf(stderr, "facet: could not write %s (0x%08lx)\n", narrow(lnk).c_str(), (unsigned long)hr);
        return 1;
    }
    printf("facet: shortcut written  %s  ->  %s --gui\n  %s\n", narrow(lnk).c_str(), narrow(target).c_str(),
           desktop ? "double-click it on the desktop" : "type  facet  in the Start Menu; right-click it there to pin it to the taskbar");
    return 0;
}

}  // namespace facet
