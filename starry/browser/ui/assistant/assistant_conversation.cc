// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/assistant/assistant_conversation.h"

#include <algorithm>
#include <utility>

#include "base/barrier_callback.h"
#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/common/chrome_paths.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/agent/agent_socket.h"
#include "starry/browser/agent/page_commands.h"
#include "starry/browser/assistant/assistant_history.h"
#include "starry/browser/assistant/assistant_images.h"
#include "starry/browser/assistant/assistant_prompt.h"
#include "starry/browser/assistant/assistant_settings.h"
#include "starry/browser/assistant/claude_code_backend.h"
#include "starry/browser/assistant/codex_backend.h"
#include "starry/browser/assistant/line_process.h"
#include "starry/browser/assistant/local_agent.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/internal_pages.h"
#include "starry/common/internal_urls.h"

namespace starry {

namespace {

// 工具结果在对话里只留开头（展开看）。
constexpr size_t kOutputLimit = 8000;
// 气泡上面的选中文字只留开头。
constexpr size_t kSelectionLimit = 800;
// 标题：第一句话的开头。
constexpr size_t kTitleLimit = 60;
// 搜索用的文字、换后端时带给模型的前文：最多这么多字。
constexpr size_t kSearchLimit = 20000;
constexpr size_t kEarlierLimit = 20000;
// 一次最多附这么多张图。
constexpr size_t kMaxImages = 8;
// 一次最多 @ 这么多个标签；它们的正文一共最多带这么多字（UTF-16），一个最多同
// browser_read_page 一次读的。再长的告诉模型用 browser_read_page 接着读。
constexpr size_t kMaxMentions = 6;
constexpr size_t kMentionBudget = 40000;
constexpr size_t kMentionLimit = 20000;
// 做事的过程中隔一会儿存一次：中途退出也不会丢太多。
constexpr base::TimeDelta kPersistDelay = base::Seconds(2);

std::string Truncate(const std::string& text, size_t limit) {
  if (text.size() <= limit) {
    return text;
  }
  return text.substr(0, limit) + "\n…";
}

std::string Text(const base::DictValue& dict, const char* key) {
  const std::string* text = dict.FindString(key);
  return text ? *text : std::string();
}

bool IsType(const base::Value& value, std::string_view type) {
  return value.is_dict() && Text(value.GetDict(), "type") == type;
}

// 第一句话的开头（按字符截，不截断 UTF-8）。
std::string TitleFrom(const std::string& text) {
  std::u16string line = base::UTF8ToUTF16(text);
  line = line.substr(0, line.find(u'\n'));
  line = std::u16string(base::TrimWhitespace(line, base::TRIM_ALL));
  if (line.size() > kTitleLimit) {
    line = line.substr(0, kTitleLimit) + u"…";
  }
  return base::UTF16ToUTF8(line);
}

// 从后面截掉，留最后 limit 个字节（不截断 UTF-8）。
std::string KeepTail(const std::string& text, size_t limit) {
  if (text.size() <= limit) {
    return text;
  }
  size_t start = text.size() - limit;
  while (start < text.size() &&
         (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) {
    ++start;
  }
  return "…" + text.substr(start);
}

// @ 的标签一起读：读完按原来的顺序交回，发出去以后、读之前关掉了的（没有 id）不带。
using IndexedTab = std::pair<size_t, assistant::ReferencedTab>;

void CollectMentions(
    base::OnceCallback<void(std::vector<assistant::ReferencedTab>)> done,
    std::vector<IndexedTab> results) {
  std::ranges::sort(results, {}, &IndexedTab::first);
  std::vector<assistant::ReferencedTab> tabs;
  for (IndexedTab& result : results) {
    if (!result.second.id.empty()) {
      tabs.push_back(std::move(result.second));
    }
  }
  std::move(done).Run(std::move(tabs));
}

}  // namespace

AssistantConversation::AssistantConversation(Host* host)
    : host_(host),
      id_(base::Uuid::GenerateRandomV4().AsLowercaseString()),
      created_(base::Time::Now()),
      tools_(
          std::make_unique<agent::ToolRunner>(host->view()->browser(), this)) {}

AssistantConversation::AssistantConversation(Host* host,
                                             assistant::HistoryRecord record)
    : host_(host),
      id_(record.id),
      created_(record.created),
      title_(record.title),
      tools_(
          std::make_unique<agent::ToolRunner>(host->view()->browser(), this)) {
  std::optional<base::Value> value =
      base::JSONReader::Read(record.payload, base::JSON_PARSE_RFC);
  if (!value || !value->is_dict()) {
    return;
  }
  base::DictValue& dict = value->GetDict();
  active_backend_ = Text(dict, "backend");
  active_model_ = Text(dict, "model");
  if (const base::DictValue* sessions = dict.FindDict("sessions")) {
    claude_session_id_ = Text(*sessions, assistant::kBackendClaudeCode);
  }
  if (const base::DictValue* usage = dict.FindDict("usage")) {
    usage_.input_tokens = usage->FindInt("input").value_or(0);
    usage_.output_tokens = usage->FindInt("output").value_or(0);
    usage_.cost = usage->FindDouble("cost").value_or(0);
  }
  next_id_ = std::max(1, dict.FindInt("nextId").value_or(1));
  if (base::ListValue* items = dict.FindList("items")) {
    for (base::Value& item : *items) {
      // 认不得的条目跳过（以后的版本加了新的种类），不让整个对话读不出来。
      if (!item.is_dict() || !item.GetDict().FindString("type") ||
          !item.GetDict().FindString("id")) {
        continue;
      }
      if (base::ListValue* images = item.GetDict().FindList("images")) {
        for (base::Value& image : *images) {
          if (!image.is_dict()) {
            continue;
          }
          const std::string image_id = Text(image.GetDict(), "id");
          auto it = record.images.find(image_id);
          if (it == record.images.end()) {
            continue;
          }
          assistant::AddImage(image_id, it->second);
          image_ids_.insert(image_id);
          image.GetDict().Set(
              "src", assistant::ImagePath(image_id, it->second.mime_type));
          restored_images_[image_id] = it->second;
        }
      }
      items_.Append(std::move(item));
    }
  }
  restored_ = !items_.empty();
}

AssistantConversation::~AssistantConversation() {
  // 等着的回调先回答掉（工具在等它们）。
  CancelReview();
  ResetBackend();
  tools_->Cancel();
  for (const std::string& image : image_ids_) {
    assistant::RemoveImage(image);
  }
}

Profile* AssistantConversation::profile() const {
  return host_->view()->browser()->GetProfile();
}

bool AssistantConversation::running() const {
  return starting_ || (backend_ && backend_->running());
}

bool AssistantConversation::waiting_for_user() const {
  return !approvals_.empty() || !questions_.empty() ||
         !backend_questions_.empty();
}

tabs::TabInterface* AssistantConversation::working_tab() const {
  return tools_->current_tab();
}

std::u16string AssistantConversation::StatusLine() const {
  if (!approvals_.empty()) {
    return L("assistant.status.waitingApproval");
  }
  if (!questions_.empty() || !backend_questions_.empty()) {
    return L("assistant.status.waitingAnswer");
  }
  if (!running()) {
    return std::u16string();
  }
  if (agent_starting_) {
    return L("assistant.status.starting");
  }
  if (!text_item_.empty()) {
    return L("assistant.status.replying");
  }
  if (!current_tool_item_.empty()) {
    for (const base::Value& item : items_) {
      if (item.is_dict() && Text(item.GetDict(), "id") == current_tool_item_) {
        return base::UTF8ToUTF16(Text(item.GetDict(), "title"));
      }
    }
  }
  return L("assistant.status.thinking");
}

std::string AssistantConversation::NextId() {
  return base::StrCat({"i", base::NumberToString(next_id_++)});
}

base::DictValue* AssistantConversation::FindItem(const std::string& id) {
  for (base::Value& item : items_) {
    if (item.is_dict()) {
      const std::string* item_id = item.GetDict().FindString("id");
      if (item_id && *item_id == id) {
        return &item.GetDict();
      }
    }
  }
  return nullptr;
}

void AssistantConversation::Append(base::DictValue item) {
  items_.Append(item.Clone());
  dirty_ = true;
  host_->ConversationEvent(this, "append", base::Value(std::move(item)));
}

void AssistantConversation::Patch(const std::string& id,
                                  base::DictValue fields) {
  if (base::DictValue* item = FindItem(id)) {
    item->Merge(fields.Clone());
  }
  dirty_ = true;
  fields.Set("id", id);
  host_->ConversationEvent(this, "patch", base::Value(std::move(fields)));
}

void AssistantConversation::Changed() {
  host_->ConversationChanged(this);
}

base::DictValue AssistantConversation::State() const {
  const assistant::Settings settings = assistant::ReadSettings(profile());
  std::string status = "idle";
  if (!approvals_.empty()) {
    status = "waitingApproval";
  } else if (!questions_.empty() || !backend_questions_.empty()) {
    status = "waitingAnswer";
  } else if (running() && agent_starting_) {
    status = "starting";
  } else if (running()) {
    status = text_item_.empty() ? "thinking" : "replying";
  }
  base::DictValue state;
  state.Set("conversation", id_);
  state.Set("title", title_);
  state.Set("configured", settings.Configured());
  state.Set("running", running());
  state.Set("status", status);
  state.Set("mode", settings.mode);
  state.Set("backend", settings.backend);
  state.Set("provider", BackendName(settings));
  if (settings.is_local()) {
    // 本机智能体：它报上来的模型，没有就是设置里填的，都没有就写它的名字。
    const bool same = active_backend_ == settings.backend;
    state.Set("model", same && !active_model_.empty() ? active_model_
                       : !settings.local_model.empty()
                           ? settings.local_model
                           : base::UTF16ToUTF8(BackendName(settings)));
  } else {
    state.Set("model", settings.model);
  }
  base::ListValue levels;
  for (const std::string& level : settings.reasoning_levels) {
    levels.Append(level);
  }
  state.Set("reasoning", settings.reasoning);
  state.Set("reasoningLevels", std::move(levels));
  state.Set("private", profile()->IsOffTheRecord());
  return state;
}

void AssistantConversation::AddNotice(const std::u16string& text,
                                      const std::string& kind,
                                      const std::string& detail) {
  base::DictValue item;
  item.Set("type", "notice");
  item.Set("id", NextId());
  item.Set("text", text);
  item.Set("kind", kind);
  item.Set("error", kind == "failed");
  if (!detail.empty()) {
    item.Set("detail", detail);
  }
  Append(std::move(item));
}

void AssistantConversation::SettingsChanged() {
  Changed();
}

void AssistantConversation::MarkSeen() {
  if (unseen_result_) {
    unseen_result_ = false;
    Changed();
  }
}

base::ListValue AssistantConversation::MentionTabs() const {
  BrowserWindowInterface* browser = host_->view()->browser();
  tabs::TabInterface* active = browser->GetActiveTabInterface();
  base::ListValue list;
  for (tabs::TabInterface* tab : tools_->UserTabs()) {
    content::WebContents* contents = tab->GetContents();
    const GURL& url = contents->GetVisibleURL();
    if (IsBlankTabUrl(url) || IsInternalURL(url) || url.SchemeIs("devtools") ||
        url.SchemeIs("chrome-extension")) {
      continue;
    }
    list.Append(
        base::DictValue()
            .Set("id", agent::ToolRunner::TabId(tab))
            .Set("title", contents->GetTitle())
            .Set("url", url.spec())
            .Set("current", tab == active)
            .Set("otherWindow", tab->GetBrowserWindowInterface() != browser));
  }
  return list;
}

void AssistantConversation::Send(const std::string& text,
                                 std::vector<agent::ToolImage> images,
                                 const std::vector<std::string>& mentions) {
  if (running()) {
    host_->ConversationRefusedSend(this);
    return;
  }
  // @ 的标签：按用户加的顺序，只认这套工具看得到的、用户自己的标签（发之前关掉的不算）。
  std::vector<tabs::TabHandle> mentioned;
  base::ListValue mention_items;
  const std::vector<tabs::TabInterface*> user_tabs = tools_->UserTabs();
  for (const std::string& id : mentions) {
    auto it = std::ranges::find_if(user_tabs, [&id](tabs::TabInterface* tab) {
      return agent::ToolRunner::TabId(tab) == id;
    });
    if (it == user_tabs.end() ||
        std::ranges::find(mentioned, (*it)->GetHandle()) != mentioned.end()) {
      continue;
    }
    if (mentioned.size() == kMaxMentions) {
      break;
    }
    content::WebContents* contents = (*it)->GetContents();
    mentioned.push_back((*it)->GetHandle());
    mention_items.Append(base::DictValue()
                             .Set("title", contents->GetTitle())
                             .Set("url", contents->GetVisibleURL().spec()));
  }
  if (text.empty() && images.empty() && mentioned.empty()) {
    return;
  }
  if (images.size() > kMaxImages) {
    images.resize(kMaxImages);
  }
  if (title_.empty()) {
    title_ = TitleFrom(text);
    // 只 @ 了标签：标题是第一个标签的标题。
    if (title_.empty() && !mention_items.empty()) {
      title_ = TitleFrom(Text(mention_items.front().GetDict(), "title"));
    }
    if (title_.empty()) {
      title_ = base::UTF16ToUTF8(L("assistant.history.imageTitle"));
    }
  }
  unseen_result_ = false;
  const std::string user_item = NextId();
  base::DictValue item;
  item.Set("type", "user");
  item.Set("id", user_item);
  item.Set("text", text);
  if (!images.empty()) {
    base::ListValue list;
    for (const agent::ToolImage& image : images) {
      const std::string image_id =
          base::Uuid::GenerateRandomV4().AsLowercaseString();
      assistant::AddImage(image_id, image);
      image_ids_.insert(image_id);
      unsaved_images_[image_id] = image;
      list.Append(
          base::DictValue()
              .Set("id", image_id)
              .Set("mime", image.mime_type)
              .Set("src", assistant::ImagePath(image_id, image.mime_type)));
    }
    item.Set("images", std::move(list));
  }
  // 气泡上面写 @ 了哪几个标签。
  if (!mention_items.empty()) {
    item.Set("mentions", std::move(mention_items));
  }

  // 这一轮在哪个标签里做：用户看着的那个；被别的对话占着时后台开的一份。
  tabs::TabInterface* active =
      host_->view()->browser()->GetActiveTabInterface();
  bool copy = false;
  tabs::TabInterface* tab = host_->ClaimTab(this, &copy);
  tools_->SetCurrentTab(tab);
  // 换了网页时气泡上面写是哪个标签（第一句不写，网页就在眼前）。
  if (active) {
    content::WebContents* contents = active->GetContents();
    const GURL& url = contents->GetLastCommittedURL();
    item.Set("url", url.spec());
    if (!last_user_url_.is_empty() && url != last_user_url_) {
      item.Set("tab", base::DictValue()
                          .Set("title", contents->GetTitle())
                          .Set("url", url.spec()));
    }
    last_user_url_ = url;
  }
  Append(std::move(item));
  text_item_.clear();
  reasoning_item_.clear();
  PersistNow();

  const assistant::Settings settings = assistant::ReadSettings(profile());
  if (!settings.Configured()) {
    AddNotice(L("assistant.error.notConfigured"), "failed");
    TurnEnded();
    return;
  }
  starting_ = true;
  ++turn_;
  turn_mode_ = settings.mode;
  turn_reasoning_ = settings.reasoning;
  Changed();

  // 消息前面附上这一轮的标签的标题、网址和用户选中的文字。
  assistant::PromptContext context;
  content::RenderFrameHost* frame = nullptr;
  if (active) {
    content::WebContents* contents = active->GetContents();
    const GURL& url = contents->GetLastCommittedURL();
    context.tab_id = agent::ToolRunner::TabId(tab ? tab : active);
    context.title = base::UTF16ToUTF8(contents->GetTitle());
    context.url = url.spec();
    if (copy) {
      context.note =
          "Another of your conversations is working in the tab the user is "
          "looking at, so the browser opened a copy of that page in the "
          "background for you: the tab above. Work there.";
    }
    if (IsBlankTabUrl(url)) {
      context.note = "The tab is a blank new tab page.";
    } else if (IsInternalURL(url) || url.SchemeIs("devtools")) {
      context.note =
          "The tab shows a browser page (settings, history…) that the tools "
          "cannot read.";
    } else {
      frame = contents->GetPrimaryMainFrame();
    }
  }
  assistant::UserMessage message;
  message.text = text;
  message.images = std::move(images);
  message.reasoning = turn_reasoning_;
  // 先取选中的文字，再读 @ 的标签，都放进 <browser_context>。
  base::OnceCallback<void(std::optional<std::string>)> start = base::BindOnce(
      [](base::WeakPtr<AssistantConversation> conversation,
         assistant::PromptContext context, assistant::UserMessage message,
         std::string user_item, std::vector<tabs::TabHandle> mentioned,
         std::optional<std::string> selection) {
        if (!conversation || !conversation->starting_) {
          return;
        }
        if (selection) {
          context.selection = *selection;
          // 选中的文字也在气泡上面（左边一条渐变竖线），只留开头。
          if (!selection->empty()) {
            conversation->Patch(
                user_item,
                base::DictValue().Set("selection",
                                      Truncate(*selection, kSelectionLimit)));
          }
        }
        conversation->ReadMentions(
            std::move(mentioned),
            base::BindOnce(
                [](base::WeakPtr<AssistantConversation> conversation,
                   assistant::PromptContext context,
                   assistant::UserMessage message,
                   std::vector<assistant::ReferencedTab> tabs) {
                  if (!conversation || !conversation->starting_) {
                    return;
                  }
                  context.tabs = std::move(tabs);
                  if (std::optional<std::string> block =
                          assistant::ContextBlock(context)) {
                    message.text =
                        message.text.empty()
                            ? *block
                            : base::StrCat({*block, "\n\n", message.text});
                  }
                  conversation->StartTurn(std::move(message));
                },
                conversation, std::move(context), std::move(message)));
      },
      weak_factory_.GetWeakPtr(), context, std::move(message), user_item,
      std::move(mentioned));
  if (!frame) {
    std::move(start).Run(std::nullopt);
    return;
  }
  agent::RunPageCommand(
      frame, "selection", base::DictValue(),
      base::BindOnce(
          [](base::OnceCallback<void(std::optional<std::string>)> start,
             agent::PageResult result) {
            std::optional<std::string> selection;
            if (result.has_value() && result->is_dict()) {
              if (const std::string* text =
                      result->GetDict().FindString("text")) {
                selection = *text;
              }
            }
            std::move(start).Run(selection);
          },
          std::move(start)));
}

void AssistantConversation::ReadMentions(
    std::vector<tabs::TabHandle> tabs,
    base::OnceCallback<void(std::vector<assistant::ReferencedTab>)> done) {
  if (tabs.empty()) {
    std::move(done).Run({});
    return;
  }
  // 正文的字数均分：一两个标签各自带得同 browser_read_page 一样长，多了各带一段。
  const size_t limit = std::min(kMentionLimit, kMentionBudget / tabs.size());
  base::RepeatingCallback<void(IndexedTab)> collect =
      base::BarrierCallback<IndexedTab>(
          tabs.size(), base::BindOnce(&CollectMentions, std::move(done)));
  for (size_t i = 0; i < tabs.size(); ++i) {
    tabs::TabInterface* tab = tabs[i].Get();
    if (!tab) {
      collect.Run({i, assistant::ReferencedTab()});
      continue;
    }
    agent::ReadTabText(
        tab, limit,
        base::BindOnce(
            [](base::RepeatingCallback<void(IndexedTab)> collect, size_t index,
               std::string id, agent::PageText page) {
              assistant::ReferencedTab referenced;
              referenced.id = std::move(id);
              referenced.title = std::move(page.title);
              referenced.url = std::move(page.url);
              referenced.text = std::move(page.text);
              referenced.total = page.total;
              referenced.error = std::move(page.error);
              collect.Run({index, std::move(referenced)});
            },
            collect, i, agent::ToolRunner::TabId(tab)));
  }
}

std::u16string AssistantConversation::BackendName(
    const assistant::Settings& settings) const {
  if (std::optional<assistant::LocalAgentKind> kind =
          assistant::LocalAgentFromBackend(settings.backend)) {
    return assistant::DisplayName(*kind);
  }
  return settings.provider_id.empty()
             ? std::u16string()
             : assistant::ProviderDisplayName(settings.provider);
}

void AssistantConversation::SwitchBackend(const std::u16string& name) {
  if (backend_) {
    AddNotice(L("assistant.notice.switched", {{"name", name}}), "info");
  }
  ResetBackend();
}

void AssistantConversation::ResetBackend() {
  CancelReview();
  chat_ = nullptr;
  // 先结束进程，再作废令牌（经它连进来的连接一起断开）。
  backend_.reset();
  if (!token_.empty()) {
    agent::AgentService::Get()->RevokeToken(token_);
    token_.clear();
  }
  backend_signature_.clear();
  backend_questions_.clear();
  agent_starting_ = false;
}

void AssistantConversation::SendToBackend(assistant::UserMessage message) {
  starting_ = false;
  restored_ = false;
  restored_images_.clear();
  backend_->Send(std::move(message));
  Changed();
}

void AssistantConversation::StartLocalTurn(const assistant::Settings& settings,
                                           assistant::UserMessage message) {
  const assistant::LocalAgentKind kind =
      *assistant::LocalAgentFromBackend(settings.backend);
  // 给模型的说明在启动时定下：允不允许临时无痕标签变了要重新启动（会话接着用）。
  const std::string signature = base::StrCat(
      {settings.backend, "|", settings.local_path, "|", settings.local_model,
       "|", settings.allow_private_tabs ? "private-tabs" : ""});
  if (backend_ && backend_signature_ == signature &&
      (kind != assistant::LocalAgentKind::kCodex ||
       static_cast<assistant::CodexBackend*>(backend_.get())
           ->has_live_thread())) {
    SendToBackend(std::move(message));
    return;
  }
  base::FilePath user_data;
  base::PathService::Get(chrome::DIR_USER_DATA, &user_data);
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&assistant::PrepareLocalLaunch, kind, settings.local_path,
                     user_data),
      base::BindOnce(&AssistantConversation::LaunchLocal,
                     weak_factory_.GetWeakPtr(), settings, signature,
                     std::move(message)));
}

void AssistantConversation::LaunchLocal(assistant::Settings settings,
                                        std::string signature,
                                        assistant::UserMessage message,
                                        LocalLaunch launch) {
  if (!starting_) {
    return;
  }
  const assistant::LocalAgentKind kind =
      *assistant::LocalAgentFromBackend(settings.backend);
  const std::u16string name = assistant::DisplayName(kind);
  if (launch.executable.empty()) {
    starting_ = false;
    AddNotice(L("assistant.setup.localAgent", {{"name", name}}), "failed");
    TurnEnded();
    return;
  }
  SwitchBackend(name);
  const bool off_the_record = profile()->IsOffTheRecord();
  // 从记录打开对话或重启 Codex 时，用浏览器保存的文字续聊。
  const bool resume_claude = kind == assistant::LocalAgentKind::kClaudeCode &&
                             !off_the_record && !claude_session_id_.empty();
  if (!resume_claude &&
      (restored_ || (kind == assistant::LocalAgentKind::kCodex &&
                     active_backend_ == settings.backend))) {
    if (std::optional<std::string> earlier = EarlierConversation()) {
      message.text = base::StrCat({*earlier, "\n\n", message.text});
    }
  }
  // 浏览器的工具经自己的 --mcp 桥交给它：环境变量里的令牌把连接归到这个对话。
  token_ =
      agent::AgentService::Get()->RegisterToken(weak_factory_.GetWeakPtr());
  base::FilePath exe;
  base::PathService::Get(base::FILE_EXE, &exe);
  base::FilePath user_data;
  base::PathService::Get(chrome::DIR_USER_DATA, &user_data);
  assistant::BrowserMcpServer server;
  server.command = exe.AsUTF8Unsafe();
  server.arguments = {"--mcp", "--user-data-dir=" + user_data.AsUTF8Unsafe()};
  server.environment[agent::kAssistantTokenVariable] = token_;
  const std::string system_prompt = assistant::SystemPrompt(
      off_the_record,
      agent::ToolOptionsFor(profile(), /*ask_user=*/true).private_tabs);
  if (kind == assistant::LocalAgentKind::kClaudeCode) {
    assistant::ClaudeCodeBackend::Config config;
    config.executable = launch.executable;
    config.model = settings.local_model;
    config.system_prompt = system_prompt;
    config.server = std::move(server);
    config.working_directory = launch.workspace;
    config.environment = std::move(launch.environment);
    if (resume_claude) {
      config.session_id = claude_session_id_;
    }
    config.persists_session = !off_the_record;
    backend_ =
        std::make_unique<assistant::ClaudeCodeBackend>(std::move(config), this);
  } else {
    assistant::CodexBackend::Config config;
    config.executable = launch.executable;
    config.model = settings.local_model;
    config.system_prompt = system_prompt;
    config.server = std::move(server);
    config.working_directory = launch.workspace;
    config.environment = std::move(launch.environment);
    config.disabled_features = std::move(launch.disabled_features);
    config.disabled_servers = std::move(launch.disabled_servers);
    backend_ =
        std::make_unique<assistant::CodexBackend>(std::move(config), this);
  }
  backend_signature_ = signature;
  active_backend_ = settings.backend;
  active_model_ = settings.local_model;
  SendToBackend(std::move(message));
}

void AssistantConversation::StartTurn(assistant::UserMessage message) {
  const assistant::Settings settings = assistant::ReadSettings(profile());
  if (settings.is_local()) {
    StartLocalTurn(settings, std::move(message));
    return;
  }
  assistant::GetApiKey(
      profile(), settings.provider_id,
      base::BindOnce(
          [](base::WeakPtr<AssistantConversation> conversation,
             assistant::Settings settings, assistant::UserMessage message,
             std::string key) {
            if (!conversation || !conversation->starting_) {
              return;
            }
            assistant::ChatBackend::Config config;
            config.provider = settings.provider;
            config.api_key = key;
            config.model = settings.model;
            config.system_prompt = assistant::SystemPrompt(
                conversation->profile()->IsOffTheRecord(),
                agent::ToolOptionsFor(conversation->profile(),
                                      /*ask_user=*/true)
                    .private_tabs);
            config.tools = agent::ToolDefinitions(agent::ToolOptionsFor(
                conversation->profile(), /*ask_user=*/true));
            // 换了服务：之前的对话内容不带过去（工具调用的格式各家不一样）。
            const std::string signature =
                base::StrCat({"chat|", settings.provider_id, "|",
                              settings.provider.base_url});
            if (!conversation->chat_ ||
                conversation->backend_signature_ != signature) {
              conversation->SwitchBackend(
                  assistant::ProviderDisplayName(settings.provider));
              auto backend = std::make_unique<assistant::ChatBackend>(
                  std::move(config),
                  conversation->profile()
                      ->GetDefaultStoragePartition()
                      ->GetURLLoaderFactoryForBrowserProcess(),
                  conversation.get());
              // 从聊天记录打开的：浏览器自己管历史，带上前文。
              if (conversation->restored_) {
                backend->SetHistory(conversation->ChatHistory());
              }
              conversation->chat_ = backend.get();
              conversation->backend_ = std::move(backend);
              conversation->backend_signature_ = signature;
            } else {
              conversation->chat_->SetConfig(std::move(config));
            }
            conversation->active_backend_ = settings.backend;
            conversation->active_model_ = settings.model;
            conversation->SendToBackend(std::move(message));
          },
          weak_factory_.GetWeakPtr(), settings, std::move(message)));
}

void AssistantConversation::Stop() {
  const bool was_running = running();
  const bool was_starting = starting_;
  starting_ = false;
  CancelReview();
  ResolvePending();
  if (backend_ && backend_->running()) {
    backend_->Stop();
    return;
  }
  if (was_starting) {
    AddNotice(L("assistant.notice.stopped"), "stopped");
    TurnEnded();
  } else if (was_running) {
    Changed();
  }
}

void AssistantConversation::Close() {
  CancelReview();
  Stop();
  PersistNow();
  ResetBackend();
  tools_->Cancel();
}

void AssistantConversation::ResolvePending() {
  auto approvals = std::move(approvals_);
  approvals_.clear();
  approval_steps_.clear();
  for (auto& [id, callback] : approvals) {
    Patch(id, base::DictValue().Set("pending", false).Set("decision", "deny"));
    std::move(callback).Run(agent::Approval());
  }
  auto questions = std::move(questions_);
  questions_.clear();
  for (auto& [id, callback] : questions) {
    Patch(id, base::DictValue()
                  .Set("pending", false)
                  .Set("answer", L("assistant.ask.unanswered")));
    std::move(callback).Run(std::nullopt);
  }
  // 后端自己问的：这一轮结束时它自己回空的。
  auto backend_questions = std::move(backend_questions_);
  backend_questions_.clear();
  for (const auto& [id, backend_id] : backend_questions) {
    Patch(id, base::DictValue()
                  .Set("pending", false)
                  .Set("answer", L("assistant.ask.unanswered")));
  }
}

void AssistantConversation::Approve(const std::string& id,
                                    const std::string& decision,
                                    const std::string& note) {
  auto it = approvals_.find(id);
  if (it == approvals_.end()) {
    return;
  }
  base::OnceCallback<void(agent::Approval)> callback = std::move(it->second);
  approvals_.erase(it);
  agent::Approval approval;
  approval.allowed = decision == "allow" || decision == "site";
  // 自动模式下用户确认的不放行整个网站：每次敏感操作都要单独审。
  approval.site_wide = decision == "site" && turn_mode_ != assistant::kModeAuto;
  approval.note = note;
  const agent::ToolStep step = std::move(approval_steps_[id]);
  approval_steps_.erase(id);
  if (approval.site_wide) {
    allowed_sites_.insert(step.site);
  }
  Patch(id, base::DictValue()
                .Set("pending", false)
                .Set("decision", approval.allowed
                                     ? (approval.site_wide ? "site" : "allow")
                                     : "deny"));
  RecordDecision(step, approval.allowed, "user");
  std::move(callback).Run(approval);
  Changed();
}

void AssistantConversation::Answer(const std::string& id,
                                   std::optional<std::string> answer) {
  if (auto backend_it = backend_questions_.find(id);
      backend_it != backend_questions_.end()) {
    const std::string backend_id = backend_it->second;
    backend_questions_.erase(backend_it);
    Patch(id, base::DictValue()
                  .Set("pending", false)
                  .Set("answer", answer ? base::UTF8ToUTF16(*answer)
                                        : L("assistant.ask.unanswered")));
    if (backend_) {
      backend_->Answer(backend_id, std::move(answer));
    }
    Changed();
    return;
  }
  auto it = questions_.find(id);
  if (it == questions_.end()) {
    return;
  }
  auto callback = std::move(it->second);
  questions_.erase(it);
  Patch(id, base::DictValue()
                .Set("pending", false)
                .Set("answer", answer ? base::UTF8ToUTF16(*answer)
                                      : L("assistant.ask.unanswered")));
  std::move(callback).Run(std::move(answer));
  Changed();
}

// ---- 后端的事件 ----

void AssistantConversation::OnTextDelta(const std::string& text) {
  agent_starting_ = false;
  if (!reasoning_item_.empty()) {
    Patch(reasoning_item_, base::DictValue().Set("done", true));
    reasoning_item_.clear();
  }
  if (text_item_.empty()) {
    text_item_ = NextId();
    base::DictValue item;
    item.Set("type", "assistant");
    item.Set("id", text_item_);
    item.Set("text", "");
    Append(std::move(item));
    Changed();
  }
  if (base::DictValue* item = FindItem(text_item_)) {
    item->Set("text", base::StrCat({Text(*item, "text"), text}));
  }
  // 不经过 Patch 也要记着改过：中途存过一次（工具做完 2 秒后）以后，一轮结束时要再存，
  // 不然聊天记录里的回答停在存盘那一刻。
  dirty_ = true;
  host_->ConversationEvent(
      this, "delta",
      base::Value(base::DictValue().Set("id", text_item_).Set("text", text)));
}

void AssistantConversation::OnReasoningDelta(const std::string& text) {
  agent_starting_ = false;
  if (reasoning_item_.empty()) {
    reasoning_item_ = NextId();
    base::DictValue item;
    item.Set("type", "reasoning");
    item.Set("id", reasoning_item_);
    item.Set("text", "");
    item.Set("done", false);
    Append(std::move(item));
  }
  if (base::DictValue* item = FindItem(reasoning_item_)) {
    item->Set("text", base::StrCat({Text(*item, "text"), text}));
  }
  dirty_ = true;
  host_->ConversationEvent(
      this, "delta",
      base::Value(
          base::DictValue().Set("id", reasoning_item_).Set("text", text)));
}

void AssistantConversation::OnToolStarted(const std::string& id,
                                          const std::string& name,
                                          const base::DictValue& arguments) {
  agent_starting_ = false;
  if (!reasoning_item_.empty()) {
    Patch(reasoning_item_, base::DictValue().Set("done", true));
    reasoning_item_.clear();
  }
  text_item_.clear();
  const std::string item_id = NextId();
  tool_items_[id] = item_id;
  current_tool_item_ = item_id;
  base::DictValue item;
  item.Set("type", "tool");
  item.Set("id", item_id);
  item.Set("name", name);
  item.Set("title", agent::ToolRunner::TitleFor(name, arguments));
  item.Set("status", "running");
  Append(std::move(item));
  Changed();
}

void AssistantConversation::StepStarted(const agent::ToolStep& step) {
  if (!current_tool_item_.empty()) {
    Patch(current_tool_item_, base::DictValue().Set("title", step.title));
  }
  Changed();
}

std::u16string AssistantConversation::AgentName() {
  // 本机智能体写它的名字，模型服务是「AI 助手」。
  if (std::optional<assistant::LocalAgentKind> kind =
          assistant::LocalAgentFromBackend(active_backend_)) {
    return assistant::DisplayName(*kind);
  }
  return L("assistant.name");
}

bool AssistantConversation::IsAssistant() {
  return true;
}

void AssistantConversation::StopRequested() {
  // 网页上提示条的「停止」：停下这一轮（同点输入框的停止）。
  Stop();
}

void AssistantConversation::OnToolFinished(const std::string& id,
                                           const agent::ToolResult& result) {
  auto it = tool_items_.find(id);
  if (it == tool_items_.end()) {
    return;
  }
  Patch(it->second, base::DictValue()
                        .Set("status", result.is_error ? "error" : "done")
                        .Set("output", Truncate(result.text, kOutputLimit)));
  if (current_tool_item_ == it->second) {
    current_tool_item_.clear();
  }
  SchedulePersist();
  Changed();
}

void AssistantConversation::OnUsage(const assistant::Usage& usage) {
  usage_.input_tokens += usage.input_tokens;
  usage_.output_tokens += usage.output_tokens;
  usage_.cost += usage.cost;
}

void AssistantConversation::OnStatus(const std::u16string& status) {
  AddNotice(status, "info");
}

void AssistantConversation::OnStarting() {
  agent_starting_ = true;
  Changed();
}

void AssistantConversation::OnModel(const std::string& model) {
  active_model_ = model;
  agent_starting_ = false;
  Changed();
}

void AssistantConversation::OnSession(const std::string& id) {
  if (active_backend_ == assistant::kBackendClaudeCode &&
      claude_session_id_ != id) {
    claude_session_id_ = id;
    dirty_ = true;
  }
}

void AssistantConversation::OnReasoningLevels(
    const std::vector<std::string>& levels) {
  if (active_backend_ != assistant::kBackendCodex) {
    return;
  }
  assistant::SetCodexReasoningLevels(
      assistant::LocalModel(profile(), assistant::kBackendCodex), levels);
  Changed();
}

void AssistantConversation::OnToolsUnavailable(const std::string& detail) {
  const assistant::Settings settings = assistant::ReadSettings(profile());
  AddNotice(
      L("assistant.notice.toolsUnavailable", {{"name", BackendName(settings)}}),
      "warning", detail);
}

void AssistantConversation::OnQuestion(const std::string& id,
                                       const agent::Question& question) {
  agent_starting_ = false;
  const std::string item_id = NextId();
  backend_questions_[item_id] = id;
  base::ListValue options;
  for (const std::string& option : question.options) {
    options.Append(option);
  }
  base::DictValue item;
  item.Set("type", "question");
  item.Set("id", item_id);
  item.Set("question", question.question);
  item.Set("detail", question.detail);
  item.Set("options", std::move(options));
  item.Set("pending", true);
  Append(std::move(item));
  Changed();
}

void AssistantConversation::OnFinished(assistant::StopReason reason,
                                       assistant::FailureKind failure,
                                       const std::string& message) {
  if (!reasoning_item_.empty()) {
    Patch(reasoning_item_, base::DictValue().Set("done", true));
    reasoning_item_.clear();
  }
  text_item_.clear();
  current_tool_item_.clear();
  starting_ = false;
  agent_starting_ = false;
  CancelReview();
  ResolvePending();
  tools_->Cancel();
  // 没做完的工具：算做完了。
  for (const auto& [tool_id, item_id] : tool_items_) {
    if (base::DictValue* item = FindItem(item_id);
        item && Text(*item, "status") == "running") {
      Patch(item_id, base::DictValue().Set("status", "done"));
    }
  }
  tool_items_.clear();
  const assistant::Settings settings = assistant::ReadSettings(profile());
  const std::u16string name = BackendName(settings);
  switch (reason) {
    case assistant::StopReason::kCompleted:
      break;
    case assistant::StopReason::kStopped:
      AddNotice(L("assistant.notice.stopped"), "stopped");
      break;
    case assistant::StopReason::kTruncated:
      AddNotice(L("assistant.notice.truncated"), "warning");
      break;
    case assistant::StopReason::kMaxSteps:
      AddNotice(L("assistant.notice.maxSteps"), "warning");
      break;
    case assistant::StopReason::kFailed: {
      // 本机智能体：进程没起来、没登录，说法里带上它的名字。下一次重新建后端。
      if (failure == assistant::FailureKind::kProcess) {
        AddNotice(L("assistant.error.process", {{"name", name}}), "failed",
                  message);
        base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE, base::BindOnce(&AssistantConversation::ResetBackend,
                                      weak_factory_.GetWeakPtr()));
        break;
      }
      if (failure == assistant::FailureKind::kAuthentication &&
          settings.is_local()) {
        AddNotice(L("assistant.error.login", {{"name", name}}), "failed",
                  message);
        break;
      }
      const char* key = "assistant.error.other";
      switch (failure) {
        case assistant::FailureKind::kNotConfigured:
          key = "assistant.error.notConfigured";
          break;
        case assistant::FailureKind::kAuthentication:
          key = "assistant.error.key";
          break;
        case assistant::FailureKind::kRateLimited:
          key = "assistant.error.rateLimited";
          break;
        case assistant::FailureKind::kNetwork:
          key = "assistant.error.network";
          break;
        case assistant::FailureKind::kInvalidResponse:
          key = "assistant.error.invalidResponse";
          break;
        case assistant::FailureKind::kRefused:
          key = "assistant.error.refused";
          break;
        default:
          break;
      }
      AddNotice(L(key), "failed", message);
      break;
    }
  }
  TurnEnded();
}

void AssistantConversation::TurnEnded() {
  starting_ = false;
  // 一轮结束时收起网页上的描边和提示条（用户就在岛里，不需要「等待下一步」）。
  tools_->EndActivity();
  if (!host_->IsActive(this)) {
    unseen_result_ = true;
  }
  PersistNow();
  Changed();
}

void AssistantConversation::ExecuteTool(const std::string& name,
                                        base::DictValue arguments,
                                        agent::ToolCallback done) {
  tools_->Run(name, std::move(arguments), std::move(done));
}

base::ListValue AssistantConversation::AgentTools() {
  return agent::McpToolDefinitions(
      agent::ToolOptionsFor(profile(), /*ask_user=*/true));
}

std::string AssistantConversation::AgentInstructions() {
  return agent::McpInstructions(
      agent::ToolOptionsFor(profile(), /*ask_user=*/true));
}

void AssistantConversation::RunAgentTool(const std::string& name,
                                         base::DictValue arguments,
                                         agent::ToolCallback done) {
  // 本机智能体经 MCP 调用：和自己调模型时用同一套工具（当前标签、ref、确认）。
  tools_->Run(name, std::move(arguments), std::move(done));
}

// ---- 确认 ----

void AssistantConversation::AuthorizeStep(
    const agent::ToolStep& step,
    base::OnceCallback<void(agent::Approval)> callback) {
  agent::Approval allow;
  allow.allowed = true;
  if (turn_mode_ == assistant::kModeAuto) {
    // 自动：别的操作直接做，敏感的先让 AI 审。
    if (step.risk != agent::StepRisk::kSensitive) {
      std::move(callback).Run(allow);
      return;
    }
    Review(step, std::move(callback));
    return;
  }
  if (step.risk == agent::StepRisk::kAction &&
      (turn_mode_ == assistant::kModeAgent ||
       allowed_sites_.contains(step.site))) {
    std::move(callback).Run(allow);
    return;
  }
  AskApproval(step, std::move(callback));
}

void AssistantConversation::AskApproval(
    const agent::ToolStep& step,
    base::OnceCallback<void(agent::Approval)> callback) {
  if (!running()) {
    std::move(callback).Run(agent::Approval());
    return;
  }
  // 等确认的时候，要操作的元素一直框着、光标停在上面（agent::AgentActivities 的 target，
  // 这一步做完才收起）。
  const std::string id = NextId();
  approvals_[id] = std::move(callback);
  approval_steps_[id] = step;
  base::DictValue item;
  item.Set("type", "approval");
  item.Set("id", id);
  item.Set("title", step.title);
  item.Set("detail", step.detail);
  item.Set("site", step.site);
  item.Set("sensitive", step.risk == agent::StepRisk::kSensitive);
  // 自动模式下用户确认时不能整站放行。
  item.Set("siteWide", turn_mode_ != assistant::kModeAuto);
  item.Set("pending", true);
  Append(std::move(item));
  Changed();
}

void AssistantConversation::AskUser(
    const agent::Question& question,
    base::OnceCallback<void(std::optional<std::string>)> callback) {
  const std::string id = NextId();
  questions_[id] = std::move(callback);
  base::ListValue options;
  for (const std::string& option : question.options) {
    options.Append(option);
  }
  base::DictValue item;
  item.Set("type", "question");
  item.Set("id", id);
  item.Set("question", question.question);
  item.Set("detail", question.detail);
  item.Set("options", std::move(options));
  item.Set("pending", true);
  Append(std::move(item));
  Changed();
}

// ---- 自动审核 ----

void AssistantConversation::Review(
    const agent::ToolStep& step,
    base::OnceCallback<void(agent::Approval)> callback) {
  const std::string notice = NextId();
  const std::u16string action = step.title;
  base::DictValue item;
  item.Set("type", "notice");
  item.Set("id", notice);
  item.Set("kind", "info");
  item.Set("text", L("assistant.review.running", {{"action", action}}));
  Append(std::move(item));

  assistant::ReviewRequest request;
  for (const base::Value& entry : items_) {
    if (IsType(entry, "user")) {
      request.user_messages.push_back(Text(entry.GetDict(), "text"));
    }
  }
  request.tool = step.tool;
  if (std::optional<base::Value> arguments =
          base::JSONReader::Read(step.arguments, base::JSON_PARSE_RFC);
      arguments && arguments->is_dict()) {
    request.arguments = std::move(arguments->GetDict());
  }
  request.page_url = step.page_url;
  request.page_title = step.page_title;
  request.action = base::UTF16ToUTF8(step.title);
  // 密码框里的字不给审核员。
  if (step.detail != u"••••••") {
    request.detail = base::UTF16ToUTF8(step.detail);
  }
  request.prior_decisions = review_history_.Clone();
  std::optional<std::string> prompt = assistant::ReviewPrompt(request);
  auto reviewer = std::make_unique<assistant::Reviewer>();
  std::unique_ptr<assistant::Backend> backend =
      prompt ? MakeReviewBackend(reviewer.get()) : nullptr;
  if (!backend) {
    Patch(notice, base::DictValue()
                      .Set("kind", "warning")
                      .Set("text", L("assistant.review.failed")));
    AskApproval(step, std::move(callback));
    return;
  }
  CancelReview();
  reviewer_ = std::move(reviewer);
  reviewer_->Start(std::move(backend), std::move(*prompt), turn_reasoning_,
                   base::BindOnce(&AssistantConversation::ReviewDone,
                                  weak_factory_.GetWeakPtr(), notice, step,
                                  std::move(callback), turn_));
  Changed();
}

std::unique_ptr<assistant::Backend> AssistantConversation::MakeReviewBackend(
    assistant::Reviewer* reviewer) {
  // 复制这一轮真正在用的配置（服务、Key、本机的登录），建一个没有历史和浏览器工具的。
  if (chat_) {
    assistant::ChatBackend::Config config = chat_->config();
    config.system_prompt = assistant::ReviewSystemPrompt();
    config.tools = base::ListValue();
    config.max_steps = 1;
    return std::make_unique<assistant::ChatBackend>(
        std::move(config),
        profile()
            ->GetDefaultStoragePartition()
            ->GetURLLoaderFactoryForBrowserProcess(),
        reviewer);
  }
  if (!backend_) {
    return nullptr;
  }
  if (active_backend_ == assistant::kBackendClaudeCode) {
    assistant::ClaudeCodeBackend::Config config =
        static_cast<assistant::ClaudeCodeBackend*>(backend_.get())->config();
    config.system_prompt = assistant::ReviewSystemPrompt();
    config.server.reset();
    config.session_id.clear();
    if (!active_model_.empty()) {
      config.model = active_model_;
    }
    return std::make_unique<assistant::ClaudeCodeBackend>(std::move(config),
                                                          reviewer);
  }
  if (active_backend_ == assistant::kBackendCodex) {
    assistant::CodexBackend::Config config =
        static_cast<assistant::CodexBackend*>(backend_.get())->config();
    config.system_prompt = assistant::ReviewSystemPrompt();
    config.server.reset();
    if (!active_model_.empty()) {
      config.model = active_model_;
    }
    return std::make_unique<assistant::CodexBackend>(std::move(config),
                                                     reviewer);
  }
  return nullptr;
}

void AssistantConversation::ReviewDone(
    std::string notice,
    agent::ToolStep step,
    base::OnceCallback<void(agent::Approval)> callback,
    int turn,
    std::optional<assistant::ReviewVerdict> verdict) {
  // 回调在审核员（和它的后端）自己的调用里：晚一点再删。
  if (reviewer_) {
    OnUsage(reviewer_->usage());
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(reviewer_));
  }
  const std::u16string action = step.title;
  if (turn != turn_ || !running()) {
    Patch(notice, base::DictValue()
                      .Set("kind", "info")
                      .Set("text", L("assistant.review.cancelled")));
    std::move(callback).Run(agent::Approval());
    Changed();
    return;
  }
  if (!verdict) {
    Patch(notice, base::DictValue()
                      .Set("kind", "warning")
                      .Set("text", L("assistant.review.failed")));
    AskApproval(step, std::move(callback));
    return;
  }
  switch (verdict->decision) {
    case assistant::ReviewDecision::kAllow: {
      Patch(notice, base::DictValue()
                        .Set("kind", "info")
                        .Set("text", L("assistant.review.allowed",
                                       {{"action", action}}))
                        .Set("detail", verdict->reason));
      RecordDecision(step, true, "auto");
      agent::Approval approval;
      approval.allowed = true;
      std::move(callback).Run(approval);
      break;
    }
    case assistant::ReviewDecision::kAsk:
      Patch(notice, base::DictValue()
                        .Set("kind", "info")
                        .Set("text", L("assistant.review.ask"))
                        .Set("detail", verdict->reason));
      AskApproval(step, std::move(callback));
      return;
    case assistant::ReviewDecision::kDeny: {
      Patch(notice,
            base::DictValue()
                .Set("kind", "warning")
                .Set("text", L("assistant.review.denied", {{"action", action}}))
                .Set("detail", verdict->reason));
      RecordDecision(step, false, "auto");
      agent::Approval approval;
      approval.message = base::StrCat(
          {"Auto review declined this step (", step.tool,
           "): ", verdict->reason,
           ". Do not retry or work around it; ask the user how to continue."});
      std::move(callback).Run(approval);
      break;
    }
  }
  Changed();
}

void AssistantConversation::RecordDecision(const agent::ToolStep& step,
                                           bool allowed,
                                           const std::string& source) {
  base::DictValue decision;
  decision.Set("tool", step.tool);
  if (std::optional<base::Value> arguments =
          base::JSONReader::Read(step.arguments, base::JSON_PARSE_RFC)) {
    decision.Set("arguments", std::move(*arguments));
  }
  decision.Set("page_url", step.page_url);
  decision.Set("allowed", allowed);
  decision.Set("source", source);
  decision.Set("user_message_count", static_cast<int>(std::ranges::count_if(
                                         items_, [](const base::Value& item) {
                                           return IsType(item, "user");
                                         })));
  review_history_.Append(std::move(decision));
}

void AssistantConversation::CancelReview() {
  if (!reviewer_) {
    return;
  }
  // 审核员的回调看到轮次变了：算取消。
  ++turn_;
  reviewer_->Cancel();
}

// ---- 聊天记录 ----

void AssistantConversation::SchedulePersist() {
  if (!persist_timer_.IsRunning()) {
    persist_timer_.Start(FROM_HERE, kPersistDelay,
                         base::BindOnce(&AssistantConversation::PersistNow,
                                        weak_factory_.GetWeakPtr()));
  }
}

void AssistantConversation::PersistNow() {
  persist_timer_.Stop();
  assistant::History* history = host_->history();
  if (!dirty_ || items_.empty() || !history) {
    return;
  }
  dirty_ = false;
  assistant::HistoryRecord record;
  record.id = id_;
  record.title = title_;
  record.created = created_;
  record.updated = base::Time::Now();
  std::string search;
  for (const base::Value& item : items_) {
    if (!item.is_dict()) {
      continue;
    }
    if (record.site.empty() && IsType(item, "user")) {
      record.site = Text(item.GetDict(), "url");
    }
    if ((IsType(item, "user") || IsType(item, "assistant")) &&
        search.size() < kSearchLimit) {
      base::StrAppend(&search, {Text(item.GetDict(), "text"), "\n"});
    }
  }
  record.search = search.substr(0, kSearchLimit);
  record.payload = RecordJson();
  record.images = std::move(unsaved_images_);
  unsaved_images_.clear();
  history->Save(std::move(record));
}

std::string AssistantConversation::RecordJson() const {
  base::ListValue items;
  for (const base::Value& value : items_) {
    if (!value.is_dict()) {
      continue;
    }
    base::DictValue item = value.GetDict().Clone();
    // 等着的确认、问题存成已经结束的（打开时不会再等）。
    if (item.FindBool("pending").value_or(false)) {
      item.Set("pending", false);
      if (Text(item, "type") == "approval") {
        item.Set("decision", "deny");
      } else {
        item.Set("answer", L("assistant.ask.unanswered"));
      }
    }
    if (Text(item, "type") == "tool" && Text(item, "status") == "running") {
      item.Set("status", "done");
    }
    if (Text(item, "type") == "reasoning") {
      item.Set("done", true);
    }
    // 图片的地址打开时重新给。
    if (base::ListValue* images = item.FindList("images")) {
      for (base::Value& image : *images) {
        if (image.is_dict()) {
          image.GetDict().Remove("src");
        }
      }
    }
    items.Append(std::move(item));
  }
  base::DictValue sessions;
  // 无痕窗口里本机智能体的会话不写磁盘，记着也接不上。
  if (!profile()->IsOffTheRecord() && !claude_session_id_.empty()) {
    sessions.Set(assistant::kBackendClaudeCode, claude_session_id_);
  }
  base::DictValue record;
  record.Set("version", 1);
  record.Set("backend", active_backend_);
  record.Set("model", active_model_);
  record.Set("sessions", std::move(sessions));
  record.Set("usage", base::DictValue()
                          .Set("input", usage_.input_tokens)
                          .Set("output", usage_.output_tokens)
                          .Set("cost", usage_.cost));
  record.Set("nextId", next_id_);
  record.Set("items", std::move(items));
  return base::WriteJson(record).value_or("{}");
}

std::vector<assistant::ChatMessage> AssistantConversation::ChatHistory() const {
  // 工具调用的 ID 和结果留不下来：这一轮用过的工具写成一句话放在回答前面。
  std::vector<assistant::ChatMessage> messages;
  std::vector<std::string> steps;
  const auto steps_note = [&steps]() {
    std::string note =
        base::StrCat({"(Steps I took: ", base::JoinString(steps, "; "), ".)"});
    steps.clear();
    return note;
  };
  // 最后一句用户的话是刚发的（后端自己会加上）：不算前文。
  size_t end = items_.size();
  while (end > 0 && !IsType(items_[end - 1], "user")) {
    --end;
  }
  if (end > 0) {
    --end;
  }
  for (size_t index = 0; index < end; ++index) {
    const base::Value& value = items_[index];
    if (!value.is_dict()) {
      continue;
    }
    const base::DictValue& item = value.GetDict();
    const std::string type = Text(item, "type");
    if (type == "user") {
      if (!steps.empty()) {
        assistant::ChatMessage message;
        message.role = assistant::ChatMessage::Role::kAssistant;
        message.content = steps_note();
        messages.push_back(std::move(message));
      }
      assistant::ChatMessage message;
      message.role = assistant::ChatMessage::Role::kUser;
      std::string text = Text(item, "text");
      if (const base::ListValue* images = item.FindList("images")) {
        size_t missing = 0;
        for (const base::Value& image : *images) {
          auto it = image.is_dict()
                        ? restored_images_.find(Text(image.GetDict(), "id"))
                        : restored_images_.end();
          if (it == restored_images_.end()) {
            ++missing;
          } else {
            message.images.push_back(it->second);
          }
        }
        if (missing) {
          text +=
              "\n(The user attached an image here that is no longer "
              "available.)";
        }
      }
      message.content = std::move(text);
      messages.push_back(std::move(message));
    } else if (type == "assistant") {
      assistant::ChatMessage message;
      message.role = assistant::ChatMessage::Role::kAssistant;
      message.content =
          steps.empty()
              ? Text(item, "text")
              : base::StrCat({steps_note(), "\n\n", Text(item, "text")});
      messages.push_back(std::move(message));
    } else if (type == "tool") {
      steps.push_back(Text(item, "title"));
    } else if (type == "question") {
      steps.push_back(
          base::StrCat({"asked the user \"", Text(item, "question"),
                        "\", who answered \"", Text(item, "answer"), "\""}));
    }
  }
  if (!steps.empty()) {
    assistant::ChatMessage message;
    message.role = assistant::ChatMessage::Role::kAssistant;
    message.content = steps_note();
    messages.push_back(std::move(message));
  }
  return messages;
}

std::optional<std::string> AssistantConversation::EarlierConversation() const {
  std::vector<std::string> lines;
  for (const base::Value& value : items_) {
    if (!value.is_dict()) {
      continue;
    }
    const base::DictValue& item = value.GetDict();
    const std::string type = Text(item, "type");
    if (type == "user") {
      lines.push_back("User: " + Text(item, "text"));
    } else if (type == "assistant") {
      lines.push_back("Assistant: " + Text(item, "text"));
    } else if (type == "question" &&
               !item.FindBool("pending").value_or(false)) {
      lines.push_back(
          base::StrCat({"Assistant asked: ", Text(item, "question"),
                        "\nUser answered: ", Text(item, "answer")}));
    }
  }
  // 最后一条是刚发的（还在 items_ 里）：不算前文。
  if (!lines.empty() && lines.back().starts_with("User: ")) {
    lines.pop_back();
  }
  if (lines.empty()) {
    return std::nullopt;
  }
  return base::StrCat(
      {"<earlier_conversation>\nThis conversation continues an earlier one. "
       "It went like this:\n\n",
       KeepTail(base::JoinString(lines, "\n\n"), kEarlierLimit),
       "\n</earlier_conversation>"});
}

// ---- 调试 ----

std::optional<std::string> AssistantConversation::FirstPendingId(
    const std::string& type) const {
  if (type == "approval" && !approvals_.empty()) {
    return approvals_.begin()->first;
  }
  if (type == "question" && !questions_.empty()) {
    return questions_.begin()->first;
  }
  if (type == "question" && !backend_questions_.empty()) {
    return backend_questions_.begin()->first;
  }
  return std::nullopt;
}

std::string AssistantConversation::DebugDescription() const {
  std::string text;
  for (const base::Value& value : items_) {
    if (!value.is_dict()) {
      continue;
    }
    const base::DictValue& item = value.GetDict();
    const std::string* type = item.FindString("type");
    const std::string* body = item.FindString("text");
    if (!body) {
      body = item.FindString("title");
    }
    if (!body) {
      body = item.FindString("question");
    }
    const std::string* status = item.FindString("status");
    std::string line = body ? *body : std::string();
    if (line.size() > 120) {
      line = line.substr(0, 120) + "…";
    }
    base::ReplaceChars(line, "\n", " ", &line);
    base::StrAppend(&text, {"\n  ", type ? *type : "?", ": ", line,
                            status ? " (" + *status + ")" : ""});
    if (const std::optional<bool> pending = item.FindBool("pending");
        pending && *pending) {
      text += " [pending]";
    }
    if (const base::ListValue* images = item.FindList("images")) {
      base::StrAppend(&text,
                      {" [", base::NumberToString(images->size()), " images]"});
    }
  }
  return base::StrCat({"running=", running() ? "yes" : "no", text});
}

}  // namespace starry
