// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/task_manager/gpu_usage.h"

#include <utility>

#include "build/build_config.h"

namespace starry {

GpuUsage::GpuUsage() = default;
GpuUsage::GpuUsage(const GpuUsage&) = default;
GpuUsage& GpuUsage::operator=(const GpuUsage&) = default;
GpuUsage::~GpuUsage() = default;

std::optional<double> GpuUsage::PercentOf(base::ProcessId pid,
                                          const GpuUsage& previous) const {
  if (!per_process_available || !previous.per_process_available ||
      !pids.contains(pid) || !previous.pids.contains(pid) ||
      timestamp <= previous.timestamp) {
    return std::nullopt;
  }
  const auto now_it = time.find(pid);
  const auto before_it = previous.time.find(pid);
  const uint64_t now = now_it == time.end() ? 0 : now_it->second;
  const uint64_t before =
      before_it == previous.time.end() ? 0 : before_it->second;
  // 客户端关掉时它的累计时间跟着消失，总数会变小：这不是负的占用。
  if (now < before) {
    return 0;
  }
  const double elapsed_ns =
      (timestamp - previous.timestamp).InMicrosecondsF() * 1000;
  return static_cast<double>(now - before) / elapsed_ns * 100;
}

#if !BUILDFLAG(IS_MAC)
// 别的平台还没有按进程的 GPU 时间：界面上写「无法读取进程 GPU 数据」。
GpuUsage GpuUsage::Read(std::set<base::ProcessId> pids) {
  GpuUsage usage;
  usage.pids = std::move(pids);
  usage.timestamp = base::TimeTicks::Now();
  return usage;
}
#endif

}  // namespace starry
