// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_ASSISTANT_ASSISTANT_CONTROLLER_H_
#define STARRY_BROWSER_UI_ASSISTANT_ASSISTANT_CONTROLLER_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/values.h"
#include "starry/browser/agent/browser_tools.h"
#include "starry/browser/assistant/assistant_history.h"
#include "starry/browser/ui/assistant/assistant_conversation.h"

namespace content {
class WebContents;
}

namespace starry {

class StarryBrowserView;

// 一个窗口里的助手：同时可以有好几个对话，一个显示着（active），别的在做事、
// 等回答或者刚做完的是胶囊（Capsules，岛上面一排）。笔：开一个新对话，在做事的
// 接着做；不留的对话存进聊天记录再结束。聊天记录普通窗口共用用户资料里的一份，
// 无痕窗口自己一份（窗口关了就没了）。
//
// 界面是岛里的网页（starry://assistant，AssistantHandler 转消息），这里给它发事件：
//   reset {items, state}   整个对话（网页刚载入、换了对话）
//   append {item}          新的一条
//   patch {id, ...}        改一条（工具做完、确认回答了）
//   delta {id, text}       回答、思考的文字接着往后写
//   state {...}            在不在做事、设置好了没有、确认规则、思考程度、样子
//   history {...}          聊天记录（网页打开了聊天记录时）
//   attach {image}         截取的页面（输入框上面多一张图）
//   mentionTabs [tab]      输入框里打 @ 时能引用的标签（网页要的时候给）
//   focus                  把焦点放进输入框
class AssistantController : public AssistantConversation::Host,
                            public assistant::History::Observer {
 public:
  // 岛里的网页（AssistantHandler）。
  class Page {
   public:
    virtual void Fire(const std::string& event, base::Value payload) = 0;

   protected:
    virtual ~Page() = default;
  };

  // 胶囊上要显示的（AssistantCapsules）。
  struct Capsule {
    std::string id;
    std::u16string title;
    std::u16string detail;
    bool busy = false;
    bool waiting = false;
    bool done = false;
  };

  explicit AssistantController(StarryBrowserView* view);
  AssistantController(const AssistantController&) = delete;
  AssistantController& operator=(const AssistantController&) = delete;
  ~AssistantController() override;

  // 岛里的网页属于哪个窗口（打开链接、菜单）。
  static StarryBrowserView* ViewForPage(content::WebContents* contents);
  static void SetPageOwner(content::WebContents* contents,
                           StarryBrowserView* view);

  AssistantConversation* active() const { return active_; }
  bool running() const;
  bool has_conversation() const;
  // 显示着的对话有要用户回答的事（确认、问题）：岛要展开。
  bool waiting_for_user() const;
  // 哪个对话有要用户回答的事（胶囊变黄、岛自己打开）。
  bool any_waiting() const;
  // 有对话在做事（岛关着时 ✦ 上的小点）。
  bool any_running() const;
  std::vector<Capsule> Capsules() const;
  // 网页开着聊天记录（胶囊收起来）。
  bool history_shown() const { return history_query_.has_value(); }

  void AttachPage(Page* page);
  void DetachPage(Page* page);

  // 网页发来的。
  void PageReady();
  // mentions：用户 @ 的标签（短 ID）。
  void Send(const std::string& text,
            std::vector<agent::ToolImage> images,
            const std::vector<std::string>& mentions = {});
  // 网页要能 @ 的标签：发 mentionTabs。
  void ListMentionTabs();
  void Stop();
  // 笔：开一个新对话（现在的是空的就只把焦点放进输入框）。
  void NewChat();
  void Approve(const std::string& id,
               const std::string& decision,
               const std::string& note);
  void Answer(const std::string& id, std::optional<std::string> answer);
  void SetMode(const std::string& mode);
  void SetReasoning(const std::string& level);
  void SetPanelStyle(const std::string& style);
  // 切到另一个对话（点胶囊、聊天记录里进行中的）。show_tab：它在哪个标签里做事就切过去。
  void Activate(const std::string& id, bool show_tab);
  // 胶囊上的 ×：停下、存好、不留了。
  void Dismiss(const std::string& id);
  // 网页打开、关上聊天记录，搜索（nullopt：关上）。
  void ShowHistory(std::optional<std::string> query);
  // 让网页打开聊天记录（「还有 N 个」的胶囊）。
  void OpenHistoryPanel();
  // 打开聊天记录里的一条：已经开着（这个窗口或者共用记录的别的窗口）就切过去。
  void OpenRecord(const std::string& id);
  void DeleteRecord(const std::string& id);
  void ClearHistory();
  // 截取正在看的网页，作为一张图片放进输入框（attach 事件）。
  void CapturePage();
  // 设置改了（服务、模型、Key、样子）：下一条消息用新的。
  void SettingsChanged();
  // 设置里关掉了助手：每个对话停下、存进聊天记录、结束（本机智能体的进程和令牌
  // 一起没了），换成一个空的。
  void CloseAll();
  // 让网页把焦点放进输入框。
  void FocusComposer();
  // 调试：显示着的对话里的每一条。
  std::string DebugDescription() const;
  std::optional<std::string> FirstPendingId(const std::string& type) const;
  size_t conversation_count() const { return conversations_.size(); }

  // 状态变了（岛要不要展开、胶囊、侧栏按钮上的小点）。
  void SetStateChangedCallback(base::RepeatingClosure callback);
  // 换了显示的对话（岛把它展开、晃掉收起的样子）。
  void SetActivatedCallback(base::RepeatingCallback<void(bool focus)> callback);

  // AssistantConversation::Host:
  StarryBrowserView* view() override;
  assistant::History* history() override;
  void ConversationEvent(AssistantConversation* conversation,
                         const std::string& event,
                         base::Value payload) override;
  void ConversationChanged(AssistantConversation* conversation) override;
  bool IsActive(const AssistantConversation* conversation) const override;
  tabs::TabInterface* ClaimTab(AssistantConversation* conversation,
                               bool* copy) override;
  void ConversationRefusedSend(AssistantConversation* conversation) override;

  // assistant::History::Observer:
  void OnHistoryChanged() override;

 private:
  AssistantConversation* Find(const std::string& id) const;
  void ActivateConversation(AssistantConversation* conversation,
                            bool focus,
                            bool show_tab);
  void Retire(AssistantConversation* conversation);
  void Fire(const std::string& event, base::Value payload);
  base::DictValue State() const;
  void SendState();
  void SendHistory();
  void OnHistoryListed(std::string query,
                       std::vector<assistant::HistorySummary> rows);
  void OnRecordLoaded(std::optional<assistant::HistoryRecord> record);
  // 网页上的提示（AgentOverlay）：岛的样子变了时重新摆提示条。
  void UpdateOverlay();

  const raw_ptr<StarryBrowserView> view_;
  raw_ptr<Page> page_ = nullptr;
  // 无痕窗口自己的聊天记录；普通窗口用 History::ForProfile。
  std::unique_ptr<assistant::History> private_history_;
  raw_ptr<assistant::History> history_ = nullptr;
  base::ScopedObservation<assistant::History, assistant::History::Observer>
      history_observation_{this};
  std::vector<std::unique_ptr<AssistantConversation>> conversations_;
  raw_ptr<AssistantConversation> active_ = nullptr;
  // 网页开着聊天记录时的搜索词。
  std::optional<std::string> history_query_;
  // 正在读盘的那一条（读完时可能已经开了）。
  std::string loading_record_;

  base::RepeatingClosure state_changed_;
  base::RepeatingCallback<void(bool)> activated_;
  base::WeakPtrFactory<AssistantController> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_ASSISTANT_ASSISTANT_CONTROLLER_H_
