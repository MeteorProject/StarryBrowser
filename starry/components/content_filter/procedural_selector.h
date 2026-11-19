// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMPONENTS_CONTENT_FILTER_PROCEDURAL_SELECTOR_H_
#define STARRY_COMPONENTS_CONTENT_FILTER_PROCEDURAL_SELECTOR_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// 扩展选择器（uBlock Origin 的 procedural cosmetic filter、AdGuard 的 extended CSS、
// Adblock Plus 的 `#?#`）：`:has-text()`、`:upward()`、`:xpath()` 这类原生 CSS 做不到的，
// 编成 JSON 交给网页进程（starry/components/content_filter/procedural/procedural.js）。
namespace starry::content_filter {

// 找到元素以后做什么（选择器末尾的动作操作符）。值写进 JSON，和 procedural.js 对应。
enum class ProceduralAction : uint8_t {
  kHide = 0,         // 藏起来
  kRemove = 1,       // :remove()
  kStyle = 2,        // :style(样式)
  kRemoveAttr = 3,   // :remove-attr(属性)
  kRemoveClass = 4,  // :remove-class(类名)
};

// 把选择器末尾的动作分出来：`.x:remove-attr(href)` → `.x`、kRemoveAttr、`href`。没有动作时
// action 是 kHide、arg 空。动作写错了（`:remove(x)`、空的 `:style()`、前面没有选择器）
// 返回 false。
bool SplitSelectorAction(std::string_view text,
                         std::string_view* selector,
                         ProceduralAction* action,
                         std::string_view* arg);

// 要脚本才能做的选择器：用了扩展的操作符，或者原生 CSS 不认的写法（`:has()` 里套 `:has()`）。
bool IsProceduralSelector(std::string_view selector);

// 编成 procedural.js 执行的 JSON：`["开头的原生选择器", [[操作, 参数…], …]]`。写错了、用了
// 不支持的操作符（`:-abp-properties()`、`:shadow()`、`:matches-prop()`）返回 nullopt。
std::optional<std::string> CompileProceduralSelector(std::string_view selector);

// `:remove-attr()`、`:remove-class()` 的参数（名字或者 `/正则/`）编成 JSON 的
// `"正则","标志"`（名字是整个对上）。
std::optional<std::string> CompileNameMatcher(std::string_view arg);

}  // namespace starry::content_filter

#endif  // STARRY_COMPONENTS_CONTENT_FILTER_PROCEDURAL_SELECTOR_H_
