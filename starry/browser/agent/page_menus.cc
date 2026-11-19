// Copyright 2026 The Starry Browser Authors

#include "starry/browser/agent/page_menus.h"

#include <algorithm>
#include <array>
#include <map>
#include <utility>

#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/extensions/context_menu_matcher.h"
#include "components/renderer_context_menu/render_view_context_menu_base.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/models/menu_model.h"
#include "ui/events/event_constants.h"
#include "ui/gfx/text_utils.h"
#include "ui/menus/simple_menu_model.h"

namespace starry::agent {

namespace {

struct Interceptor {
  int id = 0;
  std::optional<std::string> item;
  base::OnceCallback<void(PageMenu)> callback;
};

std::map<content::WebContents*, Interceptor>& Interceptors() {
  static base::NoDestructor<std::map<content::WebContents*, Interceptor>> map;
  return *map;
}

// 智能体能选的 Chrome 自己的项：在后台标签里打开链接、图片、媒体，拷贝链接、图片和
// 选中的文字，搜索，前进后退、刷新。「另存为」会弹出系统的存储面板，新窗口、无痕窗口会
// 挡到用户面前，「粘贴」会把用户剪贴板里的东西交给网页，检查、翻译、朗读、打印这些打开
// 浏览器自己的界面，都不给。
bool Allowed(int id) {
  static constexpr auto kAllowed = std::to_array<int>({
      IDC_CONTENT_CONTEXT_OPENLINKNEWTAB,
      IDC_CONTENT_CONTEXT_COPYLINKLOCATION,
      IDC_CONTENT_CONTEXT_COPYLINKTEXT,
      IDC_CONTENT_CONTEXT_COPYLINKTOTEXT,
      IDC_CONTENT_CONTEXT_OPENIMAGENEWTAB,
      IDC_CONTENT_CONTEXT_COPYIMAGE,
      IDC_CONTENT_CONTEXT_COPYIMAGELOCATION,
      IDC_CONTENT_CONTEXT_OPENAVNEWTAB,
      IDC_CONTENT_CONTEXT_COPYAVLOCATION,
      IDC_CONTENT_CONTEXT_COPY,
      IDC_CONTENT_CONTEXT_SEARCHWEBFOR,
      IDC_CONTENT_CONTEXT_SEARCHWEBFORNEWTAB,
      IDC_BACK,
      IDC_FORWARD,
      IDC_RELOAD,
      IDC_CONTENT_CONTEXT_RELOADFRAME,
  });
  return std::ranges::find(kAllowed, id) != kAllowed.end();
}

struct Entry {
  std::string title;
  raw_ptr<ui::MenuModel> model;
  size_t index = 0;
  int command_id = 0;
};

void Collect(ui::MenuModel* model,
             const std::string& prefix,
             bool extensions_only,
             std::vector<Entry>* out) {
  for (size_t i = 0; i < model->GetItemCount(); ++i) {
    if (!model->IsVisibleAt(i)) {
      continue;
    }
    const ui::MenuModel::ItemType type = model->GetTypeAt(i);
    if (type == ui::MenuModel::TYPE_SEPARATOR ||
        type == ui::MenuModel::TYPE_TITLE) {
      continue;
    }
    const int id = model->GetCommandIdAt(i);
    const bool from_extension =
        extensions::ContextMenuMatcher::IsExtensionsCustomCommandId(id);
    if (!from_extension && (extensions_only || !Allowed(id))) {
      continue;
    }
    const std::string title = base::StrCat(
        {prefix,
         base::UTF16ToUTF8(gfx::RemoveAccelerator(model->GetLabelAt(i)))});
    if (type == ui::MenuModel::TYPE_SUBMENU ||
        type == ui::MenuModel::TYPE_ACTIONABLE_SUBMENU) {
      if (from_extension && model->GetSubmenuModelAt(i)) {
        Collect(model->GetSubmenuModelAt(i), title + " › ",
                /*extensions_only=*/true, out);
      }
      continue;
    }
    if (model->IsEnabledAt(i) && !title.empty()) {
      out->push_back(Entry{title, model, i, id});
    }
  }
}

// 按名字找：不分大小写，不管前后的空白和结尾的省略号、句号。
std::string Normalized(std::string_view title) {
  std::string text =
      base::ToLowerASCII(base::TrimWhitespaceASCII(title, base::TRIM_ALL));
  while (true) {
    if (text.ends_with("…")) {
      text.resize(text.size() - std::string_view("…").size());
    } else if (text.ends_with(".")) {
      text.pop_back();
    } else {
      break;
    }
  }
  return text;
}

}  // namespace

base::ScopedClosureRunner InterceptPageMenu(
    content::WebContents* contents,
    std::optional<std::string> item,
    base::OnceCallback<void(PageMenu)> callback) {
  static int next_id = 1;
  const int id = next_id++;
  Interceptors()[contents] =
      Interceptor{id, std::move(item), std::move(callback)};
  return base::ScopedClosureRunner(base::BindOnce(
      [](content::WebContents* contents, int id) {
        auto it = Interceptors().find(contents);
        if (it != Interceptors().end() && it->second.id == id) {
          Interceptors().erase(it);
        }
      },
      contents, id));
}

bool MaybeInterceptPageMenu(RenderViewContextMenuBase& menu) {
  content::RenderFrameHost* frame = menu.GetRenderFrameHost();
  content::WebContents* contents =
      frame ? content::WebContents::FromRenderFrameHost(frame) : nullptr;
  auto it = contents ? Interceptors().find(contents) : Interceptors().end();
  if (it == Interceptors().end()) {
    return false;
  }
  Interceptor interceptor = std::move(it->second);
  Interceptors().erase(it);

  // Chrome 只给只读的；执行菜单项（ActivatedAt）不改模型本身（同 web_context_menu.cc）。
  ui::MenuModel* root = const_cast<ui::SimpleMenuModel*>(&menu.menu_model());
  std::vector<Entry> entries;
  Collect(root, std::string(), /*extensions_only=*/false, &entries);
  PageMenu result;
  for (const Entry& entry : entries) {
    result.items.push_back(entry.title);
  }
  const Entry* chosen = nullptr;
  if (interceptor.item) {
    const std::string wanted = Normalized(*interceptor.item);
    for (const Entry& entry : entries) {
      if (Normalized(entry.title) == wanted) {
        chosen = &entry;
        break;
      }
    }
    if (!chosen) {
      std::string names;
      for (const std::string& title : result.items) {
        base::StrAppend(&names, {names.empty() ? "" : ", ", "\"", title, "\""});
      }
      result.error = base::StrCat(
          {"The browser's context menu has no item \"", *interceptor.item,
           "\".",
           names.empty() ? " It has no items agents can choose here."
                         : base::StrCat({" Items: ", names, "."})});
    }
  }
  // 和真的菜单一样：打开、选、关上（关上时网页收到菜单关了）。
  root->MenuWillShow();
  if (chosen) {
    result.chosen = chosen->title;
    // 搜索选中的文字：同中键，在后台标签里打开（默认是前台，会换掉用户看着的标签）。
    const bool search =
        chosen->command_id == IDC_CONTENT_CONTEXT_SEARCHWEBFOR ||
        chosen->command_id == IDC_CONTENT_CONTEXT_SEARCHWEBFORNEWTAB;
    chosen->model->ActivatedAt(chosen->index,
                               search ? ui::EF_MIDDLE_MOUSE_BUTTON : 0);
  }
  root->MenuWillClose();
  std::move(interceptor.callback).Run(std::move(result));
  return true;
}

}  // namespace starry::agent
