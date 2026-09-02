// facet · query.h — the compiler: facet picks become Everything search syntax, so the engine does
// the filtering and the printed query is exactly what produced the counts. Pure, header-only.
//   subtree include   path:"C:\dir\"        several → <path:"A\"|path:"B\">
//   subtree exclude   !path:"C:\dir\"
//   extensions        ext:md;txt            exclude → !ext:log
//   any other term    appended verbatim (dm:, size:, file:, folder:, ...)
#pragma once
#include <string>
#include <vector>

namespace facet {

struct Filter {
    enum class Kind { DirIn, DirOut, ExtIn, ExtOut, Term };
    Kind kind = Kind::Term;
    std::wstring value;
};

// "C:/a/b" or "C:\a\b" → "C:\a\b\" — the trailing separator is what makes path: a subtree test
// ("C:\NEW\" cannot match "C:\NEWER\...").
inline std::wstring dir_prefix(std::wstring d) {
    for (auto& c : d)
        if (c == L'/') c = L'\\';
    while (d.size() > 1 && d.back() == L'\\' && d[d.size() - 2] == L'\\') d.pop_back();
    if (d.empty()) return d;
    if (d.back() != L'\\') d += L'\\';
    return d;
}
inline std::wstring dir_term(const std::wstring& d) { return L"path:\"" + dir_prefix(d) + L"\""; }

inline std::wstring join(const std::vector<std::wstring>& v, const wchar_t* sep) {
    std::wstring o;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) o += sep;
        o += v[i];
    }
    return o;
}

inline std::wstring compile(const std::wstring& base, const std::vector<Filter>& fs) {
    std::wstring out = base;
    auto app = [&](const std::wstring& t) {
        if (t.empty()) return;
        if (!out.empty()) out += L' ';
        out += t;
    };
    std::vector<std::wstring> ins, ext_in, ext_out;
    for (const auto& f : fs) {
        switch (f.kind) {
            case Filter::Kind::DirIn: ins.push_back(dir_term(f.value)); break;
            case Filter::Kind::DirOut: app(L"!" + dir_term(f.value)); break;
            case Filter::Kind::ExtIn: ext_in.push_back(f.value); break;
            case Filter::Kind::ExtOut: ext_out.push_back(f.value); break;
            case Filter::Kind::Term: app(f.value); break;
        }
    }
    if (ins.size() == 1) app(ins[0]);
    else if (ins.size() > 1) app(L"<" + join(ins, L"|") + L">");
    if (!ext_in.empty()) app(L"ext:" + join(ext_in, L";"));
    for (const auto& e : ext_out) app(L"!ext:" + e);
    return out;
}

}  // namespace facet
