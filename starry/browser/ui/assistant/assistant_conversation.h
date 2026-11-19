// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_ASSISTANT_ASSISTANT_CONVERSATION_H_
#define STARRY_BROWSER_UI_ASSISTANT_ASSISTANT_CONVERSATION_H_

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "starry/browser/agent/agent_service.h"
#include "starry/browser/agent/browser_tools.h"
#include "starry/browser/assistant/assistant_backend.h"
#include "starry/browser/assistant/assistant_prompt.h"
#include "starry/browser/assistant/assistant_reviewer.h"
#include "starry/browser/assistant/chat_backend.h"
#include "starry/browser/assistant/local_agent.h"
#include "url/gurl.h"

class Profile;

namespace tabs {
class TabInterface;
}

namespace starry {

class StarryBrowserView;

namespace assistant {
class History;
struct HistoryRecord;
struct Settings;
}  // namespace assistant

// 助手的一个对话。一个窗口可以同时有好几个（AssistantController），各有各的后端、
// 工具（当前标签、快照里的 ref）、要用户回答的事和令牌：本机智能体经 --mcp 桥带着
// 这个对话的令牌连回来，工具在这个对话自己的那套上执行。对话的内容是给网页的条目
// （type：user、assistant、reasoning、tool、approval、question、notice），变了就告诉
// Host，Host 只把显示着的那个对话的转给网页。
//
// 一轮结束、做事的过程中隔一会儿，存进聊天记录（assistant::History）；从聊天记录打开的
// 接着聊：OpenAI 兼容服务带上前文，本机智能体有原来的会话就接上（--resume、
// thread/resume），没有就把前文写进第一条。
class AssistantConversation : public assistant::Backend::Delegate,
                              public agent::ToolRunner::Delegate,
                              public agent::ConnectionOwner {
 public:
  class Host {
   public:
    virtual StarryBrowserView* view() = 0;
    virtual assistant::History* history() = 0;
    // 给网页的事件（append、patch、delta）：显示着的对话才转过去。
    virtual void ConversationEvent(AssistantConversation* conversation,
                                   const std::string& event,
                                   base::Value payload) = 0;
    // 在不在做事、要不要用户回答、标题、状态那一句变了。
    virtual void ConversationChanged(AssistantConversation* conversation) = 0;
    // 是不是显示着的那个（在后台做完的要记「还没看」、网页上的框只为它画）。
    virtual bool IsActive(const AssistantConversation* conversation) const = 0;
    // 这一轮在哪个标签里做：用户看着的那个；它被别的对话占着时后台开一份（copy）。
    virtual tabs::TabInterface* ClaimTab(AssistantConversation* conversation,
                                         bool* copy) = 0;
    // 做事时又按了发送。
    virtual void ConversationRefusedSend(AssistantConversation* conversation) {}

   protected:
    virtual ~Host() = default;
  };

  // 新的对话。
  explicit AssistantConversation(Host* host);
  // 从聊天记录打开（record 的内容是 Record() 存下的 JSON）。
  AssistantConversation(Host* host, assistant::HistoryRecord record);
  AssistantConversation(const AssistantConversation&) = delete;
  AssistantConversation& operator=(const AssistantConversation&) = delete;
  ~AssistantConversation() override;

  const std::string& id() const { return id_; }
  const std::string& title() const { return title_; }
  const base::ListValue& items() const { return items_; }
  bool empty() const { return items_.empty(); }
  bool running() const;
  // 有要用户回答的事（确认、问题）。
  bool waiting_for_user() const;
  // 在后台做完了、用户还没看。
  bool unseen_result() const { return unseen_result_; }
  // 不是当前对话时还要留着的（显示成胶囊）。
  bool live() const {
    return running() || waiting_for_user() || unseen_result_;
  }
  // 正在做的那一步写成一句话（胶囊、标题旁边）；没在做事时是空的。
  std::u16string StatusLine() const;
  // 这一轮在做事的标签。
  tabs::TabInterface* working_tab() const;
  // 给网页的状态（status、model、mode……）：Host 再加上窗口的部分。
  base::DictValue State() const;

  // 网页发来的（显示着的对话）。mentions 是用户在输入框里 @ 的标签（短 ID）：读出
  // 正文放进 <browser_context>。
  void Send(const std::string& text,
            std::vector<agent::ToolImage> images,
            const std::vector<std::string>& mentions = {});
  // 输入框里打 @ 时列出的标签（id、标题、网址、是不是正在看的、在不在别的窗口）：用户
  // 自己的标签，不含空白页和浏览器自己的页面（读不了）。
  base::ListValue MentionTabs() const;
  void Stop();
  void Approve(const std::string& id,
               const std::string& decision,
               const std::string& note);
  void Answer(const std::string& id, std::optional<std::string> answer);
  // 这一轮的结果看过了。
  void MarkSeen();
  // 不留了（换了别的对话、窗口关了）：先存好，再结束后端。
  void Close();
  // 设置改了：下一条消息用新的（后端在发的时候按设置重建）。
  void SettingsChanged();

  // 调试：对话里的每一条（类型和文字）。
  std::string DebugDescription() const;
  // 调试：第一个等着回答的确认（approval）或问题（question）。
  std::optional<std::string> FirstPendingId(const std::string& type) const;

  // assistant::Backend::Delegate:
  void OnTextDelta(const std::string& text) override;
  void OnReasoningDelta(const std::string& text) override;
  void OnToolStarted(const std::string& id,
                     const std::string& name,
                     const base::DictValue& arguments) override;
  void OnToolFinished(const std::string& id,
                      const agent::ToolResult& result) override;
  void OnUsage(const assistant::Usage& usage) override;
  void OnStatus(const std::u16string& status) override;
  void OnStarting() override;
  void OnModel(const std::string& model) override;
  void OnToolsUnavailable(const std::string& detail) override;
  void OnSession(const std::string& id) override;
  void OnReasoningLevels(const std::vector<std::string>& levels) override;
  void OnQuestion(const std::string& id,
                  const agent::Question& question) override;
  void OnFinished(assistant::StopReason reason,
                  assistant::FailureKind failure,
                  const std::string& message) override;
  void ExecuteTool(const std::string& name,
                   base::DictValue arguments,
                   agent::ToolCallback done) override;

  // agent::ToolRunner::Delegate:
  void AuthorizeStep(
      const agent::ToolStep& step,
      base::OnceCallback<void(agent::Approval)> callback) override;
  void AskUser(
      const agent::Question& question,
      base::OnceCallback<void(std::optional<std::string>)> callback) override;
  void StepStarted(const agent::ToolStep& step) override;
  std::u16string AgentName() override;
  bool IsAssistant() override;
  void StopRequested() override;

  // agent::ConnectionOwner:
  base::ListValue AgentTools() override;
  std::string AgentInstructions() override;
  void RunAgentTool(const std::string& name,
                    base::DictValue arguments,
                    agent::ToolCallback done) override;

  // 启动本机智能体要准备的（在后台线程上找好）。
  using LocalLaunch = assistant::LocalLaunch;

 private:
  Profile* profile() const;
  std::string NextId();
  base::DictValue* FindItem(const std::string& id);
  void Append(base::DictValue item);
  void Patch(const std::string& id, base::DictValue fields);
  void Changed();
  void AddNotice(const std::u16string& text,
                 const std::string& kind,
                 const std::string& detail = std::string());
  // 读出用户 @ 的标签的正文（一起读，按原来的顺序交回）。
  void ReadMentions(
      std::vector<tabs::TabHandle> tabs,
      base::OnceCallback<void(std::vector<assistant::ReferencedTab>)> done);
  // 设置好了：取 Key、建后端，发出去。
  void StartTurn(assistant::UserMessage message);
  void StartLocalTurn(const assistant::Settings& settings,
                      assistant::UserMessage message);
  void LaunchLocal(assistant::Settings settings,
                   std::string signature,
                   assistant::UserMessage message,
                   LocalLaunch launch);
  void SendToBackend(assistant::UserMessage message);
  // 要换一个后端：有旧的就告诉用户之前的对话内容不带过去，再结束它。
  void SwitchBackend(const std::u16string& name);
  void ResetBackend();
  std::u16string BackendName(const assistant::Settings& settings) const;
  void ResolvePending();
  // 一轮结束（或者没开始就出错了）：存盘、记下结果。
  void TurnEnded();

  // 自动审核（确认规则是「自动」时的敏感操作）。
  void Review(const agent::ToolStep& step,
              base::OnceCallback<void(agent::Approval)> callback);
  void ReviewDone(std::string notice_id,
                  agent::ToolStep step,
                  base::OnceCallback<void(agent::Approval)> callback,
                  int turn,
                  std::optional<assistant::ReviewVerdict> verdict);
  std::unique_ptr<assistant::Backend> MakeReviewBackend(
      assistant::Reviewer* reviewer);
  void RecordDecision(const agent::ToolStep& step,
                      bool allowed,
                      const std::string& source);
  void CancelReview();
  // 弹出确认卡片，等用户点。
  void AskApproval(const agent::ToolStep& step,
                   base::OnceCallback<void(agent::Approval)> callback);

  // 聊天记录。
  void SchedulePersist();
  void PersistNow();
  std::string RecordJson() const;
  // 从聊天记录打开后第一次发消息：OpenAI 兼容服务的前文。
  std::vector<assistant::ChatMessage> ChatHistory() const;
  // 本机智能体没有原来的会话时带给模型的前文（最近的 limit 字）。
  std::optional<std::string> EarlierConversation() const;

  const raw_ptr<Host> host_;
  const std::string id_;
  const base::Time created_;
  std::string title_;

  std::unique_ptr<assistant::Backend> backend_;
  // OpenAI 兼容服务时就是 backend_（换服务、模型时不重建）。
  raw_ptr<assistant::ChatBackend> chat_ = nullptr;
  std::string backend_signature_;
  // 实际在用的后端（settings.backend 的值）和模型。
  std::string active_backend_;
  std::string active_model_;
  // 本机智能体的令牌（AgentService）。
  std::string token_;
  // 本机智能体的会话：后端 → 会话 ID（接着聊时用）。
  std::map<std::string, std::string> sessions_;
  // 从聊天记录打开、还没发过消息：第一次发时让模型接上前文。
  bool restored_ = false;
  // 后端自己问的问题（Codex 的 request_user_input）：对话里的 id → 后端的 id。
  std::map<std::string, std::string> backend_questions_;
  std::unique_ptr<agent::ToolRunner> tools_;
  assistant::Usage usage_;

  base::ListValue items_;
  int next_id_ = 1;
  std::string text_item_;
  std::string reasoning_item_;
  std::map<std::string, std::string> tool_items_;
  std::string current_tool_item_;
  bool starting_ = false;
  bool agent_starting_ = false;
  bool unseen_result_ = false;
  // 这一轮的确认规则和思考程度（发的时候定下，中途改设置下一轮才生效）。
  std::string turn_mode_;
  std::string turn_reasoning_;
  // 第几轮：审核回来时这一轮已经结束了就不算数。
  int turn_ = 0;

  std::map<std::string, base::OnceCallback<void(agent::Approval)>> approvals_;
  std::map<std::string, agent::ToolStep> approval_steps_;
  std::map<std::string, base::OnceCallback<void(std::optional<std::string>)>>
      questions_;
  std::set<std::string> allowed_sites_;
  GURL last_user_url_;

  std::unique_ptr<assistant::Reviewer> reviewer_;
  // 之前的确认和拒绝（给审核员看）。
  base::ListValue review_history_;

  // 对话里的图片（assistant_images.h），还没存进聊天记录的单独记着。
  std::set<std::string> image_ids_;
  std::map<std::string, agent::ToolImage> unsaved_images_;
  // 从聊天记录打开的图片：接着聊时作为前文发给 OpenAI 兼容服务，用完清掉。
  std::map<std::string, agent::ToolImage> restored_images_;
  // 内容变过、还没存。只是打开看看不算：不然看一眼就跑到聊天记录最前面。
  bool dirty_ = false;
  base::OneShotTimer persist_timer_;

  base::WeakPtrFactory<AssistantConversation> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_ASSISTANT_ASSISTANT_CONVERSATION_H_
