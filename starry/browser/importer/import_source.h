// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_IMPORTER_IMPORT_SOURCE_H_
#define STARRY_BROWSER_IMPORTER_IMPORT_SOURCE_H_

#include <stdint.h>

#include <string>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "components/user_data_importer/common/importer_data_types.h"

namespace starry {

struct ChromiumBrowser;

// 能从哪里导入（设置 › 通用 › 导入浏览器数据的一行）。
struct ImportSource {
  enum class Kind {
    // Chromium 系的浏览器（Chrome、Edge、Arc、Brave……）：由 Starry 直接读取其数据文件
    // （chromium_reader.h）。
    kChromium,
    // Chrome 自带的导入器（在工具进程里跑）：Firefox、Safari（macOS），Windows 上还有
    // IE 和旧版 Edge。
    kBuiltin,
  };

  ImportSource();
  ImportSource(const ImportSource&);
  ImportSource& operator=(const ImportSource&);
  ~ImportSource();

  // 页面用来指这一行的标识，同一台机器上不变（浏览器和用户资料的目录）。
  std::string id;
  Kind kind = Kind::kChromium;
  // 浏览器的名字（品牌名，不翻译）。
  std::u16string browser;
  // 用户资料的名字；这个浏览器只有一个用户资料时是空的。
  std::u16string profile;
  // 能导入哪几样：user_data_importer::ImportItem 的位。
  uint16_t items = 0;
  // 最近一次用这个浏览器的时间（它的数据文件改动的时间），列表里最近用过的排在前面。
  base::Time last_used;

  // kChromium：哪个浏览器、它的数据目录（Local State 所在）和这个用户资料的目录。
  raw_ptr<const ChromiumBrowser> chromium = nullptr;
  base::FilePath user_data_dir;
  base::FilePath profile_dir;

  // kBuiltin：交给 Chrome 的导入器的。
  user_data_importer::SourceProfile builtin;
};

}  // namespace starry

#endif  // STARRY_BROWSER_IMPORTER_IMPORT_SOURCE_H_
