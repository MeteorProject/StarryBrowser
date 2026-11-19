// Copyright 2026 The Starry Browser Authors

#include "starry/renderer/shared_style_sheets.h"

#include "base/containers/span.h"

#include "third_party/blink/renderer/core/css/parser/css_parser_context.h"
#include "third_party/blink/renderer/core/css/style_sheet_contents.h"
#include "third_party/blink/renderer/core/execution_context/security_context.h"
#include "third_party/blink/renderer/platform/heap/collection_support/heap_hash_map.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/wtf/std_lib_extras.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace starry {

namespace {

// 同时留几份：规则更新以后旧的文档还用着旧的那份（它们自己持有），缓存里只留新的。
constexpr blink::wtf_size_t kMaxSheets = 2;

using SheetMap =
    blink::GCedHeapHashMap<blink::String,
                           blink::Member<blink::StyleSheetContents>>;

SheetMap& Sheets() {
  DEFINE_STATIC_LOCAL(blink::Persistent<SheetMap>, sheets,
                      (blink::MakeGarbageCollected<SheetMap>()));
  return *sheets;
}

blink::Vector<blink::String>& Order() {
  DEFINE_STATIC_LOCAL(blink::Vector<blink::String>, order, ());
  return order;
}

}  // namespace

blink::StyleSheetContents* SharedStyleSheetContents(
    const blink::String& source) {
  const blink::StringView prefix(base::as_byte_span(kSharedStyleSheetPrefix));
  if (!source.starts_with(prefix)) {
    return nullptr;
  }
  const blink::wtf_size_t end = source.find("*/");
  if (end == blink::String::npos) {
    return nullptr;
  }
  const blink::String key =
      source.substr(prefix.length(), end - prefix.length());
  SheetMap& sheets = Sheets();
  if (auto it = sheets.find(key); it != sheets.end()) {
    return it->value.Get();
  }
  // 和文档无关的解析环境（标准模式）：只有选择器和 display，用不到文档的设置。
  auto* context = blink::MakeGarbageCollected<blink::CSSParserContext>(
      blink::kHTMLStandardMode, blink::SecureContextMode::kInsecureContext);
  auto* contents = blink::MakeGarbageCollected<blink::StyleSheetContents>(context);
  contents->ParseString(source);
  blink::Vector<blink::String>& order = Order();
  order.push_back(key);
  sheets.Set(key, contents);
  while (order.size() > kMaxSheets) {
    sheets.erase(order.front());
    order.EraseAt(0);
  }
  return contents;
}

}  // namespace starry
