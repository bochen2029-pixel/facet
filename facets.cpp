// facet · facets.cpp — the fold. add() is O(1) amortized per row (one hash probe on the parent
// path, usually short-circuited by the last-parent cache); finish() propagates subtree totals
// bottom-up in one pass (children always carry higher ids than parents), sorts, and clusters
// the (mtime, dir) events into write bursts.
#include "facets.h"

#include <algorithm>
#include <cwctype>

namespace facet {

namespace {

const char* kSizeLabels[kSizeBuckets] = {
    "folders / no size", "empty (0 B)", "1 B - 4 KB", "4 KB - 64 KB", "64 KB - 1 MB",
    "1 MB - 16 MB", "16 MB - 256 MB", "256 MB - 4 GB", "4 GB and up",
};
const wchar_t* kSizeQueries[kSizeBuckets] = {
    L"folder:", L"size:0", L"size:1..4095", L"size:4096..65535", L"size:65536..1048575",
    L"size:1048576..16777215", L"size:16777216..268435455", L"size:268435456..4294967295",
    L"size:>=4294967296",
};

}  // namespace

Facets::Facets(const FacetConfig& cfg) : cfg_(cfg) {
    if (cfg_.now == 0) cfg_.now = now_filetime();
    nodes.emplace_back();   // the root
    const uint64_t mid = local_midnight(cfg_.now);
    edge_today_ = mid;
    edge_yest_ = mid - kTicksPerDay;
    edge_week_ = mid - 7 * kTicksPerDay;
    edge_month_ = mid - 30 * kTicksPerDay;
    edge_year_ = mid - 365 * kTicksPerDay;
    auto date = [](uint64_t t) { return widen(fmt_local_date(t)); };
    modified.push_back({ "today", L"dm:today", 0, 0 });
    modified.push_back({ "yesterday", L"dm:yesterday", 0, 0 });
    modified.push_back({ "2 - 7 days ago", L"dm:" + date(edge_week_) + L".." + date(edge_yest_ - 1), 0, 0 });
    modified.push_back({ "8 - 30 days ago", L"dm:" + date(edge_month_) + L".." + date(edge_week_ - 1), 0, 0 });
    modified.push_back({ "31 - 365 days ago", L"dm:" + date(edge_year_) + L".." + date(edge_month_ - 1), 0, 0 });
    modified.push_back({ "over a year ago", L"dm:<" + date(edge_year_), 0, 0 });
    modified.push_back({ "in the future", L"", 0, 0 });
    modified.push_back({ "no date", L"", 0, 0 });
    for (int i = 0; i < kSizeBuckets; ++i) sizes.push_back({ kSizeLabels[i], kSizeQueries[i], 0, 0 });
}

uint32_t Facets::dir_node(std::wstring_view p) {
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.remove_suffix(1);
    if (p.empty()) return 0;
    if (std::wstring_view(last_key_) == p) return last_node_;
    auto it = index_.find(p);
    if (it != index_.end()) {
        last_key_.assign(p);
        last_node_ = it->second;
        return it->second;
    }
    const size_t pos = p.find_last_of(L'\\');
    uint32_t parent = 0;
    std::wstring_view leaf = p;
    if (pos != std::wstring_view::npos && pos >= 2) {   // "\\server" stays one component
        parent = dir_node(p.substr(0, pos));
        leaf = p.substr(pos + 1);
    }
    const uint32_t id = (uint32_t)nodes.size();
    DirNode n;
    n.parent = parent;
    n.depth = nodes[parent].depth + 1;
    n.name.assign(leaf);
    nodes.push_back(std::move(n));
    nodes[parent].children.push_back(id);
    index_.emplace(std::wstring(p), id);
    last_key_.assign(p);
    last_node_ = id;
    return id;
}

uint32_t Facets::ext_slot(std::wstring_view e) {
    std::wstring key(e);
    for (auto& c : key) c = (wchar_t)towlower(c);
    auto it = ext_index_.find(std::wstring_view(key));
    if (it != ext_index_.end()) return it->second;
    const uint32_t id = (uint32_t)exts.size();
    exts.push_back({ key, 0, 0 });
    ext_index_.emplace(std::move(key), id);
    return id;
}

int Facets::mod_bucket(uint64_t t) const {
    if (t == kUnknown64) return 7;
    if (t > cfg_.now + 3600 * kTicksPerSec) return 6;
    if (t >= edge_today_) return 0;
    if (t >= edge_yest_) return 1;
    if (t >= edge_week_) return 2;
    if (t >= edge_month_) return 3;
    if (t >= edge_year_) return 4;
    return 5;
}

int Facets::size_bucket(uint64_t s, bool folder) {
    if (folder || s == kUnknown64) return 0;
    if (s == 0) return 1;
    if (s < 4096ull) return 2;
    if (s < 65536ull) return 3;
    if (s < 1048576ull) return 4;
    if (s < 16777216ull) return 5;
    if (s < 268435456ull) return 6;
    if (s < 4294967296ull) return 7;
    return 8;
}

void Facets::add(const EsItem& it) {
    items++;
    if (it.folder) folders++;
    else files++;
    const uint32_t d = dir_node(std::wstring_view(it.path, it.path_len));
    DirNode& n = nodes[d];
    n.self++;
    n.count++;
    uint64_t sz = 0;
    if (!it.folder && it.size != kUnknown64) {
        sz = it.size;
        n.bytes += sz;
        bytes += sz;
    } else {
        unknown_size++;
    }
    if (!it.folder) {
        const std::wstring_view nm(it.name, it.name_len);
        const size_t dot = nm.find_last_of(L'.');
        std::wstring_view e;
        if (dot != std::wstring_view::npos && dot > 0 && dot + 1 < nm.size() && nm.size() - dot - 1 <= 16)
            e = nm.substr(dot + 1);
        ExtStat& es = exts[ext_slot(e)];
        es.count++;
        es.bytes += sz;
    }
    const int mb = mod_bucket(it.mtime);
    modified[(size_t)mb].count++;
    modified[(size_t)mb].bytes += sz;
    if (it.mtime != kUnknown64 && it.mtime + 3600 * kTicksPerSec >= cfg_.now && it.mtime <= cfg_.now + 3600 * kTicksPerSec)
        last_hour++;
    const int sb = size_bucket(it.size, it.folder);
    sizes[(size_t)sb].count++;
    sizes[(size_t)sb].bytes += sz;
    if (it.mtime != kUnknown64) events_.emplace_back(it.mtime, d);
    else unknown_mtime++;
    if (rows.size() < cfg_.keep_rows) {
        Row r;
        r.dir = d;
        r.name_off = (uint32_t)names_.size();
        r.name_len = it.name_len;
        names_.append(it.name, it.name_len);
        r.size = it.size;
        r.mtime = it.mtime;
        r.folder = it.folder;
        rows.push_back(r);
    }
}

void Facets::finish() {
    // subtree totals: every child has a higher id than its parent, so one reverse pass suffices
    for (size_t id = nodes.size(); id-- > 1;) {
        const DirNode& n = nodes[id];
        DirNode& p = nodes[n.parent];
        p.count += n.count;
        p.bytes += n.bytes;
    }
    for (auto& n : nodes)
        std::sort(n.children.begin(), n.children.end(), [&](uint32_t a, uint32_t b) {
            if (nodes[a].count != nodes[b].count) return nodes[a].count > nodes[b].count;
            return nodes[a].name < nodes[b].name;
        });
    std::sort(exts.begin(), exts.end(), [](const ExtStat& a, const ExtStat& b) {
        if (a.count != b.count) return a.count > b.count;
        return a.ext < b.ext;
    });
    ext_index_.clear();   // indices moved; the map is not needed after finish()

    // write bursts: sort every dated item, split where the silence exceeds the gap
    std::sort(events_.begin(), events_.end());
    const uint64_t gap = (uint64_t)cfg_.burst_gap_s * kTicksPerSec;
    std::vector<Burst> top;
    auto consider = [&](const Burst& b) {
        burst_total++;
        if (b.count <= 2) {
            handpaced_bursts++;
            handpaced_files += b.count;
        }
        if (cfg_.top_bursts == 0) return;
        if (top.size() < cfg_.top_bursts || b.count > top.back().count) {
            auto pos = std::upper_bound(top.begin(), top.end(), b, [](const Burst& x, const Burst& y) { return x.count > y.count; });
            top.insert(pos, b);
            if (top.size() > cfg_.top_bursts) top.pop_back();
        }
    };
    size_t i = 0;
    while (i < events_.size()) {
        size_t j = i;
        while (j + 1 < events_.size() && events_[j + 1].first - events_[j].first <= gap) ++j;
        Burst b;
        b.start = events_[i].first;
        b.end = events_[j].first;
        b.count = (uint32_t)(j - i + 1);
        b.first = i;
        consider(b);
        i = j + 1;
    }
    // where each kept burst landed: descend while one child holds >= 90 % of the writes below;
    // where that stops on a split, name the biggest pieces (each descended the same way)
    for (auto& b : top) {
        std::unordered_map<uint32_t, uint32_t> c;
        for (size_t k = b.first; k < b.first + b.count; ++k)
            for (uint32_t n = events_[k].second; n != 0; n = nodes[n].parent) c[n]++;
        c[0] = b.count;
        auto descend = [&](uint32_t n) {
            for (;;) {
                uint32_t best = 0, bc = 0;
                for (uint32_t ch : nodes[n].children) {
                    auto it = c.find(ch);
                    if (it != c.end() && it->second > bc) { bc = it->second; best = ch; }
                }
                if (!best || bc * 10 < c[n] * 9) return n;
                n = best;
            }
        };
        const uint32_t cur = descend(0);
        b.dir = cur;
        b.dir_share = (double)c[cur] / (double)b.count;
        std::vector<std::pair<uint32_t, uint32_t>> kids;   // (count, child)
        for (uint32_t ch : nodes[cur].children) {
            auto it = c.find(ch);
            if (it != c.end()) kids.emplace_back(it->second, ch);
        }
        if (kids.size() >= 2) {
            std::sort(kids.begin(), kids.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
            if (kids.size() > 3) kids.resize(3);
            for (const auto& [cnt, ch] : kids) b.parts.emplace_back(descend(ch), (double)cnt / (double)b.count);
        }
        b.query = widen("dm:" + fmt_filetime_iso(b.start) + ".." + fmt_filetime_iso(b.end));
    }
    bursts = std::move(top);
}

std::wstring Facets::dir_path(uint32_t id) const {
    std::vector<uint32_t> chain;
    for (uint32_t n = id; n != 0 && n < nodes.size(); n = nodes[n].parent) chain.push_back(n);
    std::wstring out;
    for (size_t i = chain.size(); i-- > 0;) {
        out += nodes[chain[i]].name;
        out += L'\\';
    }
    return out;
}

std::wstring_view Facets::row_name(const Row& r) const {
    return std::wstring_view(names_.data() + r.name_off, r.name_len);
}

std::wstring Facets::row_path(const Row& r) const {
    std::wstring p = dir_path(r.dir);
    p.append(row_name(r));
    return p;
}

// ---------------------------------------------------------------- the directory view
uint64_t fold_threshold(const Opts& o, uint64_t items) {
    if (o.min_set) return (uint64_t)o.min_count;
    return std::max<uint64_t>((uint64_t)o.min_count, (items + 99) / 100);
}

std::wstring collapsed_label(const Facets& f, uint32_t& cur, bool full_prefix) {
    std::wstring label = full_prefix ? f.dir_path(cur) : (f.nodes[cur].name + L"\\");
    while (f.nodes[cur].children.size() == 1 && f.nodes[cur].self == 0) {
        cur = f.nodes[cur].children[0];
        label += f.nodes[cur].name + L"\\";
    }
    return label;
}

static void expand_dir(const Facets& f, const Opts& o, uint32_t node, int level, uint64_t thr, std::vector<DirLine>& out) {
    if (level > o.depth) return;
    const DirNode& n = f.nodes[node];
    if (n.children.empty()) return;
    int shown = 0, rest_n = 0;
    uint64_t rest = 0;
    for (uint32_t c : n.children) {
        const uint64_t cnt = f.nodes[c].count;
        if (cnt < thr || shown >= o.top) { rest_n++; rest += cnt; continue; }
        shown++;
        uint32_t cur = c;
        const std::wstring label = collapsed_label(f, cur, false);
        out.push_back({ level, narrow(label), f.nodes[cur].count, f.nodes[cur].bytes, true, false, cur });
        if (f.nodes[cur].count * 20 >= f.items) expand_dir(f, o, cur, level + 1, thr, out);   // open only what holds >= 5 %
    }
    if (n.self > 0 && shown > 0) {
        out.push_back({ level, "(files right here)", n.self, 0, false, true, node });
        out.back().here = true;
    }
    if (rest_n) out.push_back({ level, ssprintf("+%d more %s", rest_n, rest_n == 1 ? "directory" : "directories"), rest, 0, false, true, 0 });
}

std::vector<DirLine> dir_lines(const Facets& f, const Opts& o) {
    std::vector<DirLine> out;
    struct Top { uint32_t node; uint64_t count; bool root_files; };
    std::vector<Top> tops;
    for (uint32_t drv : f.nodes[0].children) {
        const DirNode& dn = f.nodes[drv];
        if (dn.self > 0) tops.push_back({ drv, dn.self, true });
        for (uint32_t c : dn.children) tops.push_back({ c, f.nodes[c].count, false });
    }
    std::sort(tops.begin(), tops.end(), [](const Top& a, const Top& b) { return a.count > b.count; });
    const uint64_t thr = fold_threshold(o, f.items);
    int shown = 0, rest_n = 0;
    uint64_t rest = 0;
    for (const auto& t : tops) {
        if (t.count < thr || shown >= o.top) { rest_n++; rest += t.count; continue; }
        shown++;
        if (t.root_files) {
            out.push_back({ 0, narrow(f.dir_path(t.node)) + "  (files at the root)", t.count, 0, false, false, t.node });
            out.back().here = true;
            continue;
        }
        uint32_t cur = t.node;
        const std::wstring label = collapsed_label(f, cur, true);
        out.push_back({ 0, narrow(label), f.nodes[cur].count, f.nodes[cur].bytes, true, false, cur });
        if (f.nodes[cur].count * 20 >= f.items) expand_dir(f, o, cur, 1, thr, out);
    }
    if (rest_n) out.push_back({ 0, ssprintf("+%d more top-level %s", rest_n, rest_n == 1 ? "directory" : "directories"), rest, 0, false, true, 0 });
    return out;
}

std::vector<DirLine> flat_lines(const Facets& f, const Opts& o) {
    std::vector<DirLine> all;
    for (uint32_t id = 1; id < (uint32_t)f.nodes.size(); ++id) {
        const DirNode& n = f.nodes[id];
        if (n.depth == (uint32_t)o.flat) all.push_back({ 0, narrow(f.dir_path(id)), n.count, n.bytes, true, false, id });
        else if (n.depth < (uint32_t)o.flat && n.self > 0) {
            all.push_back({ 0, narrow(f.dir_path(id)) + "  (files right here)", n.self, 0, false, false, id });
            all.back().here = true;
        }
    }
    std::sort(all.begin(), all.end(), [](const DirLine& a, const DirLine& b) { return a.count > b.count; });
    const uint64_t thr = fold_threshold(o, f.items);
    std::vector<DirLine> out;
    int rest_n = 0;
    uint64_t rest = 0;
    for (auto& l : all) {
        if (l.count < thr || (int)out.size() >= o.top) { rest_n++; rest += l.count; continue; }
        out.push_back(std::move(l));
    }
    if (rest_n) out.push_back({ 0, ssprintf("+%d more", rest_n), rest, 0, false, true, 0 });
    return out;
}

}  // namespace facet
