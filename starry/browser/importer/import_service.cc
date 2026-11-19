// Copyright 2026 The Starry Browser Authors

#include "starry/browser/importer/import_service.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/importer/external_process_importer_host.h"
#include "chrome/browser/importer/firefox_profile_lock.h"
#include "chrome/browser/importer/importer_list.h"
#include "chrome/browser/importer/profile_writer.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/search_engines/template_url_service_factory.h"
#include "components/history/core/browser/history_backend.h"
#include "components/history/core/browser/history_database.h"
#include "components/history/core/browser/history_db_task.h"
#include "components/history/core/browser/history_service.h"
#include "components/keyed_service/core/service_access_type.h"
#include "components/password_manager/core/common/password_manager_pref_names.h"
#include "components/prefs/pref_service.h"
#include "components/search_engines/template_url.h"
#include "components/search_engines/template_url_service.h"
#include "starry/browser/importer/chromium_browsers.h"
#include "starry/browser/l10n/l10n.h"

namespace starry {

namespace {

const char kUserDataKey = 0;

// 历史记录一次写多少次访问（写一批就让历史记录的线程去做别的，再接着写）。
constexpr size_t kHistoryBatchSize = 1000;

constexpr base::TaskTraits kBlockingTraits = {
    base::MayBlock(), base::TaskPriority::USER_VISIBLE,
    base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN};

std::u16string SourceName(const ImportSource& source) {
  if (source.profile.empty()) {
    return source.browser;
  }
  return L("settings.import.sourceWithProfile",
           {{"browser", source.browser}, {"profile", source.profile}});
}

size_t CountBookmarks(
    const std::vector<user_data_importer::ImportedBookmarkEntry>& entries) {
  return std::ranges::count_if(entries, [](const auto& entry) {
    return !entry.is_folder && entry.url.is_valid();
  });
}

// 把读出来的访问写进历史记录，同一个网址、同一时刻已经有的跳过（再导入一次不重复）。
// 在历史记录的线程上分批做。
class ImportHistoryTask : public history::HistoryDBTask {
 public:
  ImportHistoryTask(history::URLRows rows,
                    base::OnceCallback<void(size_t)> done)
      : rows_(std::move(rows)), done_(std::move(done)) {}

  bool RunOnDBThread(history::HistoryBackend* backend,
                     history::HistoryDatabase* db) override {
    const size_t end = std::min(next_ + kHistoryBatchSize, rows_.size());
    history::URLRows batch;
    for (; next_ < end; ++next_) {
      history::URLRow& row = rows_[next_];
      // 太早的（Chrome 只留 90 天）写了也会被清掉。
      if (backend->IsExpiredVisitTime(row.last_visit())) {
        continue;
      }
      history::URLRow existing;
      if (const history::URLID id = db->GetRowForURL(row.url(), &existing)) {
        auto [visits, inserted] = known_visits_.try_emplace(id);
        if (inserted) {
          history::VisitVector rows;
          db->GetVisitsForURL(id, &rows);
          for (const history::VisitRow& visit : rows) {
            visits->second.insert(visit.visit_time);
          }
        }
        if (!visits->second.insert(row.last_visit()).second) {
          continue;
        }
      }
      batch.push_back(std::move(row));
    }
    if (!batch.empty()) {
      added_ += batch.size();
      // 每一行变成一次访问（网址第一次出现时连同标题、访问次数、输入次数加进来）。
      backend->AddPagesWithDetails(batch, history::SOURCE_BROWSED);
    }
    return next_ >= rows_.size();
  }

  void DoneRunOnMainThread() override { std::move(done_).Run(added_); }

 private:
  history::URLRows rows_;
  base::OnceCallback<void(size_t)> done_;
  size_t next_ = 0;
  size_t added_ = 0;
  // 已经在历史记录里的网址的访问时刻。
  std::map<history::URLID, std::set<base::Time>> known_visits_;
};

// Firefox 的用户资料要有 places.sqlite（书签和历史记录）才有东西可导入；Firefox 自己会
// 建一个从来不用的 default。
struct BuiltinDetection {
  std::vector<ImportSource> sources;
  bool safari_needs_access = false;
};

base::Time ModifiedTime(const base::FilePath& path) {
  base::File::Info info;
  return base::GetFileInfo(path, &info) ? info.last_modified : base::Time();
}

BuiltinDetection DescribeBuiltinSources(
    std::vector<user_data_importer::SourceProfile> profiles) {
  BuiltinDetection result;
  size_t firefox_profiles = 0;
  for (user_data_importer::SourceProfile& profile : profiles) {
    ImportSource source;
    source.kind = ImportSource::Kind::kBuiltin;
    source.items = profile.services_supported;
    source.browser = profile.importer_name;
    switch (profile.importer_type) {
      case user_data_importer::TYPE_FIREFOX: {
        const base::FilePath places =
            profile.source_path.Append(FILE_PATH_LITERAL("places.sqlite"));
        if (!base::PathExists(places)) {
          continue;
        }
        source.id = "firefox:" + profile.source_path.BaseName().AsUTF8Unsafe();
        source.profile = profile.profile;
        source.last_used = ModifiedTime(places);
        ++firefox_profiles;
        break;
      }
#if BUILDFLAG(IS_MAC)
      case user_data_importer::TYPE_SAFARI:
        source.id = "safari";
        source.browser = u"Safari";
        source.last_used = ModifiedTime(base::GetHomeDir().Append(
            FILE_PATH_LITERAL("Library/Safari/History.db")));
        break;
#endif
#if BUILDFLAG(IS_WIN)
      case user_data_importer::TYPE_IE:
        source.id = "ie";
        break;
      case user_data_importer::TYPE_EDGE:
        source.id = "edge-legacy";
        break;
#endif
      default:
        // 书签文件另外选。
        continue;
    }
    source.builtin = std::move(profile);
    result.sources.push_back(std::move(source));
  }
  // 只有一个 Firefox 的用户资料时不显示它的名字（default-release）。
  if (firefox_profiles == 1) {
    for (ImportSource& source : result.sources) {
      if (source.builtin.importer_type == user_data_importer::TYPE_FIREFOX) {
        source.profile.clear();
      }
    }
  }
#if BUILDFLAG(IS_MAC)
  // Safari 的书签在 ~/Library/Safari 里，要「完全磁盘访问权限」才读得了（没有时 Chrome
  // 的导入器不列出 Safari）。
  if (!std::ranges::any_of(result.sources, [](const ImportSource& source) {
        return source.id == "safari";
      })) {
    base::File file(base::GetHomeDir().Append(
                        FILE_PATH_LITERAL("Library/Safari/Bookmarks.plist")),
                    base::File::FLAG_OPEN | base::File::FLAG_READ);
    result.safari_needs_access =
        !file.IsValid() &&
        file.error_details() == base::File::FILE_ERROR_ACCESS_DENIED;
  }
#endif
  return result;
}

}  // namespace

// Chrome 的导入器写进来的时候数一下（它自己不报数目）。
class CountingProfileWriter : public ProfileWriter {
 public:
  explicit CountingProfileWriter(Profile* profile)
      : ProfileWriter(profile), profile_(profile) {}

  void AddPasswordForm(const password_manager::PasswordForm& form) override {
    if (profile_->GetPrefs()->GetBoolean(
            password_manager::prefs::kCredentialsEnableService)) {
      ++passwords;
    }
    ProfileWriter::AddPasswordForm(form);
  }

  void AddHistoryPage(const history::URLRows& page,
                      history::VisitSource visit_source) override {
    history += page.size();
    ProfileWriter::AddHistoryPage(page, visit_source);
  }

  void AddBookmarks(
      const std::vector<user_data_importer::ImportedBookmarkEntry>& entries,
      const std::u16string& top_level_folder_name) override {
    bookmarks += CountBookmarks(entries);
    ProfileWriter::AddBookmarks(entries, top_level_folder_name);
  }

  void AddKeywords(TemplateURLService::OwnedTemplateURLVector template_urls,
                   bool unique_on_host_and_path) override {
    TemplateURLService* service =
        TemplateURLServiceFactory::GetForProfile(profile_);
    const size_t before = service->GetTemplateURLs().size();
    ProfileWriter::AddKeywords(std::move(template_urls),
                               unique_on_host_and_path);
    search_engines += service->GetTemplateURLs().size() - before;
  }

  void AddAutocompleteFormDataEntries(
      const std::vector<autofill::AutocompleteEntry>& entries) override {
    autofill += entries.size();
    ProfileWriter::AddAutocompleteFormDataEntries(entries);
  }

  size_t bookmarks = 0;
  size_t history = 0;
  size_t passwords = 0;
  size_t search_engines = 0;
  size_t autofill = 0;

 private:
  ~CountingProfileWriter() override = default;

  const raw_ptr<Profile> profile_;
};

ImportService::Job::Job() = default;
ImportService::Job::Job(const Job&) = default;
ImportService::Job& ImportService::Job::operator=(const Job&) = default;
ImportService::Job::~Job() = default;

// static
ImportService* ImportService::Get(Profile* profile) {
  Profile* original = profile->GetOriginalProfile();
  auto* service =
      static_cast<ImportService*>(original->GetUserData(&kUserDataKey));
  if (!service) {
    auto created = std::make_unique<ImportService>(original);
    service = created.get();
    original->SetUserData(&kUserDataKey, std::move(created));
  }
  return service;
}

ImportService::ImportService(Profile* profile) : profile_(profile) {}

ImportService::~ImportService() {
  // 同 Chrome 的 ImportDataHandler：还在导入的让它做完，不再听它的。
  if (importer_host_) {
    importer_host_->set_observer(nullptr);
    importer_host_ = nullptr;
  }
}

void ImportService::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void ImportService::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void ImportService::NotifyChanged() {
  observers_.Notify(&Observer::OnImportChanged);
}

// ---- 找来源 ----

void ImportService::DetectSources() {
  if (detecting()) {
    return;
  }
  found_sources_.clear();
  pending_detections_ = 1;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, kBlockingTraits, base::BindOnce(&DetectChromiumSources),
      base::BindOnce(&ImportService::OnChromiumDetected,
                     weak_factory_.GetWeakPtr()));
  // 未指定外部覆盖目录时，同时扫描系统内置导入器（Firefox、Safari）。
  if (!ImportHomeForTesting()) {
    ++pending_detections_;
    importer_list_ = std::make_unique<ImporterList>();
    importer_list_->DetectSourceProfiles(
        g_browser_process->GetApplicationLocale(),
        /*include_interactive_profiles=*/false,
        base::BindOnce(&ImportService::OnBuiltinDetected,
                       weak_factory_.GetWeakPtr()));
  }
  NotifyChanged();
}

void ImportService::OnChromiumDetected(std::vector<ImportSource> sources) {
  for (ImportSource& source : sources) {
    found_sources_.push_back(std::move(source));
  }
  DetectionDone();
}

void ImportService::OnBuiltinDetected() {
  std::vector<user_data_importer::SourceProfile> profiles;
  for (size_t i = 0; i < importer_list_->count(); ++i) {
    profiles.push_back(importer_list_->GetSourceProfileAt(i));
  }
  importer_list_.reset();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, kBlockingTraits,
      base::BindOnce(&DescribeBuiltinSources, std::move(profiles)),
      base::BindOnce(
          [](base::WeakPtr<ImportService> service, BuiltinDetection result) {
            if (!service) {
              return;
            }
            service->safari_needs_access_ = result.safari_needs_access;
            for (ImportSource& source : result.sources) {
              service->found_sources_.push_back(std::move(source));
            }
            service->DetectionDone();
          },
          weak_factory_.GetWeakPtr()));
}

void ImportService::DetectionDone() {
  if (--pending_detections_ > 0) {
    return;
  }
  // 最近用过的浏览器在前（大概就是用户平时用的）。
  sources_ = std::move(found_sources_);
  found_sources_.clear();
  std::ranges::stable_sort(sources_, std::ranges::greater(),
                           &ImportSource::last_used);
  detected_ = true;
  NotifyChanged();
}

// ---- 导入 ----

bool ImportService::Start(const std::string& source_id, uint16_t items) {
  if (running()) {
    return false;
  }
  const auto it = std::ranges::find(sources_, source_id, &ImportSource::id);
  if (it == sources_.end()) {
    return false;
  }
  items &= it->items;
  if (!items) {
    return false;
  }
  // 导入的时候列表可能重新找过，用一份拷贝。
  const ImportSource source = *it;
  job_.emplace();
  job_->source_id = source.id;
  job_->source_name = SourceName(source);
  job_->items = items;
  ++generation_;
  NotifyChanged();
  if (source.kind == ImportSource::Kind::kChromium) {
    StartChromium(source, items);
  } else {
    StartBuiltin(source.builtin, items);
  }
  return true;
}

bool ImportService::StartBookmarksFile(const base::FilePath& file) {
  if (running()) {
    return false;
  }
  user_data_importer::SourceProfile profile;
  profile.importer_type = user_data_importer::TYPE_BOOKMARKS_FILE;
  profile.importer_name = file.BaseName().LossyDisplayName();
  profile.source_path = file;
  profile.services_supported = user_data_importer::FAVORITES;
  job_.emplace();
  job_->source_id = "bookmarksFile";
  job_->source_name = profile.importer_name;
  job_->items = user_data_importer::FAVORITES;
  ++generation_;
  NotifyChanged();
  StartBuiltin(profile, user_data_importer::FAVORITES);
  return true;
}

void ImportService::ClearJob() {
  if (running()) {
    return;
  }
  job_.reset();
  NotifyChanged();
}

void ImportService::StartChromium(const ImportSource& source, uint16_t items) {
  ChromiumReadOptions options;
  options.items = items;
  options.bookmark_bar_name = L("bookmarks.folder.bookmarkBar");
  options.mobile_bookmarks_name = L("bookmarks.folder.mobile");
  options.on_item = base::BindPostTaskToCurrentDefault(
      base::BindRepeating(&ImportService::OnChromiumItemStarted,
                          weak_factory_.GetWeakPtr(), generation_));
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, kBlockingTraits,
      base::BindOnce(&ReadChromiumProfile, source, std::move(options)),
      base::BindOnce(&ImportService::OnChromiumRead, weak_factory_.GetWeakPtr(),
                     generation_));
}

void ImportService::OnChromiumItemStarted(int generation,
                                          user_data_importer::ImportItem item) {
  if (generation != generation_ || !running()) {
    return;
  }
  job_->current = item;
  NotifyChanged();
}

void ImportService::OnChromiumRead(int generation, ChromiumImportData data) {
  if (generation != generation_ || !running()) {
    return;
  }
  job_->undecryptable_passwords = data.undecryptable_passwords;
  job_->password_key_unavailable = data.password_key_unavailable;
  job_->failed_items = data.failed;
  pending_data_ = std::move(data);
  WriteWhenReady();
}

void ImportService::WriteWhenReady() {
  if (!pending_data_ || !running()) {
    return;
  }
  bookmarks::BookmarkModel* bookmark_model =
      BookmarkModelFactory::GetForBrowserContext(profile_);
  if (!pending_data_->bookmarks.empty() && !bookmark_model->loaded()) {
    if (!bookmark_observation_.IsObserving()) {
      bookmark_observation_.Observe(bookmark_model);
    }
    return;
  }
  TemplateURLService* template_urls =
      TemplateURLServiceFactory::GetForProfile(profile_);
  if (!pending_data_->search_engines.empty() && !template_urls->loaded()) {
    if (!template_url_subscription_) {
      template_url_subscription_ =
          template_urls->RegisterOnLoadedCallback(base::BindOnce(
              &ImportService::WriteWhenReady, weak_factory_.GetWeakPtr()));
      template_urls->Load();
    }
    return;
  }
  ChromiumImportData data = std::move(*pending_data_);
  pending_data_.reset();
  WriteChromiumData(std::move(data));
}

void ImportService::BookmarkModelLoaded(bool ids_reassigned) {
  bookmark_observation_.Reset();
  WriteWhenReady();
}

void ImportService::BookmarkModelBeingDeleted() {
  bookmark_observation_.Reset();
}

void ImportService::WriteChromiumData(ChromiumImportData data) {
  Job& job = *job_;
  auto writer = base::MakeRefCounted<ProfileWriter>(profile_.get());
  if (!data.bookmarks.empty()) {
    job.bookmarks = CountBookmarks(data.bookmarks);
    writer->AddBookmarks(data.bookmarks, L("settings.import.bookmarksFolder",
                                           {{"browser", job.source_name}}));
  }
  if (!data.search_engines.empty()) {
    TemplateURLService* service =
        TemplateURLServiceFactory::GetForProfile(profile_);
    const size_t before = service->GetTemplateURLs().size();
    TemplateURLService::OwnedTemplateURLVector engines;
    for (const TemplateURLData& engine : data.search_engines) {
      engines.push_back(std::make_unique<TemplateURL>(engine));
    }
    // 和已有的同一个网站、同一个路径的（两个 Google 这种）不导入。
    writer->AddKeywords(std::move(engines), /*unique_on_host_and_path=*/true);
    job.search_engines = service->GetTemplateURLs().size() - before;
  }
  if (!data.passwords.empty()) {
    // 密码管理器关着时 ProfileWriter 不写。
    if (profile_->GetPrefs()->GetBoolean(
            password_manager::prefs::kCredentialsEnableService)) {
      for (const password_manager::PasswordForm& form : data.passwords) {
        writer->AddPasswordForm(form);
      }
      job.passwords = data.passwords.size();
    } else {
      job.failed_items |= user_data_importer::PASSWORDS;
    }
  }
  if (!data.autofill.empty()) {
    writer->AddAutocompleteFormDataEntries(data.autofill);
    job.autofill = data.autofill.size();
  }
  history::HistoryService* history = HistoryServiceFactory::GetForProfile(
      profile_, ServiceAccessType::EXPLICIT_ACCESS);
  if (!data.history.empty() && history) {
    job.current = user_data_importer::HISTORY;
    NotifyChanged();
    history->ScheduleDBTask(
        FROM_HERE,
        std::make_unique<ImportHistoryTask>(
            std::move(data.history),
            base::BindOnce(&ImportService::OnHistoryWritten,
                           weak_factory_.GetWeakPtr(), generation_)),
        &history_tracker_);
    return;
  }
  Finish();
}

void ImportService::OnHistoryWritten(int generation, size_t added) {
  if (generation != generation_ || !running()) {
    return;
  }
  job_->history = added;
  Finish();
}

void ImportService::Finish() {
  Job& job = *job_;
  job.current = user_data_importer::NONE;
  // 要导入的几样一样也没读出来才算失败；读出来是空的（比如没有书签）算导入完了。
  if ((job.failed_items & job.items) == job.items) {
    job.status = Status::kFailed;
    job.error = "unreadable";
  } else {
    job.status = Status::kDone;
  }
  NotifyChanged();
}

// ---- Chrome 的导入器 ----

void ImportService::StartBuiltin(
    const user_data_importer::SourceProfile& profile,
    uint16_t items) {
  // Firefox 开着时它的用户资料是锁着的：先看一下，告诉用户退出 Firefox 再试（Chrome 弹它
  // 自己的对话框，这里不用它，导入器设成 headless）。
  if (profile.importer_type == user_data_importer::TYPE_FIREFOX) {
    FirefoxProfileLock lock(profile.source_path);
    if (!lock.HasAcquired()) {
      job_->status = Status::kFailed;
      job_->error = "firefoxRunning";
      NotifyChanged();
      return;
    }
  }
  builtin_item_ended_ = false;
  counting_writer_ = base::MakeRefCounted<CountingProfileWriter>(profile_);
  importer_host_ = new ExternalProcessImporterHost();
  importer_host_->set_headless();
  importer_host_->set_observer(this);
  importer_host_->StartImportSettings(profile, profile_, items,
                                      counting_writer_.get());
}

void ImportService::ImportStarted() {}

void ImportService::ImportItemStarted(user_data_importer::ImportItem item) {
  if (running()) {
    job_->current = item;
    NotifyChanged();
  }
}

void ImportService::ImportItemEnded(user_data_importer::ImportItem item) {
  builtin_item_ended_ = true;
}

void ImportService::ImportEnded() {
  // 导入器在这之后删掉自己。
  importer_host_ = nullptr;
  scoped_refptr<CountingProfileWriter> writer = std::move(counting_writer_);
  if (!running() || !writer) {
    return;
  }
  Job& job = *job_;
  job.bookmarks = writer->bookmarks;
  job.history = writer->history;
  job.passwords = writer->passwords;
  job.search_engines = writer->search_engines;
  job.autofill = writer->autofill;
  job.current = user_data_importer::NONE;
  if (builtin_item_ended_) {
    job.status = Status::kDone;
  } else {
    job.status = Status::kFailed;
    job.error = "unreadable";
  }
  NotifyChanged();
}

}  // namespace starry
