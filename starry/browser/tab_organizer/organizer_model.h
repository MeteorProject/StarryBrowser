// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_TAB_ORGANIZER_ORGANIZER_MODEL_H_
#define STARRY_BROWSER_TAB_ORGANIZER_ORGANIZER_MODEL_H_

#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/types/expected.h"
#include "starry/browser/assistant/assistant_backend.h"
#include "starry/browser/assistant/assistant_settings.h"
#include "starry/browser/assistant/local_agent.h"

class Profile;

namespace starry::tab_organizer {

// 用「设置 › 标签管理 › AI 整理」选的模型跑一次（assistant::ReadOrganizerSettings）：一个
// 新的、没有浏览器工具的后端，只跑一步，回答是一段文字（调用的人要的 JSON）。做法同自动
// 审核（assistant_reviewer.h）：OpenAI 兼容服务、本机的 Claude Code 和 Codex 都行；本机
// 智能体用空的工作目录、不存会话、关掉自带的工具和用户配的 MCP 服务器。
class OrganizerModel : public assistant::Backend::Delegate {
 public:
  // 出错的原因（给调试、提示用）：没设置好、找不到本机智能体、超时、它想调用工具、
  // 后端出错（带上后端的说法）。
  using Result = base::expected<std::string, std::string>;
  using Callback = base::OnceCallback<void(Result)>;

  explicit OrganizerModel(Profile* profile);
  OrganizerModel(const OrganizerModel&) = delete;
  OrganizerModel& operator=(const OrganizerModel&) = delete;
  ~OrganizerModel() override;

  // 只能跑一次。回调可能在后端自己的调用里来：在回调里不要直接删掉它，用 DeleteSoon
  // （它的后端跟着删，本机智能体的进程跟着结束）。
  void Run(std::string system_prompt, std::string prompt, Callback callback);
  // 停下：回调收到出错。
  void Cancel();

  // 用的是哪个后端（kBackendChat……），Run 之后才有。
  const std::string& backend() const { return settings_.backend; }
  const assistant::Usage& usage() const { return usage_; }

  // assistant::Backend::Delegate:
  void OnTextDelta(const std::string& text) override;
  void OnReasoningDelta(const std::string& text) override {}
  void OnToolStarted(const std::string& id,
                     const std::string& name,
                     const base::DictValue& arguments) override;
  void OnToolFinished(const std::string& id,
                      const agent::ToolResult& result) override {}
  void OnUsage(const assistant::Usage& usage) override;
  void OnFinished(assistant::StopReason reason,
                  assistant::FailureKind failure,
                  const std::string& message) override;

 private:
  void StartChat(std::string key);
  void StartLocal(assistant::LocalLaunch launch);
  void Send();
  void Finish(Result result);

  raw_ptr<Profile> profile_;
  assistant::Settings settings_;
  std::string system_prompt_;
  std::string prompt_;
  std::unique_ptr<assistant::Backend> backend_;
  std::string text_;
  Callback callback_;
  assistant::Usage usage_;
  base::OneShotTimer timeout_;
  base::WeakPtrFactory<OrganizerModel> weak_factory_{this};
};

}  // namespace starry::tab_organizer

#endif  // STARRY_BROWSER_TAB_ORGANIZER_ORGANIZER_MODEL_H_
