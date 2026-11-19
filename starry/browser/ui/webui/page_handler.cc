// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/webui/page_handler.h"

#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "components/tabs/public/tab_interface.h"
#include "components/vector_icons/vector_icons.h"
#include "content/public/browser/host_zoom_map.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/assistant/assistant_controller.h"
#include "starry/browser/ui/design/controls.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/dialog/dialog_host_view.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/menu/menu_host_view.h"
#include "third_party/blink/public/common/page/page_zoom.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/models/image_model.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "ui/color/color_id.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/rect_f.h"
#include "url/gurl.h"

namespace starry {

namespace {

// 将页面传入的图标名称映射为 Starry 的矢量图标。
const gfx::VectorIcon* IconByName(std::string_view name) {
  struct Entry {
    std::string_view name;
    // 指向静态的图标数据。
    RAW_PTR_EXCLUSION const gfx::VectorIcon* icon;
  };
  static constexpr Entry kIcons[] = {
      {"archivebox", &icons::kArchivebox},
      {"arrow.up.forward", &icons::kArrowUpRight},
      {"arrow.uturn.backward", &icons::kArrowUturnBackward},
      {"arrow.clockwise", &icons::kArrowClockwise},
      {"arrow.down", &icons::kArrowDown},
      {"arrow.down.circle", &icons::kArrowDownCircle},
      {"checkmark.circle", &icons::kCheckmarkCircle},
      {"folder", &icons::kFolder},
      {"gearshape", &vector_icons::kSettingsIcon},
      {"pause", &icons::kPause},
      {"play", &icons::kPlay},
      {"clock", &icons::kClock},
      {"globe", &icons::kGlobe},
      {"info.circle", &icons::kInfoCircle},
      {"link", &icons::kLink},
      {"macwindow", &icons::kMacWindow},
      {"magnifyingglass", &icons::kMagnifyingGlass},
      {"minus.circle", &icons::kMinusCircle},
      {"pencil", &icons::kPencil},
      {"plus", &icons::kPlus},
      {"plus.square.on.square", &icons::kPlusSquareOnSquare},
      {"shield.lefthalf.filled", &icons::kShieldLefthalfFilled},
      {"shield.slash", &icons::kShieldSlash},
      {"star", &icons::kStar},
      {"star.fill", &icons::kStarFill},
      {"star.slash", &icons::kStarSlash},
      {"trash", &icons::kTrash},
      {"trash.fill", &icons::kTrashFill},
      {"xmark", &icons::kXmark},
  };
  for (const Entry& entry : kIcons) {
    if (entry.name == name) {
      return entry.icon;
    }
  }
  return nullptr;
}

StarryBrowserView* BrowserViewFor(content::WebContents* contents) {
  // AI 助手的岛里的网页不是标签页。
  if (StarryBrowserView* view = AssistantController::ViewForPage(contents)) {
    return view;
  }
  tabs::TabInterface* tab = tabs::TabInterface::MaybeGetFromContents(contents);
  if (!tab || !tab->GetBrowserWindowInterface()) {
    return nullptr;
  }
  return StarryBrowserView::From(tab->GetBrowserWindowInterface());
}

// 页面里的矩形（CSS 像素，相对视口）换成浏览器视图的坐标。
gfx::Rect PageRectToView(content::WebContents* contents,
                         views::View* view,
                         const gfx::RectF& rect) {
  const double zoom = blink::ZoomLevelToZoomFactor(
      content::HostZoomMap::GetZoomLevel(contents));
  gfx::RectF scaled = gfx::ScaleRect(rect, zoom);
  const gfx::Rect container = contents->GetContainerBounds();
  scaled.Offset(container.x(), container.y());
  const gfx::Rect screen = gfx::ToEnclosingRect(scaled);
  gfx::Point origin = screen.origin();
  views::View::ConvertPointFromScreen(view, &origin);
  return gfx::Rect(origin, screen.size());
}

ButtonVariant VariantByName(std::string_view name) {
  if (name == "filled") {
    return ButtonVariant::kFilled;
  }
  if (name == "danger") {
    return ButtonVariant::kDanger;
  }
  if (name == "secondary") {
    return ButtonVariant::kSecondary;
  }
  return ButtonVariant::kTertiary;
}

WindowOpenDisposition DispositionByName(std::string_view target) {
  if (target == "foreground") {
    return WindowOpenDisposition::NEW_FOREGROUND_TAB;
  }
  if (target == "background") {
    return WindowOpenDisposition::NEW_BACKGROUND_TAB;
  }
  if (target == "window") {
    return WindowOpenDisposition::NEW_WINDOW;
  }
  return WindowOpenDisposition::CURRENT_TAB;
}

}  // namespace

StarryPageHandler::StarryPageHandler() = default;

StarryPageHandler::~StarryPageHandler() {
  // 菜单还开着时页面关了：菜单在窗口里，模型要先关掉。
  if (menu_) {
    if (StarryBrowserView* view =
            BrowserViewFor(web_ui()->GetWebContents())) {
      if (view->menu_host()) {
        view->menu_host()->Close();
      }
    }
  }
}

void StarryPageHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "showMenu", base::BindRepeating(&StarryPageHandler::HandleShowMenu,
                                      base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "showDialog", base::BindRepeating(&StarryPageHandler::HandleShowDialog,
                                        base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "openUrl", base::BindRepeating(&StarryPageHandler::HandleOpenUrl,
                                     base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "copyText", base::BindRepeating(&StarryPageHandler::HandleCopyText,
                                      base::Unretained(this)));
}

void StarryPageHandler::HandleShowMenu(const base::ListValue& args) {
  AllowJavascript();
  const std::string callback_id = args[0].GetString();
  // 上一个菜单还没回：当作没选。
  if (!menu_callback_.empty()) {
    ResolveJavascriptCallback(base::Value(menu_callback_), base::Value());
    menu_callback_.clear();
  }
  content::WebContents* contents = web_ui()->GetWebContents();
  StarryBrowserView* view = BrowserViewFor(contents);
  if (!view || args.size() < 3) {
    ResolveJavascriptCallback(base::Value(callback_id), base::Value());
    return;
  }

  menu_ = std::make_unique<ui::SimpleMenuModel>(this);
  submenus_.clear();
  menu_ids_.clear();
  menu_checked_.clear();
  menu_enabled_.clear();
  picked_.reset();
  BuildMenu(args[1].GetList(), menu_.get());

  const base::DictValue& anchor = args[2].GetDict();
  const gfx::RectF rect(anchor.FindDouble("x").value_or(0),
                        anchor.FindDouble("y").value_or(0),
                        anchor.FindDouble("width").value_or(0),
                        anchor.FindDouble("height").value_or(0));
  const gfx::Rect in_view = PageRectToView(contents, view, rect);
  MenuOptions options;
  if (anchor.FindBool("below").value_or(false)) {
    options.anchor = in_view;
  }
  menu_callback_ = callback_id;
  view->ShowMenu(menu_.get(), in_view.origin(),
                 base::BindOnce(&StarryPageHandler::OnMenuClosed,
                                weak_factory_.GetWeakPtr()),
                 std::move(options));
}

void StarryPageHandler::BuildMenu(const base::ListValue& items,
                                  ui::SimpleMenuModel* model) {
  for (const base::Value& item_value : items) {
    const base::DictValue& item = item_value.GetDict();
    if (item.FindBool("separator").value_or(false)) {
      model->AddSeparator(ui::NORMAL_SEPARATOR);
      continue;
    }
    if (const std::string* header = item.FindString("header")) {
      model->AddTitle(base::UTF8ToUTF16(*header));
      continue;
    }
    const std::string* id = item.FindString("id");
    const std::string* title = item.FindString("title");
    if (!id || !title) {
      continue;
    }
    const int command = static_cast<int>(menu_ids_.size()) + 1;
    const std::optional<bool> checked = item.FindBool("checked");
    menu_ids_.push_back(*id);
    menu_checked_.push_back(checked.value_or(false));
    menu_enabled_.push_back(item.FindBool("enabled").value_or(true));
    if (const base::ListValue* children = item.FindList("children")) {
      auto submenu = std::make_unique<ui::SimpleMenuModel>(this);
      BuildMenu(*children, submenu.get());
      model->AddSubMenu(command, base::UTF8ToUTF16(*title), submenu.get());
      submenus_.push_back(std::move(submenu));
    } else if (checked) {
      model->AddCheckItem(command, base::UTF8ToUTF16(*title));
    } else {
      model->AddItem(command, base::UTF8ToUTF16(*title));
    }
    if (const std::string* icon_name = item.FindString("icon")) {
      if (const gfx::VectorIcon* icon = IconByName(*icon_name)) {
        model->SetIcon(
            model->GetItemCount() - 1,
            ui::ImageModel::FromVectorIcon(*icon, ui::kColorMenuIcon, 14));
      }
    }
  }
}

void StarryPageHandler::OnMenuClosed() {
  if (menu_callback_.empty()) {
    return;
  }
  const std::string callback_id = std::move(menu_callback_);
  menu_callback_.clear();
  ResolveJavascriptCallback(
      base::Value(callback_id),
      picked_ ? base::Value(*picked_) : base::Value());
  picked_.reset();
}

bool StarryPageHandler::IsCommandIdChecked(int command_id) const {
  const size_t index = static_cast<size_t>(command_id - 1);
  return index < menu_checked_.size() && menu_checked_[index];
}

bool StarryPageHandler::IsCommandIdEnabled(int command_id) const {
  const size_t index = static_cast<size_t>(command_id - 1);
  return index < menu_enabled_.size() && menu_enabled_[index];
}

void StarryPageHandler::ExecuteCommand(int command_id, int event_flags) {
  const size_t index = static_cast<size_t>(command_id - 1);
  if (index < menu_ids_.size()) {
    picked_ = menu_ids_[index];
  }
}

void StarryPageHandler::HandleShowDialog(const base::ListValue& args) {
  AllowJavascript();
  const std::string callback_id = args[0].GetString();
  StarryBrowserView* view = BrowserViewFor(web_ui()->GetWebContents());
  if (!view || args.size() < 2) {
    ResolveJavascriptCallback(base::Value(callback_id), base::Value());
    return;
  }
  const base::DictValue& dict = args[1].GetDict();
  DialogSpec spec;
  if (const std::string* icon = dict.FindString("icon")) {
    spec.icon = IconByName(*icon);
  }
  if (const std::string* tone = dict.FindString("tone")) {
    spec.tone = *tone == "danger"    ? DialogSpec::Tone::kDanger
                : *tone == "warning" ? DialogSpec::Tone::kWarning
                                     : DialogSpec::Tone::kAccent;
  }
  if (const std::string* title = dict.FindString("title")) {
    spec.title = base::UTF8ToUTF16(*title);
  }
  if (const std::string* message = dict.FindString("message")) {
    spec.message = base::UTF8ToUTF16(*message);
  }
  if (const std::string* field = dict.FindString("field")) {
    spec.field = base::UTF8ToUTF16(*field);
  }
  if (const base::ListValue* buttons = dict.FindList("buttons")) {
    for (const base::Value& button_value : *buttons) {
      const base::DictValue& button = button_value.GetDict();
      const std::string* title = button.FindString("title");
      const std::string* variant = button.FindString("variant");
      spec.buttons.push_back(
          {title ? base::UTF8ToUTF16(*title) : std::u16string(),
           VariantByName(variant ? *variant : std::string_view()),
           button.FindBool("cancel").value_or(false)});
    }
  }
  if (spec.buttons.empty()) {
    spec.buttons.push_back({L("common.ok"), ButtonVariant::kFilled, /*cancel=*/true});
  }
  view->PresentDialog(
      std::move(spec),
      base::BindOnce(
          [](base::WeakPtr<StarryPageHandler> handler,
             const std::string& callback_id,
             std::optional<size_t> button, const std::u16string& input) {
            if (!handler || !handler->IsJavascriptAllowed()) {
              return;
            }
            base::DictValue result;
            if (button) {
              result.Set("button", static_cast<int>(*button));
            }
            result.Set("input", input);
            handler->ResolveJavascriptCallback(base::Value(callback_id),
                                               base::Value(std::move(result)));
          },
          weak_factory_.GetWeakPtr(), callback_id));
}

void StarryPageHandler::HandleOpenUrl(const base::ListValue& args) {
  if (args.size() < 2 || !args[0].is_string() || !args[1].is_string()) {
    return;
  }
  const GURL url(args[0].GetString());
  if (!url.is_valid()) {
    return;
  }
  content::WebContents* contents = web_ui()->GetWebContents();
  StarryBrowserView* view = BrowserViewFor(contents);
  if (!view) {
    return;
  }
  NavigateParams params(view->browser(), url,
                        ui::PAGE_TRANSITION_AUTO_BOOKMARK);
  params.disposition = DispositionByName(args[1].GetString());
  params.source_contents = contents;
  Navigate(&params);
}

void StarryPageHandler::HandleCopyText(const base::ListValue& args) {
  if (args.empty() || !args[0].is_string()) {
    return;
  }
  ui::ScopedClipboardWriter(ui::ClipboardBuffer::kCopyPaste)
      .WriteText(base::UTF8ToUTF16(args[0].GetString()));
}

}  // namespace starry
