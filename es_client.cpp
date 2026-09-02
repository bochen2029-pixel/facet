// facet · es_client.cpp — Everything over WM_COPYDATA: QUERY2 out, LIST2 in, pages streamed
// into a sink. No SDK DLL, no process handles: FindWindow + SendMessage, exactly what es.exe does.
#include "es_client.h"
#include "everything_ipc.h"

#include <cstring>

namespace facet {

// ---------------------------------------------------------------- OS glue
std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)std::max(n, 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)std::max(n, 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
uint64_t now_filetime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}
double now_ms() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)freq.QuadPart;
}
static bool to_local(uint64_t ft, SYSTEMTIME& lt) {
    if (ft == kUnknown64 || ft == 0) return false;
    FILETIME f{ (DWORD)(ft & 0xFFFFFFFFu), (DWORD)(ft >> 32) };
    SYSTEMTIME ut;
    if (!FileTimeToSystemTime(&f, &ut)) return false;
    return SystemTimeToTzSpecificLocalTime(nullptr, &ut, &lt) != 0;
}
std::string fmt_filetime(uint64_t ft, bool with_seconds) {
    SYSTEMTIME t;
    if (!to_local(ft, t)) return "-";
    return with_seconds ? ssprintf("%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond)
                        : ssprintf("%04d-%02d-%02d %02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
}
std::string fmt_filetime_iso(uint64_t ft) {
    SYSTEMTIME t;
    if (!to_local(ft, t)) return "";
    return ssprintf("%04d-%02d-%02dT%02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}
std::string fmt_local_date(uint64_t ft) {
    SYSTEMTIME t;
    if (!to_local(ft, t)) return "";
    return ssprintf("%04d-%02d-%02d", t.wYear, t.wMonth, t.wDay);
}
std::string fmt_local_time(uint64_t ft) {
    SYSTEMTIME t;
    if (!to_local(ft, t)) return "";
    return ssprintf("%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
}
uint64_t local_midnight(uint64_t ft) {
    SYSTEMTIME lt;
    if (!to_local(ft, lt)) return 0;
    lt.wHour = lt.wMinute = lt.wSecond = lt.wMilliseconds = 0;
    SYSTEMTIME ut;
    FILETIME f;
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &lt, &ut) || !SystemTimeToFileTime(&ut, &f)) return 0;
    return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime;
}

std::string EsInfo::version() const { return ssprintf("%d.%d.%d.%d", major, minor, revision, build); }

uint32_t ipc_sort(SortKey k, bool asc) {
    switch (k) {
        case SortKey::Name: return asc ? ipc::NameAsc : ipc::NameDesc;
        case SortKey::Path: return asc ? ipc::PathAsc : ipc::PathDesc;
        case SortKey::Size: return asc ? ipc::SizeAsc : ipc::SizeDesc;
        case SortKey::Ext: return asc ? ipc::ExtAsc : ipc::ExtDesc;
        case SortKey::Created: return asc ? ipc::CreatedAsc : ipc::CreatedDesc;
        case SortKey::Recent: return asc ? ipc::RecentAsc : ipc::RecentDesc;
        default: return asc ? ipc::ModifiedAsc : ipc::ModifiedDesc;
    }
}

// ---------------------------------------------------------------- reply parsing
bool parse_list2(const void* data, size_t cb, std::vector<EsItem>& out, EsPage& page) {
    out.clear();
    page = EsPage{};
    if (!data || cb < sizeof(ipc::List2)) return false;
    const auto* base = static_cast<const uint8_t*>(data);
    ipc::List2 list;
    memcpy(&list, base, sizeof list);
    page.total = list.totitems;
    page.offset = list.offset;
    page.count = list.numitems;
    if ((uint64_t)list.numitems * sizeof(ipc::Item2) + sizeof(ipc::List2) > cb) return false;
    const DWORD rf = list.request_flags;
    out.reserve(list.numitems);
    for (DWORD i = 0; i < list.numitems; ++i) {
        ipc::Item2 item;
        memcpy(&item, base + sizeof(ipc::List2) + (size_t)i * sizeof(ipc::Item2), sizeof item);
        size_t off = item.data_offset;
        if (off > cb) return false;
        EsItem it;
        it.folder = (item.flags & ipc::kItemFolder) != 0;
        it.drive = (item.flags & ipc::kItemDrive) != 0;
        auto str = [&](const wchar_t*& s, uint32_t& n) -> bool {
            if (off + 4 > cb) return false;
            uint32_t len;
            memcpy(&len, base + off, 4);
            off += 4;
            const size_t bytes = ((size_t)len + 1) * sizeof(wchar_t);
            if (off + bytes > cb) return false;
            s = reinterpret_cast<const wchar_t*>(base + off);
            n = len;
            off += bytes;
            return true;
        };
        auto u64 = [&](uint64_t& v) -> bool {
            if (off + 8 > cb) return false;
            memcpy(&v, base + off, 8);
            off += 8;
            return true;
        };
        auto skip = [&](size_t n) -> bool {
            if (off + n > cb) return false;
            off += n;
            return true;
        };
        const wchar_t* ds = nullptr;
        uint32_t dl = 0;
        uint64_t dv = 0;
        if ((rf & ipc::kReqName) && !str(it.name, it.name_len)) return false;
        if ((rf & ipc::kReqPath) && !str(it.path, it.path_len)) return false;
        if ((rf & ipc::kReqFullPath) && !str(ds, dl)) return false;
        if ((rf & ipc::kReqExt) && !str(ds, dl)) return false;
        if (rf & ipc::kReqSize) {
            if (!u64(it.size)) return false;
            if (it.size >= (1ull << 63)) it.size = kUnknown64;
        }
        if ((rf & ipc::kReqCreated) && !u64(dv)) return false;
        if (rf & ipc::kReqModified) {
            if (!u64(it.mtime)) return false;
            if (it.mtime == 0 || it.mtime >= (1ull << 63)) it.mtime = kUnknown64;
        }
        if ((rf & ipc::kReqAccessed) && !skip(8)) return false;
        if ((rf & ipc::kReqAttr) && !skip(4)) return false;
        if ((rf & ipc::kReqFileListName) && !str(ds, dl)) return false;
        if ((rf & ipc::kReqRunCount) && !skip(4)) return false;
        if ((rf & ipc::kReqDateRun) && !skip(8)) return false;
        if ((rf & ipc::kReqRecent) && !skip(8)) return false;
        if ((rf & ipc::kReqHlName) && !str(ds, dl)) return false;
        if ((rf & ipc::kReqHlPath) && !str(ds, dl)) return false;
        if ((rf & ipc::kReqHlFull) && !str(ds, dl)) return false;
        out.push_back(it);
    }
    return true;
}

// ---------------------------------------------------------------- finding and starting Everything
namespace {

bool file_exists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring env_str(const wchar_t* name) {
    wchar_t buf[4096];
    const DWORD n = GetEnvironmentVariableW(name, buf, (DWORD)std::size(buf));
    return (n && n < std::size(buf)) ? std::wstring(buf, n) : std::wstring();
}

// Everything 1.5 alpha runs as a named instance; its IPC window carries the instance name
HWND find_ipc_window() {
    HWND h = FindWindowW(ipc::kWndClass, nullptr);
    if (!h) h = FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION_(1.5a)", nullptr);
    return h;
}

bool launch_everything(const std::wstring& exe, std::string* err) {
    std::wstring cmd = L"\"" + exe + L"\" -startup";     // -startup: index in the tray, open no window
    const size_t slash = exe.find_last_of(L'\\');
    const std::wstring dir = slash == std::wstring::npos ? L"." : exe.substr(0, slash);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf2 = buf;   // CreateProcess may edit its command line; the retry gets a fresh copy
    // CREATE_BREAKAWAY_FROM_JOB: outlive an agent harness's job object; fall back when the job forbids it
    BOOL ok = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB, nullptr, dir.c_str(), &si, &pi);
    if (!ok && GetLastError() == ERROR_ACCESS_DENIED)
        ok = CreateProcessW(exe.c_str(), buf2.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi);
    if (!ok) {        if (err) *err = ssprintf("could not start %s (error %lu)", narrow(exe).c_str(), (unsigned long)GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace

const char* everything_install_hint() {
    return "Everything is not installed on this machine (no Everything.exe in the registry, Program Files,\n"
           "  LocalAppData\\Programs, scoop, chocolatey or PATH). facet rides on Everything's index and cannot\n"
           "  work without it. Get it, free (~2 MB): https://www.voidtools.com/downloads/  -> Everything 1.4\n"
           "  installer (x64) or the portable zip. Install it, run it once so it indexes the drives (about a\n"
           "  minute), then retry. Or point facet at an Everything.exe: --everything-exe PATH or\n"
           "  FACET_EVERYTHING=PATH.";
}

std::wstring find_everything_exe(const std::wstring& override_path) {
    if (!override_path.empty()) return file_exists(override_path) ? override_path : L"";
    const std::wstring ev = env_str(L"FACET_EVERYTHING");
    if (!ev.empty() && file_exists(ev)) return ev;
    std::vector<std::wstring> c;
    auto reg = [&](HKEY root, const wchar_t* key, const wchar_t* value, REGSAM view) {
        HKEY h;
        if (RegOpenKeyExW(root, key, 0, KEY_READ | view, &h) != ERROR_SUCCESS) return;
        wchar_t buf[2048];
        DWORD cb = sizeof buf, type = 0;
        if (RegQueryValueExW(h, value, nullptr, &type, (BYTE*)buf, &cb) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
            std::wstring v(buf, cb / sizeof(wchar_t));
            while (!v.empty() && v.back() == 0) v.pop_back();
            if (type == REG_EXPAND_SZ) {
                wchar_t ex[2048];
                if (ExpandEnvironmentStringsW(v.c_str(), ex, (DWORD)std::size(ex))) v = ex;
            }
            if (!v.empty() && v[0] == L'"') {
                const size_t q = v.find(L'"', 1);
                if (q != std::wstring::npos) v = v.substr(1, q - 1);
            }
            const size_t comma = v.rfind(L',');                 // DisplayIcon may be "path,0"
            if (comma != std::wstring::npos && v.size() - comma <= 4) v.resize(comma);
            if (v.empty()) return;
            const bool is_exe = v.size() > 4 && _wcsicmp(v.c_str() + v.size() - 4, L".exe") == 0;
            if (is_exe) c.push_back(v);
            else {
                if (v.back() != L'\\') v += L'\\';
                c.push_back(v + L"Everything.exe");
                c.push_back(v + L"Everything64.exe");
            }
        }
        RegCloseKey(h);
    };
    for (REGSAM view : { (REGSAM)KEY_WOW64_64KEY, (REGSAM)KEY_WOW64_32KEY }) {
        for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER }) {
            reg(root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\Everything.exe", L"", view);
            for (const wchar_t* k : { L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Everything",
                                      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Everything 1.5a",
                                      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Everything 1.5" }) {
                reg(root, k, L"DisplayIcon", view);
                reg(root, k, L"InstallLocation", view);
            }
        }
    }
    const std::wstring roots[] = {
        env_str(L"ProgramFiles"), env_str(L"ProgramFiles(x86)"), env_str(L"ProgramW6432"),
        env_str(L"LOCALAPPDATA") + L"\\Programs", env_str(L"ProgramData") + L"\\chocolatey\\lib\\everything\\tools",
        env_str(L"USERPROFILE") + L"\\scoop\\apps\\everything\\current",
    };
    for (const auto& r : roots) {
        if (r.empty() || r[0] == L'\\') continue;
        for (const wchar_t* sub : { L"\\Everything\\Everything.exe", L"\\Everything\\Everything64.exe",
                                    L"\\Everything 1.5a\\Everything64.exe", L"\\Everything 1.5a\\Everything.exe",
                                    L"\\Everything.exe", L"\\Everything64.exe" })
            c.push_back(r + sub);
    }
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"Everything.exe", nullptr, MAX_PATH, found, nullptr)) c.push_back(found);
    for (const auto& p : c)
        if (file_exists(p)) return p;
    return L"";
}

// ---------------------------------------------------------------- the client
struct Everything::Impl {
    HWND target = nullptr;
    HWND reply = nullptr;
    EsInfo info;
    bool info_ok = false;
    bool start_tried = false;   // one launch attempt per Everything object
    bool db_waited = false;
    const EsSink* sink = nullptr;
    bool got = false;      // a reply arrived for the outstanding query
    bool more = true;      // the sink wants more pages
    bool bad = false;      // the reply did not parse
    EsPage page;
    std::vector<EsItem> items;
};

namespace {

constexpr DWORD kReplyId = 0xFACE7001;
constexpr const wchar_t* kReplyClass = L"facet.everything.reply";

LRESULT CALLBACK reply_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCCREATE) {
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        return TRUE;
    }
    if (m == WM_COPYDATA) {
        auto* p = (Everything::Impl*)GetWindowLongPtrW(h, GWLP_USERDATA);
        auto* cds = (COPYDATASTRUCT*)lp;
        if (p && cds && cds->dwData == kReplyId) {
            p->got = true;
            if (!parse_list2(cds->lpData, cds->cbData, p->items, p->page)) {
                p->bad = true;
                return TRUE;
            }
            if (p->sink && p->more) p->more = (*p->sink)(p->page, p->items.data(), (uint32_t)p->items.size());
            p->items.clear();
            return TRUE;
        }
    }
    return DefWindowProcW(h, m, wp, lp);
}

bool ensure_reply_window(Everything::Impl* p, std::string* err) {
    if (p->reply) return true;
    const HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = reply_proc;
    wc.hInstance = hi;
    wc.lpszClassName = kReplyClass;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        if (err) *err = "cannot register the IPC reply window class";
        return false;
    }
    p->reply = CreateWindowExW(0, kReplyClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hi, p);
    if (!p->reply) {
        if (err) *err = ssprintf("cannot create the IPC reply window (error %lu)", (unsigned long)GetLastError());
        return false;
    }
    return true;
}

LRESULT probe(HWND target, WPARAM cmd, LPARAM lp) {
    DWORD_PTR r = 0;
    if (!SendMessageTimeoutW(target, ipc::kWmIpc, cmd, lp, SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &r)) return -1;
    return (LRESULT)r;
}

bool send_query(Everything::Impl* p, const std::wstring& q, uint32_t sort, uint32_t flags, uint32_t offset,
                uint32_t max, uint32_t req, uint32_t timeout_ms, std::string* err) {
    std::vector<uint8_t> buf(sizeof(ipc::Query2) + (q.size() + 1) * sizeof(wchar_t));
    ipc::Query2 hdr{};
    hdr.reply_hwnd = (DWORD)(uintptr_t)p->reply;
    hdr.reply_copydata_message = kReplyId;
    hdr.search_flags = flags;
    hdr.offset = offset;
    hdr.max_results = max;
    hdr.request_flags = req;
    hdr.sort_type = sort;
    memcpy(buf.data(), &hdr, sizeof hdr);
    memcpy(buf.data() + sizeof hdr, q.c_str(), (q.size() + 1) * sizeof(wchar_t));
    COPYDATASTRUCT cds{};
    cds.dwData = ipc::kCopyDataQuery2W;
    cds.cbData = (DWORD)buf.size();
    cds.lpData = buf.data();
    p->got = false;
    p->bad = false;
    p->more = true;
    p->page = EsPage{};
    DWORD_PTR res = 0;
    // SMTO_NORMAL: this thread keeps servicing sent messages while it waits, so the reply
    // (which Everything SENDs back as WM_COPYDATA) can land re-entrantly during this call.
    if (!SendMessageTimeoutW(p->target, WM_COPYDATA, (WPARAM)p->reply, (LPARAM)&cds,
                             SMTO_NORMAL | SMTO_ABORTIFHUNG, timeout_ms, &res)) {
        if (err) *err = "Everything did not answer the query (timeout or hung)";
        return false;
    }
    if (!res) {
        if (err) *err = "Everything rejected the query (database still loading?)";
        return false;
    }
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (!p->got) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            if (err) *err = "timed out waiting for Everything's reply";
            return false;
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, (DWORD)(deadline - now), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
            if (p->got) break;
        }
    }
    if (p->bad) {
        if (err) *err = "malformed reply from Everything";
        return false;
    }
    return true;
}

}  // namespace

Everything::Everything() : p_(new Impl) {}
Everything::~Everything() {
    if (p_->reply) DestroyWindow(p_->reply);
    delete p_;
}
const EsInfo& Everything::info() const { return p_->info; }

bool Everything::running() const { return find_ipc_window() != nullptr; }

bool Everything::connect(std::string* err) {
    HWND h = find_ipc_window();
    if (!h && launch.allow_start && !p_->start_tried) {
        p_->start_tried = true;
        const std::wstring exe = find_everything_exe(launch.exe_override);
        if (exe.empty()) {
            if (err) {
                *err = launch.exe_override.empty() ? std::string(everything_install_hint())
                                                   : "no Everything.exe at " + narrow(launch.exe_override) + "\n  " + everything_install_hint();
            }
            p_->target = nullptr;
            return false;
        }
        if (on_note) on_note("Everything is not running - starting " + narrow(exe) + " -startup (tray only)");
        if (!launch_everything(exe, err)) return false;
        const ULONGLONG t0 = GetTickCount64();
        while (!(h = find_ipc_window()) && GetTickCount64() - t0 < launch.wait_ms) Sleep(100);
        if (!h) {
            if (err) *err = ssprintf("started Everything but its IPC window did not appear within %u s", launch.wait_ms / 1000);
            return false;
        }
        while (probe(h, ipc::kIsDbLoaded, 0) != 1 && GetTickCount64() - t0 < launch.wait_ms) Sleep(200);
        p_->db_waited = true;
        if (on_note) on_note(ssprintf("Everything is up - database loaded after %.1f s", (double)(GetTickCount64() - t0) / 1000.0));
    }
    if (!h) {
        if (err) {
            const std::wstring exe = find_everything_exe(launch.exe_override);
            *err = "Everything is not running (IPC window not found)";
            if (exe.empty()) *err += "\n  " + std::string(everything_install_hint());
            else *err += "\n  start it: \"" + narrow(exe) + "\"" + (launch.allow_start ? "" : "   (facet starts it itself unless --no-start)");
        }
        p_->target = nullptr;
        p_->info_ok = false;
        return false;
    }
    if (h != p_->target || !p_->info_ok) {
        p_->target = h;
        EsInfo i;
        i.major = (int)probe(h, ipc::kGetMajorVersion, 0);
        i.minor = (int)probe(h, ipc::kGetMinorVersion, 0);
        i.revision = (int)probe(h, ipc::kGetRevision, 0);
        i.build = (int)probe(h, ipc::kGetBuildNumber, 0);
        i.db_loaded = probe(h, ipc::kIsDbLoaded, 0) == 1;
        i.size_indexed = probe(h, ipc::kIsFileInfoIndexed, ipc::kFileInfoSize) == 1;
        i.modified_indexed = probe(h, ipc::kIsFileInfoIndexed, ipc::kFileInfoDateModified) == 1;
        i.created_indexed = probe(h, ipc::kIsFileInfoIndexed, ipc::kFileInfoDateCreated) == 1;
        p_->info = i;
        p_->info_ok = i.major > 0;
    }
    if (!p_->info.db_loaded && !p_->db_waited) {
        // a freshly started Everything answers version probes before its database is in memory
        p_->db_waited = true;
        const ULONGLONG t0 = GetTickCount64();
        while (probe(h, ipc::kIsDbLoaded, 0) != 1 && GetTickCount64() - t0 < 8000) Sleep(200);
        p_->info.db_loaded = probe(h, ipc::kIsDbLoaded, 0) == 1;
    }
    return true;
}

bool Everything::fast_sort(uint32_t s) const {
    return p_->target && probe(p_->target, ipc::kIsFastSort, (LPARAM)s) == 1;
}

bool Everything::query(const std::wstring& q, uint32_t sort, uint32_t flags, uint32_t max_items,
                       uint32_t page_size, uint32_t req, const EsSink& sink, std::string* err) {
    if (!connect(err) || !ensure_reply_window(p_, err)) return false;
    if (page_size == 0) page_size = 65536;
    p_->sink = &sink;
    uint32_t offset = 0;
    bool ok = true;
    for (;;) {
        uint32_t want = page_size;
        if (max_items) {
            if (offset >= max_items) break;
            want = std::min(want, max_items - offset);
        }
        if (!send_query(p_, q, sort, flags, offset, want, req, timeout_ms, err)) {
            ok = false;
            break;
        }
        const uint32_t got = p_->page.count;
        offset += got;
        if (!p_->more || got == 0 || offset >= p_->page.total) break;
    }
    p_->sink = nullptr;
    return ok;
}

bool Everything::count(const std::wstring& q, uint32_t flags, uint32_t* total, std::string* err) {
    if (!connect(err) || !ensure_reply_window(p_, err)) return false;
    p_->sink = nullptr;
    if (!send_query(p_, q, ipc::NameAsc, flags, 0, 0, ipc::kReqName, timeout_ms, err)) return false;
    if (total) *total = p_->page.total;
    return true;
}

}  // namespace facet
