// Copyright 2026 The Starry Browser Authors

#include "starry/browser/agent/page_commands.h"

#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/ref_counted.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "content/public/browser/render_frame_host.h"
#include "starry/browser/resources/starry_agent_resources.h"

namespace starry::agent {

const char kFrameListenerScript[] = R"js(
(() => {
  if (globalThis.__starryFrameTokens) return true;
  globalThis.__starryFrameTokens = [];
  addEventListener('message', event => {
    const data = event.data;
    if (event.source === parent && data && typeof data.starryAgentFrame === 'string') {
      globalThis.__starryFrameTokens.push(data.starryAgentFrame);
      if (globalThis.__starryFrameTokens.length > 50) globalThis.__starryFrameTokens.shift();
    }
  });
  return true;
})()
)js";

const char kFrameTokensScript[] =
    "JSON.stringify(globalThis.__starryFrameTokens || [])";

namespace {

constexpr base::TimeDelta kTimeout = base::Seconds(10);

const std::string& PageScript() {
  static const base::NoDestructor<std::string> script([] {
    for (const AgentResource& resource : GetAgentResources()) {
      if (resource.path == "agent-page.js") {
        return std::string(reinterpret_cast<const char*>(resource.data.data()),
                           resource.data.size());
      }
    }
    return std::string();
  }());
  return *script;
}

// 先到的那个（结果或者超时）调用回调。
class Once : public base::RefCounted<Once> {
 public:
  explicit Once(base::OnceCallback<void(std::optional<base::Value>)> callback)
      : callback_(std::move(callback)) {}

  void Run(std::optional<base::Value> value) {
    if (callback_) {
      std::move(callback_).Run(std::move(value));
    }
  }

 private:
  friend class base::RefCounted<Once>;
  ~Once() = default;
  base::OnceCallback<void(std::optional<base::Value>)> callback_;
};

}  // namespace

void RunInAgentWorld(
    content::RenderFrameHost* frame,
    const std::string& script,
    base::OnceCallback<void(std::optional<base::Value>)> callback) {
  if (!frame || !frame->IsRenderFrameLive()) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  auto once = base::MakeRefCounted<Once>(std::move(callback));
  frame->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(script),
      base::BindOnce([](scoped_refptr<Once> once,
                        base::Value value) { once->Run(std::move(value)); },
                     once),
      kAgentWorldId);
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce([](scoped_refptr<Once> once) { once->Run(std::nullopt); },
                     once),
      kTimeout);
}

void RunPageCommand(content::RenderFrameHost* frame,
                    std::string_view command,
                    base::DictValue args,
                    PageCallback callback) {
  std::string name_json;
  base::JSONWriter::Write(base::Value(command), &name_json);
  std::string args_json;
  base::JSONWriter::Write(args, &args_json);
  // 每次连同脚本一起发：装过的文档里脚本开头就返回。
  const std::string script = base::StrCat(
      {PageScript(), "\n;globalThis.__starryAgent.call(", name_json, ", ",
       args_json, ")"});
  RunInAgentWorld(
      frame, script,
      base::BindOnce(
          [](PageCallback callback, std::optional<base::Value> value) {
            if (!value) {
              std::move(callback).Run(base::unexpected(
                  "The page did not respond (it may be busy or showing a "
                  "dialog)."));
              return;
            }
            if (!value->is_string()) {
              std::move(callback).Run(
                  base::unexpected("The page tools are not available here."));
              return;
            }
            std::optional<base::Value> parsed =
                base::JSONReader::Read(value->GetString(),
                                       base::JSON_PARSE_CHROMIUM_EXTENSIONS);
            if (!parsed || !parsed->is_dict()) {
              std::move(callback).Run(
                  base::unexpected("The page returned an unexpected result."));
              return;
            }
            base::DictValue& dict = parsed->GetDict();
            if (const std::string* error = dict.FindString("error")) {
              std::move(callback).Run(base::unexpected(*error));
              return;
            }
            base::Value* result = dict.Find("value");
            std::move(callback).Run(result ? std::move(*result)
                                           : base::Value());
          },
          std::move(callback)));
}

}  // namespace starry::agent
