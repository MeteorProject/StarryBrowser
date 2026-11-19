// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/assistant/assistant_controller.h"

#include <algorithm>
#include <utility>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tabs/public/tab_interface.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_user_data.h"
#include "starry/browser/assistant/assistant_settings.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/assistant/agent_overlay.h"
#include "starry/browser/ui/assistant/assistant_island.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/internal_pages.h"
#include "starry/common/internal_urls.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/base_window.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/geometry/size_conversions.h"

namespace starry {

namespace {

// 岛里的网页属于哪个窗口。
class AssistantPageOwner
    : public content::WebContentsUserData<AssistantPageOwner> {
 public:
  StarryBrowserView* view() const { return view_; }
  void set_view(StarryBrowserView* view) { view_ = view; }

 private:
  friend class content::WebContentsUserData<AssistantPageOwner>;
  explicit AssistantPageOwner(content::WebContents* contents)
      : content::WebContentsUserData<AssistantPageOwner>(*contents) {}
  raw_ptr<StarryBrowserView> view_ = nullptr;
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(AssistantPageOwner);

// 所有窗口的助手：打开一条聊天记录前看看是不是已经在别的窗口开着；一个对话占着的标签
// 别的对话不碰。
std::vector<AssistantController*>& Controllers() {
  static base::NoDestructor<std::vector<AssistantController*>> controllers;
  return *controllers;
}

// 截取的页面最长边；聊天记录里进行中的对话的缩略图。
constexpr int kCaptureSide = 2048;
constexpr int kThumbnailSide = 480;

// 截下网页现在的样子（只有网页，没有玻璃），编成 JPEG。
void CaptureTab(
    content::WebContents* contents,
    int max_side,
    base::OnceCallback<void(std::optional<agent::ToolImage>)> done) {
  content::RenderWidgetHostView* view =
      contents ? contents->GetRenderWidgetHostView() : nullptr;
  if (!view) {
    std::move(done).Run(std::nullopt);
    return;
  }
  gfx::Size size = gfx::ScaleToCeiledSize(view->GetViewBounds().size(),
                                          view->GetDeviceScaleFactor());
  const int longest = std::max(size.width(), size.height());
  if (longest <= 0) {
    std::move(done).Run(std::nullopt);
    return;
  }
  if (longest > max_side) {
    size =
        gfx::ScaleToRoundedSize(size, static_cast<float>(max_side) / longest);
  }
  view->CopyFromSurface(
      gfx::Rect(), size, base::Seconds(5),
      base::BindOnce(
          [](base::OnceCallback<void(std::optional<agent::ToolImage>)> done,
             const content::CopyFromSurfaceResult& result) {
            if (!result.has_value() || result->bitmap.drawsNothing()) {
              std::move(done).Run(std::nullopt);
              return;
            }
            base::ThreadPool::PostTaskAndReplyWithResult(
                FROM_HERE, {base::TaskPriority::USER_VISIBLE},
                base::BindOnce(
                    [](SkBitmap bitmap) -> std::optional<agent::ToolImage> {
                      std::optional<std::vector<uint8_t>> jpeg =
                          gfx::JPEGCodec::Encode(bitmap, 85);
                      if (!jpeg) {
                        return std::nullopt;
                      }
                      return agent::ToolImage{"image/jpeg",
                                              base::Base64Encode(*jpeg)};
                    },
                    result->bitmap),
                std::move(done));
          },
          std::move(done)));
}

}  // namespace

// static
StarryBrowserView* AssistantController::ViewForPage(
    content::WebContents* contents) {
  AssistantPageOwner* owner =
      contents ? AssistantPageOwner::FromWebContents(contents) : nullptr;
  return owner ? owner->view() : nullptr;
}

// static
void AssistantController::SetPageOwner(content::WebContents* contents,
                                       StarryBrowserView* view) {
  AssistantPageOwner::CreateForWebContents(contents);
  AssistantPageOwner::FromWebContents(contents)->set_view(view);
}

AssistantController::AssistantController(StarryBrowserView* view)
    : view_(view) {
  Profile* profile = view->browser()->GetProfile();
  // 无痕窗口的聊天记录和普通窗口、别的无痕窗口都分开，只在内存里。
  if (profile->IsOffTheRecord()) {
    private_history_ = std::make_unique<assistant::History>(base::FilePath());
    history_ = private_history_.get();
  } else {
    history_ = assistant::History::ForProfile(profile);
  }
  history_observation_.Observe(history_.get());
  conversations_.push_back(std::make_unique<AssistantConversation>(this));
  active_ = conversations_.back().get();
  Controllers().push_back(this);
}

AssistantController::~AssistantController() {
  std::erase(Controllers(), this);
  history_observation_.Reset();
  // 窗口关了：每个对话先存好再结束。
  active_ = nullptr;
  for (auto& conversation : conversations_) {
    conversation->Close();
  }
  conversations_.clear();
}

StarryBrowserView* AssistantController::view() {
  return view_;
}

assistant::History* AssistantController::history() {
  return history_;
}

bool AssistantController::running() const {
  return active_ && active_->running();
}

bool AssistantController::has_conversation() const {
  return active_ && !active_->empty();
}

bool AssistantController::waiting_for_user() const {
  return active_ && active_->waiting_for_user();
}

bool AssistantController::any_waiting() const {
  return std::ranges::any_of(conversations_, [](const auto& conversation) {
    return conversation->waiting_for_user();
  });
}

bool AssistantController::any_running() const {
  return std::ranges::any_of(conversations_, [](const auto& conversation) {
    return conversation->running();
  });
}

std::vector<AssistantController::Capsule> AssistantController::Capsules()
    const {
  std::vector<Capsule> capsules;
  for (const auto& conversation : conversations_) {
    if (conversation.get() == active_ || !conversation->live()) {
      continue;
    }
    Capsule capsule;
    capsule.id = conversation->id();
    capsule.title = base::UTF8ToUTF16(conversation->title());
    capsule.busy = conversation->running();
    capsule.waiting = conversation->waiting_for_user();
    capsule.done = conversation->unseen_result() && !capsule.busy;
    capsule.detail =
        capsule.done ? L("assistant.capsule.done") : conversation->StatusLine();
    capsules.push_back(std::move(capsule));
  }
  return capsules;
}

void AssistantController::SetStateChangedCallback(
    base::RepeatingClosure callback) {
  state_changed_ = std::move(callback);
}

void AssistantController::SetActivatedCallback(
    base::RepeatingCallback<void(bool)> callback) {
  activated_ = std::move(callback);
}

void AssistantController::AttachPage(Page* page) {
  page_ = page;
}

void AssistantController::DetachPage(Page* page) {
  if (page_ == page) {
    page_ = nullptr;
    history_query_.reset();
  }
}

AssistantConversation* AssistantController::Find(const std::string& id) const {
  for (const auto& conversation : conversations_) {
    if (conversation->id() == id) {
      return conversation.get();
    }
  }
  return nullptr;
}

void AssistantController::Fire(const std::string& event, base::Value payload) {
  if (page_) {
    page_->Fire(event, std::move(payload));
  }
}

base::DictValue AssistantController::State() const {
  base::DictValue state = active_->State();
  const assistant::Settings settings =
      assistant::ReadSettings(view_->browser()->GetProfile());
  state.Set("docked", settings.docked());
  state.Set("capsules", static_cast<int>(Capsules().size()));
  return state;
}

void AssistantController::SendState() {
  if (!active_) {
    return;
  }
  Fire("state", base::Value(State()));
  UpdateOverlay();
  if (state_changed_) {
    state_changed_.Run();
  }
}

void AssistantController::UpdateOverlay() {
  // 描边和提示条跟着智能体的状态（agent::AgentActivities）自己变；岛的样子变了（开着、
  // 停在右边）时提示条要让开或者藏起来。
  if (AgentOverlay* overlay = view_->agent_overlay()) {
    overlay->Update();
  }
}

void AssistantController::PageReady() {
  base::DictValue reset;
  reset.Set("items", active_->items().Clone());
  reset.Set("state", State());
  Fire("reset", base::Value(std::move(reset)));
  if (history_query_) {
    SendHistory();
  }
}

void AssistantController::FocusComposer() {
  Fire("focus", base::Value());
}

void AssistantController::SettingsChanged() {
  // 设置页里换了样子：开着的直接变过去。
  view_->assistant_island()->PanelStyleChanged();
  for (const auto& conversation : conversations_) {
    conversation->SettingsChanged();
  }
  SendState();
}

void AssistantController::CloseAll() {
  // 同析构：先没有显示的对话（结束时发来的事件不再给网页），一个个存好再结束。
  active_ = nullptr;
  std::vector<std::unique_ptr<AssistantConversation>> closing =
      std::move(conversations_);
  conversations_.clear();
  for (auto& conversation : closing) {
    conversation->Close();
  }
  closing.clear();
  conversations_.push_back(std::make_unique<AssistantConversation>(this));
  active_ = conversations_.back().get();
  history_query_.reset();
  PageReady();
  SendState();
}

void AssistantController::Send(const std::string& text,
                               std::vector<agent::ToolImage> images,
                               const std::vector<std::string>& mentions) {
  active_->Send(text, std::move(images), mentions);
}

void AssistantController::ListMentionTabs() {
  Fire("mentionTabs", base::Value(active_->MentionTabs()));
}

void AssistantController::Stop() {
  active_->Stop();
}

void AssistantController::Approve(const std::string& id,
                                  const std::string& decision,
                                  const std::string& note) {
  active_->Approve(id, decision, note);
}

void AssistantController::Answer(const std::string& id,
                                 std::optional<std::string> answer) {
  active_->Answer(id, std::move(answer));
}

void AssistantController::SetMode(const std::string& mode) {
  assistant::SetMode(view_->browser()->GetProfile(), mode);
  SendState();
}

void AssistantController::SetReasoning(const std::string& level) {
  assistant::SetReasoning(view_->browser()->GetProfile(), level);
  SendState();
}

void AssistantController::SetPanelStyle(const std::string& style) {
  // 所有窗口一起换，开着的面板直接变形。
  assistant::SetPanelStyle(view_->browser()->GetProfile(), style);
  for (AssistantController* controller : Controllers()) {
    if (controller->view_->browser()->GetProfile()->GetOriginalProfile() ==
        view_->browser()->GetProfile()->GetOriginalProfile()) {
      controller->view_->assistant_island()->PanelStyleChanged();
      controller->SendState();
    }
  }
}

void AssistantController::NewChat() {
  if (active_->empty()) {
    if (activated_) {
      activated_.Run(/*focus=*/true);
    }
    FocusComposer();
    return;
  }
  conversations_.push_back(std::make_unique<AssistantConversation>(this));
  ActivateConversation(conversations_.back().get(), /*focus=*/true,
                       /*show_tab=*/false);
}

void AssistantController::Activate(const std::string& id, bool show_tab) {
  if (AssistantConversation* conversation = Find(id)) {
    ActivateConversation(conversation, /*focus=*/false, show_tab);
  }
}

void AssistantController::ActivateConversation(
    AssistantConversation* conversation,
    bool focus,
    bool show_tab) {
  AssistantConversation* previous = active_;
  active_ = conversation;
  conversation->MarkSeen();
  // 不在做事、不等回答、也没有新结果的：存进聊天记录、不留了；在忙的变成胶囊。
  if (previous && previous != conversation && !previous->live()) {
    Retire(previous);
  }
  // 它在哪个标签里做事（等确认时要看网页上标出的那一处）就切过去。
  if (show_tab) {
    if (tabs::TabInterface* tab = conversation->working_tab()) {
      TabStripModel* model = view_->browser()->GetTabStripModel();
      const int index = model->GetIndexOfTab(tab);
      if (index != TabStripModel::kNoTab && index != model->active_index()) {
        model->ActivateTabAt(index);
      }
    }
  }
  history_query_.reset();
  PageReady();
  SendState();
  if (activated_) {
    activated_.Run(focus);
  }
  if (focus) {
    FocusComposer();
  }
}

void AssistantController::Retire(AssistantConversation* conversation) {
  auto it = std::ranges::find_if(conversations_, [&](const auto& item) {
    return item.get() == conversation;
  });
  if (it == conversations_.end()) {
    return;
  }
  std::unique_ptr<AssistantConversation> owned = std::move(*it);
  conversations_.erase(it);
  owned->Close();
}

void AssistantController::Dismiss(const std::string& id) {
  AssistantConversation* conversation = Find(id);
  if (!conversation || conversation == active_) {
    return;
  }
  Retire(conversation);
  SendState();
}

void AssistantController::ShowHistory(std::optional<std::string> query) {
  history_query_ = std::move(query);
  if (history_query_) {
    SendHistory();
  }
  if (state_changed_) {
    state_changed_.Run();
  }
}

void AssistantController::OpenHistoryPanel() {
  Fire("openHistory", base::Value());
}

void AssistantController::SendHistory() {
  history_->List(*history_query_,
                 base::BindOnce(&AssistantController::OnHistoryListed,
                                weak_factory_.GetWeakPtr(), *history_query_));
}

void AssistantController::OnHistoryListed(
    std::string query,
    std::vector<assistant::HistorySummary> rows) {
  if (!history_query_ || *history_query_ != query) {
    return;
  }
  // 进行中的对话（搜索时不显示）：带网页缩略图的小卡片，下面的列表里就不重复了。
  base::ListValue live;
  std::set<std::string> live_ids;
  for (const auto& conversation : conversations_) {
    if (!conversation->live() || conversation->empty()) {
      continue;
    }
    live_ids.insert(conversation->id());
    if (!query.empty()) {
      continue;
    }
    const bool busy = conversation->running();
    const bool done = conversation->unseen_result() && !busy;
    live.Append(base::DictValue()
                    .Set("id", conversation->id())
                    .Set("title", conversation->title())
                    .Set("waiting", conversation->waiting_for_user())
                    .Set("status", done ? L("assistant.capsule.done")
                                        : conversation->StatusLine()));
    if (tabs::TabInterface* tab = conversation->working_tab()) {
      CaptureTab(
          tab->GetContents(), kThumbnailSide,
          base::BindOnce(
              [](base::WeakPtr<AssistantController> controller, std::string id,
                 std::optional<agent::ToolImage> image) {
                if (!controller || !image) {
                  return;
                }
                controller->Fire(
                    "historyThumb",
                    base::Value(base::DictValue().Set("id", id).Set(
                        "src", base::StrCat({"data:", image->mime_type,
                                             ";base64,", image->base64}))));
              },
              weak_factory_.GetWeakPtr(), conversation->id()));
    }
  }
  base::ListValue list;
  for (const assistant::HistorySummary& row : rows) {
    if (live_ids.contains(row.id)) {
      continue;
    }
    list.Append(
        base::DictValue()
            .Set("id", row.id)
            .Set("title", row.title)
            .Set("site", row.site)
            .Set("updated", row.updated.InMillisecondsFSinceUnixEpoch()));
  }
  Fire("history", base::Value(base::DictValue()
                                  .Set("query", query)
                                  .Set("private", history_->ephemeral())
                                  .Set("live", std::move(live))
                                  .Set("rows", std::move(list))));
}

void AssistantController::OnHistoryChanged() {
  if (history_query_) {
    SendHistory();
  }
}

void AssistantController::OpenRecord(const std::string& id) {
  if (AssistantConversation* live = Find(id)) {
    ActivateConversation(live, /*focus=*/false, /*show_tab=*/false);
    return;
  }
  // 共用这份聊天记录的别的窗口开着它：切到那个窗口。
  for (AssistantController* other : Controllers()) {
    if (other == this || other->history_ != history_) {
      continue;
    }
    if (AssistantConversation* live = other->Find(id)) {
      other->view_->browser()->GetWindow()->Activate();
      other->view_->assistant_island()->Open(/*focus=*/false);
      other->ActivateConversation(live, /*focus=*/false, /*show_tab=*/false);
      history_query_.reset();
      SendState();
      return;
    }
  }
  loading_record_ = id;
  history_->Load(id, base::BindOnce(&AssistantController::OnRecordLoaded,
                                    weak_factory_.GetWeakPtr()));
}

void AssistantController::OnRecordLoaded(
    std::optional<assistant::HistoryRecord> record) {
  if (!record || record->id != loading_record_) {
    return;
  }
  loading_record_.clear();
  // 等读盘的时候可能已经开了。
  if (AssistantConversation* live = Find(record->id)) {
    ActivateConversation(live, /*focus=*/false, /*show_tab=*/false);
    return;
  }
  conversations_.push_back(
      std::make_unique<AssistantConversation>(this, std::move(*record)));
  ActivateConversation(conversations_.back().get(), /*focus=*/false,
                       /*show_tab=*/false);
}

void AssistantController::DeleteRecord(const std::string& id) {
  history_->Delete(id);
}

void AssistantController::ClearHistory() {
  history_->DeleteAll();
}

void AssistantController::CapturePage() {
  tabs::TabInterface* tab = view_->browser()->GetActiveTabInterface();
  if (!tab) {
    return;
  }
  CaptureTab(tab->GetContents(), kCaptureSide,
             base::BindOnce(
                 [](base::WeakPtr<AssistantController> controller,
                    std::optional<agent::ToolImage> image) {
                   if (!controller || !image) {
                     return;
                   }
                   controller->Fire(
                       "attach", base::Value(base::DictValue()
                                                 .Set("mime", image->mime_type)
                                                 .Set("data", image->base64)));
                 },
                 weak_factory_.GetWeakPtr()));
}

// ---- AssistantConversation::Host ----

void AssistantController::ConversationEvent(AssistantConversation* conversation,
                                            const std::string& event,
                                            base::Value payload) {
  if (conversation == active_) {
    Fire(event, std::move(payload));
  }
}

void AssistantController::ConversationChanged(
    AssistantConversation* conversation) {
  SendState();
}

bool AssistantController::IsActive(
    const AssistantConversation* conversation) const {
  return conversation == active_;
}

void AssistantController::ConversationRefusedSend(
    AssistantConversation* conversation) {
  if (conversation == active_) {
    view_->assistant_island()->Shake();
  }
}

tabs::TabInterface* AssistantController::ClaimTab(
    AssistantConversation* conversation,
    bool* copy) {
  *copy = false;
  tabs::TabInterface* tab = view_->browser()->GetActiveTabInterface();
  if (!tab) {
    return nullptr;
  }
  bool taken = false;
  for (AssistantController* controller : Controllers()) {
    for (const auto& other : controller->conversations_) {
      if (other.get() != conversation && other->running() &&
          other->working_tab() == tab) {
        taken = true;
      }
    }
  }
  // 空白页、浏览器自己的页面工具本来就读不了，不用开一份。
  const GURL url = tab->GetContents()->GetLastCommittedURL();
  if (!taken || IsBlankTabUrl(url) || IsInternalURL(url) ||
      url.SchemeIs("devtools")) {
    return tab;
  }
  // 用户看着的标签被别的对话占着：后台开一份同样的网页给这个对话。
  NavigateParams params(view_->browser(), url, ui::PAGE_TRANSITION_LINK);
  params.disposition = WindowOpenDisposition::NEW_BACKGROUND_TAB;
  Navigate(&params);
  content::WebContents* contents = params.navigated_or_inserted_contents;
  tabs::TabInterface* opened =
      contents ? tabs::TabInterface::MaybeGetFromContents(contents) : nullptr;
  if (!opened) {
    return tab;
  }
  *copy = true;
  return opened;
}

std::string AssistantController::DebugDescription() const {
  std::string text = base::StrCat(
      {"conversations=", base::NumberToString(conversations_.size()),
       " capsules=", base::NumberToString(Capsules().size()), " "});
  return text + active_->DebugDescription();
}

std::optional<std::string> AssistantController::FirstPendingId(
    const std::string& type) const {
  return active_->FirstPendingId(type);
}

}  // namespace starry
