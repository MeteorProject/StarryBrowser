// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_ASSISTANT_CODEX_BACKEND_H_
#define STARRY_BROWSER_ASSISTANT_CODEX_BACKEND_H_

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "starry/browser/assistant/assistant_backend.h"
#include "starry/browser/assistant/local_agent.h"

// 本机的 Codex 后端：`codex app-server`（IDE 插件用的 JSON-RPC，标准输入输出每行
// 一条，没有 jsonrpc 字段），一个对话一个进程、一个 thread。用它自己的登录和模型；
// 它自带的工具（Shell、改文件、联网搜索、图片、浏览器、电脑控制）和用户配置的 MCP
// 服务器都关掉，只留浏览器的工具（-c mcp_servers.starry.*）。协议基于 codex-cli
// 0.166。
namespace starry::assistant {

class LineProcess;

class CodexBackend : public Backend {
 public:
  struct Config {
    Config();
    Config(const Config&);
    Config& operator=(const Config&);
    ~Config();

    base::FilePath executable;
    std::string model;
    std::string system_prompt;
    // 没有：自动审核用的，没有浏览器的工具，system_prompt 换掉它自己的、thread 不存。
    std::optional<BrowserMcpServer> server;
    base::FilePath working_directory;
    base::EnvironmentMap environment;
    // 接着上次的 thread。
    std::string thread_id;
    // 要关掉的功能开关（只放这个版本认得的，见 CodexFeatures）。
    std::vector<std::string> disabled_features;
    // 用户配置里的 MCP 服务器，全部关掉。
    std::vector<std::string> disabled_servers;
    // thread 不写到磁盘（无痕窗口）。
    bool ephemeral = false;
  };

  // 想关掉的功能开关：都是它自带的工具。版本不同，实际只关 `codex features list` 里有的。
  static const std::vector<std::string>& UnwantedFeatures();
  static std::vector<std::string> Arguments(const Config& config);

  CodexBackend(Config config, Delegate* delegate);
  CodexBackend(const CodexBackend&) = delete;
  CodexBackend& operator=(const CodexBackend&) = delete;
  ~CodexBackend() override;

  const std::string& thread_id() const { return thread_id_; }
  const Config& config() const { return config_; }
  // 这一轮要发的 effort：模型不支持的不发；从某个等级换回「默认」时发模型默认的
  //（turn/start 的 effort 对之后的每一轮都有效）。
  std::string TurnEffort(const std::string& requested) const;

  // Backend:
  bool running() const override;
  void Send(UserMessage message) override;
  void Stop() override;
  void Answer(const std::string& id,
              std::optional<std::string> answer) override;

 private:
  using Response =
      base::OnceCallback<void(base::expected<base::Value, std::string>)>;
  struct Message {
    Message();
    Message(Message&&);
    Message& operator=(Message&&);
    ~Message();
    std::string text;
    std::vector<agent::ToolImage> images;
    std::string reasoning;
  };
  // Codex 要用户回答的问题（item/tool/requestUserInput，一次可能有几个，一个一个问）。
  struct PendingInput {
    PendingInput();
    PendingInput(PendingInput&&);
    PendingInput& operator=(PendingInput&&);
    ~PendingInput();
    base::Value request_id;
    std::vector<std::pair<std::string, agent::Question>> questions;
    base::DictValue answers;
  };

  void Launch();
  // 取模型列表（它自己缓存的，不调用模型）：每个模型支持哪些思考程度。
  void LoadCatalog();
  // 知道了模型和列表：告诉面板这个模型支持哪些思考程度。
  void PublishLevels();
  void OpenThread();
  void StartTurn(Message message);
  void Request(const std::string& method,
               base::DictValue params,
               Response response);
  void Notify(const std::string& method);
  void Respond(const base::Value& id, base::DictValue result);
  void RespondError(const base::Value& id, const std::string& message);
  void HandleLine(std::string line);
  void AnswerServerRequest(const base::Value& id,
                           const std::string& method,
                           const base::DictValue& params);
  void HandleNotification(const std::string& method,
                          const base::DictValue& params);
  void ItemStarted(const base::DictValue& item);
  void ItemCompleted(const base::DictValue& item);
  void AskNext();
  void Exited(int status, std::string errors);
  void Finish(StopReason reason,
              FailureKind failure = FailureKind::kNone,
              const std::string& message = std::string());

  Config config_;
  const raw_ptr<Delegate> delegate_;
  std::unique_ptr<LineProcess> process_;
  bool running_ = false;
  bool stopping_ = false;
  bool ready_ = false;
  int next_id_ = 1;
  std::map<int, Response> responses_;
  std::string thread_id_;
  std::string turn_id_;
  std::optional<base::ListValue> catalog_;
  std::string thread_model_;
  std::optional<std::vector<std::string>> supported_levels_;
  std::string default_level_;
  // 发过的思考程度。
  std::string sent_level_;
  std::optional<Message> queued_;
  std::set<std::string> streamed_messages_;
  std::vector<PendingInput> pending_inputs_;
  base::OneShotTimer kill_timer_;
  base::WeakPtrFactory<CodexBackend> weak_factory_{this};
};

}  // namespace starry::assistant

#endif  // STARRY_BROWSER_ASSISTANT_CODEX_BACKEND_H_
