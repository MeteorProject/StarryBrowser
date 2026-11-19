// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/task_manager/task_manager_model.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/i18n/string_compare.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/task_manager/providers/task.h"
#include "chrome/browser/task_manager/providers/web_contents/web_contents_tag.h"
#include "chrome/browser/task_manager/providers/web_contents/web_contents_tags_manager.h"
#include "chrome/browser/task_manager/task_manager_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/grit/branded_strings.h"
#include "chrome/grit/generated_resources.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/browser_child_process_host_iterator.h"
#include "content/public/browser/child_process_data.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/process_type.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/process_manager.h"
#include "extensions/browser/view_type_utils.h"
#include "extensions/common/constants.h"
#include "extensions/common/extension.h"
#include "pdf/buildflags.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/tabs/tab_cell.h"
#include "starry/browser/ui/webui/starry_pages.h"
#include "starry/common/internal_urls.h"
#include "ui/base/base_window.h"
#include "ui/base/l10n/l10n_util.h"
#include "url/gurl.h"

namespace starry {

namespace {

using task_manager::Task;
using task_manager::TaskManagerInterface;

constexpr char kNetworkServiceName[] = "network.mojom.NetworkService";
// 结束进程后延迟补刷新列表的时间。
constexpr base::TimeDelta kAfterEndDelay = base::Milliseconds(300);

// Chrome 的任务标题带着「标签页：」「Service Worker：」这样的前缀。去掉前缀当标题，前缀
// 告诉我们这是哪一种任务。
enum class Hint {
  kNone,
  kTab,
  kExtension,
  kSubframe,
  kBackForwardCache,
  kPrerender,
  kPrint,
  kBackground,
  kServiceWorker,
  kSharedWorker,
  kDedicatedWorker,
  kUtility,
  kTool,
};

struct PrefixRule {
  int message_id;
  Hint hint;
  bool incognito;
};

constexpr PrefixRule kPrefixRules[] = {
    {IDS_TASK_MANAGER_TAB_PREFIX, Hint::kTab, false},
    {IDS_TASK_MANAGER_TAB_INCOGNITO_PREFIX, Hint::kTab, true},
    {IDS_TASK_MANAGER_APP_PREFIX, Hint::kTab, false},
    {IDS_TASK_MANAGER_APP_INCOGNITO_PREFIX, Hint::kTab, true},
    {IDS_TASK_MANAGER_EXTENSION_PREFIX, Hint::kExtension, false},
    {IDS_TASK_MANAGER_EXTENSION_INCOGNITO_PREFIX, Hint::kExtension, true},
    {IDS_TASK_MANAGER_SUBFRAME_PREFIX, Hint::kSubframe, false},
    {IDS_TASK_MANAGER_SUBFRAME_INCOGNITO_PREFIX, Hint::kSubframe, true},
#if BUILDFLAG(ENABLE_PDF)
    {IDS_TASK_MANAGER_PDF_SUBFRAME_PREFIX, Hint::kSubframe, false},
    {IDS_TASK_MANAGER_PDF_SUBFRAME_INCOGNITO_PREFIX, Hint::kSubframe, true},
#endif
    {IDS_TASK_MANAGER_BACK_FORWARD_CACHE_PREFIX, Hint::kBackForwardCache,
     false},
    {IDS_TASK_MANAGER_BACK_FORWARD_CACHE_INCOGNITO_PREFIX,
     Hint::kBackForwardCache, true},
    {IDS_TASK_MANAGER_BACK_FORWARD_CACHE_SUBFRAME_PREFIX,
     Hint::kBackForwardCache, false},
    {IDS_TASK_MANAGER_BACK_FORWARD_CACHE_INCOGNITO_SUBFRAME_PREFIX,
     Hint::kBackForwardCache, true},
    {IDS_TASK_MANAGER_PRERENDER_PREFIX, Hint::kPrerender, false},
    {IDS_TASK_MANAGER_NO_STATE_PREFETCH_PREFIX, Hint::kPrerender, false},
    {IDS_TASK_MANAGER_PRINT_PREFIX, Hint::kPrint, false},
    {IDS_TASK_MANAGER_BACKGROUND_PREFIX, Hint::kBackground, false},
    {IDS_TASK_MANAGER_BACKGROUND_APP_PREFIX, Hint::kBackground, false},
    {IDS_TASK_MANAGER_SERVICE_WORKER_PREFIX, Hint::kServiceWorker, false},
    {IDS_TASK_MANAGER_SHARED_WORKER_PREFIX, Hint::kSharedWorker, false},
    {IDS_TASK_MANAGER_DEDICATED_WORKER_PREFIX, Hint::kDedicatedWorker, false},
    {IDS_TASK_MANAGER_UTILITY_PREFIX, Hint::kUtility, false},
    {IDS_TASK_MANAGER_TOOL_PREFIX, Hint::kTool, false},
};

struct Stripped {
  std::u16string title;
  Hint hint = Hint::kNone;
  bool incognito = false;
};

// 按语言文件里占位符前后的文字去掉前缀（各语言的前缀位置不同，有的在后面）。
Stripped StripPrefix(const std::u16string& title) {
  static const base::NoDestructor<std::vector<
      std::pair<std::pair<std::u16string, std::u16string>, PrefixRule>>>
      patterns([] {
        std::vector<
            std::pair<std::pair<std::u16string, std::u16string>, PrefixRule>>
            result;
        constexpr char16_t kMarker[] = u"\uE000";
        for (const PrefixRule& rule : kPrefixRules) {
          const std::u16string formatted =
              l10n_util::GetStringFUTF16(rule.message_id, kMarker);
          const size_t at = formatted.find(kMarker);
          if (at == std::u16string::npos) {
            continue;
          }
          result.push_back(
              {{formatted.substr(0, at), formatted.substr(at + 1)}, rule});
        }
        // 长的先比：「无痕标签页：」要先于「标签页：」。
        std::stable_sort(result.begin(), result.end(),
                         [](const auto& a, const auto& b) {
                           return a.first.first.size() + a.first.second.size() >
                                  b.first.first.size() + b.first.second.size();
                         });
        return result;
      }());
  for (const auto& [affixes, rule] : *patterns) {
    const auto& [prefix, suffix] = affixes;
    if (title.size() < prefix.size() + suffix.size() ||
        !title.starts_with(prefix) || !title.ends_with(suffix)) {
      continue;
    }
    std::u16string inner = title.substr(
        prefix.size(), title.size() - prefix.size() - suffix.size());
    return {std::u16string(base::TrimWhitespace(inner, base::TRIM_ALL)),
            rule.hint, rule.incognito};
  }
  return {title, Hint::kNone, false};
}

const extensions::Extension* FindExtension(const std::string& id) {
  ProfileManager* manager = g_browser_process->profile_manager();
  if (!manager) {
    return nullptr;
  }
  for (Profile* profile : manager->GetLoadedProfiles()) {
    if (const extensions::Extension* extension =
            extensions::ExtensionRegistry::Get(profile)->GetExtensionById(
                id, extensions::ExtensionRegistry::EVERYTHING)) {
      return extension;
    }
  }
  return nullptr;
}

ui::ImageModel IconOf(const TaskManagerInterface* tm, int64_t id) {
  const gfx::ImageSkia& image = tm->GetIcon(id);
  return image.isNull() ? ui::ImageModel()
                        : ui::ImageModel::FromImageSkia(image);
}

// 有网页的任务：标签、扩展页面、开发者工具。
void DescribeContents(TaskManagerInterface* tm,
                      int64_t id,
                      content::WebContents* contents,
                      const Stripped& stripped,
                      TaskEntry& entry) {
  if (tabs::TabInterface* tab =
          tabs::TabInterface::MaybeGetFromContents(contents)) {
    const tabs::TabData data = StarryTabData(tab);
    entry.source = TaskEntry::Source::kTab;
    entry.tab = tab->GetHandle();
    entry.title = TabDisplayTitle(data);
    entry.icon = data.favicon;
    entry.themify_icon = data.should_themify_favicon;
    entry.symbol = &icons::kGlobe;
    entry.is_private = contents->GetBrowserContext()->IsOffTheRecord();
    entry.detail =
        contents->GetLastCommittedURL().SchemeIs(extensions::kExtensionScheme)
            ? L("taskManager.detail.extensionPage")
        : entry.is_private ? L("taskManager.detail.privateTab")
                           : L("taskManager.detail.tab");
    entry.rank = 0;
    return;
  }
  const extensions::mojom::ViewType type = extensions::GetViewType(contents);
  if (const extensions::Extension* extension =
          extensions::ProcessManager::Get(contents->GetBrowserContext())
              ->GetExtensionForWebContents(contents)) {
    entry.source = TaskEntry::Source::kExtension;
    entry.title = base::UTF8ToUTF16(extension->name());
    entry.extension_id = extension->id();
    entry.icon = IconOf(tm, id);
    entry.symbol = &icons::kPuzzlepiece;
    entry.is_private = contents->GetBrowserContext()->IsOffTheRecord();
    switch (type) {
      case extensions::mojom::ViewType::kExtensionBackgroundPage:
        entry.detail = L("taskManager.detail.extensionBackground");
        entry.stops_extension = true;
        break;
      case extensions::mojom::ViewType::kExtensionPopup:
        entry.detail = L("taskManager.detail.extensionPopup");
        break;
      case extensions::mojom::ViewType::kOffscreenDocument:
        entry.detail = L("taskManager.detail.extensionOffscreen");
        break;
      case extensions::mojom::ViewType::kExtensionSidePanel:
        entry.detail = L("taskManager.detail.extensionSidePanel");
        break;
      default:
        entry.detail = L("taskManager.detail.extensionPage");
        break;
    }
    entry.rank = 1;
    return;
  }
  entry.source = TaskEntry::Source::kOther;
  entry.title =
      contents->GetTitle().empty() ? stripped.title : contents->GetTitle();
  entry.is_private = contents->GetBrowserContext()->IsOffTheRecord();
  entry.rank = 2;
  if (type == extensions::mojom::ViewType::kDeveloperTools) {
    entry.detail = L("taskManager.detail.devTools");
    entry.symbol = &icons::kSliderHorizontal3;
  } else if (stripped.hint == Hint::kTool ||
             IsInternalURL(contents->GetLastCommittedURL()) ||
             IsInternalURL(contents->GetVisibleURL())) {
    // 不在标签里的浏览器界面（Chrome 预先开好的地址栏弹出层这类）。
    entry.detail = L("taskManager.detail.browserUi");
    entry.symbol = &icons::kMacWindow;
  } else {
    entry.detail = L("taskManager.detail.page");
    entry.symbol = &icons::kGlobe;
  }
}

// 没有网页的网页进程任务：嵌入的网页、Worker、预载入的页面、备用进程。
void DescribeRendererTask(TaskManagerInterface* tm,
                          int64_t id,
                          const Stripped& stripped,
                          std::map<std::string, ui::ImageModel>& icons_cache,
                          TaskEntry& entry) {
  entry.source = TaskEntry::Source::kOther;
  entry.title = stripped.title;
  entry.is_private = stripped.incognito;
  entry.symbol = &icons::kGlobe;
  if (tm->GetSubType(id) == Task::SubType::kSpareRenderer) {
    entry.title = L("taskManager.spare.title");
    entry.detail = L("taskManager.spare.detail");
    entry.rank = 5;
    return;
  }
  const Task::Type type = tm->GetType(id);
  if (type == Task::SERVICE_WORKER || type == Task::SHARED_WORKER ||
      type == Task::DEDICATED_WORKER) {
    entry.symbol = &icons::kGearshape;
    entry.rank = 4;
    // 扩展的 Service Worker（Manifest V3 的后台）：写扩展的名字。
    const GURL url(stripped.title);
    if (url.SchemeIs(extensions::kExtensionScheme)) {
      if (const extensions::Extension* extension =
              FindExtension(std::string(url.host()))) {
        entry.source = TaskEntry::Source::kExtension;
        entry.title = base::UTF8ToUTF16(extension->name());
        entry.extension_id = extension->id();
        entry.detail = L("taskManager.detail.extensionWorker");
        entry.stops_extension = type == Task::SERVICE_WORKER;
        entry.symbol = &icons::kPuzzlepiece;
        if (auto it = icons_cache.find(extension->id());
            it != icons_cache.end()) {
          entry.icon = it->second;
        }
        entry.rank = 1;
        return;
      }
    }
    entry.detail =
        type == Task::SERVICE_WORKER  ? L("taskManager.detail.serviceWorker")
        : type == Task::SHARED_WORKER ? L("taskManager.detail.sharedWorker")
                                      : L("taskManager.detail.dedicatedWorker");
    return;
  }
  switch (stripped.hint) {
    case Hint::kSubframe:
      entry.detail = L("taskManager.detail.subframe");
      entry.rank = 3;
      return;
    case Hint::kBackForwardCache:
      entry.detail = L("taskManager.detail.backForwardCache");
      entry.rank = 3;
      return;
    case Hint::kPrerender:
      entry.detail = L("taskManager.detail.prerender");
      entry.rank = 3;
      return;
    case Hint::kPrint:
      entry.detail = L("taskManager.detail.print");
      entry.rank = 2;
      return;
    case Hint::kBackground:
      entry.detail = L("taskManager.detail.backgroundPage");
      entry.rank = 2;
      return;
    case Hint::kTab:
      entry.detail = entry.is_private ? L("taskManager.detail.privateTab")
                                      : L("taskManager.detail.tab");
      entry.rank = 0;
      return;
    case Hint::kExtension:
      entry.source = TaskEntry::Source::kExtension;
      entry.detail = L("taskManager.detail.extensionPage");
      entry.symbol = &icons::kPuzzlepiece;
      entry.rank = 1;
      return;
    case Hint::kTool:
      entry.detail = L("taskManager.detail.browserUi");
      entry.symbol = &icons::kMacWindow;
      entry.rank = 2;
      return;
    default:
      entry.detail = L("taskManager.detail.renderer");
      entry.rank = 5;
      return;
  }
}

TaskProcess::Kind KindFor(Task::Type type, bool network) {
  switch (type) {
    case Task::BROWSER:
      return TaskProcess::Kind::kBrowser;
    case Task::GPU:
      return TaskProcess::Kind::kGpu;
    case Task::UTILITY:
      return network ? TaskProcess::Kind::kNetwork
                     : TaskProcess::Kind::kService;
    case Task::RENDERER:
    case Task::EXTENSION:
    case Task::GUEST:
    case Task::DEDICATED_WORKER:
    case Task::SHARED_WORKER:
    case Task::SERVICE_WORKER:
      return TaskProcess::Kind::kRenderer;
    default:
      return TaskProcess::Kind::kService;
  }
}

template <typename T>
int Compare(const T& a, const T& b) {
  return a < b ? -1 : (b < a ? 1 : 0);
}

}  // namespace

// UsageHistory

UsageHistory::UsageHistory(size_t capacity)
    : capacity_(std::max<size_t>(capacity, 1)) {}
UsageHistory::UsageHistory(const UsageHistory&) = default;
UsageHistory& UsageHistory::operator=(const UsageHistory&) = default;
UsageHistory::~UsageHistory() = default;

void UsageHistory::Append(double value) {
  values_.push_back(value);
  if (values_.size() > capacity_) {
    values_.erase(values_.begin(),
                  values_.begin() + (values_.size() - capacity_));
  }
}

std::optional<double> UsageHistory::latest() const {
  if (values_.empty()) {
    return std::nullopt;
  }
  return values_.back();
}

std::optional<double> UsageHistory::peak() const {
  if (values_.empty()) {
    return std::nullopt;
  }
  return *std::max_element(values_.begin(), values_.end());
}

TaskEntry::TaskEntry() = default;
TaskEntry::TaskEntry(const TaskEntry&) = default;
TaskEntry& TaskEntry::operator=(const TaskEntry&) = default;
TaskEntry::~TaskEntry() = default;

TaskProcess::TaskProcess() = default;
TaskProcess::TaskProcess(const TaskProcess&) = default;
TaskProcess& TaskProcess::operator=(const TaskProcess&) = default;
TaskProcess::~TaskProcess() = default;

const std::u16string& TaskProcess::title() const {
  static const base::NoDestructor<std::u16string> empty;
  return tasks.empty() ? *empty : tasks.front().title;
}

// TaskManagerModel

TaskManagerModel::TaskManagerModel(base::RepeatingClosure changed)
    : task_manager::TaskManagerObserver(
          kTaskSamplingInterval,
          task_manager::REFRESH_TYPE_CPU |
              task_manager::REFRESH_TYPE_MEMORY_FOOTPRINT),
      changed_(std::move(changed)) {
  UErrorCode status = U_ZERO_ERROR;
  collator_.reset(icu::Collator::createInstance(
      icu::Locale(l10n::FormatLocale().c_str()), status));
  if (U_FAILURE(status)) {
    collator_.reset();
  }
  // 第一次加进去时 Chrome 马上刷新一次（OnTasksRefreshed 在这里面就来了）。
  TaskManagerInterface::GetTaskManager()->AddObserver(this);
}

TaskManagerModel::~TaskManagerModel() {
  if (observed_task_manager()) {
    observed_task_manager()->RemoveObserver(this);
  }
}

void TaskManagerModel::Select(std::optional<base::ProcessId> pid) {
  selection_ = pid;
}

const TaskProcess* TaskManagerModel::Find(base::ProcessId pid) const {
  for (const TaskProcess& process : processes_) {
    if (process.pid == pid) {
      return &process;
    }
  }
  return nullptr;
}

const TaskProcess* TaskManagerModel::selected_process() const {
  return selection_ ? Find(*selection_) : nullptr;
}

void TaskManagerModel::MoveSelection(int offset) {
  if (processes_.empty()) {
    return;
  }
  int index = offset > 0 ? 0 : static_cast<int>(processes_.size()) - 1;
  for (size_t i = 0; i < processes_.size(); ++i) {
    if (selection_ && processes_[i].pid == *selection_) {
      index = static_cast<int>(i) + offset;
      break;
    }
  }
  index = std::clamp(index, 0, static_cast<int>(processes_.size()) - 1);
  selection_ = processes_[index].pid;
}

const ProcessHistory* TaskManagerModel::HistoryFor(base::ProcessId pid) const {
  auto it = process_histories_.find(pid);
  return it == process_histories_.end() ? nullptr : &it->second;
}

void TaskManagerModel::Sort(Column column) {
  if (sort_column_ == column) {
    ascending_ = !ascending_;
  } else {
    sort_column_ = column;
    ascending_ = column == Column::kTask;
  }
  processes_ = Sorted(std::move(processes_));
  changed_.Run();
}

std::vector<TaskProcess> TaskManagerModel::Sorted(
    std::vector<TaskProcess> list) const {
  std::stable_sort(
      list.begin(), list.end(),
      [this](const TaskProcess& a, const TaskProcess& b) {
        int order = 0;
        switch (sort_column_) {
          case Column::kTask:
            order = collator_ ? base::i18n::CompareString16WithCollator(
                                    *collator_, a.title(), b.title())
                              : Compare(a.title(), b.title());
            break;
          case Column::kMemory:
            order = Compare(a.memory.value_or(0), b.memory.value_or(0));
            break;
          case Column::kCpu:
            order = Compare(a.cpu.value_or(0), b.cpu.value_or(0));
            break;
          case Column::kGpu:
            order = Compare(a.gpu.value_or(0), b.gpu.value_or(0));
            break;
          case Column::kPid:
            order = Compare(a.pid, b.pid);
            break;
        }
        if (order == 0) {
          if (a.kind != b.kind) {
            return a.kind < b.kind;
          }
          return a.pid < b.pid;
        }
        return ascending_ ? order < 0 : order > 0;
      });
  return list;
}

void TaskManagerModel::OnTaskAdded(task_manager::TaskId id) {
  ScheduleCollect(base::TimeDelta());
}

void TaskManagerModel::OnTaskToBeRemoved(task_manager::TaskId id) {
  // 这时任务还在列表里：等它拿掉了再收集。
  ScheduleCollect(base::TimeDelta());
}

void TaskManagerModel::OnTasksRefreshed(
    const task_manager::TaskIdList& task_ids) {
  Collect(/*record=*/true);
}

void TaskManagerModel::ScheduleCollect(base::TimeDelta delay) {
  if (collect_scheduled_ && delay.is_zero()) {
    return;
  }
  collect_scheduled_ = true;
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<TaskManagerModel> model) {
            if (model) {
              model->collect_scheduled_ = false;
              model->Collect(/*record=*/false);
            }
          },
          weak_factory_.GetWeakPtr()),
      delay);
}

bool TaskManagerModel::IsLive(int64_t id) const {
  const TaskManagerInterface* tm = observed_task_manager();
  if (!tm) {
    return false;
  }
  const task_manager::TaskIdList& ids = tm->GetTaskIdsList();
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

void TaskManagerModel::Collect(bool record) {
  TaskManagerInterface* tm = observed_task_manager();
  if (!tm) {
    return;
  }
  if (reading_) {
    pending_ = true;
    pending_record_ |= record;
    return;
  }

  // 任务对应的网页：标签、扩展页面、开发者工具。
  std::map<int64_t, content::WebContents*> contents_by_task;
  for (task_manager::WebContentsTag* tag :
       task_manager::WebContentsTagsManager::GetInstance()->tracked_tags()) {
    content::WebContents* contents = tag->web_contents();
    if (!contents) {
      continue;
    }
    const int64_t id = tm->GetTaskIdForWebContents(contents);
    if (id != -1) {
      contents_by_task[id] = contents;
    }
  }
  // 实用进程的名字，认出网络进程。
  std::map<content::ChildProcessId, std::pair<std::u16string, bool>> utilities;
  for (content::BrowserChildProcessHostIterator it(
           content::PROCESS_TYPE_UTILITY);
       !it.Done(); ++it) {
    const content::ChildProcessData& data = it.GetData();
    utilities[data.GetChildProcessId()] = {
        data.name, data.metrics_name == kNetworkServiceName};
  }

  std::vector<TaskProcess> list;
  std::map<base::ProcessId, size_t> index_of;
  for (const int64_t id : tm->GetTaskIdsList()) {
    const base::ProcessId pid = tm->GetProcessId(id);
    if (pid == base::kNullProcessId) {
      continue;
    }
    const Task::Type type = tm->GetType(id);
    const Stripped stripped = StripPrefix(tm->GetTitle(id));
    TaskEntry entry;
    entry.id = id;
    bool network = false;
    switch (type) {
      case Task::BROWSER:
        entry.source = TaskEntry::Source::kBrowser;
        entry.title = l10n_util::GetStringUTF16(IDS_PRODUCT_NAME);
        entry.detail = L("taskManager.browser.detail");
        entry.icon = IconOf(tm, id);
        entry.symbol = &icons::kMacWindow;
        break;
      case Task::GPU:
        entry.source = TaskEntry::Source::kGpu;
        entry.title = L("taskManager.gpu.title");
        entry.detail = L("taskManager.gpu.detail");
        entry.symbol = &icons::kCpu;
        break;
      case Task::UTILITY: {
        auto it = utilities.find(tm->GetChildProcessUniqueId(id));
        network = it != utilities.end() && it->second.second;
        if (network) {
          entry.source = TaskEntry::Source::kNetwork;
          entry.title = L("taskManager.network.title");
          entry.detail = L("taskManager.network.detail");
          entry.symbol = &icons::kNetwork;
        } else {
          entry.source = TaskEntry::Source::kService;
          entry.title = it != utilities.end() && !it->second.first.empty()
                            ? it->second.first
                            : stripped.title;
          entry.detail = L("taskManager.detail.service");
          entry.symbol = &icons::kGearshape;
        }
        break;
      }
      case Task::RENDERER:
      case Task::EXTENSION:
      case Task::GUEST:
      case Task::DEDICATED_WORKER:
      case Task::SHARED_WORKER:
      case Task::SERVICE_WORKER:
        if (auto it = contents_by_task.find(id); it != contents_by_task.end()) {
          DescribeContents(tm, id, it->second, stripped, entry);
          if (entry.source == TaskEntry::Source::kExtension &&
              !entry.icon.IsEmpty()) {
            extension_icons_[entry.extension_id] = entry.icon;
          }
        } else {
          DescribeRendererTask(tm, id, stripped, extension_icons_, entry);
        }
        break;
      default:
        entry.source = TaskEntry::Source::kService;
        entry.title = stripped.title;
        entry.detail = L("taskManager.detail.service");
        entry.symbol = &icons::kGearshape;
        break;
    }

    auto [it, inserted] = index_of.try_emplace(pid, list.size());
    if (inserted) {
      TaskProcess process;
      process.pid = pid;
      process.kind = KindFor(type, network);
      if (const std::optional<base::ByteSize> memory =
              tm->GetMemoryFootprintUsage(id)) {
        process.memory = memory->InBytes();
      }
      const double cpu = tm->GetPlatformIndependentCPUUsage(id);
      if (!std::isnan(cpu)) {
        process.cpu = cpu;
      }
      process.can_end = process.kind != TaskProcess::Kind::kBrowser &&
                        TaskManagerInterface::IsEndProcessEnabled() &&
                        tm->IsTaskKillable(id);
      list.push_back(std::move(process));
    }
    TaskProcess& process = list[it->second];
    if (auto contents = contents_by_task.find(id);
        contents != contents_by_task.end()) {
      content::RenderWidgetHost* widget =
          contents->second->GetPrimaryMainFrame()->GetRenderWidgetHost();
      if (widget && widget->IsCurrentlyUnresponsive()) {
        process.unresponsive = true;
      }
    }
    process.tasks.push_back(std::move(entry));
  }
  std::set<base::ProcessId> pids;
  for (TaskProcess& process : list) {
    std::stable_sort(
        process.tasks.begin(), process.tasks.end(),
        [](const TaskEntry& a, const TaskEntry& b) { return a.rank < b.rank; });
    pids.insert(process.pid);
  }

  // 遍历 IORegistry 要几毫秒，放到界面线程外面。
  reading_ = true;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&GpuUsage::Read, std::move(pids)),
      base::BindOnce(&TaskManagerModel::OnGpuRead, weak_factory_.GetWeakPtr(),
                     std::move(list), record));
}

void TaskManagerModel::OnGpuRead(std::vector<TaskProcess> list,
                                 bool record,
                                 GpuUsage gpu) {
  reading_ = false;
  if (gpu_sample_) {
    for (TaskProcess& process : list) {
      process.gpu = gpu.PercentOf(process.pid, *gpu_sample_);
    }
  }
  gpu_sample_ = gpu;
  device_gpu_ = gpu.device_utilization;
  gpu_available_ = gpu.per_process_available;
  if (record) {
    Record(list);
  }
  processes_ = Sorted(std::move(list));
  loaded_ = true;
  if (selection_ && !Find(*selection_)) {
    selection_.reset();
  }
  changed_.Run();

  if (pending_) {
    const bool pending_record = pending_record_;
    pending_ = false;
    pending_record_ = false;
    Collect(pending_record);
  }
}

void TaskManagerModel::Record(const std::vector<TaskProcess>& list) {
  // CPU、GPU 要两次读数才算得出，还没有时先不添。
  double memory = 0;
  double cpu = 0;
  double gpu = 0;
  bool has_memory = false;
  bool has_cpu = false;
  bool has_gpu = false;
  std::map<base::ProcessId, ProcessHistory> histories;
  for (const TaskProcess& process : list) {
    ProcessHistory history;
    if (auto it = process_histories_.find(process.pid);
        it != process_histories_.end()) {
      history = std::move(it->second);
    }
    if (process.memory) {
      memory += *process.memory;
      has_memory = true;
      history.memory.Append(*process.memory);
    }
    if (process.cpu) {
      cpu += *process.cpu;
      has_cpu = true;
      history.cpu.Append(*process.cpu);
    }
    if (process.gpu) {
      gpu += *process.gpu;
      has_gpu = true;
    }
    histories[process.pid] = std::move(history);
  }
  if (has_memory) {
    memory_history_.Append(memory);
  }
  if (has_cpu) {
    cpu_history_.Append(cpu);
  }
  if (has_gpu) {
    gpu_history_.Append(gpu);
  }
  process_histories_ = std::move(histories);
}

std::optional<TaskManagerModel::Confirmation> TaskManagerModel::ConfirmationFor(
    const TaskProcess& process) const {
  switch (process.kind) {
    case TaskProcess::Kind::kBrowser:
      return std::nullopt;
    case TaskProcess::Kind::kGpu:
      return Confirmation{L("taskManager.end.gpu.title"),
                          L("taskManager.end.gpu.message")};
    case TaskProcess::Kind::kNetwork:
      return Confirmation{L("taskManager.end.network.title"),
                          L("taskManager.end.network.message")};
    case TaskProcess::Kind::kService:
      return Confirmation{
          L("taskManager.end.named.title", {{"name", process.title()}}),
          L("taskManager.end.service.message")};
    case TaskProcess::Kind::kRenderer:
      break;
  }
  const bool stops_extension =
      std::any_of(process.tasks.begin(), process.tasks.end(),
                  [](const TaskEntry& entry) { return entry.stops_extension; });
  const size_t count = process.tasks.size();
  if (count <= 1 && !stops_extension) {
    return std::nullopt;
  }
  std::u16string message;
  if (count > 1) {
    std::vector<std::u16string> titles;
    for (const TaskEntry& entry : process.tasks) {
      titles.push_back(entry.title);
    }
    message = L("taskManager.end.pages.message",
                {{"count", count}, {"pages", l10n::JoinList(titles)}});
  }
  if (stops_extension) {
    if (!message.empty()) {
      message += L("common.sentenceJoiner");
    }
    message += L("taskManager.end.extension.message");
  }
  return Confirmation{
      count > 1 ? L("taskManager.end.process.title")
                : L("taskManager.end.named.title", {{"name", process.title()}}),
      message};
}

void TaskManagerModel::End(base::ProcessId pid) {
  const TaskProcess* process = Find(pid);
  TaskManagerInterface* tm = observed_task_manager();
  if (!process || !process->can_end || !tm || process->tasks.empty()) {
    return;
  }
  // 刷新之后页面换了进程的，不动新的进程。
  const int64_t id = process->tasks.front().id;
  if (!IsLive(id) || tm->GetProcessId(id) != pid) {
    return;
  }
  tm->KillTask(id);
  ScheduleCollect(kAfterEndDelay);
}

void TaskManagerModel::Open(base::ProcessId pid) {
  const TaskProcess* process = Find(pid);
  TaskManagerInterface* tm = observed_task_manager();
  if (!process || process->tasks.empty() || !tm) {
    return;
  }
  const TaskEntry& entry = process->tasks.front();
  switch (entry.source) {
    case TaskEntry::Source::kTab:
      // 在标签所在窗口中选中并激活窗口。
      if (tabs::TabInterface* tab = entry.tab.Get()) {
        if (BrowserWindowInterface* browser =
                tab->GetBrowserWindowInterface()) {
          TabStripModel* model = browser->GetTabStripModel();
          const int index = model->GetIndexOfTab(tab);
          if (index != TabStripModel::kNoTab) {
            model->ActivateTabAt(index);
          }
          browser->GetWindow()->Show();
          browser->GetWindow()->Activate();
        }
      } else if (IsLive(entry.id)) {
        tm->ActivateTask(entry.id);
      }
      break;
    case TaskEntry::Source::kExtension:
      if (BrowserWindowInterface* browser =
              GetLastActiveBrowserWindowInterfaceWithAnyProfile()) {
        OpenStarrySettings(browser, "extensions");
        browser->GetWindow()->Activate();
      }
      break;
    default:
      break;
  }
}

}  // namespace starry
