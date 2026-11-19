// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_ASSISTANT_ASSISTANT_REVIEWER_H_
#define STARRY_BROWSER_ASSISTANT_ASSISTANT_REVIEWER_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "starry/browser/assistant/assistant_backend.h"

// 「自动」确认规则的一次敏感操作审核：执行脚本、往密码框里输入。每次用一个新的、
// 没有浏览器工具的后端（同一个服务和模型，或者同一个本机智能体），不带执行任务的对话
// 历史；只有 user_messages 是用户说的话，网页内容、参数都只是要审的证据。
namespace starry::assistant {

enum class ReviewDecision { kAllow, kAsk, kDeny };

struct ReviewVerdict {
  ReviewDecision decision = ReviewDecision::kAsk;
  std::string reason;
};

// 审核员的系统提示（英文，读它的是模型）。
const char* ReviewSystemPrompt();

// 要审的一步。
struct ReviewRequest {
  ReviewRequest();
  ReviewRequest(ReviewRequest&&);
  ReviewRequest& operator=(ReviewRequest&&);
  ~ReviewRequest();

  std::vector<std::string> user_messages;
  std::string tool;
  // 工具的参数（密码已经换掉）。
  base::DictValue arguments;
  std::string page_url;
  std::string page_title;
  std::string action;
  std::string detail;
  // 这次对话里之前的确认和拒绝（谁、什么时候）。
  base::ListValue prior_decisions;
};

// 给审核员的那一条（JSON）。不截断源码或用户的话；太长（放不下）时是 nullopt，
// 留给用户确认。
std::optional<std::string> ReviewPrompt(const ReviewRequest& request);
// 审核员的回答：必须正好是 {"decision": …, "reason": …}。认不得时是 nullopt。
std::optional<ReviewVerdict> ParseVerdict(const std::string& text);

// 跑一次审核。回调的 nullopt 是没审成（超时、认不得的回答、它想调用工具、出错），
// 调用者回到让用户确认。回调可能在后端自己的调用里来：在回调里不要直接删掉审核员，
// 用 DeleteSoon（它的后端跟着删，本机智能体的进程跟着结束）。
class Reviewer : public Backend::Delegate {
 public:
  using Callback = base::OnceCallback<void(std::optional<ReviewVerdict>)>;

  // backend 用 SetBackend 交进来（建后端时要 Delegate）。
  Reviewer();
  Reviewer(const Reviewer&) = delete;
  Reviewer& operator=(const Reviewer&) = delete;
  ~Reviewer() override;

  void Start(std::unique_ptr<Backend> backend,
             std::string prompt,
             std::string reasoning,
             Callback callback);
  // 停下：回调收到 nullopt。
  void Cancel();
  const Usage& usage() const { return usage_; }

  // Backend::Delegate:
  void OnTextDelta(const std::string& text) override;
  void OnReasoningDelta(const std::string& text) override {}
  void OnToolStarted(const std::string& id,
                     const std::string& name,
                     const base::DictValue& arguments) override;
  void OnToolFinished(const std::string& id,
                      const agent::ToolResult& result) override {}
  void OnUsage(const Usage& usage) override;
  void OnFinished(StopReason reason,
                  FailureKind failure,
                  const std::string& message) override;
  void ExecuteTool(const std::string& name,
                   base::DictValue arguments,
                   agent::ToolCallback done) override;

 private:
  void Finish(std::optional<ReviewVerdict> verdict);

  std::unique_ptr<Backend> backend_;
  std::string text_;
  Callback callback_;
  Usage usage_;
  base::OneShotTimer timeout_;
  base::WeakPtrFactory<Reviewer> weak_factory_{this};
};

}  // namespace starry::assistant

#endif  // STARRY_BROWSER_ASSISTANT_ASSISTANT_REVIEWER_H_
