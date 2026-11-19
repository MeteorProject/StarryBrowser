// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMPONENTS_CONTENT_FILTER_SCRIPTLETS_H_
#define STARRY_COMPONENTS_CONTENT_FILTER_SCRIPTLETS_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace starry::content_filter {

// 脚本片段（`网站##+js(名字, 参数…)`）：在网页自己的脚本之前跑的一小段脚本，改掉网页
// 用来放广告、查拦截的函数和属性。代码是自己写的
// （scriptlets/*.js，编译时编进来），名字、参数、行为和 uBlock Origin 的一样。

inline constexpr uint16_t kNoScriptlet = 0xffff;

// 按规则里写的名字找：去掉末尾的 `.js`、AdGuard 的 `ubo-` 前缀，认别名（`set`、`aopr`……）
// 和 AdGuard、Adblock Plus 的几个名字。认不得返回 kNoScriptlet。
uint16_t FindScriptlet(std::string_view name);

// 规范名字（例外按它对）。
std::string_view ScriptletName(uint16_t id);

// 只有信得过的列表（uBlock Origin 自己的、自定义规则）能用的：能往网页里写任意内容、
// 改任意的响应。
bool IsTrustedScriptlet(uint16_t id);

// 参数写成 JS 的数组字面量（JSON 的写法）。
std::string EncodeScriptletArgs(const std::vector<std::string>& args);

// 拼一个文档要跑的脚本：用到的模块各放一份，后面是每条规则的调用，整个包在一个函数里。
// 参数里的 `war:名字`（替身资源当响应内容）由这里把资源的内容带上。
class ScriptletScript {
 public:
  ScriptletScript();
  ScriptletScript(const ScriptletScript&) = delete;
  ScriptletScript& operator=(const ScriptletScript&) = delete;
  ~ScriptletScript();

  // encoded_args 是 EncodeScriptletArgs 的结果，要活到 Build() 以后。
  void Add(uint16_t id, std::string_view encoded_args);

  bool empty() const { return calls_.empty(); }
  size_t size() const { return calls_.size(); }

  std::string Build() const;

 private:
  std::vector<std::pair<uint16_t, std::string_view>> calls_;
};

}  // namespace starry::content_filter

#endif  // STARRY_COMPONENTS_CONTENT_FILTER_SCRIPTLETS_H_
