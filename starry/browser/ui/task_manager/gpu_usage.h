// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TASK_MANAGER_GPU_USAGE_H_
#define STARRY_BROWSER_UI_TASK_MANAGER_GPU_USAGE_H_

#include <cstdint>
#include <map>
#include <optional>
#include <set>

#include "base/process/process_handle.h"
#include "base/time/time.h"

namespace starry {

// 进程 GPU 占用与整机 GPU 利用率采样，供任务管理器使用。
//
// Chrome 不给每个进程的 GPU 占用：网页的绘制、canvas、视频解码都在 GPU 进程里做。能拿到的是
// 系统记下的按进程的 GPU 时间：
// - macOS：每个 Metal 客户端是加速器（IOAccelerator）下面的一个用户客户端，
//   IOUserClientCreator 写着打开它的进程（「pid 123, 名字」），AppUsage 里每一项的
//   accumulatedGPUTime 是累计用掉的 GPU 时间（纳秒）；整机的忙碌程度在加速器的
//   PerformanceStatistics 的「Device Utilization %」里，活动监视器读的也是它。这些是驱动
//   写进 IORegistry 的属性，没有文档，读不到时给空的。Intel、AMD 的驱动不给每个进程的 GPU
//   时间。
// - Windows、Linux：还没有读，per_process_available 总是 false（界面上写「无法读取」）。
//
// 遍历一次几毫秒，不要在界面线程上调用（会阻塞）。
struct GpuUsage {
  GpuUsage();
  GpuUsage(const GpuUsage&);
  GpuUsage& operator=(const GpuUsage&);
  ~GpuUsage();

  // 读一次：只取 pids 里的进程的 GPU 时间，整机的忙碌程度每次都读。
  static GpuUsage Read(std::set<base::ProcessId> pids);

  // 从 previous 到这一次，GPU 有多少时间在为这个进程工作：100 表示整段时间都是。上一次没读
  // 这个进程（累计时间是它从启动以来的，算不出这一段）、或者驱动不给数字时为空。
  std::optional<double> PercentOf(base::ProcessId pid,
                                  const GpuUsage& previous) const;

  // 要求的进程里，有 GPU 客户端的那些累计用掉的 GPU 时间（纳秒）。没有客户端的进程不在
  // 里面，就是没用过 GPU。
  std::map<base::ProcessId, uint64_t> time;
  // 这次要求读的进程。
  std::set<base::ProcessId> pids;
  // 驱动给不给每个进程的 GPU 时间。
  bool per_process_available = false;
  // 整机 GPU 的忙碌程度，0–100，包括别的 App；有几块 GPU 时取最忙的。
  std::optional<double> device_utilization;
  base::TimeTicks timestamp;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_TASK_MANAGER_GPU_USAGE_H_
