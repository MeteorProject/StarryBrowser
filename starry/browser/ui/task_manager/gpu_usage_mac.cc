// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/task_manager/gpu_usage.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "base/apple/foundation_util.h"
#include "base/apple/scoped_cftyperef.h"
#include "base/mac/scoped_ioobject.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/sys_string_conversions.h"

namespace starry {

namespace {

base::apple::ScopedCFTypeRef<CFTypeRef> Property(io_registry_entry_t entry,
                                                 CFStringRef key) {
  return base::apple::ScopedCFTypeRef<CFTypeRef>(
      IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0));
}

// 「pid 1234, Starry Browser Helper (GPU)」里的 1234。
std::optional<base::ProcessId> PidFromCreator(CFStringRef creator) {
  const std::string text = base::SysCFStringRefToUTF8(creator);
  if (!text.starts_with("pid ")) {
    return std::nullopt;
  }
  size_t end = 4;
  while (end < text.size() && base::IsAsciiDigit(text[end])) {
    ++end;
  }
  int pid = 0;
  if (!base::StringToInt(std::string_view(text).substr(4, end - 4), &pid)) {
    return std::nullopt;
  }
  return pid;
}

uint64_t AccumulatedTime(CFArrayRef usage) {
  uint64_t total = 0;
  for (CFIndex i = 0; i < CFArrayGetCount(usage); ++i) {
    CFDictionaryRef entry =
        base::apple::CFCast<CFDictionaryRef>(CFArrayGetValueAtIndex(usage, i));
    if (!entry) {
      continue;
    }
    CFNumberRef time = base::apple::GetValueFromDictionary<CFNumberRef>(
        entry, CFSTR("accumulatedGPUTime"));
    int64_t value = 0;
    if (time && CFNumberGetValue(time, kCFNumberSInt64Type, &value) &&
        value > 0) {
      total += static_cast<uint64_t>(value);
    }
  }
  return total;
}

}  // namespace

GpuUsage GpuUsage::Read(std::set<base::ProcessId> pids) {
  GpuUsage usage;
  usage.pids = std::move(pids);
  io_iterator_t accelerators = IO_OBJECT_NULL;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceMatching("IOAccelerator"),
                                   &accelerators) == KERN_SUCCESS) {
    base::mac::ScopedIOObject<io_iterator_t> accelerators_scoper(accelerators);
    while (io_object_t accelerator = IOIteratorNext(accelerators)) {
      base::mac::ScopedIOObject<io_object_t> accelerator_scoper(accelerator);
      const auto statistics =
          Property(accelerator, CFSTR("PerformanceStatistics"));
      if (CFDictionaryRef dictionary =
              base::apple::CFCast<CFDictionaryRef>(statistics.get())) {
        double value = 0;
        if (CFNumberRef number =
                base::apple::GetValueFromDictionary<CFNumberRef>(
                    dictionary, CFSTR("Device Utilization %"));
            number && CFNumberGetValue(number, kCFNumberDoubleType, &value)) {
          usage.device_utilization =
              std::max(usage.device_utilization.value_or(0),
                       std::clamp(value, 0.0, 100.0));
        }
      }
      // 客户端挂在加速器下面，不是单独登记的服务，只能顺着树找。
      io_iterator_t clients = IO_OBJECT_NULL;
      if (IORegistryEntryCreateIterator(accelerator, kIOServicePlane,
                                        kIORegistryIterateRecursively,
                                        &clients) != KERN_SUCCESS) {
        continue;
      }
      base::mac::ScopedIOObject<io_iterator_t> clients_scoper(clients);
      while (io_object_t client = IOIteratorNext(clients)) {
        base::mac::ScopedIOObject<io_object_t> client_scoper(client);
        const auto creator = Property(client, CFSTR("IOUserClientCreator"));
        CFStringRef creator_string =
            base::apple::CFCast<CFStringRef>(creator.get());
        if (!creator_string) {
          continue;
        }
        const std::optional<base::ProcessId> pid =
            PidFromCreator(creator_string);
        if (!pid) {
          continue;
        }
        // 只取要求的进程；还不知道驱动给不给数字时，别的进程的也看一眼，只为确认这一点。
        const bool wanted = usage.pids.contains(*pid);
        if (!wanted && usage.per_process_available) {
          continue;
        }
        const auto app_usage = Property(client, CFSTR("AppUsage"));
        CFArrayRef array = base::apple::CFCast<CFArrayRef>(app_usage.get());
        if (!array) {
          continue;
        }
        usage.per_process_available = true;
        if (wanted) {
          usage.time[*pid] += AccumulatedTime(array);
        }
      }
    }
  }
  usage.timestamp = base::TimeTicks::Now();
  return usage;
}

}  // namespace starry
