// Copyright 2026 The Starry Browser Authors

#include "starry/browser/content_blocking/content_blocking_service.h"

#include <algorithm>
#include <utility>

#include "base/containers/span.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/hash/hash.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/uuid.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/net/system_network_context_manager.h"
#include "chrome/common/chrome_paths.h"
#include "net/base/filename_util.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_status_code.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "starry/browser/content_blocking/filter_config.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/components/content_filter/domain_set.h"
#include "starry/components/content_filter/filter_list.h"
#include "url/url_util.h"

#if defined(STARRY_DEVTOOLS)
#include "starry_internal/browser/devtools.h"
#endif

namespace starry::content_blocking {

namespace {

constexpr base::FilePath::CharType kDirectoryName[] =
    FILE_PATH_LITERAL("Starry Content Blocking");
constexpr base::FilePath::CharType kStateFile[] =
    FILE_PATH_LITERAL("State.json");
constexpr base::FilePath::CharType kListsDirectory[] = FILE_PATH_LITERAL("Lists");
constexpr base::FilePath::CharType kCustomRulesFile[] =
    FILE_PATH_LITERAL("CustomRules.txt");

// 不是过滤列表（网页、空的）或者超过 40 MB 的不要。
constexpr size_t kMaxListSize = 40 * 1024 * 1024;
constexpr size_t kMaxIncludes = 40;
constexpr base::TimeDelta kDefaultUpdateInterval = base::Days(4);
constexpr base::TimeDelta kRetryInterval = base::Hours(1);
constexpr base::TimeDelta kStartupUpdateDelay = base::Minutes(1);
constexpr base::TimeDelta kUpdateCheckInterval = base::Hours(1);
constexpr base::TimeDelta kRebuildDelay = base::Milliseconds(400);

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("starry_content_blocking_lists", R"(
        semantics {
          sender: "Starry Browser content blocking"
          description:
            "Downloads the ad and tracker filter lists the user subscribed "
            "to (EasyList and similar), and the files they include."
          trigger:
            "Content blocking is turned on and a list has never been "
            "downloaded or is older than its update interval; the user adds a "
            "list or presses Update now."
          data: "None. Conditional request headers (ETag, Last-Modified)."
          destination: OTHER
          destination_other: "The filter list's server."
        }
        policy {
          cookies_allowed: NO
          setting: "Settings > Privacy > Ad and tracker blocking."
          policy_exception_justification: "Not implemented."
        })");

using Category = ContentBlockingService::Category;
using Subscription = ContentBlockingService::Subscription;

struct CatalogEntry {
  const char* id;
  // 列表的名字（不翻译）；没有时用 title_key。
  const char* title;
  const char* title_key;
  Category category;
  const char* url;
  const char* homepage;
  const char* summary_key;
  bool enabled;
};

// 内置订阅目录。列表均在运行时下载，不随浏览器打包（它们采用
// CC BY-SA 或 GPLv3 许可）。
constexpr CatalogEntry kCatalog[] = {
    {"easylist", "EasyList", nullptr, Category::kAds,
     "https://easylist.to/easylist/easylist.txt", "https://easylist.to/",
     "contentBlocking.list.easylist.summary", true},
    {"easylist-china", "EasyList China", nullptr, Category::kAds,
     "https://easylist-downloads.adblockplus.org/easylistchina.txt",
     "https://github.com/easylist/easylistchina",
     "contentBlocking.list.easylistChina.summary", true},
    {"adguard-chinese", nullptr, "contentBlocking.list.adguardChinese.title",
     Category::kAds, "https://filters.adtidy.org/extension/ublock/filters/224.txt",
     "https://github.com/AdguardTeam/AdguardFilters",
     "contentBlocking.list.adguardChinese.summary", false},
    {"ublock-filters", nullptr, "contentBlocking.list.ublockFilters.title",
     Category::kAds, "https://ublockorigin.github.io/uAssets/filters/filters.txt",
     "https://github.com/uBlockOrigin/uAssets",
     "contentBlocking.list.ublockFilters.summary", false},
    {"ublock-unbreak", nullptr, "contentBlocking.list.ublockUnbreak.title",
     Category::kAds, "https://ublockorigin.github.io/uAssets/filters/unbreak.txt",
     "https://github.com/uBlockOrigin/uAssets",
     "contentBlocking.list.ublockUnbreak.summary", false},
    // YouTube 等频繁变动网站的快速修复规则（主要为脚本片段），每数小时更新。
    {"ublock-quick-fixes", nullptr,
     "contentBlocking.list.ublockQuickFixes.title", Category::kAds,
     "https://ublockorigin.github.io/uAssets/filters/quick-fixes.txt",
     "https://github.com/uBlockOrigin/uAssets",
     "contentBlocking.list.ublockQuickFixes.summary", false},
    {"easyprivacy", "EasyPrivacy", nullptr, Category::kPrivacy,
     "https://easylist.to/easylist/easyprivacy.txt", "https://easylist.to/",
     "contentBlocking.list.easyprivacy.summary", true},
    {"ublock-privacy", nullptr, "contentBlocking.list.ublockPrivacy.title",
     Category::kPrivacy, "https://ublockorigin.github.io/uAssets/filters/privacy.txt",
     "https://github.com/uBlockOrigin/uAssets",
     "contentBlocking.list.ublockPrivacy.summary", false},
    {"peter-lowe", nullptr, "contentBlocking.list.peterLowe.title",
     Category::kPrivacy,
     "https://pgl.yoyo.org/adservers/serverlist.php?hostformat=adblockplus&"
     "showintro=1&mimetype=plaintext",
     "https://pgl.yoyo.org/adservers/", "contentBlocking.list.peterLowe.summary",
     false},
    {"easylist-cookie", nullptr, "contentBlocking.list.easylistCookie.title",
     Category::kAnnoyances, "https://secure.fanboy.co.nz/fanboy-cookiemonster.txt",
     "https://easylist.to/", "contentBlocking.list.easylistCookie.summary", false},
    {"fanboy-annoyance", nullptr, "contentBlocking.list.fanboyAnnoyance.title",
     Category::kAnnoyances, "https://secure.fanboy.co.nz/fanboy-annoyance.txt",
     "https://easylist.to/", "contentBlocking.list.fanboyAnnoyance.summary",
     false},
};

// 跟着别的列表一起开的内置列表：开 uBlock 过滤器时一起开快速修复（uBlock Origin 默认两个都开着，
// YouTube 的假缓冲要快速修复里的规则）。关掉时不跟着关。跟着的列表在目录里排在后面。
struct CompanionEntry {
  const char* id;
  const char* leader;
};

constexpr CompanionEntry kCompanions[] = {
    {"ublock-quick-fixes", "ublock-filters"},
};

const char* LeaderOf(std::string_view id) {
  for (const CompanionEntry& companion : kCompanions) {
    if (id == companion.id) {
      return companion.leader;
    }
  }
  return nullptr;
}

const char* CategoryName(Category category) {
  switch (category) {
    case Category::kAds:
      return "ads";
    case Category::kPrivacy:
      return "privacy";
    case Category::kAnnoyances:
      return "annoyances";
    case Category::kCustom:
      return "custom";
  }
}

Category CategoryFromName(std::string_view name) {
  if (name == "ads") {
    return Category::kAds;
  }
  if (name == "privacy") {
    return Category::kPrivacy;
  }
  if (name == "annoyances") {
    return Category::kAnnoyances;
  }
  return Category::kCustom;
}

Subscription FromCatalog(const CatalogEntry& entry) {
  Subscription subscription;
  subscription.id = entry.id;
  subscription.url = GURL(entry.url);
  subscription.title = entry.title ? entry.title : "";
  subscription.title_key = entry.title_key ? entry.title_key : "";
  subscription.summary_key = entry.summary_key;
  subscription.homepage = GURL(entry.homepage);
  subscription.category = entry.category;
  subscription.enabled = entry.enabled;
  subscription.builtin = true;
  return subscription;
}

double TimeValue(base::Time time) {
  return time.is_null() ? 0 : time.InSecondsFSinceUnixEpoch();
}

base::Time TimeFromValue(std::optional<double> seconds) {
  return seconds && *seconds > 0
             ? base::Time::FromSecondsSinceUnixEpoch(*seconds)
             : base::Time();
}

std::string GetString(const base::DictValue& dict, std::string_view key) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string();
}

std::string ContentHash(std::string_view text) {
  return base::NumberToString(base::FastHash(base::as_byte_span(text)));
}

// ---- 文件（在 file_runner_ 上）----

struct DiskState {
  std::optional<base::Value> state;
  std::string custom_rules;
  // 已经有文字的订阅（Lists/<ID>.txt 在）。
  std::vector<std::string> lists;
};

DiskState ReadDiskState(const base::FilePath& directory) {
  DiskState disk;
  std::string json;
  if (base::ReadFileToString(directory.Append(kStateFile), &json)) {
    disk.state = base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  }
  base::ReadFileToString(directory.Append(kCustomRulesFile), &disk.custom_rules);
  base::FileEnumerator lists(directory.Append(kListsDirectory),
                             /*recursive=*/false, base::FileEnumerator::FILES,
                             FILE_PATH_LITERAL("*.txt"));
  for (base::FilePath path = lists.Next(); !path.empty(); path = lists.Next()) {
    disk.lists.push_back(path.BaseName().RemoveExtension().AsUTF8Unsafe());
  }
  return disk;
}

bool WriteText(const base::FilePath& path, const std::string& text) {
  if (!base::CreateDirectory(path.DirName())) {
    return false;
  }
  return base::ImportantFileWriter::WriteFileAtomically(path, text);
}

struct BuiltEngine {
  scoped_refptr<const content_filter::FilterEngine> engine;
  std::vector<content_filter::FilterListStats> stats;
  base::TimeDelta elapsed;
};

// 建引擎用的一份列表。
struct EngineList {
  std::string id;
  base::FilePath path;
  // 信得过的列表：能用 trusted- 开头的脚本片段（同 uBlock Origin：只有它自己的列表）。
  bool trusted = false;
};

BuiltEngine BuildEngine(std::vector<EngineList> files,
                        std::string custom_rules) {
  const base::TimeTicks start = base::TimeTicks::Now();
  std::vector<content_filter::FilterListSource> sources;
  for (EngineList& file : files) {
    std::string text;
    if (base::ReadFileToString(file.path, &text)) {
      sources.push_back({std::move(file.id), std::move(text), file.trusted});
    }
  }
  // 自定义规则是用户自己写的，信得过。
  if (!custom_rules.empty()) {
    sources.push_back({"custom", std::move(custom_rules), /*trusted=*/true});
  }
  BuiltEngine built;
  built.engine = content_filter::FilterEngine::Build(sources, &built.stats);
  built.elapsed = base::TimeTicks::Now() - start;
  return built;
}

}  // namespace

// ---- 下载一份列表（加上它 !#include 的文件）----

class ListDownload {
 public:
  struct Result {
    // 200（有新内容）、304（没变）或者 0（失败，error 是原因）。
    int status = 0;
    std::string text;
    std::string etag;
    std::string last_modified;
    std::string error;
  };
  using Callback = base::OnceCallback<void(Result)>;

  ListDownload(GURL url,
               std::string etag,
               std::string last_modified,
               Callback callback)
      : url_(std::move(url)),
        etag_(std::move(etag)),
        last_modified_(std::move(last_modified)),
        callback_(std::move(callback)) {}

  void Start() {
    auto request = std::make_unique<network::ResourceRequest>();
    request->url = url_;
    request->credentials_mode = network::mojom::CredentialsMode::kOmit;
    request->load_flags = net::LOAD_BYPASS_CACHE | net::LOAD_DISABLE_CACHE;
    if (!etag_.empty()) {
      request->headers.SetHeader("If-None-Match", etag_);
    }
    if (!last_modified_.empty()) {
      request->headers.SetHeader("If-Modified-Since", last_modified_);
    }
    loader_ = network::SimpleURLLoader::Create(std::move(request),
                                               kTrafficAnnotation);
    loader_->SetAllowHttpErrorResults(true);
    loader_->SetTimeoutDuration(base::Seconds(60));
    loader_->DownloadToString(
        Factory(),
        base::BindOnce(&ListDownload::OnMain, base::Unretained(this)),
        kMaxListSize);
  }

 private:
  static network::mojom::URLLoaderFactory* Factory() {
    return g_browser_process->system_network_context_manager()
        ->GetURLLoaderFactory();
  }

  int ResponseCode() const {
    const network::mojom::URLResponseHead* head = loader_->ResponseInfo();
    return head && head->headers ? head->headers->response_code() : 0;
  }

  void Finish(Result result) { std::move(callback_).Run(std::move(result)); }

  void Fail(std::u16string error) {
    Result result;
    result.error = base::UTF16ToUTF8(error);
    Finish(std::move(result));
  }

  void OnMain(std::optional<std::string> body) {
    const int code = ResponseCode();
    if (!body && loader_->NetError() == net::ERR_INSUFFICIENT_RESOURCES) {
      Fail(L("contentBlocking.download.error.tooLarge"));
      return;
    }
    if (code == 0) {
      Fail(L("contentBlocking.download.error.noResponse"));
      return;
    }
    if (code == net::HTTP_NOT_MODIFIED) {
      Result result;
      result.status = net::HTTP_NOT_MODIFIED;
      Finish(std::move(result));
      return;
    }
    if (code < 200 || code >= 300 || !body) {
      Fail(L("contentBlocking.download.error.status",
             {{"status", base::NumberToString(code)}}));
      return;
    }
    if (!content_filter::LooksLikeFilterList(*body)) {
      Fail(L("contentBlocking.download.error.notFilterList"));
      return;
    }
    const net::HttpResponseHeaders* headers = loader_->ResponseInfo()->headers.get();
    result_.status = code;
    result_.etag = headers->GetNormalizedHeader("ETag").value_or("");
    result_.last_modified =
        headers->GetNormalizedHeader("Last-Modified").value_or("");
    result_.text = std::move(*body);
    // `!#include`：只跟同一网站、同一协议的文件，最多 40 个，不再往下展开。
    for (const std::string& path :
         content_filter::IncludeDirectives(result_.text)) {
      const GURL include = url_.Resolve(path);
      if (include.is_valid() && include.host() == url_.host() &&
          include.scheme() == url_.scheme() &&
          includes_.size() < kMaxIncludes) {
        includes_.emplace_back(path, include);
      }
    }
    FetchNextInclude();
  }

  void FetchNextInclude() {
    if (next_include_ >= includes_.size()) {
      result_.text = content_filter::ExpandIncludes(
          result_.text,
          [this](std::string_view path) -> std::optional<std::string> {
            auto it = included_.find(std::string(path));
            if (it == included_.end()) {
              return std::nullopt;
            }
            return it->second;
          });
      Finish(std::move(result_));
      return;
    }
    auto request = std::make_unique<network::ResourceRequest>();
    request->url = includes_[next_include_].second;
    request->credentials_mode = network::mojom::CredentialsMode::kOmit;
    request->load_flags = net::LOAD_BYPASS_CACHE | net::LOAD_DISABLE_CACHE;
    loader_ = network::SimpleURLLoader::Create(std::move(request),
                                               kTrafficAnnotation);
    loader_->SetAllowHttpErrorResults(true);
    loader_->SetTimeoutDuration(base::Seconds(60));
    loader_->DownloadToString(
        Factory(),
        base::BindOnce(&ListDownload::OnInclude, base::Unretained(this)),
        kMaxListSize);
  }

  void OnInclude(std::optional<std::string> body) {
    if (body && ResponseCode() == net::HTTP_OK &&
        content_filter::LooksLikeFilterList(*body)) {
      included_[includes_[next_include_].first] = std::move(*body);
    }
    ++next_include_;
    FetchNextInclude();
  }

  const GURL url_;
  const std::string etag_;
  const std::string last_modified_;
  Callback callback_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  Result result_;
  std::vector<std::pair<std::string, GURL>> includes_;
  size_t next_include_ = 0;
  std::map<std::string, std::string> included_;
};

struct ContentBlockingService::LoadedState {
  DiskState disk;
};

struct ContentBlockingService::BuildResult {
  BuiltEngine built;
};

Subscription::Subscription() = default;
Subscription::Subscription(const Subscription&) = default;
Subscription& Subscription::operator=(const Subscription&) = default;
Subscription::~Subscription() = default;

base::TimeDelta Subscription::UpdateInterval() const {
  return std::clamp(expires.value_or(kDefaultUpdateInterval), base::Hours(1),
                    base::Days(14));
}

bool Subscription::IsDue(base::Time now) const {
  if (!last_error.empty() && !last_attempt.is_null() &&
      now - last_attempt < kRetryInterval) {
    return false;
  }
  return last_updated.is_null() || now - last_updated >= UpdateInterval();
}

// static
ContentBlockingService* ContentBlockingService::Get() {
  static base::NoDestructor<ContentBlockingService> service;
  return service.get();
}

ContentBlockingService::ContentBlockingService()
    : file_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})) {}

ContentBlockingService::~ContentBlockingService() = default;

void ContentBlockingService::Start() {
  if (started_) {
    return;
  }
  started_ = true;
  base::FilePath user_data;
  base::PathService::Get(chrome::DIR_USER_DATA, &user_data);
  directory_ = user_data.Append(kDirectoryName);

#if defined(STARRY_DEVTOOLS)
  local_lists_ = devtools::ContentBlockingLocalLists();
  automatic_updates_ =
      local_lists_.empty() && !devtools::IsContentBlockingOffline();
#else
  automatic_updates_ = true;
#endif

  // 读到状态之前先当作开着、还没建好：恢复的标签马上开始加载时，代理先等一下。
  ready_ = false;
  PublishConfig();
  file_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](base::FilePath directory) {
            auto state = std::make_unique<LoadedState>();
            state->disk = ReadDiskState(directory);
            return state;
          },
          directory_),
      base::BindOnce(&ContentBlockingService::OnStateLoaded,
                     weak_factory_.GetWeakPtr()));
}

void ContentBlockingService::Shutdown() {
  StopUpdates();
  rebuild_timer_.Stop();
  downloads_.clear();
  download_queue_.clear();
  queue_running_ = false;
}

void ContentBlockingService::OnStateLoaded(std::unique_ptr<LoadedState> loaded) {
  loaded_ = true;
  DiskState& disk = loaded->disk;
  custom_rules_ = std::move(disk.custom_rules);
  std::vector<Subscription> saved;
  if (disk.state && disk.state->is_dict()) {
    const base::DictValue& state = disk.state->GetDict();
    enabled_ = state.FindBool("enabled").value_or(true);
    strip_tracking_parameters_ =
        state.FindBool("stripTrackingParameters").value_or(true);
    if (const base::ListValue* allowlist = state.FindList("allowlist")) {
      for (const base::Value& site : *allowlist) {
        if (site.is_string() && !site.GetString().empty()) {
          allowlist_.push_back(site.GetString());
        }
      }
    }
    if (const base::ListValue* list = state.FindList("subscriptions")) {
      for (const base::Value& value : *list) {
        if (!value.is_dict()) {
          continue;
        }
        const base::DictValue& dict = value.GetDict();
        Subscription subscription;
        subscription.id = GetString(dict, "id");
        subscription.url = GURL(GetString(dict, "url"));
        if (subscription.id.empty() || !subscription.url.is_valid()) {
          continue;
        }
        subscription.title = GetString(dict, "title");
        subscription.homepage = GURL(GetString(dict, "homepage"));
        subscription.category = CategoryFromName(GetString(dict, "category"));
        subscription.enabled = dict.FindBool("enabled").value_or(false);
        subscription.builtin = dict.FindBool("builtin").value_or(false);
        subscription.last_updated = TimeFromValue(dict.FindDouble("lastUpdated"));
        subscription.last_attempt = TimeFromValue(dict.FindDouble("lastAttempt"));
        if (std::optional<double> expires = dict.FindDouble("expires");
            expires && *expires > 0) {
          subscription.expires = base::Seconds(*expires);
        }
        subscription.etag = GetString(dict, "etag");
        subscription.last_modified = GetString(dict, "lastModified");
        subscription.version = GetString(dict, "version");
        subscription.last_error = GetString(dict, "lastError");
        subscription.content_hash = GetString(dict, "contentHash");
        saved.push_back(std::move(subscription));
      }
    }
  }
  std::sort(allowlist_.begin(), allowlist_.end());
  allowlist_.erase(std::unique(allowlist_.begin(), allowlist_.end()),
                   allowlist_.end());

  if (!local_lists_.empty()) {
    // 只用本地文件（测试）。
    for (size_t i = 0; i < local_lists_.size(); ++i) {
      Subscription subscription;
      subscription.id = "local-" + base::NumberToString(i);
      subscription.url = net::FilePathToFileURL(local_lists_[i]);
      subscription.title = local_lists_[i].BaseName().AsUTF8Unsafe();
      subscription.enabled = true;
      subscription.local = true;
      subscription.has_text = true;
      subscriptions_.push_back(std::move(subscription));
    }
    enabled_ = true;
  } else {
    // 内置的按目录更新网址和说明，保留开关和更新情况；目录里新加的按默认开关加入；
    // 目录里去掉的丢掉；自己添加的原样放在后面。
    for (const CatalogEntry& entry : kCatalog) {
      Subscription subscription = FromCatalog(entry);
      auto it = std::find_if(saved.begin(), saved.end(), [&](const auto& s) {
        return s.builtin && s.id == subscription.id;
      });
      if (it != saved.end()) {
        Subscription existing = *it;
        if (existing.url != subscription.url) {
          existing.last_updated = base::Time();
          existing.etag.clear();
          existing.last_modified.clear();
        }
        existing.url = subscription.url;
        existing.title = subscription.title;
        existing.title_key = subscription.title_key;
        existing.summary_key = subscription.summary_key;
        existing.homepage = subscription.homepage;
        existing.category = subscription.category;
        subscription = std::move(existing);
      } else if (const char* leader = LeaderOf(subscription.id)) {
        // 目录里新加的、跟着别的列表开的：那个开着就一起开（以前就开了 uBlock 过滤器的
        // 升级以后也有快速修复）。
        for (const Subscription& added : subscriptions_) {
          if (added.id == leader && added.enabled) {
            subscription.enabled = true;
          }
        }
      }
      subscriptions_.push_back(std::move(subscription));
    }
    for (Subscription& subscription : saved) {
      if (!subscription.builtin) {
        subscription.category = Category::kCustom;
        subscriptions_.push_back(std::move(subscription));
      }
    }
    for (Subscription& subscription : subscriptions_) {
      subscription.has_text =
          std::find(disk.lists.begin(), disk.lists.end(), subscription.id) !=
          disk.lists.end();
    }
  }
  if (enabled_) {
    Rebuild();
    StartUpdates();
  } else {
    ready_ = true;
    PublishConfig();
  }
  Changed();
}

base::DictValue ContentBlockingService::SerializeState() const {
  base::ListValue list;
  for (const Subscription& subscription : subscriptions_) {
    base::DictValue dict;
    dict.Set("id", subscription.id);
    dict.Set("url", subscription.url.spec());
    dict.Set("title", subscription.title);
    if (subscription.homepage.is_valid()) {
      dict.Set("homepage", subscription.homepage.spec());
    }
    dict.Set("category", CategoryName(subscription.category));
    dict.Set("enabled", subscription.enabled);
    dict.Set("builtin", subscription.builtin);
    dict.Set("lastUpdated", TimeValue(subscription.last_updated));
    dict.Set("lastAttempt", TimeValue(subscription.last_attempt));
    if (subscription.expires) {
      dict.Set("expires", subscription.expires->InSecondsF());
    }
    dict.Set("etag", subscription.etag);
    dict.Set("lastModified", subscription.last_modified);
    dict.Set("version", subscription.version);
    dict.Set("lastError", subscription.last_error);
    dict.Set("contentHash", subscription.content_hash);
    list.Append(std::move(dict));
  }
  base::ListValue allowlist;
  for (const std::string& site : allowlist_) {
    allowlist.Append(site);
  }
  base::DictValue state;
  state.Set("enabled", enabled_);
  state.Set("stripTrackingParameters", strip_tracking_parameters_);
  state.Set("allowlist", std::move(allowlist));
  state.Set("subscriptions", std::move(list));
  return state;
}

void ContentBlockingService::SaveState() {
  if (!loaded_ || !local_lists_.empty()) {
    return;
  }
  std::string json;
  base::JSONWriter::WriteWithOptions(SerializeState(),
                                     base::JSONWriter::OPTIONS_PRETTY_PRINT,
                                     &json);
  file_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(base::IgnoreResult(&WriteText),
                     directory_.Append(kStateFile), std::move(json)));
}

void ContentBlockingService::Changed() {
  for (Observer& observer : observers_) {
    observer.OnContentBlockingChanged();
  }
}

void ContentBlockingService::PublishConfig() {
  SetCurrentFilterConfig(base::MakeRefCounted<FilterConfig>(
      engine_, enabled_, ready_ || !enabled_, strip_tracking_parameters_,
      allowlist_));
}

base::FilePath ContentBlockingService::ListFile(const std::string& id) const {
  return directory_.Append(kListsDirectory).AppendASCII(id + ".txt");
}

base::FilePath ContentBlockingService::TextFile(
    const Subscription& subscription) const {
  if (subscription.local) {
    base::FilePath path;
    net::FileURLToFilePath(subscription.url, &path);
    return path;
  }
  return ListFile(subscription.id);
}

Subscription* ContentBlockingService::Find(const std::string& id) {
  auto it = std::find_if(subscriptions_.begin(), subscriptions_.end(),
                         [&](const Subscription& s) { return s.id == id; });
  return it == subscriptions_.end() ? nullptr : &*it;
}

bool ContentBlockingService::HasListText(
    const Subscription& subscription) const {
  return subscription.has_text;
}

// ---- 开关 ----

void ContentBlockingService::SetEnabled(bool enabled) {
  if (enabled == enabled_) {
    return;
  }
  enabled_ = enabled;
  SaveState();
  if (enabled_) {
    if (!engine_) {
      ready_ = false;
    }
    PublishConfig();
    Rebuild();
    StartUpdates();
  } else {
    StopUpdates();
    rebuild_timer_.Stop();
    ++build_generation_;
    if (activity_ == Activity::kBuilding) {
      activity_ = Activity::kIdle;
    }
    PublishConfig();
  }
  Changed();
}

void ContentBlockingService::SetStripTrackingParameters(bool strip) {
  if (strip == strip_tracking_parameters_) {
    return;
  }
  strip_tracking_parameters_ = strip;
  SaveState();
  PublishConfig();
  Changed();
}

// ---- 订阅 ----

void ContentBlockingService::SetSubscriptionEnabled(const std::string& id,
                                                    bool enabled) {
  Subscription* subscription = Find(id);
  if (!subscription || subscription->enabled == enabled) {
    return;
  }
  std::vector<Subscription*> changed = {subscription};
  if (enabled && subscription->builtin) {
    for (const CompanionEntry& companion : kCompanions) {
      Subscription* follower =
          id == companion.leader ? Find(companion.id) : nullptr;
      if (follower && follower->builtin && !follower->enabled) {
        changed.push_back(follower);
      }
    }
  }
  int queued = 0;
  for (Subscription* list : changed) {
    list->enabled = enabled;
    if (enabled_ && enabled && !list->has_text && automatic_updates_) {
      download_queue_.push_back(list->id);
      ++queued;
    }
  }
  SaveState();
  if (enabled_) {
    if (queued == 0) {
      ScheduleRebuild(kRebuildDelay);
    } else if (!queue_running_) {
      downloads_done_ = 0;
      downloads_total_ = static_cast<int>(download_queue_.size());
      DownloadNext();
    } else {
      downloads_total_ += queued;
    }
  }
  Changed();
}

void ContentBlockingService::AddSubscription(
    const std::string& input,
    base::OnceCallback<void(std::string error)> callback) {
  std::string text(base::TrimWhitespaceASCII(input, base::TRIM_ALL));
  // Adblock Plus 的订阅链接 `abp:subscribe?location=…`。
  if (base::StartsWith(text, "abp:", base::CompareCase::INSENSITIVE_ASCII)) {
    const size_t query = text.find('?');
    if (query != std::string::npos) {
      for (std::string_view pair : base::SplitStringPiece(
               std::string_view(text).substr(query + 1), "&",
               base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
        if (pair.starts_with("location=")) {
          text = base::UnescapeBinaryURLComponent(pair.substr(9));
          break;
        }
      }
    }
  }
  const GURL url(text);
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS() || !url.has_host()) {
    std::move(callback).Run(
        base::UTF16ToUTF8(L("contentBlocking.add.error.invalidURL")));
    return;
  }
  for (const Subscription& existing : subscriptions_) {
    if (existing.url == url) {
      const std::string name =
          existing.title.empty() ? base::UTF16ToUTF8(L(existing.title_key))
                                 : existing.title;
      std::move(callback).Run(base::UTF16ToUTF8(
          L("contentBlocking.add.error.duplicate", {{"name", name}})));
      return;
    }
  }
  Subscription subscription;
  subscription.id = "custom-" + base::Uuid::GenerateRandomV4().AsLowercaseString();
  subscription.url = url;
  subscription.title = std::string(url.host());
  subscription.category = Category::kCustom;
  subscription.enabled = true;
  const std::string id = subscription.id;
  subscriptions_.push_back(std::move(subscription));
  Changed();
  Download(id, base::BindOnce(
                   [](base::WeakPtr<ContentBlockingService> self,
                      std::string id,
                      base::OnceCallback<void(std::string)> callback,
                      bool changed) {
                     if (!self) {
                       return;
                     }
                     Subscription* added = self->Find(id);
                     if (!added) {
                       std::move(callback).Run(base::UTF16ToUTF8(
                           L("contentBlocking.add.error.removed")));
                       return;
                     }
                     if (!added->last_error.empty()) {
                       const std::string error = added->last_error;
                       self->RemoveSubscription(id);
                       std::move(callback).Run(error);
                       return;
                     }
                     self->SaveState();
                     self->ScheduleRebuild(base::TimeDelta());
                     std::move(callback).Run(std::string());
                   },
                   weak_factory_.GetWeakPtr(), id, std::move(callback)));
}

void ContentBlockingService::RemoveSubscription(const std::string& id) {
  auto it = std::find_if(subscriptions_.begin(), subscriptions_.end(),
                         [&](const Subscription& s) { return s.id == id; });
  if (it == subscriptions_.end() || it->builtin || it->local) {
    return;
  }
  const bool was_enabled = it->enabled && it->has_text;
  subscriptions_.erase(it);
  file_runner_->PostTask(
      FROM_HERE, base::BindOnce(base::IgnoreResult(&base::DeleteFile),
                                ListFile(id)));
  SaveState();
  if (was_enabled) {
    ScheduleRebuild(kRebuildDelay);
  }
  Changed();
}

void ContentBlockingService::UpdateNow() {
  CheckForUpdates(/*only_due=*/false);
}

// ---- 自定义规则、不拦截的网站 ----

void ContentBlockingService::SetCustomRules(const std::string& text) {
  if (text == custom_rules_) {
    return;
  }
  custom_rules_ = text;
  if (local_lists_.empty()) {
    const base::FilePath path = directory_.Append(kCustomRulesFile);
    if (text.empty()) {
      file_runner_->PostTask(
          FROM_HERE,
          base::BindOnce(base::IgnoreResult(&base::DeleteFile), path));
    } else {
      file_runner_->PostTask(
          FROM_HERE, base::BindOnce(base::IgnoreResult(&WriteText), path, text));
    }
  }
  ScheduleRebuild(kRebuildDelay);
  Changed();
}

bool ContentBlockingService::IsAllowlisted(const GURL& url) const {
  if (!url.has_host()) {
    return false;
  }
  std::string_view host = url.host();
  for (const std::string& site : allowlist_) {
    if (host == site ||
        (host.size() > site.size() && host.ends_with(site) &&
         host[host.size() - site.size() - 1] == '.')) {
      return true;
    }
  }
  return false;
}

void ContentBlockingService::SetAllowlisted(const std::string& input,
                                            bool allowlisted) {
  // 接受网站名，也接受整个网址。
  std::string host = input;
  if (const GURL url(input); url.is_valid() && url.has_host()) {
    host = std::string(url.host());
  } else if (const GURL with_scheme("https://" + input);
             with_scheme.is_valid() && with_scheme.has_host()) {
    host = std::string(with_scheme.host());
  }
  const std::string normalized =
      content_filter::NormalizeHost(host, /*strip_www=*/false);
  if (normalized.empty()) {
    return;
  }
  const std::string site = normalized.starts_with("www.") && normalized.size() > 4
                               ? normalized.substr(4)
                               : normalized;
  if (allowlisted) {
    if (std::find(allowlist_.begin(), allowlist_.end(), site) !=
        allowlist_.end()) {
      return;
    }
    allowlist_.push_back(site);
    std::sort(allowlist_.begin(), allowlist_.end());
  } else {
    std::erase_if(allowlist_, [&](const std::string& entry) {
      return entry == site || normalized == entry ||
             (normalized.size() > entry.size() && normalized.ends_with(entry) &&
              normalized[normalized.size() - entry.size() - 1] == '.');
    });
  }
  SaveState();
  PublishConfig();
  Changed();
}

// ---- 建引擎 ----

void ContentBlockingService::ScheduleRebuild(base::TimeDelta delay) {
  if (!enabled_) {
    return;
  }
  rebuild_timer_.Start(FROM_HERE, delay,
                       base::BindOnce(&ContentBlockingService::Rebuild,
                                      base::Unretained(this)));
}

void ContentBlockingService::Rebuild() {
  rebuild_timer_.Stop();
  if (!enabled_ || !loaded_) {
    return;
  }
  const int generation = ++build_generation_;
  std::vector<EngineList> files;
  for (const Subscription& subscription : subscriptions_) {
    if (subscription.enabled && subscription.has_text) {
      // 内置的 uBlock Origin 列表、测试用的本地文件信得过；用户加的订阅不算。
      const bool trusted =
          subscription.local ||
          (subscription.builtin && subscription.id.starts_with("ublock-"));
      files.push_back({subscription.id, TextFile(subscription), trusted});
    }
  }
  if (activity_ == Activity::kIdle) {
    activity_ = Activity::kBuilding;
    Changed();
  }
  build_started_ = base::TimeTicks::Now();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](std::vector<EngineList> files, std::string custom) {
            auto result = std::make_unique<BuildResult>();
            result->built = BuildEngine(std::move(files), std::move(custom));
            return result;
          },
          std::move(files), custom_rules_),
      base::BindOnce(&ContentBlockingService::OnBuilt,
                     weak_factory_.GetWeakPtr(), generation));
}

void ContentBlockingService::OnBuilt(int generation,
                                     std::unique_ptr<BuildResult> result) {
  if (generation != build_generation_ || !enabled_) {
    return;
  }
  BuiltEngine& built = result->built;
  engine_ = built.engine;
  custom_rule_count_ = 0;
  custom_skipped_ = 0;
  for (Subscription& subscription : subscriptions_) {
    subscription.has_stats = false;
  }
  std::vector<std::string> list_ids;
  for (const content_filter::FilterListStats& stats : built.stats) {
    if (stats.id == "custom") {
      custom_rule_count_ = stats.rules();
      custom_skipped_ = stats.skipped;
      continue;
    }
    list_ids.push_back(stats.id);
    if (Subscription* subscription = Find(stats.id)) {
      subscription->has_stats = true;
      subscription->rules = stats.rules();
      subscription->skipped = stats.skipped;
    }
  }
  total_rules_ = engine_->rule_count();
  last_build_ = base::Time::Now();
  ready_ = true;
  if (activity_ == Activity::kBuilding) {
    activity_ = Activity::kIdle;
  }
  PublishConfig();
  Changed();
  if (!local_lists_.empty()) {
    LOG(WARNING) << "[starry-content-blocking] ready rules=" << total_rules_
                 << " network=" << engine_->network_rule_count()
                 << " cosmetic=" << engine_->cosmetic_rule_count()
                 << " scriptlet=" << engine_->scriptlet_rule_count()
                 << " generic=" << engine_->generic_selectors().size()
                 << " lists=" << base::JoinString(list_ids, ",")
                 << " build_ms=" << built.elapsed.InMilliseconds();
  }
}

// ---- 下载、更新 ----

void ContentBlockingService::StartUpdates() {
  if (!automatic_updates_ || update_timer_.IsRunning()) {
    return;
  }
  // 有启用了还没下载的（第一次开启）马上下；否则等启动忙完再查。
  const bool missing = std::any_of(
      subscriptions_.begin(), subscriptions_.end(),
      [](const Subscription& s) { return s.enabled && !s.has_text; });
  startup_update_timer_.Start(
      FROM_HERE, missing ? base::TimeDelta() : kStartupUpdateDelay,
      base::BindOnce(&ContentBlockingService::CheckForUpdates,
                     base::Unretained(this), /*only_due=*/true));
  update_timer_.Start(
      FROM_HERE, kUpdateCheckInterval,
      base::BindRepeating(&ContentBlockingService::CheckForUpdates,
                          base::Unretained(this), /*only_due=*/true));
}

void ContentBlockingService::StopUpdates() {
  startup_update_timer_.Stop();
  update_timer_.Stop();
}

void ContentBlockingService::CheckForUpdates(bool only_due) {
  if (!enabled_ || !automatic_updates_ || queue_running_) {
    return;
  }
  const base::Time now = base::Time::Now();
  download_queue_.clear();
  for (const Subscription& subscription : subscriptions_) {
    if (subscription.enabled && !subscription.local &&
        (!only_due || !subscription.has_text || subscription.IsDue(now))) {
      download_queue_.push_back(subscription.id);
    }
  }
  if (download_queue_.empty()) {
    return;
  }
  downloads_done_ = 0;
  downloads_total_ = static_cast<int>(download_queue_.size());
  queue_changed_ = false;
  DownloadNext();
}

void ContentBlockingService::DownloadNext() {
  if (download_queue_.empty() || !enabled_) {
    download_queue_.clear();
    queue_running_ = false;
    activity_ = Activity::kIdle;
    Changed();
    if (queue_changed_) {
      queue_changed_ = false;
      ScheduleRebuild(base::TimeDelta());
    }
    return;
  }
  queue_running_ = true;
  activity_ = Activity::kDownloading;
  Changed();
  const std::string id = download_queue_.front();
  download_queue_.erase(download_queue_.begin());
  Download(id, base::BindOnce(
                   [](base::WeakPtr<ContentBlockingService> self, bool changed) {
                     if (!self) {
                       return;
                     }
                     self->queue_changed_ |= changed;
                     ++self->downloads_done_;
                     self->DownloadNext();
                   },
                   weak_factory_.GetWeakPtr()));
}

void ContentBlockingService::Download(
    const std::string& id,
    base::OnceCallback<void(bool changed)> done) {
  Subscription* subscription = Find(id);
  if (!subscription || subscription->local) {
    std::move(done).Run(false);
    return;
  }
  subscription->last_attempt = base::Time::Now();
  auto download = std::make_unique<ListDownload>(
      subscription->url,
      subscription->has_text ? subscription->etag : std::string(),
      subscription->has_text ? subscription->last_modified : std::string(),
      base::BindOnce(
          [](base::WeakPtr<ContentBlockingService> self, std::string id,
             base::OnceCallback<void(bool)> done, ListDownload::Result result) {
            if (!self) {
              return;
            }
            self->OnSubscriptionTextReady(id, std::move(done), result.status,
                                          std::move(result.text),
                                          std::move(result.etag),
                                          std::move(result.last_modified),
                                          std::move(result.error));
          },
          weak_factory_.GetWeakPtr(), id, std::move(done)));
  ListDownload* raw = download.get();
  downloads_[id] = std::move(download);
  raw->Start();
}

void ContentBlockingService::OnSubscriptionTextReady(
    const std::string& id,
    base::OnceCallback<void(bool changed)> done,
    int status,
    std::string text,
    std::string etag,
    std::string last_modified,
    std::string error) {
  // 下载器在它自己的回调里，晚一步再删。
  if (auto it = downloads_.find(id); it != downloads_.end()) {
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(it->second));
    downloads_.erase(it);
  }
  Subscription* subscription = Find(id);
  if (!subscription) {
    std::move(done).Run(false);
    return;
  }
  if (status == 0) {
    // 失败时继续用旧的，记下原因，1 小时后再试。
    subscription->last_error = std::move(error);
    SaveState();
    Changed();
    std::move(done).Run(false);
    return;
  }
  subscription->last_error.clear();
  subscription->last_updated = base::Time::Now();
  if (status == net::HTTP_NOT_MODIFIED) {
    SaveState();
    Changed();
    std::move(done).Run(false);
    return;
  }
  const content_filter::FilterListMetadata metadata =
      content_filter::ParseMetadata(text);
  subscription->etag = std::move(etag);
  subscription->last_modified = std::move(last_modified);
  subscription->expires = metadata.expires;
  subscription->version = metadata.version.value_or("");
  if (!subscription->builtin) {
    if (metadata.title && !metadata.title->empty()) {
      subscription->title = *metadata.title;
    }
    if (metadata.homepage && GURL(*metadata.homepage).is_valid()) {
      subscription->homepage = GURL(*metadata.homepage);
    }
  }
  const std::string hash = ContentHash(text);
  const bool changed = hash != subscription->content_hash || !subscription->has_text;
  subscription->content_hash = hash;
  file_runner_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&WriteText, ListFile(id), std::move(text)),
      base::BindOnce(
          [](base::WeakPtr<ContentBlockingService> self, std::string id,
             bool changed, base::OnceCallback<void(bool)> done, bool written) {
            if (!self) {
              return;
            }
            if (Subscription* subscription = self->Find(id); subscription) {
              if (written) {
                subscription->has_text = true;
              } else {
                subscription->last_error = "write failed";
              }
            }
            self->SaveState();
            self->Changed();
            std::move(done).Run(changed && written);
          },
          weak_factory_.GetWeakPtr(), id, changed, std::move(done)));
}

void ContentBlockingService::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void ContentBlockingService::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

}  // namespace starry::content_blocking
