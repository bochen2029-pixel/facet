// facet · scan.cpp — everywhere as a child: two pipes, two reader threads, one JSONL parse.
#include "scan.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app_util.h"
#include "es_client.h"

namespace facet {

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
std::wstring self_dir() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    std::wstring p(buf, n);
    const size_t s = p.find_last_of(L'\\');
    return s == std::wstring::npos ? L"" : p.substr(0, s + 1);
}

// JSON string body → text (the subset everywhere emits: \\ \" \/ \n \r \t \uXXXX)
std::string json_unescape(std::string_view s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
        const char e = s[++i];
        switch (e) {
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'u': {
                if (i + 4 < s.size()) {
                    unsigned cp = (unsigned)strtoul(std::string(s.substr(i + 1, 4)).c_str(), nullptr, 16);
                    i += 4;
                    if (cp < 0x80) o += (char)cp;
                    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
                    else { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
                }
                break;
            }
            default: o += e;   // \\ \" \/
        }
    }
    return o;
}

// {"type":"count","path":"...","count":N}
bool parse_count_record(std::string_view line, std::string& path, uint32_t& count) {
    if (line.find("\"type\":\"count\"") == std::string_view::npos) return false;
    const size_t p = line.find("\"path\":\"");
    if (p == std::string_view::npos) return false;
    size_t i = p + 8, start = i;
    while (i < line.size() && line[i] != '"') { if (line[i] == '\\') ++i; ++i; }
    if (i >= line.size()) return false;
    path = json_unescape(line.substr(start, i - start));
    const size_t c = line.find("\"count\":", i);
    if (c == std::string_view::npos) return false;
    count = (uint32_t)strtoul(std::string(line.substr(c + 8, 12)).c_str(), nullptr, 10);
    return true;
}

}  // namespace

std::wstring find_everywhere_exe(const std::wstring& override_path) {
    if (!override_path.empty()) return file_exists(override_path) ? override_path : L"";
    const std::wstring ev = env_str(L"FACET_EVERYWHERE");
    if (!ev.empty() && file_exists(ev)) return ev;
    const std::wstring c[] = {
        self_dir() + L"everywhere.exe",
        L"C:\\everywhere\\build\\Release\\everywhere.exe",
        L"C:\\everywhere\\everywhere.exe",
        env_str(L"ProgramFiles") + L"\\everywhere\\everywhere.exe",
        env_str(L"LOCALAPPDATA") + L"\\Programs\\everywhere\\everywhere.exe",
    };
    for (const auto& p : c)
        if (p.size() > 16 && file_exists(p)) return p;
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"everywhere.exe", nullptr, MAX_PATH, found, nullptr)) return found;
    return L"";
}

const char* everywhere_install_hint() {
    return "everywhere (the GPU content grep) is not available: no everywhere.exe next to facet, in\n"
           "  C:\\everywhere\\build\\Release, in Program Files, or on PATH. Build it from C:\\everywhere (see its\n"
           "  README: cmake + CUDA), or point facet at one: --everywhere-exe PATH or FACET_EVERYWHERE=PATH.\n"
           "  Without it facet still does names, dates, sizes and tapes; only \"contains\" needs it.";
}

std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring o = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { bs++; continue; }
        if (c == L'"') { o.append(bs * 2 + 1, L'\\'); o += L'"'; bs = 0; continue; }
        o.append(bs, L'\\');
        bs = 0;
        o += c;
    }
    o.append(bs * 2, L'\\');
    o += L'"';
    return o;
}

struct ContentScan::Impl {
    HANDLE proc = nullptr, out_thread = nullptr, err_thread = nullptr;
    HANDLE in_w = nullptr, out_r = nullptr, err_r = nullptr;
    std::string out, errtxt;
    std::wstring cmd;
    uint64_t fed = 0;
    double t0 = 0;
    bool started = false, in_open = false;
};

namespace {
struct ReadJob { HANDLE h; std::string* into; };
DWORD WINAPI pump(LPVOID p) {
    auto* j = (ReadJob*)p;
    char buf[1 << 16];
    DWORD n = 0;
    while (ReadFile(j->h, buf, sizeof buf, &n, nullptr) && n) j->into->append(buf, n);
    delete j;
    return 0;
}
}  // namespace

ContentScan::ContentScan() : p_(new Impl) {}
ContentScan::~ContentScan() {
    kill();
    delete p_;
}
uint64_t ContentScan::fed() const { return p_->fed; }
std::wstring ContentScan::command() const { return p_->cmd; }

bool ContentScan::start(const std::wstring& exe, const std::wstring& phrase, bool literal, bool icase, std::string* err) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE in_r = nullptr, out_w = nullptr, err_w = nullptr;
    if (!CreatePipe(&in_r, &p_->in_w, &sa, 1 << 16) || !CreatePipe(&p_->out_r, &out_w, &sa, 1 << 16) || !CreatePipe(&p_->err_r, &err_w, &sa, 1 << 14)) {
        if (err) *err = "cannot create the pipes to everywhere";
        return false;
    }
    SetHandleInformation(p_->in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(p_->out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(p_->err_r, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd = quote_arg(exe) + L" --files-from -";
    if (literal) cmd += L" -F";
    if (icase) cmd += L" -i";
    cmd += L" -e " + quote_arg(phrase) + L" --jsonl -c --quiet";
    p_->cmd = cmd;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = err_w;
    PROCESS_INFORMATION pi{};
    const std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));
    const BOOL ok = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi);
    CloseHandle(in_r);
    CloseHandle(out_w);
    CloseHandle(err_w);
    if (!ok) {
        if (err) *err = ssprintf("cannot start everywhere (%s, error %lu)", narrow(exe).c_str(), (unsigned long)GetLastError());
        CloseHandle(p_->in_w); CloseHandle(p_->out_r); CloseHandle(p_->err_r);
        p_->in_w = p_->out_r = p_->err_r = nullptr;
        return false;
    }
    CloseHandle(pi.hThread);
    p_->proc = pi.hProcess;
    p_->out_thread = CreateThread(nullptr, 0, pump, new ReadJob{ p_->out_r, &p_->out }, 0, nullptr);
    p_->err_thread = CreateThread(nullptr, 0, pump, new ReadJob{ p_->err_r, &p_->errtxt }, 0, nullptr);
    p_->started = true;
    p_->in_open = true;
    p_->t0 = now_ms();
    return true;
}

bool ContentScan::feed(std::wstring_view path) {
    if (!p_->in_open) return false;
    std::string line = narrow(path);
    line += '\n';
    const char* d = line.data();
    size_t left = line.size();
    while (left) {
        DWORD w = 0;
        if (!WriteFile(p_->in_w, d, (DWORD)left, &w, nullptr)) return false;
        d += w;
        left -= w;
    }
    p_->fed++;
    return true;
}

void ContentScan::kill() {
    if (p_->in_open) { CloseHandle(p_->in_w); p_->in_w = nullptr; p_->in_open = false; }
    if (p_->proc) {
        if (WaitForSingleObject(p_->proc, 0) == WAIT_TIMEOUT) TerminateProcess(p_->proc, 3);
        CloseHandle(p_->proc);
        p_->proc = nullptr;
    }
    for (HANDLE* t : { &p_->out_thread, &p_->err_thread })
        if (*t) { WaitForSingleObject(*t, 2000); CloseHandle(*t); *t = nullptr; }
    for (HANDLE* h : { &p_->out_r, &p_->err_r })
        if (*h) { CloseHandle(*h); *h = nullptr; }
}

ScanResult ContentScan::finish(const std::function<bool()>& cancelled) {
    ScanResult r;
    r.fed = p_->fed;
    if (!p_->started) { r.err = "scan was not started"; return r; }
    if (p_->in_open) { CloseHandle(p_->in_w); p_->in_w = nullptr; p_->in_open = false; }   // EOF: everywhere finishes
    for (;;) {
        const DWORD w = WaitForSingleObject(p_->proc, 100);
        if (w == WAIT_OBJECT_0) break;
        if (cancelled && cancelled()) {
            TerminateProcess(p_->proc, 3);
            WaitForSingleObject(p_->proc, 2000);
            r.err = "cancelled";
            kill();
            return r;
        }
    }
    DWORD code = 0;
    GetExitCodeProcess(p_->proc, &code);
    r.exit_code = (int)code;
    r.ms = now_ms() - p_->t0;
    kill();   // joins the readers: out / errtxt are complete now
    if (code == 2 || code > 2) {
        std::string e = p_->errtxt;
        while (!e.empty() && (e.back() == '\n' || e.back() == '\r')) e.pop_back();
        r.err = ssprintf("everywhere exited %lu", (unsigned long)code) + (e.empty() ? "" : ": " + e.substr(0, 400));
        return r;
    }
    size_t i = 0;
    const std::string& out = p_->out;
    while (i < out.size()) {
        size_t j = out.find('\n', i);
        if (j == std::string::npos) j = out.size();
        std::string_view line(out.data() + i, j - i);
        i = j + 1;
        std::string path;
        uint32_t count = 0;
        if (!parse_count_record(line, path, count)) continue;
        std::wstring w = widen(path);
        for (auto& c : w)
            if (c == L'/') c = L'\\';
        r.hits.push_back({ std::move(w), count });
        r.matches += count;
    }
    r.ok = true;
    return r;
}

}  // namespace facet
