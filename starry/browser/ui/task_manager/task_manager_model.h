// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TASK_MANAGER_TASK_MANAGER_MODEL_H_
#define STARRY_BROWSER_UI_TASK_MANAGER_TASK_MANAGER_MODEL_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/process/process_handle.h"
#include "base/time/time.h"
#include "chrome/browser/task_manager/task_manager_observer.h"
#include "components/tabs/public/tab_interface.h"
#include "starry/browser/ui/task_manager/gpu_usage.h"
#include "third_party/icu/source/i18n/unicode/coll.h"
#include "ui/base/models/image_model.h"

namespace gfx {
struct VectorIcon;
}

namespace starry {

// 采样节奏和历史曲线长度：每 1.5 秒刷新一次，曲线保留最近 2 分钟。
inline constexpr base::TimeDelta kTaskSamplingInterval =
    base::Milliseconds(1500);
inline constexpr size_t kTaskHistoryLength = 80;

// 固定容量的最近读数环形缓冲，满后挤掉最旧记录；绘制时最新读数位于右端。
class UsageHistory {
 public:
  explicit UsageHistory(size_t capacity = kTaskHistoryLength);
  UsageHistory(const UsageHistory&);
  UsageHistory& operator=(const UsageHistory&);
  ~UsageHistory();

  void Append(double value);
  size_t capacity() const { return capacity_; }
  // 从旧到新。
  const std::vector<double>& values() const { return values_; }
  std::optional<double> latest() const;
  std::optional<double> peak() const;

 private:
  size_t capacity_;
  std::vector<double> values_;
};

// 一个进程自己的曲线（底栏里选中进程的小曲线）。
struct ProcessHistory {
  UsageHistory cpu;
  UsageHistory memory;
};

// 进程内的一项任务：标签页、扩展页面，或浏览器、GPU、网络进程本身。
struct TaskEntry {
  enum class Source {
    kBrowser,
    kGpu,
    kNetwork,
    // 别的实用进程（存储、音频……）。
    kService,
    kTab,
    kExtension,
    // 嵌入的网页、Worker、预载入的页面……
    kOther,
  };

  TaskEntry();
  TaskEntry(const TaskEntry&);
  TaskEntry& operator=(const TaskEntry&);
  ~TaskEntry();

  // Chrome 任务管理器里的编号。
  int64_t id = -1;
  Source source = Source::kOther;
  std::u16string title;
  std::u16string detail;
  // 网站图标、扩展图标；浏览器自己的页面是矢量图标。空的时候画 symbol。
  ui::ImageModel icon;
  raw_ptr<const gfx::VectorIcon> symbol = nullptr;
  // 单色的网站图标（新标签页的）：按主题着色。
  bool themify_icon = false;
  bool is_private = false;
  // 结束它所在的进程会停掉扩展（后台页、扩展的 Service Worker）。
  bool stops_extension = false;
  std::string extension_id;
  // 标签（source 是 kTab 时）：双击、「切换到标签页」切到它。
  tabs::TabHandle tab;
  // 同一进程里谁排在前面（标签、扩展、其他、嵌入的网页、Worker）。
  int rank = 0;
};

// 一个进程：浏览器自己、GPU、网络和别的服务，或者一个网页进程（里面可能有几个页面）。
struct TaskProcess {
  // 排序值相同时按此枚举顺序排列。
  enum class Kind { kBrowser = 0, kGpu, kNetwork, kService, kRenderer };

  TaskProcess();
  TaskProcess(const TaskProcess&);
  TaskProcess& operator=(const TaskProcess&);
  ~TaskProcess();

  const std::u16string& title() const;

  base::ProcessId pid = base::kNullProcessId;
  Kind kind = Kind::kRenderer;
  // 第一项是主任务。
  std::vector<TaskEntry> tasks;
  std::optional<int64_t> memory;
  // 第一次读数时还算不出来。
  std::optional<double> cpu;
  // 这段时间里 GPU 为它工作的比例；第一次读数时、读不到每个进程的 GPU 时间时没有。
  std::optional<double> gpu;
  // 网页进程卡住了，不再响应。
  bool unresponsive = false;
  bool can_end = false;
};

// 任务管理器数据模型：窗口打开期间每 1.5 秒从 Chrome 任务管理器收集一次进程信息，
// 读取内存、CPU 和 GPU 占用，并记录最近 2 分钟的趋势曲线。曲线随窗口生命周期释放。
class TaskManagerModel : public task_manager::TaskManagerObserver {
 public:
  enum class Column { kTask, kMemory, kCpu, kGpu, kPid };

  struct Confirmation {
    std::u16string title;
    std::u16string message;
  };

  // changed 在进程列表（或者排序）变了之后调用。
  explicit TaskManagerModel(base::RepeatingClosure changed);
  TaskManagerModel(const TaskManagerModel&) = delete;
  TaskManagerModel& operator=(const TaskManagerModel&) = delete;
  ~TaskManagerModel() override;

  const std::vector<TaskProcess>& processes() const { return processes_; }
  // 读到过一次进程了。
  bool loaded() const { return loaded_; }
  Column sort_column() const { return sort_column_; }
  bool ascending() const { return ascending_; }

  std::optional<base::ProcessId> selection() const { return selection_; }
  void Select(std::optional<base::ProcessId> pid);
  const TaskProcess* selected_process() const;
  const TaskProcess* Find(base::ProcessId pid) const;
  // ↑↓：没选中时 ↓ 选第一个、↑ 选最后一个，到头不绕回。
  void MoveSelection(int offset);

  // 整个浏览器（列表里所有进程）的曲线。
  const UsageHistory& cpu_history() const { return cpu_history_; }
  const UsageHistory& memory_history() const { return memory_history_; }
  const UsageHistory& gpu_history() const { return gpu_history_; }
  const ProcessHistory* HistoryFor(base::ProcessId pid) const;
  // 整机 GPU 的忙碌程度，包括别的 App：看 GPU 忙是不是浏览器造成的。
  std::optional<double> device_gpu() const { return device_gpu_; }
  // 读不读得到每个进程的 GPU 时间；还没读过时为空。
  std::optional<bool> gpu_available() const { return gpu_available_; }

  // 点同一列切换升降序；换一列时文字按升序、数字按降序（占用大的在上面）。
  void Sort(Column column);

  // 结束前要不要确认：只有一个页面、也不会停掉扩展的进程直接结束（同 Chrome），其余影响
  // 更大的先问一下。不用问时为空。
  std::optional<Confirmation> ConfirmationFor(const TaskProcess& process) const;
  // 结束进程（浏览器自己不能结束）。过一会儿补刷新一次列表。
  void End(base::ProcessId pid);
  // 双击、回车：切到这个标签；扩展页面打开扩展管理页。
  void Open(base::ProcessId pid);

  // task_manager::TaskManagerObserver:
  void OnTaskAdded(task_manager::TaskId id) override;
  void OnTaskToBeRemoved(task_manager::TaskId id) override;
  void OnTasksRefreshed(const task_manager::TaskIdList& task_ids) override;

 private:
  // 收集进程；record 为 true 时读数记进曲线（定时的刷新），false 时只更新列表（进程
  // 增减、结束进程后的补刷新）。GPU 在后台线程上读，读好了再交给界面。
  void Collect(bool record);
  void ScheduleCollect(base::TimeDelta delay);
  void OnGpuRead(std::vector<TaskProcess> list, bool record, GpuUsage gpu);
  void Record(const std::vector<TaskProcess>& list);
  std::vector<TaskProcess> Sorted(std::vector<TaskProcess> list) const;
  bool IsLive(int64_t id) const;

  base::RepeatingClosure changed_;
  std::vector<TaskProcess> processes_;
  bool loaded_ = false;
  Column sort_column_ = Column::kMemory;
  bool ascending_ = false;
  std::optional<base::ProcessId> selection_;

  UsageHistory cpu_history_;
  UsageHistory memory_history_;
  UsageHistory gpu_history_;
  std::map<base::ProcessId, ProcessHistory> process_histories_;
  std::optional<double> device_gpu_;
  std::optional<bool> gpu_available_;
  std::optional<GpuUsage> gpu_sample_;

  // 后台正在读 GPU：这期间要求的刷新等它读完再做。
  bool reading_ = false;
  bool pending_ = false;
  bool pending_record_ = false;
  bool collect_scheduled_ = false;
  // 扩展的图标（扩展页面的任务带着），给同一扩展的 Service Worker 用。
  std::map<std::string, ui::ImageModel> extension_icons_;
  std::unique_ptr<icu::Collator> collator_;

  base::WeakPtrFactory<TaskManagerModel> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_TASK_MANAGER_TASK_MANAGER_MODEL_H_
