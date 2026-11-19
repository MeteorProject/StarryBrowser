// Copyright 2026 The Starry Browser Authors

#include "starry/components/content_filter/filter_engine.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <utility>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "starry/components/content_filter/domain_set.h"
#include "starry/components/content_filter/filter_list.h"
#include "starry/components/content_filter/filter_parser.h"
#include "starry/components/content_filter/network_filter.h"
#include "starry/components/content_filter/html_filter.h"
#include "starry/components/content_filter/procedural_selector.h"
#include "starry/components/content_filter/redirect_resources.h"
#include "starry/components/content_filter/request.h"
#include "starry/components/content_filter/response_filter.h"
#include "starry/components/content_filter/scriptlets.h"
#include "starry/components/content_filter/tracking_parameters.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_set.h"
#include "third_party/re2/src/re2/re2.h"

namespace starry::content_filter {

namespace {

// 键 → 一组编号，建好后压成一段连续的数组（大多数桶只有一两条，不值得每个桶一个
// vector）。
template <typename Key>
class CompactIndex {
 public:
  void Add(Key key, uint32_t id) { pending_.emplace_back(key, id); }

  void Finalize() {
    std::stable_sort(pending_.begin(), pending_.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    ids_.reserve(pending_.size());
    for (size_t i = 0; i < pending_.size();) {
      const Key key = pending_[i].first;
      const uint32_t begin = static_cast<uint32_t>(ids_.size());
      while (i < pending_.size() && pending_[i].first == key) {
        ids_.push_back(pending_[i].second);
        ++i;
      }
      map_.emplace(key, std::make_pair(
                            begin, static_cast<uint32_t>(ids_.size()) - begin));
    }
    pending_.clear();
    pending_.shrink_to_fit();
    ids_.shrink_to_fit();
  }

  base::span<const uint32_t> Find(Key key) const {
    auto it = map_.find(key);
    if (it == map_.end()) {
      return {};
    }
    return base::span(ids_).subspan(it->second.first, it->second.second);
  }

  bool empty() const { return map_.empty(); }

 private:
  std::vector<std::pair<Key, uint32_t>> pending_;
  absl::flat_hash_map<Key, std::pair<uint32_t, uint32_t>> map_;
  std::vector<uint32_t> ids_;
};

struct FindResult {
  bool matched = false;
  // 纯网站的集合里碰到的没有规则对象。
  raw_ptr<const NetworkFilter> filter = nullptr;
};

// 一类网络规则的索引（见 FilterEngine 的说明）。
class NetworkIndex {
 public:
  void AddPlainHost(uint64_t host) { plain_hosts_.insert(host); }
  void AddHost(uint64_t host, uint32_t id) { hosts_.Add(host, id); }
  void AddToken(uint32_t token, uint32_t id) {
    tokens_.Add(token, id);
    bloom_.set(token & (kBloomBits - 1));
  }
  void AddSourceDomain(uint64_t host, uint32_t id) {
    source_domains_.Add(host, id);
  }
  void AddAny(uint32_t id) { any_.push_back(id); }

  void Finalize() {
    hosts_.Finalize();
    tokens_.Finalize();
    source_domains_.Finalize();
    any_.shrink_to_fit();
  }

  bool empty() const {
    return plain_hosts_.empty() && hosts_.empty() && tokens_.empty() &&
           source_domains_.empty() && any_.empty();
  }

  // 第一条能用（accept）并且匹配的规则。allow_plain 为 false 时不看纯网站的集合
  // （$genericblock）。
  template <typename Accept>
  FindResult Find(const Request& request,
                  const std::vector<NetworkFilter>& filters,
                  bool allow_plain,
                  Accept&& accept) const {
    FindResult result;
    ForEach(request, filters, allow_plain, [&](const NetworkFilter* filter) {
      if (filter && !accept(*filter)) {
        return false;
      }
      result.matched = true;
      result.filter = filter;
      return true;
    });
    return result;
  }

  // 每条匹配的规则调用一次 visit（同一条可能来两次）；visit 返回 true 时停下。
  template <typename Visit>
  void ForEach(const Request& request,
               const std::vector<NetworkFilter>& filters,
               bool allow_plain,
               Visit&& visit) const {
    if (empty()) {
      return;
    }
    const HostSuffixes& host = request.host_suffixes();
    // 纯网站的规则是默认的类型：不管顶层页面和弹出窗口。
    if (allow_plain && !plain_hosts_.empty() &&
        (request.type() & kNetworkTypes)) {
      for (uint64_t suffix : host.plain) {
        if (plain_hosts_.contains(suffix) && visit(nullptr)) {
          return;
        }
      }
    }
    auto check = [&](base::span<const uint32_t> ids) {
      for (uint32_t id : ids) {
        const NetworkFilter& filter = filters[id];
        if (filter.Matches(request) && visit(&filter)) {
          return true;
        }
      }
      return false;
    };
    if (!hosts_.empty()) {
      for (uint64_t suffix : host.plain) {
        if (check(hosts_.Find(suffix))) {
          return;
        }
      }
    }
    if (!tokens_.empty()) {
      for (uint32_t token : request.tokens()) {
        if (bloom_.test(token & (kBloomBits - 1)) &&
            check(tokens_.Find(token))) {
          return;
        }
      }
    }
    if (!source_domains_.empty() && request.has_source()) {
      const HostSuffixes& source = request.source_suffixes();
      for (uint64_t suffix : source.plain) {
        if (check(source_domains_.Find(suffix))) {
          return;
        }
      }
      for (uint64_t suffix : source.entity) {
        if (check(source_domains_.Find(suffix))) {
          return;
        }
      }
    }
    check(any_);
  }

  size_t plain_host_count() const { return plain_hosts_.size(); }

 private:
  static constexpr size_t kBloomBits = 1 << 16;

  absl::flat_hash_set<uint64_t> plain_hosts_;
  CompactIndex<uint64_t> hosts_;
  CompactIndex<uint32_t> tokens_;
  CompactIndex<uint64_t> source_domains_;
  std::vector<uint32_t> any_;
  // 桶里有没有这个分词（散列的低 16 位）：大多数分词不在任何桶里，先挡掉。
  std::bitset<kBloomBits> bloom_;
};

enum Category : size_t {
  kBlock,
  kImportant,
  kException,
  kDocException,
  kRedirect,
  kRedirectException,
  kCsp,
  kCspException,
  kRemoveParam,
  kRemoveParamException,
  kReplace,
  kReplaceException,
  kCategoryCount,
};

Category CategoryOf(const NetworkFilter& filter) {
  const bool exception = filter.is_exception();
  switch (filter.kind) {
    case NetworkFilterKind::kBlock:
      if (exception) {
        return filter.doc_flags ? kDocException : kException;
      }
      return filter.is_important() ? kImportant : kBlock;
    case NetworkFilterKind::kRedirect:
      return exception ? kRedirectException : kRedirect;
    case NetworkFilterKind::kCsp:
      return exception ? kCspException : kCsp;
    case NetworkFilterKind::kRemoveParam:
      return exception ? kRemoveParamException : kRemoveParam;
    case NetworkFilterKind::kReplace:
      return exception ? kReplaceException : kReplace;
  }
}

// 限定网站的元素隐藏。
struct SpecificRule {
  CosmeticFilter::Kind kind;
  std::string selector;
  std::string style;
  DomainSet domains;
};

// 扩展选择器（都限定网站）。key 是例外对的写法（CosmeticFilter::selector）。
struct ProceduralRule {
  std::string key;
  std::string selector;
  ProceduralAction action;
  // `:style()` 的样式，`:remove-attr()`、`:remove-class()` 编好的名字。
  std::string arg;
  DomainSet domains;
};

// HTML 过滤和去掉响应头（都限定网站）。key 是例外对的写法（`^` 开头，CosmeticFilter::selector）。
struct HtmlRule {
  std::string key;
  // 去掉响应头的是 null。
  std::unique_ptr<HtmlSelector> selector;
  // 要去掉的响应头（小写）。
  std::string header;
  DomainSet domains;
};

// 限定网站的例外（`网站#@#选择器`）。
struct CosmeticException {
  std::string selector;
  DomainSet domains;
};

// 一条脚本片段。key 是「规范名字\n参数（EncodeScriptletArgs）」：例外按它对（别名和
// 规范名字算一样），同一条写了几次只跑一次。
struct ScriptletRule {
  uint16_t scriptlet;
  std::string key;
  DomainSet domains;

  std::string_view args() const {
    return std::string_view(key).substr(ScriptletName(scriptlet).size() + 1);
  }
};

// `网站#@#+js(…)`：这些网站上不跑 key 这条；key 空的（`#@#+js()`）是一个也不跑。
struct ScriptletException {
  std::string key;
  DomainSet domains;
};

std::string ScriptletKey(const ScriptletFilter& filter) {
  if (filter.scriptlet == kNoScriptlet) {
    return std::string();
  }
  std::string key(ScriptletName(filter.scriptlet));
  key += '\n';
  key += EncodeScriptletArgs(filter.args);
  return key;
}

// 响应体不是文字的请求（同 uBlock Origin），$replace 不管。
constexpr uint32_t kBinaryResponseTypes =
    kTypeImage | kTypeMedia | kTypeFont | kTypeWebSocket | kTypePing;

// 一个文档最多跑几条（正常的网站是几条到十几条）。
constexpr size_t kMaxScriptletsPerDocument = 256;
// 一个文档最多几条扩展选择器（Facebook 这样的网站有几十条）。
constexpr size_t kMaxProceduralPerDocument = 512;

bool RemoveParamMatches(const NetworkFilter& filter,
                        std::string_view name,
                        std::string_view value) {
  const std::string& spec = filter.value;
  if (spec.empty()) {
    return true;
  }
  if (filter.value_regex) {
    std::string pair(name);
    pair += '=';
    pair.append(value);
    return re2::RE2::PartialMatch(pair, *filter.value_regex);
  }
  if (spec.front() == '~') {
    return std::string_view(spec).substr(1) != name;
  }
  return spec == name;
}

void AppendRule(std::string& css,
                std::string_view selector,
                std::string_view declarations) {
  css.append(selector);
  css += '{';
  css.append(declarations);
  css += "}\n";
}

}  // namespace

struct FilterEngine::Data {
  std::vector<NetworkFilter> filters;
  std::array<NetworkIndex, kCategoryCount> indexes;
  size_t network_rules = 0;
  size_t cosmetic_rules = 0;

  // 不限网站的元素隐藏。exclusions[i] 不为空时第 i 个只在这些网站以外用（`~a.com##.x`，
  // 几份列表都写了时每一份都排除才不用）。
  std::vector<std::string> generic;
  std::vector<std::vector<DomainSet>> generic_exclusions;
  absl::flat_hash_map<std::string, uint32_t> generic_ids;
  CompactIndex<uint64_t> generic_excluded_hosts;
  uint64_t generic_version = 0;

  std::vector<SpecificRule> specific;
  CompactIndex<uint64_t> specific_hosts;
  std::vector<CosmeticException> exceptions;
  CompactIndex<uint64_t> exception_hosts;
  // 不限网站的例外（`#@#选择器`）：哪里都不用这个选择器。
  absl::flat_hash_set<std::string> global_exceptions;
  // 扩展选择器，按网站分桶（例外和上面的共用）。
  std::vector<ProceduralRule> procedural;
  CompactIndex<uint64_t> procedural_hosts;
  // HTML 过滤，按网站分桶（例外和上面的共用）。
  std::vector<HtmlRule> html;
  CompactIndex<uint64_t> html_hosts;

  // 脚本片段：限定网站的按网站分桶，不限网站的（可能排除了一些）每次都看。
  size_t scriptlet_rules = 0;
  std::vector<ScriptletRule> scriptlets;
  CompactIndex<uint64_t> scriptlet_hosts;
  std::vector<uint32_t> generic_scriptlets;
  std::vector<ScriptletException> scriptlet_exceptions;
  CompactIndex<uint64_t> scriptlet_exception_hosts;
  // 不限网站的例外：哪里都不跑这条（空的是哪里都不跑脚本片段）。
  absl::flat_hash_set<std::string> global_scriptlet_exceptions;

  NetworkIndex& index(Category category) { return indexes[category]; }
  const NetworkIndex& index(Category category) const {
    return indexes[category];
  }
};

NetworkMatch::NetworkMatch() = default;
NetworkMatch::NetworkMatch(const NetworkMatch&) = default;
NetworkMatch& NetworkMatch::operator=(const NetworkMatch&) = default;
NetworkMatch::~NetworkMatch() = default;

CosmeticResult::CosmeticResult() = default;
CosmeticResult::CosmeticResult(CosmeticResult&&) = default;
CosmeticResult& CosmeticResult::operator=(CosmeticResult&&) = default;
CosmeticResult::~CosmeticResult() = default;

HtmlFilterResult::HtmlFilterResult() = default;
HtmlFilterResult::HtmlFilterResult(HtmlFilterResult&&) = default;
HtmlFilterResult& HtmlFilterResult::operator=(HtmlFilterResult&&) = default;
HtmlFilterResult::~HtmlFilterResult() = default;

FilterEngine::FilterEngine(std::unique_ptr<Data> data)
    : data_(std::move(data)), id_([] {
        static std::atomic<uint64_t> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
      }()) {}

FilterEngine::~FilterEngine() = default;

// static
scoped_refptr<FilterEngine> FilterEngine::CreateEmpty() {
  return base::WrapRefCounted(new FilterEngine(std::make_unique<Data>()));
}

// static
scoped_refptr<FilterEngine> FilterEngine::Build(
    const std::vector<FilterListSource>& sources,
    std::vector<FilterListStats>* stats) {
  auto data = std::make_unique<Data>();

  // 第一遍：所有列表的 $badfilter（跨列表生效）。
  absl::flat_hash_set<std::string> badfilters;
  for (const FilterListSource& source : sources) {
    if (source.text.find("badfilter") == std::string::npos) {
      continue;
    }
    ForEachActiveLine(
        source.text,
        [&](std::string_view line) {
          if (line.find("badfilter") == std::string_view::npos) {
            return;
          }
          ParsedLine parsed = ParseFilterLine(line);
          if (parsed.badfilter) {
            badfilters.insert(std::move(parsed.badfilter_target));
          }
        },
        /*replace_allowed=*/source.trusted);
  }

  // 第二遍：解析，按原文去重（几份列表常有同样的规则）。
  std::vector<NetworkFilter> network;
  std::vector<CosmeticFilter> cosmetic;
  std::vector<ScriptletFilter> scriptlets;
  absl::flat_hash_set<uint64_t> seen;
  if (stats) {
    stats->clear();
  }
  for (size_t list = 0; list < sources.size(); ++list) {
    const FilterListSource& source = sources[list];
    FilterListStats list_stats;
    list_stats.id = source.id;
    auto visit = [&](std::string_view line) {
      if (line.front() == '!' ||
          (line.front() == '[' && line.back() == ']')) {
        return;
      }
      if (!seen.insert(HashHost(line)).second) {
        return;
      }
      if (!badfilters.empty() && badfilters.contains(line)) {
        return;
      }
      ParsedLine parsed = ParseFilterLine(line);
      switch (parsed.type) {
        case ParsedLine::Type::kNone:
          return;
        case ParsedLine::Type::kSkipped:
          ++list_stats.skipped;
          ++list_stats.skipped_by_reason[static_cast<size_t>(parsed.reason)];
          return;
        case ParsedLine::Type::kNetwork: {
          // $replace 能改网页的任何内容：只收信得过的列表里的（同 uBlock Origin）。
          if (!source.trusted &&
              std::any_of(parsed.network.begin(), parsed.network.end(),
                          [](const NetworkFilter& filter) {
                            return filter.kind == NetworkFilterKind::kReplace &&
                                   !filter.is_exception();
                          })) {
            ++list_stats.skipped;
            ++list_stats.skipped_by_reason[static_cast<size_t>(
                SkipReason::kOption)];
            return;
          }
          bool added = false;
          for (NetworkFilter& filter : parsed.network) {
            if (!badfilters.empty() && filter.IsPlainHost() &&
                badfilters.contains(HostFilterText(filter.host_part()))) {
              continue;
            }
            filter.list = static_cast<uint16_t>(list);
            network.push_back(std::move(filter));
            added = true;
          }
          if (added) {
            ++list_stats.network;
          }
          return;
        }
        case ParsedLine::Type::kCosmetic:
          cosmetic.push_back(std::move(parsed.cosmetic));
          ++list_stats.cosmetic;
          return;
        case ParsedLine::Type::kScriptlet:
          // trusted- 的只收信得过的列表里的（例外不要紧）。
          if (!source.trusted &&
              std::any_of(parsed.scriptlets.begin(), parsed.scriptlets.end(),
                          [](const ScriptletFilter& filter) {
                            return !filter.exception &&
                                   IsTrustedScriptlet(filter.scriptlet);
                          })) {
            ++list_stats.skipped;
            ++list_stats.skipped_by_reason[static_cast<size_t>(
                SkipReason::kScriptlet)];
            return;
          }
          for (ScriptletFilter& filter : parsed.scriptlets) {
            scriptlets.push_back(std::move(filter));
          }
          ++list_stats.scriptlet;
          return;
      }
    };
    // 预处理的 cap_html_filtering 按列表信不信得过求值。
    ForEachActiveLine(source.text, visit, /*replace_allowed=*/source.trusted);
    data->network_rules += list_stats.network;
    data->cosmetic_rules += list_stats.cosmetic;
    data->scriptlet_rules += list_stats.scriptlet;
    if (stats) {
      stats->push_back(std::move(list_stats));
    }
  }

  // ---- 网络规则的索引 ----

  // 分词出现了几次：每条规则挑最少见的那个。
  absl::flat_hash_map<uint32_t, uint32_t> token_counts;
  std::vector<std::vector<uint32_t>> candidates(network.size());
  for (size_t i = 0; i < network.size(); ++i) {
    const NetworkFilter& filter = network[i];
    if (filter.host_length > 0) {
      continue;
    }
    candidates[i] = filter.TokenCandidates();
    for (uint32_t token : candidates[i]) {
      ++token_counts[token];
    }
  }
  data->filters.reserve(network.size());
  for (size_t i = 0; i < network.size(); ++i) {
    NetworkFilter& filter = network[i];
    const Category category = CategoryOf(filter);
    NetworkIndex& index = data->index(category);
    if (filter.IsPlainHost() &&
        (category == kBlock || category == kException)) {
      index.AddPlainHost(HashHost(filter.host_part()));
      continue;
    }
    const uint32_t id = static_cast<uint32_t>(data->filters.size());
    if (filter.host_length > 0) {
      index.AddHost(HashHost(filter.host_part()), id);
    } else if (!candidates[i].empty()) {
      uint32_t best = candidates[i].front();
      uint32_t best_count = token_counts[best];
      for (uint32_t token : candidates[i]) {
        const uint32_t count = token_counts[token];
        if (count < best_count) {
          best = token;
          best_count = count;
        }
      }
      index.AddToken(best, id);
    } else if (filter.domains && filter.domains->has_includes()) {
      for (uint64_t host : filter.domains->includes()) {
        index.AddSourceDomain(host, id);
      }
    } else {
      index.AddAny(id);
    }
    data->filters.push_back(std::move(filter));
  }
  network.clear();
  network.shrink_to_fit();
  data->filters.shrink_to_fit();
  for (NetworkIndex& index : data->indexes) {
    index.Finalize();
  }

  // ---- 元素隐藏 ----

  for (CosmeticFilter& filter : cosmetic) {
    if (filter.exception) {
      if (filter.domains.empty()) {
        data->global_exceptions.insert(filter.selector);
      } else if (filter.domains.has_includes()) {
        const uint32_t id = static_cast<uint32_t>(data->exceptions.size());
        for (uint64_t host : filter.domains.includes()) {
          data->exception_hosts.Add(host, id);
        }
        data->exceptions.push_back(
            {std::move(filter.selector), std::move(filter.domains)});
      }
      continue;
    }
    if (filter.kind == CosmeticFilter::Kind::kHtml ||
        filter.kind == CosmeticFilter::Kind::kResponseHeader) {
      std::unique_ptr<HtmlSelector> selector;
      if (filter.kind == CosmeticFilter::Kind::kHtml) {
        selector = HtmlSelector::Compile(filter.procedural);
        if (!selector) {
          continue;
        }
      }
      const uint32_t id = static_cast<uint32_t>(data->html.size());
      for (uint64_t host : filter.domains.includes()) {
        data->html_hosts.Add(host, id);
      }
      data->html.push_back({std::move(filter.selector), std::move(selector),
                            std::move(filter.style),
                            std::move(filter.domains)});
      continue;
    }
    if (!filter.procedural.empty() && filter.domains.has_includes()) {
      const uint32_t id = static_cast<uint32_t>(data->procedural.size());
      for (uint64_t host : filter.domains.includes()) {
        data->procedural_hosts.Add(host, id);
      }
      const bool also_css = filter.kind != CosmeticFilter::Kind::kProcedural;
      data->procedural.push_back(
          {also_css ? filter.selector : std::move(filter.selector),
           std::move(filter.procedural), filter.action,
           std::move(filter.style),
           also_css ? filter.domains : std::move(filter.domains)});
      if (!also_css) {
        continue;
      }
    }
    if (filter.kind == CosmeticFilter::Kind::kProcedural) {
      continue;
    }
    if (!filter.domains.has_includes()) {
      if (filter.kind != CosmeticFilter::Kind::kHide) {
        continue;
      }
      // 不限网站（可能排除了一些网站）。
      auto [it, inserted] = data->generic_ids.try_emplace(
          filter.selector, static_cast<uint32_t>(data->generic.size()));
      if (inserted) {
        data->generic.push_back(filter.selector);
        data->generic_exclusions.emplace_back();
        if (filter.domains.has_excludes()) {
          data->generic_exclusions.back().push_back(std::move(filter.domains));
        }
        continue;
      }
      std::vector<DomainSet>& exclusions = data->generic_exclusions[it->second];
      if (exclusions.empty() || !filter.domains.has_excludes()) {
        // 有一条到处都用的：不再排除哪里。
        exclusions.clear();
      } else {
        exclusions.push_back(std::move(filter.domains));
      }
      continue;
    }
    const uint32_t id = static_cast<uint32_t>(data->specific.size());
    for (uint64_t host : filter.domains.includes()) {
      data->specific_hosts.Add(host, id);
    }
    data->specific.push_back({filter.kind, std::move(filter.selector),
                              std::move(filter.style),
                              std::move(filter.domains)});
  }
  cosmetic.clear();
  // 到处都不用的选择器从通用的里去掉（编号重排）。
  if (!data->global_exceptions.empty()) {
    std::vector<std::string> generic;
    std::vector<std::vector<DomainSet>> exclusions;
    for (size_t i = 0; i < data->generic.size(); ++i) {
      if (data->global_exceptions.contains(data->generic[i])) {
        continue;
      }
      generic.push_back(std::move(data->generic[i]));
      exclusions.push_back(std::move(data->generic_exclusions[i]));
    }
    data->generic = std::move(generic);
    data->generic_exclusions = std::move(exclusions);
  }
  data->generic_ids.clear();
  uint64_t version = 14695981039346656037ull;
  for (size_t i = 0; i < data->generic.size(); ++i) {
    data->generic_ids.emplace(data->generic[i], static_cast<uint32_t>(i));
    version = (version ^ HashHost(data->generic[i])) * 1099511628211ull;
    for (const DomainSet& set : data->generic_exclusions[i]) {
      for (uint64_t host : set.excludes()) {
        data->generic_excluded_hosts.Add(host, static_cast<uint32_t>(i));
      }
    }
  }
  data->generic_version = data->generic.empty() ? 0 : (version | 1);
  data->generic_excluded_hosts.Finalize();
  data->specific_hosts.Finalize();
  data->exception_hosts.Finalize();
  data->specific.shrink_to_fit();
  data->exceptions.shrink_to_fit();
  data->procedural_hosts.Finalize();
  data->procedural.shrink_to_fit();
  data->html_hosts.Finalize();
  data->html.shrink_to_fit();

  // ---- 脚本片段 ----

  for (ScriptletFilter& filter : scriptlets) {
    std::string key = ScriptletKey(filter);
    if (filter.exception) {
      if (filter.domains.empty()) {
        data->global_scriptlet_exceptions.insert(std::move(key));
      } else if (filter.domains.has_includes()) {
        const uint32_t id =
            static_cast<uint32_t>(data->scriptlet_exceptions.size());
        for (uint64_t host : filter.domains.includes()) {
          data->scriptlet_exception_hosts.Add(host, id);
        }
        data->scriptlet_exceptions.push_back(
            {std::move(key), std::move(filter.domains)});
      }
      continue;
    }
    const uint32_t id = static_cast<uint32_t>(data->scriptlets.size());
    if (filter.domains.has_includes()) {
      for (uint64_t host : filter.domains.includes()) {
        data->scriptlet_hosts.Add(host, id);
      }
    } else {
      data->generic_scriptlets.push_back(id);
    }
    data->scriptlets.push_back(
        {filter.scriptlet, std::move(key), std::move(filter.domains)});
  }
  scriptlets.clear();
  data->scriptlet_hosts.Finalize();
  data->scriptlet_exception_hosts.Finalize();
  data->scriptlets.shrink_to_fit();
  data->scriptlet_exceptions.shrink_to_fit();

  return base::WrapRefCounted(new FilterEngine(std::move(data)));
}

NetworkMatch FilterEngine::MatchRequest(const Request& request,
                                        uint32_t doc_flags) const {
  NetworkMatch match;
  if (doc_flags & (kDocDocument | kDocUrlBlock)) {
    return match;
  }
  const Data& data = *data_;
  // $genericblock：只用限定了网页的规则。
  const bool generic_allowed = !(doc_flags & kDocGenericBlock);
  auto accept_block = [&](const NetworkFilter& filter) {
    return generic_allowed ||
           (filter.domains && filter.domains->has_includes());
  };
  bool blocked = false;
  const FindResult important = data.index(kImportant)
                                   .Find(request, data.filters, generic_allowed,
                                         accept_block);
  if (important.matched) {
    // 只有同样带 $important 的例外压得过。
    const FindResult exception = data.index(kException).Find(
        request, data.filters, /*allow_plain=*/false,
        [](const NetworkFilter& filter) { return filter.is_important(); });
    if (!exception.matched) {
      blocked = true;
      match.important = true;
    }
  }
  if (!blocked) {
    const FindResult block = data.index(kBlock).Find(
        request, data.filters, generic_allowed, accept_block);
    if (block.matched) {
      const FindResult exception =
          data.index(kException)
              .Find(request, data.filters, /*allow_plain=*/true,
                    [](const NetworkFilter&) { return true; });
      if (exception.matched) {
        match.action = NetworkMatch::Action::kAllow;
      } else {
        blocked = true;
      }
    }
  }
  if (blocked) {
    match.action = NetworkMatch::Action::kBlock;
    const FindResult redirect = data.index(kRedirect).Find(
        request, data.filters, /*allow_plain=*/false,
        [](const NetworkFilter& filter) {
          return FindRedirectResource(filter.value) != nullptr;
        });
    if (redirect.matched && redirect.filter) {
      const std::string& name = redirect.filter->value;
      const FindResult exception = data.index(kRedirectException)
                                       .Find(request, data.filters,
                                             /*allow_plain=*/false,
                                             [&](const NetworkFilter& filter) {
                                               return filter.value.empty() ||
                                                      filter.value == name;
                                             });
      if (!exception.matched) {
        match.action = NetworkMatch::Action::kRedirect;
        match.redirect = name;
      }
    }
    return match;
  }

  // 没拦下：去掉网址参数。
  const NetworkIndex& removeparam = data.index(kRemoveParam);
  if (removeparam.empty() || !request.url().has_query()) {
    return match;
  }
  std::vector<const NetworkFilter*> removers;
  removeparam.ForEach(request, data.filters, /*allow_plain=*/false,
                      [&](const NetworkFilter* filter) {
                        if (filter) {
                          removers.push_back(filter);
                        }
                        return false;
                      });
  if (removers.empty()) {
    return match;
  }
  std::vector<const NetworkFilter*> keepers;
  bool keep_all = false;
  data.index(kRemoveParamException)
      .ForEach(request, data.filters, /*allow_plain=*/false,
               [&](const NetworkFilter* filter) {
                 if (!filter) {
                   return false;
                 }
                 if (filter->value.empty()) {
                   keep_all = true;
                   return true;
                 }
                 keepers.push_back(filter);
                 return false;
               });
  if (keep_all) {
    return match;
  }
  match.rewritten = RemoveQueryParameters(
      request.url(), [&](std::string_view name, std::string_view value) {
        for (const NetworkFilter* keeper : keepers) {
          if (keeper->value == name ||
              (keeper->value_regex &&
               RemoveParamMatches(*keeper, name, value))) {
            return false;
          }
        }
        for (const NetworkFilter* remover : removers) {
          if (RemoveParamMatches(*remover, name, value)) {
            return true;
          }
        }
        return false;
      });
  return match;
}

std::vector<std::string> FilterEngine::CspDirectives(const Request& request,
                                                     uint32_t doc_flags) const {
  std::vector<std::string> result;
  if (doc_flags & (kDocDocument | kDocUrlBlock)) {
    return result;
  }
  const Data& data = *data_;
  const NetworkIndex& csp = data.index(kCsp);
  if (csp.empty()) {
    return result;
  }
  std::vector<std::string_view> values;
  csp.ForEach(request, data.filters, /*allow_plain=*/false,
              [&](const NetworkFilter* filter) {
                if (filter) {
                  values.push_back(filter->value);
                }
                return false;
              });
  if (values.empty()) {
    return result;
  }
  bool disabled = false;
  std::vector<std::string_view> excepted;
  data.index(kCspException)
      .ForEach(request, data.filters, /*allow_plain=*/false,
               [&](const NetworkFilter* filter) {
                 if (!filter) {
                   return false;
                 }
                 if (filter->value.empty()) {
                   disabled = true;
                   return true;
                 }
                 excepted.push_back(filter->value);
                 return false;
               });
  if (disabled) {
    return result;
  }
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  for (std::string_view value : values) {
    if (std::find(excepted.begin(), excepted.end(), value) == excepted.end()) {
      result.emplace_back(value);
    }
  }
  return result;
}

std::vector<const ReplaceFilter*> FilterEngine::ReplaceFilters(
    const Request& request,
    uint32_t doc_flags) const {
  std::vector<const ReplaceFilter*> result;
  const Data& data = *data_;
  const NetworkIndex& index = data.index(kReplace);
  if (index.empty() || (doc_flags & (kDocDocument | kDocUrlBlock)) ||
      (request.type() & kBinaryResponseTypes)) {
    return result;
  }
  std::vector<const NetworkFilter*> filters;
  index.ForEach(request, data.filters, /*allow_plain=*/false,
                [&](const NetworkFilter* filter) {
                  if (filter && filter->replace) {
                    filters.push_back(filter);
                  }
                  return false;
                });
  if (filters.empty()) {
    return result;
  }
  bool disabled = false;
  std::vector<std::string_view> excepted;
  data.index(kReplaceException)
      .ForEach(request, data.filters, /*allow_plain=*/false,
               [&](const NetworkFilter* filter) {
                 if (!filter) {
                   return false;
                 }
                 if (filter->value.empty()) {
                   disabled = true;
                   return true;
                 }
                 excepted.push_back(filter->value);
                 return false;
               });
  if (disabled) {
    return result;
  }
  // 按列表里的顺序（data.filters 里的位置），同样写法的只要一条。
  std::sort(filters.begin(), filters.end());
  filters.erase(std::unique(filters.begin(), filters.end()), filters.end());
  absl::flat_hash_set<std::string_view> added;
  for (const NetworkFilter* filter : filters) {
    if (std::find(excepted.begin(), excepted.end(), filter->value) !=
            excepted.end() ||
        !added.insert(filter->value).second) {
      continue;
    }
    result.push_back(filter->replace.get());
  }
  return result;
}

uint32_t FilterEngine::DocumentFlags(const GURL& url,
                                     const GURL& parent) const {
  if (!url.SchemeIsHTTPOrHTTPS()) {
    return 0;
  }
  const Data& data = *data_;
  const NetworkIndex& index = data.index(kDocException);
  if (index.empty()) {
    return 0;
  }
  const bool frame = parent.is_valid() && parent.SchemeIsHTTPOrHTTPS();
  Request request(url, frame ? parent : url,
                  frame ? kTypeSubdocument : kTypeDocument);
  uint32_t flags = 0;
  index.ForEach(request, data.filters, /*allow_plain=*/false,
                [&](const NetworkFilter* filter) {
                  if (filter) {
                    flags |= filter->doc_flags;
                  }
                  return false;
                });
  return flags;
}

CosmeticResult FilterEngine::Cosmetics(const GURL& url,
                                       uint32_t doc_flags) const {
  CosmeticResult result;
  if ((doc_flags & kDocDocument) || !url.SchemeIsHTTPOrHTTPS()) {
    return result;
  }
  // $elemhide 只关元素隐藏，脚本片段由 $jsinject、`#@#+js()` 关。
  const bool hide = !(doc_flags & kDocElemHide);
  const Data& data = *data_;
  HostSuffixes host;
  host.Compute(url.host());
  auto for_each_suffix = [&](auto&& visit) {
    for (uint64_t suffix : host.plain) {
      visit(suffix);
    }
    for (uint64_t suffix : host.entity) {
      visit(suffix);
    }
  };

  // 这个网站上不用的选择器（`网站#@#选择器`）。
  absl::flat_hash_set<std::string_view> excepted;
  if (hide && !data.exception_hosts.empty()) {
    for_each_suffix([&](uint64_t suffix) {
      for (uint32_t id : data.exception_hosts.Find(suffix)) {
        const CosmeticException& exception = data.exceptions[id];
        if (exception.domains.Matches(host)) {
          excepted.insert(exception.selector);
        }
      }
    });
  }

  result.generic =
      hide && !(doc_flags & kDocGenericHide) && !data.generic.empty();
  if (result.generic) {
    for (std::string_view selector : excepted) {
      if (auto it = data.generic_ids.find(selector);
          it != data.generic_ids.end()) {
        result.generic_exclusions.push_back(it->second);
      }
    }
    if (!data.generic_excluded_hosts.empty()) {
      for_each_suffix([&](uint64_t suffix) {
        for (uint32_t id : data.generic_excluded_hosts.Find(suffix)) {
          const std::vector<DomainSet>& sets = data.generic_exclusions[id];
          if (std::none_of(sets.begin(), sets.end(),
                           [&](const DomainSet& set) {
                             return set.Matches(host);
                           })) {
            result.generic_exclusions.push_back(id);
          }
        }
      });
    }
    std::sort(result.generic_exclusions.begin(),
              result.generic_exclusions.end());
    result.generic_exclusions.erase(
        std::unique(result.generic_exclusions.begin(),
                    result.generic_exclusions.end()),
        result.generic_exclusions.end());
  }

  if (hide && !(doc_flags & kDocSpecificHide) && !data.specific_hosts.empty()) {
    std::vector<uint32_t> ids;
    for_each_suffix([&](uint64_t suffix) {
      for (uint32_t id : data.specific_hosts.Find(suffix)) {
        ids.push_back(id);
      }
    });
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    absl::flat_hash_set<std::string_view> hidden;
    for (uint32_t id : ids) {
      const SpecificRule& rule = data.specific[id];
      if (!rule.domains.Matches(host) || excepted.contains(rule.selector) ||
          data.global_exceptions.contains(rule.selector)) {
        continue;
      }
      if (rule.kind == CosmeticFilter::Kind::kStyle &&
          (!excepted.empty() || !data.global_exceptions.empty())) {
        const std::string key = StyleExceptionKey(rule.selector, rule.style);
        if (excepted.contains(key) || data.global_exceptions.contains(key)) {
          continue;
        }
      }
      if (rule.kind == CosmeticFilter::Kind::kHide) {
        if (!hidden.insert(rule.selector).second) {
          continue;
        }
        AppendRule(result.css, rule.selector, "display:none!important");
      } else {
        AppendRule(result.css, rule.selector, rule.style);
      }
      ++result.specific_count;
    }
  }

  // 扩展选择器（都限定网站）：按列表里的顺序，同一条只要一次。
  if (hide && !(doc_flags & kDocSpecificHide) &&
      !data.procedural_hosts.empty()) {
    std::vector<uint32_t> ids;
    for_each_suffix([&](uint64_t suffix) {
      for (uint32_t id : data.procedural_hosts.Find(suffix)) {
        ids.push_back(id);
      }
    });
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    absl::flat_hash_set<std::string_view> added;
    std::string json;
    for (uint32_t id : ids) {
      const ProceduralRule& rule = data.procedural[id];
      if (!rule.domains.Matches(host) || excepted.contains(rule.key) ||
          data.global_exceptions.contains(rule.key) ||
          !added.insert(rule.key).second) {
        continue;
      }
      json += json.empty() ? "[[" : ",[";
      json += rule.selector;
      json += ',';
      json += base::NumberToString(static_cast<int>(rule.action));
      if (rule.action == ProceduralAction::kStyle) {
        std::vector<std::string>& styles = result.procedural_styles;
        auto it = std::find(styles.begin(), styles.end(), rule.arg);
        if (it == styles.end()) {
          it = styles.insert(styles.end(), rule.arg);
        }
        json += ',';
        json += base::NumberToString(it - styles.begin());
      } else if (!rule.arg.empty()) {
        json += ',';
        json += rule.arg;
      }
      json += ']';
      if (++result.procedural_count >= kMaxProceduralPerDocument) {
        break;
      }
    }
    if (!json.empty()) {
      json += ']';
      result.procedural = std::move(json);
    }
  }

  // 脚本片段：限定这个网站的加上不限网站的（$generichide 时不要），去掉例外的，按列表里的
  // 顺序。
  if ((doc_flags & kDocJsInject) || data.scriptlets.empty() ||
      data.global_scriptlet_exceptions.contains(std::string_view())) {
    return result;
  }
  absl::flat_hash_set<std::string_view> skipped;
  if (!data.scriptlet_exception_hosts.empty()) {
    for_each_suffix([&](uint64_t suffix) {
      for (uint32_t id : data.scriptlet_exception_hosts.Find(suffix)) {
        const ScriptletException& exception = data.scriptlet_exceptions[id];
        if (exception.domains.Matches(host)) {
          skipped.insert(exception.key);
        }
      }
    });
  }
  if (skipped.contains(std::string_view())) {
    return result;
  }
  std::vector<uint32_t> ids;
  if (!(doc_flags & kDocGenericHide)) {
    ids = data.generic_scriptlets;
  }
  if (!data.scriptlet_hosts.empty()) {
    for_each_suffix([&](uint64_t suffix) {
      for (uint32_t id : data.scriptlet_hosts.Find(suffix)) {
        ids.push_back(id);
      }
    });
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  ScriptletScript script;
  absl::flat_hash_set<std::string_view> added;
  for (uint32_t id : ids) {
    const ScriptletRule& rule = data.scriptlets[id];
    if (!rule.domains.Matches(host) || skipped.contains(rule.key) ||
        data.global_scriptlet_exceptions.contains(rule.key) ||
        !added.insert(rule.key).second) {
      continue;
    }
    script.Add(rule.scriptlet, rule.args());
    if (script.size() >= kMaxScriptletsPerDocument) {
      break;
    }
  }
  if (!script.empty()) {
    result.scriptlet_count = script.size();
    result.scripts = script.Build();
  }
  return result;
}

HtmlFilterResult FilterEngine::HtmlFilters(const GURL& url,
                                           uint32_t doc_flags) const {
  HtmlFilterResult result;
  const Data& data = *data_;
  if ((doc_flags & kDocDocument) || !url.SchemeIsHTTPOrHTTPS() ||
      data.html_hosts.empty()) {
    return result;
  }
  HostSuffixes host;
  host.Compute(url.host());
  auto for_each_suffix = [&](auto&& visit) {
    for (uint64_t suffix : host.plain) {
      visit(suffix);
    }
    for (uint64_t suffix : host.entity) {
      visit(suffix);
    }
  };
  std::vector<uint32_t> ids;
  for_each_suffix([&](uint64_t suffix) {
    for (uint32_t id : data.html_hosts.Find(suffix)) {
      ids.push_back(id);
    }
  });
  if (ids.empty()) {
    return result;
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  // 这个网站上的例外（`网站#@#^…`）。
  absl::flat_hash_set<std::string_view> excepted;
  for_each_suffix([&](uint64_t suffix) {
    for (uint32_t id : data.exception_hosts.Find(suffix)) {
      const CosmeticException& exception = data.exceptions[id];
      if (exception.selector.starts_with('^') &&
          exception.domains.Matches(host)) {
        excepted.insert(exception.selector);
      }
    }
  });
  absl::flat_hash_set<std::string_view> added;
  for (uint32_t id : ids) {
    const HtmlRule& rule = data.html[id];
    if (!rule.domains.Matches(host) || excepted.contains(rule.key) ||
        data.global_exceptions.contains(rule.key) ||
        !added.insert(rule.key).second) {
      continue;
    }
    if (rule.selector) {
      result.selectors.push_back(rule.selector.get());
    } else {
      result.removed_headers.push_back(rule.header);
    }
  }
  return result;
}

const std::vector<std::string>& FilterEngine::generic_selectors() const {
  return data_->generic;
}

uint64_t FilterEngine::generic_version() const {
  return data_->generic_version;
}

size_t FilterEngine::network_rule_count() const {
  return data_->network_rules;
}

size_t FilterEngine::cosmetic_rule_count() const {
  return data_->cosmetic_rules;
}

size_t FilterEngine::scriptlet_rule_count() const {
  return data_->scriptlet_rules;
}

}  // namespace starry::content_filter
