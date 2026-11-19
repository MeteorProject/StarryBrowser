// Copyright 2026 The Starry Browser Authors

#include "starry/browser/tab_organizer/tab_archive.h"

#include <algorithm>
#include <set>
#include <utility>

#include "base/base64.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/i18n/case_conversion.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/pickle.h"
#include "base/rand_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/supports_user_data.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_tabrestore.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/thumbnails/thumbnail_image.h"
#include "chrome/browser/ui/thumbnails/thumbnail_tab_helper.h"
#include "components/sessions/content/content_serialized_navigation_builder.h"
#include "components/sessions/core/serialized_user_agent_override.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/starry_hooks.h"
#include "starry/common/internal_urls.h"

namespace starry::tab_organizer {

namespace {

constexpr base::FilePath::CharType kFileName[] = FILE_PATH_LITERAL("Tab Archive");
constexpr base::FilePath::CharType kThumbnailDir[] =
    FILE_PATH_LITERAL("Tab Archive Thumbnails");
// 最多留几个缩略图（一个几十 KB，最新的那些有）。
constexpr size_t kMaxThumbnails = 600;
constexpr char kUserDataKey[] = "starry.tab_archive";
constexpr int kVersion = 1;
// 最多存多少个（多的删掉最旧的）。
constexpr size_t kMaxTabs = 3000;
// 一个标签存当前这一条前面的几条、后面的几条前进后退。
constexpr int kBackEntries = 6;
constexpr int kForwardEntries = 2;
// 归档多久以后只留当前这一条前进后退（文件不会太大）。
constexpr base::TimeDelta kKeepHistoryFor = base::Days(7);
// 一条前进后退最多写多少字节（不存网页的状态，一般远小于这个）。
constexpr int kMaxNavigationBytes = 16 * 1024;
// 改了以后多久写盘。
constexpr base::TimeDelta kWriteDelay = base::Seconds(2);

class ArchiveHolder : public base::SupportsUserData::Data {
 public:
  explicit ArchiveHolder(const base::FilePath& path) : archive(path) {}
  TabArchive archive;
};

const char* ReasonName(ArchiveReason reason) {
  switch (reason) {
    case ArchiveReason::kIdle:
      return "idle";
    case ArchiveReason::kDuplicate:
      return "duplicate";
    case ArchiveReason::kManual:
      return "manual";
    case ArchiveReason::kAssistant:
      return "assistant";
  }
  return "idle";
}

ArchiveReason ReasonFromName(std::string_view name) {
  if (name == "duplicate") {
    return ArchiveReason::kDuplicate;
  }
  if (name == "manual") {
    return ArchiveReason::kManual;
  }
  if (name == "assistant") {
    return ArchiveReason::kAssistant;
  }
  return ArchiveReason::kIdle;
}

base::Time TimeFrom(std::optional<double> value) {
  return value ? base::Time::FromMillisecondsSinceUnixEpoch(*value)
               : base::Time();
}

std::string NewId() {
  return base::HexEncode(base::RandBytesAsVector(8));
}

// 缩略图的文件名是 ID：只接受自己生成的那种（十六进制）。
bool IsFileSafeId(std::string_view id) {
  return !id.empty() && id.size() <= 32 &&
         std::ranges::all_of(id, base::IsHexDigit<char>);
}

void WriteThumbnailFile(const base::FilePath& dir,
                        const base::FilePath& path,
                        scoped_refptr<base::RefCountedBytes> data) {
  if (base::CreateDirectory(dir)) {
    base::WriteFile(path, data->as_vector());
  }
}

void DeleteFiles(const std::vector<base::FilePath>& paths) {
  for (const base::FilePath& path : paths) {
    base::DeleteFile(path);
  }
}

void SweepThumbnailDir(const base::FilePath& dir,
                       const std::set<std::string>& keep) {
  base::FileEnumerator files(dir, /*recursive=*/false,
                             base::FileEnumerator::FILES,
                             FILE_PATH_LITERAL("*.jpg"));
  for (base::FilePath path = files.Next(); !path.empty(); path = files.Next()) {
    if (!keep.contains(path.BaseName().RemoveExtension().MaybeAsASCII())) {
      base::DeleteFile(path);
    }
  }
}

scoped_refptr<base::RefCountedMemory> ReadThumbnailFile(
    const base::FilePath& path) {
  std::string data;
  if (path.empty() || !base::ReadFileToString(path, &data)) {
    return nullptr;
  }
  return base::MakeRefCounted<base::RefCountedString>(std::move(data));
}

// 标签预览存的缩略图（JPEG）；没显示过的、没载入的没有。
scoped_refptr<base::RefCountedBytes> CaptureThumbnail(
    content::WebContents* contents) {
  tabs::TabInterface* tab = tabs::TabInterface::MaybeGetFromContents(contents);
  ThumbnailTabHelper* helper = tab ? ThumbnailTabHelper::From(tab) : nullptr;
  scoped_refptr<ThumbnailImage> image = helper ? helper->thumbnail() : nullptr;
  if (!image || !image->has_data()) {
    return nullptr;
  }
  // 压缩好的数据只交给订阅的人（同步地）。
  ThumbnailImage::CompressedThumbnailData data;
  std::unique_ptr<ThumbnailImage::Subscription> subscription =
      image->Subscribe();
  subscription->SetCompressedImageCallback(base::BindRepeating(
      [](ThumbnailImage::CompressedThumbnailData* out,
         ThumbnailImage::CompressedThumbnailData data) {
        *out = std::move(data);
      },
      base::Unretained(&data)));
  image->RequestCompressedThumbnailData();
  subscription.reset();
  if (!data || data->data.empty()) {
    return nullptr;
  }
  return base::MakeRefCounted<base::RefCountedBytes>(data->data);
}

std::string EncodeNavigation(const sessions::SerializedNavigationEntry& entry) {
  base::Pickle pickle;
  entry.WriteToPickle(kMaxNavigationBytes, &pickle);
  return base::Base64Encode(pickle.AsBytes());
}

std::optional<sessions::SerializedNavigationEntry> DecodeNavigation(
    const std::string& encoded) {
  std::optional<std::vector<uint8_t>> bytes = base::Base64Decode(encoded);
  if (!bytes) {
    return std::nullopt;
  }
  base::PickleIterator iterator = base::PickleIterator::WithData(*bytes);
  sessions::SerializedNavigationEntry entry;
  if (!entry.ReadFromPickle(&iterator)) {
    return std::nullopt;
  }
  return entry;
}

// 只有网址和标题的一条（标签里没有前进后退时、读进来的坏了时）。
sessions::SerializedNavigationEntry MakeNavigation(const GURL& url,
                                                   const std::u16string& title) {
  std::unique_ptr<content::NavigationEntry> entry =
      content::NavigationEntry::Create();
  entry->SetURL(url);
  entry->SetVirtualURL(url);
  entry->SetTitle(title);
  return sessions::ContentSerializedNavigationBuilder::FromNavigationEntry(
      0, entry.get());
}

// 只留当前这一条前进后退。
void TrimHistory(ArchivedTab& tab) {
  if (tab.navigations.size() <= 1) {
    return;
  }
  const size_t current = std::clamp<size_t>(tab.current, 0,
                                            tab.navigations.size() - 1);
  sessions::SerializedNavigationEntry entry =
      std::move(tab.navigations[current]);
  entry.set_index(0);
  tab.navigations.clear();
  tab.navigations.push_back(std::move(entry));
  tab.current = 0;
}

base::DictValue ToValue(const ArchivedTab& tab) {
  base::ListValue navigations;
  for (const sessions::SerializedNavigationEntry& entry : tab.navigations) {
    navigations.Append(EncodeNavigation(entry));
  }
  base::DictValue extra;
  for (const auto& [key, value] : tab.extra_data) {
    extra.Set(key, value);
  }
  return base::DictValue()
      .Set("id", tab.id)
      .Set("url", tab.url.spec())
      .Set("title", tab.title)
      .Set("lastActive", tab.last_active.InMillisecondsFSinceUnixEpoch())
      .Set("archived", tab.archived.InMillisecondsFSinceUnixEpoch())
      .Set("reason", ReasonName(tab.reason))
      .Set("batch", tab.batch)
      .Set("navigations", std::move(navigations))
      .Set("current", tab.current)
      .Set("extra", std::move(extra))
      .Set("thumbnail", tab.thumbnail)
      .Set("topic", tab.topic)
      .Set("topicTried", tab.topic_tried);
}

std::optional<ArchivedTab> FromValue(const base::DictValue& value) {
  const std::string* id = value.FindString("id");
  const std::string* url = value.FindString("url");
  if (!id || !url || !GURL(*url).is_valid()) {
    return std::nullopt;
  }
  ArchivedTab tab;
  tab.id = *id;
  tab.url = GURL(*url);
  if (const std::string* title = value.FindString("title")) {
    tab.title = base::UTF8ToUTF16(*title);
  }
  tab.last_active = TimeFrom(value.FindDouble("lastActive"));
  tab.archived = TimeFrom(value.FindDouble("archived"));
  if (const std::string* reason = value.FindString("reason")) {
    tab.reason = ReasonFromName(*reason);
  }
  if (const std::string* batch = value.FindString("batch")) {
    tab.batch = *batch;
  }
  if (const base::ListValue* navigations = value.FindList("navigations")) {
    for (const base::Value& encoded : *navigations) {
      if (!encoded.is_string()) {
        continue;
      }
      if (std::optional<sessions::SerializedNavigationEntry> entry =
              DecodeNavigation(encoded.GetString())) {
        entry->set_index(static_cast<int>(tab.navigations.size()));
        tab.navigations.push_back(std::move(*entry));
      }
    }
  }
  tab.current = value.FindInt("current").value_or(0);
  tab.thumbnail = value.FindBool("thumbnail").value_or(false);
  if (const std::string* topic = value.FindString("topic")) {
    tab.topic = base::UTF8ToUTF16(*topic);
  }
  tab.topic_tried =
      value.FindBool("topicTried").value_or(false) || !tab.topic.empty();
  if (tab.navigations.empty()) {
    tab.navigations.push_back(MakeNavigation(tab.url, tab.title));
    tab.current = 0;
  }
  tab.current = std::clamp(tab.current, 0,
                           static_cast<int>(tab.navigations.size()) - 1);
  if (const base::DictValue* extra = value.FindDict("extra")) {
    for (const auto [key, item] : *extra) {
      if (item.is_string()) {
        tab.extra_data[key] = item.GetString();
      }
    }
  }
  return tab;
}

std::optional<std::string> ReadFile(const base::FilePath& path) {
  std::string data;
  if (!base::ReadFileToString(path, &data)) {
    return std::nullopt;
  }
  return data;
}

}  // namespace

ArchivedTab::ArchivedTab() = default;
ArchivedTab::ArchivedTab(const ArchivedTab&) = default;
ArchivedTab::ArchivedTab(ArchivedTab&&) = default;
ArchivedTab& ArchivedTab::operator=(const ArchivedTab&) = default;
ArchivedTab& ArchivedTab::operator=(ArchivedTab&&) = default;
ArchivedTab::~ArchivedTab() = default;

// static
TabArchive* TabArchive::ForProfile(Profile* profile) {
  Profile* original = profile->GetOriginalProfile();
  auto* holder =
      static_cast<ArchiveHolder*>(original->GetUserData(kUserDataKey));
  if (!holder) {
    auto created =
        std::make_unique<ArchiveHolder>(original->GetPath().Append(kFileName));
    holder = created.get();
    original->SetUserData(kUserDataKey, std::move(created));
  }
  return &holder->archive;
}

TabArchive::TabArchive(const base::FilePath& path)
    : path_(path),
      thumbnail_dir_(path.DirName().Append(kThumbnailDir)),
      file_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})),
      writer_(path, file_runner_, kWriteDelay) {
  file_runner_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&ReadFile, path_),
      base::BindOnce(&TabArchive::OnLoaded, weak_factory_.GetWeakPtr()));
}

TabArchive::~TabArchive() {
  if (writer_.HasPendingWrite()) {
    writer_.DoScheduledWrite();
  }
}

// static
ArchivedTab TabArchive::Capture(content::WebContents* contents,
                                ArchiveReason reason) {
  ArchivedTab tab;
  tab.id = NewId();
  tab.reason = reason;
  tab.archived = base::Time::Now();
  tab.last_active = contents->GetLastActiveTime();
  tab.title = contents->GetTitle();
  content::NavigationController& controller = contents->GetController();
  const int count = controller.GetEntryCount();
  const int current = controller.GetLastCommittedEntryIndex();
  if (count > 0 && current >= 0 && current < count) {
    const int first = std::max(0, current - kBackEntries);
    const int last = std::min(count, current + kForwardEntries + 1);
    for (int i = first; i < last; ++i) {
      sessions::SerializedNavigationEntry entry =
          sessions::ContentSerializedNavigationBuilder::FromNavigationEntry(
              i - first, controller.GetEntryAtIndex(i));
      // 不存网页的状态（表单、滚动的位置）：文件小一些，恢复时照网址重新载入。
      entry.set_encoded_page_state(std::string());
      tab.navigations.push_back(std::move(entry));
    }
    tab.current = current - first;
    tab.url = controller.GetEntryAtIndex(current)->GetVirtualURL();
  }
  if (!tab.url.is_valid()) {
    tab.url = contents->GetVisibleURL();
  }
  if (tab.navigations.empty()) {
    tab.navigations.push_back(MakeNavigation(tab.url, tab.title));
    tab.current = 0;
  }
  AddTabExtraData(contents, /*pinned=*/false, &tab.extra_data);
  tab.thumbnail_data = CaptureThumbnail(contents);
  return tab;
}

// static
content::WebContents* TabArchive::Open(BrowserWindowInterface* browser,
                                       const ArchivedTab& tab,
                                       int index,
                                       bool select) {
  TabStripModel* model = browser->GetTabStripModel();
  if (index < 0 || index > model->count()) {
    index = model->count();
  }
  index = std::max(index, model->IndexOfFirstNonPinnedTab());
  const int current = std::clamp(
      tab.current, 0, static_cast<int>(tab.navigations.size()) - 1);
  // 最后一次用的时间是现在：刚恢复的不能马上又被当成闲置的。
  return chrome::AddRestoredTab(
      browser, tab.navigations, index, current, std::string(), std::nullopt,
      select, /*pin=*/false, base::TimeTicks::Now(), base::Time::Now(),
      /*session_storage_namespace=*/nullptr,
      sessions::SerializedUserAgentOverride(), tab.extra_data,
      /*from_session_restore=*/false, std::nullopt);
}

void TabArchive::Add(std::vector<ArchivedTab> tabs) {
  // 浏览器自己的页面（设置、历史记录……）不进归档。
  std::erase_if(tabs,
                [](const ArchivedTab& tab) { return IsInternalURL(tab.url); });
  if (tabs.empty()) {
    return;
  }
  // 新的在前：这一批按归档的先后倒过来放在最前面。
  std::stable_sort(tabs.begin(), tabs.end(),
                   [](const ArchivedTab& a, const ArchivedTab& b) {
                     return a.archived > b.archived;
                   });
  tabs_.insert(tabs_.begin(), std::make_move_iterator(tabs.begin()),
               std::make_move_iterator(tabs.end()));
  if (tabs_.size() > kMaxTabs) {
    std::vector<const ArchivedTab*> dropped;
    for (size_t i = kMaxTabs; i < tabs_.size(); ++i) {
      dropped.push_back(&tabs_[i]);
    }
    DropThumbnails(dropped);
    tabs_.resize(kMaxTabs);
  }
  StoreThumbnails();
  Changed();
}

std::optional<ArchivedTab> TabArchive::Take(const std::string& id) {
  auto it = std::ranges::find(tabs_, id, &ArchivedTab::id);
  if (it == tabs_.end()) {
    return std::nullopt;
  }
  DropThumbnails({&*it});
  ArchivedTab tab = std::move(*it);
  tab.thumbnail = false;
  tabs_.erase(it);
  Changed();
  return tab;
}

void TabArchive::Remove(const std::vector<std::string>& ids) {
  std::vector<const ArchivedTab*> dropped;
  for (const ArchivedTab& tab : tabs_) {
    if (std::ranges::find(ids, tab.id) != ids.end()) {
      dropped.push_back(&tab);
    }
  }
  if (dropped.empty()) {
    return;
  }
  DropThumbnails(dropped);
  std::erase_if(tabs_, [&](const ArchivedTab& tab) {
    return std::ranges::find(ids, tab.id) != ids.end();
  });
  Changed();
}

void TabArchive::SetTopics(
    const std::map<std::string, std::u16string>& topics) {
  bool changed = false;
  for (ArchivedTab& tab : tabs_) {
    auto found = topics.find(tab.id);
    if (found == topics.end()) {
      continue;
    }
    if (tab.topic != found->second || !tab.topic_tried) {
      tab.topic = found->second;
      tab.topic_tried = true;
      changed = true;
    }
  }
  if (changed) {
    Changed();
  }
}

void TabArchive::Clear() {
  if (tabs_.empty()) {
    return;
  }
  tabs_.clear();
  // 整个目录删掉（连上次没删成的）。
  file_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(base::IgnoreResult(&base::DeletePathRecursively),
                     thumbnail_dir_));
  Changed();
}

void TabArchive::Expire(base::TimeDelta retention) {
  const base::Time now = base::Time::Now();
  bool changed = false;
  if (retention.is_positive()) {
    const auto expired = [&](const ArchivedTab& tab) {
      return now - tab.archived > retention;
    };
    std::vector<const ArchivedTab*> dropped;
    for (const ArchivedTab& tab : tabs_) {
      if (expired(tab)) {
        dropped.push_back(&tab);
      }
    }
    DropThumbnails(dropped);
    std::erase_if(tabs_, expired);
    changed = !dropped.empty();
  }
  for (ArchivedTab& tab : tabs_) {
    if (tab.navigations.size() > 1 && now - tab.archived > kKeepHistoryFor) {
      TrimHistory(tab);
      changed = true;
    }
  }
  if (changed) {
    Changed();
  }
}

const ArchivedTab* TabArchive::Find(const std::string& id) const {
  auto it = std::ranges::find(tabs_, id, &ArchivedTab::id);
  return it == tabs_.end() ? nullptr : &*it;
}

std::vector<const ArchivedTab*> TabArchive::Search(std::u16string_view query,
                                                   size_t limit) const {
  std::vector<std::u16string> words;
  for (std::u16string_view word : base::SplitStringPiece(
           query, base::kWhitespaceUTF16, base::TRIM_WHITESPACE,
           base::SPLIT_WANT_NONEMPTY)) {
    words.push_back(base::i18n::ToLower(word));
  }
  std::vector<const ArchivedTab*> found;
  for (const ArchivedTab& tab : tabs_) {
    if (found.size() >= limit) {
      break;
    }
    if (!words.empty()) {
      const std::u16string text = base::i18n::ToLower(
          base::StrCat({tab.title, u" ", base::UTF8ToUTF16(tab.url.spec()),
                        u" ", tab.topic}));
      const bool all = std::ranges::all_of(words, [&](const auto& word) {
        return text.find(word) != std::u16string::npos;
      });
      if (!all) {
        continue;
      }
    }
    found.push_back(&tab);
  }
  return found;
}

void TabArchive::ReadThumbnail(
    const std::string& id,
    base::OnceCallback<void(scoped_refptr<base::RefCountedMemory>)> callback)
    const {
  const ArchivedTab* tab = Find(id);
  if (!tab || !tab->thumbnail) {
    std::move(callback).Run(nullptr);
    return;
  }
  file_runner_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&ReadThumbnailFile, ThumbnailPath(id)),
      std::move(callback));
}

base::FilePath TabArchive::ThumbnailPath(const std::string& id) const {
  if (!IsFileSafeId(id)) {
    return base::FilePath();
  }
  return thumbnail_dir_.AppendASCII(id + ".jpg");
}

void TabArchive::StoreThumbnails() {
  std::vector<base::FilePath> stale;
  size_t kept = 0;
  for (ArchivedTab& tab : tabs_) {
    if (!tab.thumbnail && !tab.thumbnail_data) {
      continue;
    }
    const base::FilePath path = ThumbnailPath(tab.id);
    if (kept >= kMaxThumbnails || path.empty()) {
      if (tab.thumbnail && !path.empty()) {
        stale.push_back(path);
      }
      tab.thumbnail = false;
      tab.thumbnail_data = nullptr;
      continue;
    }
    ++kept;
    if (tab.thumbnail_data) {
      file_runner_->PostTask(
          FROM_HERE, base::BindOnce(&WriteThumbnailFile, thumbnail_dir_, path,
                                    std::move(tab.thumbnail_data)));
      tab.thumbnail = true;
    }
  }
  if (!stale.empty()) {
    file_runner_->PostTask(FROM_HERE,
                           base::BindOnce(&DeleteFiles, std::move(stale)));
  }
}

void TabArchive::DropThumbnails(const std::vector<const ArchivedTab*>& tabs) {
  std::vector<base::FilePath> paths;
  for (const ArchivedTab* tab : tabs) {
    if (tab->thumbnail) {
      if (base::FilePath path = ThumbnailPath(tab->id); !path.empty()) {
        paths.push_back(std::move(path));
      }
    }
  }
  if (!paths.empty()) {
    file_runner_->PostTask(FROM_HERE,
                           base::BindOnce(&DeleteFiles, std::move(paths)));
  }
}

void TabArchive::SweepThumbnails() {
  std::set<std::string> keep;
  for (const ArchivedTab& tab : tabs_) {
    if (tab.thumbnail) {
      keep.insert(tab.id);
    }
  }
  file_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&SweepThumbnailDir, thumbnail_dir_, std::move(keep)));
}

void TabArchive::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void TabArchive::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

std::string TabArchive::DescribeForTesting() const {
  std::string text = base::StrCat(
      {"archive loaded=", loaded_ ? "1" : "0", " count=",
       base::NumberToString(tabs_.size()), "\n"});
  const base::Time now = base::Time::Now();
  for (const ArchivedTab& tab : tabs_) {
    const std::string topic =
        tab.topic_tried
            ? base::StrCat({" topic=\"", base::UTF16ToUTF8(tab.topic), "\""})
            : std::string();
    base::StrAppend(
        &text,
        {"  archived \"", base::UTF16ToUTF8(tab.title), "\" ", tab.url.spec(),
         " reason=", ReasonName(tab.reason), " batch=", tab.batch, " age_s=",
         base::NumberToString((now - tab.archived).InSeconds()), " idle_h=",
         base::NumberToString((tab.archived - tab.last_active).InHours()),
         " navs=", base::NumberToString(tab.navigations.size()),
         " current=", base::NumberToString(tab.current),
         " thumb=", tab.thumbnail ? "1" : "0", topic, "\n"});
  }
  return text;
}

std::optional<std::string> TabArchive::SerializeData() {
  base::ListValue list;
  for (const ArchivedTab& tab : tabs_) {
    list.Append(ToValue(tab));
  }
  return base::WriteJson(
      base::DictValue().Set("version", kVersion).Set("tabs", std::move(list)));
}

void TabArchive::OnLoaded(std::optional<std::string> data) {
  std::vector<ArchivedTab> loaded;
  if (data) {
    if (std::optional<base::DictValue> root = base::JSONReader::ReadDict(
            *data, base::JSON_PARSE_CHROMIUM_EXTENSIONS)) {
      if (const base::ListValue* list = root->FindList("tabs")) {
        for (const base::Value& item : *list) {
          if (!item.is_dict()) {
            continue;
          }
          if (std::optional<ArchivedTab> tab = FromValue(item.GetDict())) {
            loaded.push_back(std::move(*tab));
          }
        }
      }
    }
  }
  // 以前归档的浏览器自己的页面拿掉（现在不进归档了）：缩略图在下面扫掉。
  const size_t before = loaded.size();
  std::erase_if(loaded,
                [](const ArchivedTab& tab) { return IsInternalURL(tab.url); });
  if (loaded.size() != before) {
    dirty_before_load_ = true;
  }
  loaded_ = true;
  // 读进来之前加的更新，排在前面。
  tabs_.insert(tabs_.end(), std::make_move_iterator(loaded.begin()),
               std::make_move_iterator(loaded.end()));
  if (tabs_.size() > kMaxTabs) {
    tabs_.resize(kMaxTabs);
  }
  // 读进来之前加的缩略图已经在写了（同一个序列上，先写后扫）。
  StoreThumbnails();
  SweepThumbnails();
  if (dirty_before_load_) {
    dirty_before_load_ = false;
    writer_.ScheduleWrite(this);
  }
  observers_.Notify(&Observer::OnArchiveChanged);
}

void TabArchive::Changed() {
  if (loaded_) {
    writer_.ScheduleWrite(this);
  } else {
    dirty_before_load_ = true;
  }
  observers_.Notify(&Observer::OnArchiveChanged);
}

}  // namespace starry::tab_organizer
