// Copyright 2026 The Starry Browser Authors

#include "starry/components/content_filter/scriptlets.h"

#include <algorithm>

#include "base/json/string_escape.h"
#include "base/strings/string_util.h"
#include "starry/components/content_filter/redirect_resources.h"
#include "starry/components/content_filter/scriptlet_library.h"

namespace starry::content_filter {

namespace {

namespace library = scriptlet_library;

uint16_t FindExact(std::string_view name) {
  const base::span<const library::Scriptlet> scriptlets = library::Scriptlets();
  const auto it = std::lower_bound(
      scriptlets.begin(), scriptlets.end(), name,
      [](const library::Scriptlet& scriptlet, std::string_view value) {
        return scriptlet.name < value;
      });
  if (it != scriptlets.end() && it->name == name) {
    return static_cast<uint16_t>(it - scriptlets.begin());
  }
  const base::span<const library::Alias> aliases = library::Aliases();
  const auto alias = std::lower_bound(
      aliases.begin(), aliases.end(), name,
      [](const library::Alias& entry, std::string_view value) {
        return entry.alias < value;
      });
  if (alias != aliases.end() && alias->alias == name) {
    return alias->scriptlet;
  }
  return kNoScriptlet;
}

bool IsTextResource(std::string_view mime_type) {
  return mime_type.starts_with("text/") ||
         mime_type.starts_with("application/javascript") ||
         mime_type.starts_with("application/json") ||
         mime_type.starts_with("application/xml");
}

bool IsResourceNameChar(char c) {
  return base::IsAsciiAlphaNumeric(c) || c == '.' || c == '_' || c == '-';
}

// 参数里写成整个 `"war:名字"` 的替身资源名字。
void CollectWarNames(std::string_view encoded,
                     std::vector<std::string_view>* names) {
  static constexpr std::string_view kPrefix = "\"war:";
  for (size_t pos = encoded.find(kPrefix); pos != std::string_view::npos;
       pos = encoded.find(kPrefix, pos + 1)) {
    const size_t start = pos + kPrefix.size();
    size_t end = start;
    while (end < encoded.size() && IsResourceNameChar(encoded[end])) {
      ++end;
    }
    if (end == start || end >= encoded.size() || encoded[end] != '"') {
      continue;
    }
    const std::string_view name = encoded.substr(start, end - start);
    if (std::find(names->begin(), names->end(), name) == names->end()) {
      names->push_back(name);
    }
  }
}

}  // namespace

uint16_t FindScriptlet(std::string_view name) {
  name = base::TrimWhitespaceASCII(name, base::TRIM_ALL);
  if (name.starts_with("ubo-")) {
    name.remove_prefix(4);
  }
  if (name.ends_with(".js")) {
    name.remove_suffix(3);
  }
  if (name.empty()) {
    return kNoScriptlet;
  }
  return FindExact(name);
}

std::string_view ScriptletName(uint16_t id) {
  return library::Scriptlets()[id].name;
}

bool IsTrustedScriptlet(uint16_t id) {
  return library::Scriptlets()[id].trusted;
}

std::string EncodeScriptletArgs(const std::vector<std::string>& args) {
  std::string encoded = "[";
  for (size_t i = 0; i < args.size(); ++i) {
    if (i > 0) {
      encoded += ',';
    }
    base::EscapeJSONString(args[i], /*put_in_quotes=*/true, &encoded);
  }
  encoded += ']';
  return encoded;
}

ScriptletScript::ScriptletScript() = default;

ScriptletScript::~ScriptletScript() = default;

void ScriptletScript::Add(uint16_t id, std::string_view encoded_args) {
  calls_.emplace_back(id, encoded_args);
}

std::string ScriptletScript::Build() const {
  const base::span<const library::Module> modules = library::Modules();
  const base::span<const library::Scriptlet> scriptlets = library::Scriptlets();

  // 用到的模块，依赖的放前面，各一份（core 总在最前面）。
  std::vector<bool> included(modules.size());
  std::vector<uint8_t> order;
  auto include = [&](uint8_t module) {
    if (module != 0 && !included[module]) {
      included[module] = true;
      order.push_back(module);
    }
  };
  std::vector<std::string_view> war_names;
  for (const auto& [id, args] : calls_) {
    const library::Scriptlet& scriptlet = scriptlets[id];
    for (uint8_t dependency : modules[scriptlet.module].dependencies) {
      include(dependency);
    }
    include(scriptlet.module);
    CollectWarNames(args, &war_names);
  }

  size_t size = modules[0].code.size() + 256;
  for (uint8_t module : order) {
    size += modules[module].code.size();
  }
  for (const auto& [id, args] : calls_) {
    size += args.size() + 32;
  }
  std::string script;
  script.reserve(size);
  script += "(function(){'use strict';\nconst $war={";
  bool first = true;
  for (std::string_view name : war_names) {
    const RedirectResource* resource =
        FindRedirectResource(CanonicalRedirectName(name));
    if (!resource || !IsTextResource(resource->mime_type)) {
      continue;
    }
    if (!first) {
      script += ',';
    }
    first = false;
    base::EscapeJSONString(name, /*put_in_quotes=*/true, &script);
    script += ':';
    base::EscapeJSONString(resource->body, /*put_in_quotes=*/true, &script);
  }
  script += "};\n";
  script.append(modules[0].code);
  for (uint8_t module : order) {
    script.append(modules[module].code);
  }
  for (const auto& [id, args] : calls_) {
    script += "$run(";
    script.append(scriptlets[id].function);
    script += ',';
    script.append(args);
    script += ");\n";
  }
  script += "})();\n";
  return script;
}

}  // namespace starry::content_filter
