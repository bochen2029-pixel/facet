// facet · facets.h — the aggregation organ: fold a result stream into facets (directory trie ·
// extensions · modified buckets · size buckets · write bursts) with bounded memory. Every row
// is touched once; rows are retained verbatim only up to a cap (for --list and the window).
// Pure std + the item view from es_client; no windows.h.
#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "app_util.h"
#include "es_client.h"

namespace facet {

struct WHash {
    using is_transparent = void;
    size_t operator()(std::wstring_view s) const noexcept { return std::hash<std::wstring_view>{}(s); }
};
struct WEq {
    using is_transparent = void;
    bool operator()(std::wstring_view a, std::wstring_view b) const noexcept { return a == b; }
};

struct DirNode {
    uint32_t parent = 0;
    uint32_t depth = 0;               // root = 0, "C:" = 1, "C:\Users" = 2
    std::wstring name;                // one path component; the root's is empty
    uint64_t count = 0;               // items in the subtree (complete after finish())
    uint64_t bytes = 0;               // known bytes in the subtree
    uint64_t self = 0;                // items sitting directly in this directory
    std::vector<uint32_t> children;   // by count desc after finish()
};

struct ExtStat {
    std::wstring ext;                 // lower-case, "" = no extension
    uint64_t count = 0, bytes = 0;
};

struct BucketStat {
    std::string label;
    std::wstring query;               // the Everything term selecting exactly this bucket ("" = none)
    uint64_t count = 0, bytes = 0;
};

struct Burst {
    uint64_t start = 0, end = 0;      // FILETIME of the first and last write
    uint32_t count = 0;
    uint32_t dir = 0;                 // deepest directory holding >= 90 % of the burst (0 = root: it is split)
    double dir_share = 0.0;           // fraction of the burst inside dir
    std::vector<std::pair<uint32_t, double>> parts;   // when split: the biggest pieces below dir, with shares
    std::wstring query;               // dm:START..END (second precision) — selects exactly these writes
    size_t first = 0;                 // internal: index into the sorted event array
};

struct Row {                          // a retained result
    uint32_t dir = 0;
    uint32_t name_off = 0, name_len = 0;
    uint64_t size = kUnknown64, mtime = kUnknown64;
    bool folder = false;
};

struct FacetConfig {
    uint64_t now = 0;                 // FILETIME; 0 = the clock at construction
    uint32_t keep_rows = 0;           // rows to retain verbatim (0 = none)
    uint32_t burst_gap_s = 60;        // seconds of silence that close a burst
    uint32_t top_bursts = 10;         // bursts resolved with a dominant directory
};

constexpr int kModBuckets = 8;
constexpr int kSizeBuckets = 9;

class Facets {
public:
    explicit Facets(const FacetConfig& cfg);
    void add(const EsItem& it);
    void finish();

    // totals
    uint64_t items = 0, files = 0, folders = 0, bytes = 0, unknown_size = 0, unknown_mtime = 0;
    uint64_t last_hour = 0;           // items modified within the last hour (a note, not a bucket)
    // facets
    std::vector<DirNode> nodes;       // [0] = root
    std::vector<ExtStat> exts;        // by count desc after finish()
    std::vector<BucketStat> modified; // kModBuckets, fixed order
    std::vector<BucketStat> sizes;    // kSizeBuckets, fixed order
    std::vector<Burst> bursts;        // top_bursts by count desc
    uint64_t burst_total = 0, handpaced_bursts = 0, handpaced_files = 0;
    // retained rows
    std::vector<Row> rows;
    std::wstring dir_path(uint32_t node) const;          // "C:\a\b\"  (root = "")
    std::wstring row_path(const Row& r) const;
    std::wstring_view row_name(const Row& r) const;
    const FacetConfig& config() const { return cfg_; }

private:
    uint32_t dir_node(std::wstring_view p);
    uint32_t ext_slot(std::wstring_view e);
    int mod_bucket(uint64_t mtime) const;
    static int size_bucket(uint64_t size, bool folder);

    FacetConfig cfg_;
    std::unordered_map<std::wstring, uint32_t, WHash, WEq> index_;
    std::wstring last_key_;
    uint32_t last_node_ = 0;
    std::unordered_map<std::wstring, uint32_t, WHash, WEq> ext_index_;
    std::vector<std::pair<uint64_t, uint32_t>> events_;   // (mtime, dir) for every dated item
    std::wstring names_;
    uint64_t edge_today_ = 0, edge_yest_ = 0, edge_week_ = 0, edge_month_ = 0, edge_year_ = 0;
};

// ---- the directory view shared by the report, the JSON and the window ----
struct DirLine {
    int level = 0;
    std::string label;          // UTF-8
    uint64_t count = 0, bytes = 0;
    bool has_bytes = true;
    bool note = false;          // a fold / files-here line
    uint32_t node = 0;          // 0 = none
};
// rows below this are folded into "+N more": --min verbatim, else 1 % of the result set
uint64_t fold_threshold(const Opts& o, uint64_t items);
// "a\b\c\" — a chain of single children with nothing of their own collapses to one label
std::wstring collapsed_label(const Facets& f, uint32_t& cur, bool full_prefix);
// top entries are drive + first folder, ranked across drives, each expanded o.depth levels
std::vector<DirLine> dir_lines(const Facets& f, const Opts& o);
// --flat N: every prefix at depth N (plus the files of shallower directories), ranked
std::vector<DirLine> flat_lines(const Facets& f, const Opts& o);

}  // namespace facet
