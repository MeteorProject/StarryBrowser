// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/sidebar/extension_bar.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/i18n/case_conversion.h"
#include "base/scoped_observation.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/extensions/extension_view_host.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/extensions/extension_action_delegate.h"
#include "chrome/browser/ui/extensions/extension_action_view_model.h"
#include "chrome/browser/ui/extensions/extension_popup_types.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/extensions/extension_popup.h"
#include "components/tabs/public/tab_interface.h"
#include "extensions/browser/extension_host.h"
#include "extensions/browser/extension_host_observer.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/design/controls.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/menu/menu_host_view.h"
#include "starry/browser/ui/motion/motion.h"
#include "starry/browser/ui/webui/starry_pages.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/color/color_id.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/views/bubble/bubble_anchor.h"
#include "ui/views/bubble/bubble_border.h"
#include "ui/views/context_menu_controller.h"

namespace starry {

namespace {

constexpr int kButtonSize = ExtensionBar::kHeight;
constexpr int kButtonGap = 2;
// 按钮出现时从 0.6 缩放并淡入。
constexpr float kAppearScale = 0.6f;

// 扩展菜单的命令：打开第 i 个、固定第 i 个、管理扩展。
constexpr int kActivateBase = 1;
constexpr int kPinBase = 1000;
constexpr int kPinMenu = 1999;
constexpr int kManage = 2000;

content::WebContents* ActiveContents(BrowserWindowInterface* browser) {
  tabs::TabInterface* tab = browser->GetActiveTabInterface();
  return tab ? tab->GetContents() : nullptr;
}

}  // namespace

// 一个扩展的按钮：图标是 ExtensionActionViewModel 画的（带徽标，28 × 28），这个标签页上
// 不可用时淡一点（点了照 Chrome 弹右键菜单）。
class ExtensionActionButton : public IconButton,
                              public views::ContextMenuController {
  METADATA_HEADER(ExtensionActionButton, IconButton)

 public:
  ExtensionActionButton(PressedCallback callback,
                        base::RepeatingClosure context_menu,
                        ExtensionActionViewModel* model)
      : IconButton(std::move(callback), icons::kPuzzlepiece, u""),
        context_menu_(std::move(context_menu)),
        model_(model) {
    set_context_menu_controller(this);
  }

  void Refresh(content::WebContents* contents) {
    image_ = model_->GetIcon(contents, gfx::Size(kButtonSize, kButtonSize));
    enabled_for_tab_ = model_->IsEnabled(contents);
    SetTooltipText(model_->GetTooltip(contents));
    SchedulePaint();
  }

  ExtensionActionViewModel* model() const { return model_; }

 protected:
  void PaintIcon(gfx::Canvas* canvas, SkColor color) override {
    if (image_.IsEmpty() || !GetColorProvider()) {
      IconButton::PaintIcon(canvas, color);
      return;
    }
    const gfx::ImageSkia skia = image_.Rasterize(GetColorProvider());
    const int x = (width() - skia.width()) / 2;
    const int y = (height() - skia.height()) / 2;
    if (enabled_for_tab_) {
      canvas->DrawImageInt(skia, x, y);
    } else {
      cc::PaintFlags flags;
      flags.setAlphaf(0.4f);
      canvas->DrawImageInt(skia, x, y, flags);
    }
  }

  // views::ContextMenuController:
  void ShowContextMenuForViewImpl(
      views::View* source,
      const gfx::Point& point,
      ui::mojom::MenuSourceType source_type) override {
    context_menu_.Run();
  }

 private:
  base::RepeatingClosure context_menu_;
  raw_ptr<ExtensionActionViewModel> model_;
  ui::ImageModel image_;
  bool enabled_for_tab_ = true;
};

BEGIN_METADATA(ExtensionActionButton)
END_METADATA

namespace {

// ExtensionActionViewModel 交给界面做的事（同 Chrome 的
// ExtensionActionDelegateDesktop）：弹出框挂在侧栏的按钮上。快捷键不在这里登记：Chrome
// 藏起来的工具栏里的那一份已经登记了。
class BarActionDelegate : public ExtensionActionDelegate,
                          public extensions::ExtensionHostObserver {
 public:
  BarActionDelegate(ExtensionBar* bar, BrowserWindowInterface* browser)
      : bar_(bar), browser_(browser) {}
  BarActionDelegate(const BarActionDelegate&) = delete;
  BarActionDelegate& operator=(const BarActionDelegate&) = delete;
  ~BarActionDelegate() override = default;

  // ExtensionActionDelegate:
  void AttachToModel(ExtensionActionViewModel* model) override {
    model_ = model;
  }
  void DetachFromModel() override { model_ = nullptr; }
  void RegisterCommand() override {}
  void UnregisterCommand() override {}
  bool IsShowingPopup() const override { return popup_host_ != nullptr; }

  void HidePopup() override {
    if (!popup_host_) {
      return;
    }
    if (has_opened_popup_) {
      popup_host_->Close();
    }
    if (popup_host_) {
      OnPopupClosed();
    }
  }

  gfx::NativeView GetPopupNativeViewForTesting() override {
    return popup_host_ && popup_host_->view()
               ? popup_host_->view()->GetNativeView()
               : gfx::NativeView();
  }

  void TriggerPopup(std::unique_ptr<extensions::ExtensionViewHost> host,
                    PopupShowAction show_action,
                    bool by_user,
                    ShowPopupCallback callback) override {
    // 同一时间只有一个弹出框；扩展菜单先关掉。
    bar_->HideActivePopup();
    bar_->CloseMenu();
    popup_host_ = host.get();
    observation_.Observe(popup_host_.get());
    has_opened_popup_ = true;
    const std::string id = model_->GetId();
    bar_->SetPopupOwner(id);
    // 往网页那边弹出：侧栏里往右、顶边和按钮对齐；工具栏里往下、右边和按钮对齐。
    ExtensionPopup::ShowPopup(browser_, std::move(host),
                              views::BubbleAnchor(bar_->PopupAnchorFor(id)),
                              bar_->placement() == ChromePlacement::kToolbar
                                  ? views::BubbleBorder::TOP_RIGHT
                                  : views::BubbleBorder::LEFT_TOP,
                              show_action, std::move(callback));
  }

  void ShowContextMenuAsFallback() override {
    if (model_) {
      bar_->ShowContextMenuFor(model_->GetId());
    }
  }

  void CloseExtensionsMenuIfOpen() override { bar_->CloseMenu(); }

  // extensions::ExtensionHostObserver:
  void OnExtensionHostDestroyed(extensions::ExtensionHost* host) override {
    OnPopupClosed();
  }

 private:
  void OnPopupClosed() {
    observation_.Reset();
    popup_host_ = nullptr;
    has_opened_popup_ = false;
    if (model_) {
      bar_->SetPopupOwner(std::string());
    }
  }

  raw_ptr<ExtensionBar> bar_;
  raw_ptr<BrowserWindowInterface> browser_;
  raw_ptr<ExtensionActionViewModel> model_ = nullptr;
  raw_ptr<extensions::ExtensionViewHost> popup_host_ = nullptr;
  bool has_opened_popup_ = false;
  base::ScopedObservation<extensions::ExtensionHost,
                          extensions::ExtensionHostObserver>
      observation_{this};
};

}  // namespace

struct ExtensionBar::Entry {
  std::unique_ptr<ExtensionActionViewModel> model;
  base::CallbackListSubscription icon_subscription;
  raw_ptr<ExtensionActionButton> button = nullptr;
  LayerMotion motion;
};

ExtensionBar::ExtensionBar(BrowserWindowInterface* browser,
                           ChromePlacement placement)
    : browser_(browser), placement_(placement) {
  menu_button_ = AddChildView(std::make_unique<IconButton>(
      base::BindRepeating(&ExtensionBar::ShowExtensionsMenu,
                          base::Unretained(this)),
      icons::kPuzzlepiece, L("common.extensions"), kButtonSize, 14));
  model_ = ToolbarActionsModel::Get(browser_->GetProfile());
  if (model_) {
    model_observation_.Observe(model_.get());
  }
  browser_->GetTabStripModel()->AddObserver(this);
  Sync(/*animated=*/false);
}

ExtensionBar::~ExtensionBar() {
  CloseMenu();
  HideActivePopup();
}

void ExtensionBar::SetTheme(const Theme& theme) {
  theme_ = theme;
  menu_button_->SetTheme(theme);
  for (const auto& [id, entry] : entries_) {
    if (entry->button) {
      entry->button->SetTheme(theme);
    }
  }
}

void ExtensionBar::SetContentChangedCallback(base::RepeatingClosure callback) {
  content_changed_ = std::move(callback);
}

ExtensionBar::Entry* ExtensionBar::Find(const std::string& id) const {
  auto it = entries_.find(id);
  return it == entries_.end() ? nullptr : it->second.get();
}

std::vector<ExtensionBar::Entry*> ExtensionBar::SortedEntries() const {
  std::vector<Entry*> sorted;
  for (const auto& [id, entry] : entries_) {
    sorted.push_back(entry.get());
  }
  std::sort(sorted.begin(), sorted.end(), [](Entry* a, Entry* b) {
    return base::i18n::ToLower(a->model->GetActionName()) <
           base::i18n::ToLower(b->model->GetActionName());
  });
  return sorted;
}

void ExtensionBar::Sync(bool animated) {
  const bool had_content = HasContent();
  if (!model_ || !model_->actions_initialized()) {
    for (auto& [id, entry] : entries_) {
      if (entry->button) {
        RemoveChildViewT(entry->button.get());
      }
    }
    entries_.clear();
    pinned_.clear();
  } else {
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (model_->HasAction(it->first)) {
        ++it;
        continue;
      }
      if (it->second->button) {
        RemoveChildViewT(it->second->button.get());
      }
      it = entries_.erase(it);
    }
    for (const ToolbarActionsModel::ActionId& id : model_->action_ids()) {
      if (entries_.contains(id)) {
        continue;
      }
      auto entry = std::make_unique<Entry>();
      entry->model = ExtensionActionViewModel::Create(
          id, browser_, std::make_unique<BarActionDelegate>(this, browser_));
      if (!entry->model) {
        continue;
      }
      entry->icon_subscription = entry->model->RegisterIconUpdateObserver(
          base::BindRepeating(&ExtensionBar::RefreshButtons,
                              weak_factory_.GetWeakPtr()));
      entries_[id] = std::move(entry);
    }

    const std::vector<ToolbarActionsModel::ActionId>& pinned_ids =
        model_->pinned_action_ids();
    pinned_.clear();
    for (auto& [id, entry] : entries_) {
      const bool pinned = std::ranges::find(pinned_ids, id) != pinned_ids.end();
      if (!pinned && entry->button) {
        RemoveChildViewT(entry->button.get());
        entry->button = nullptr;
      } else if (pinned && !entry->button) {
        auto button = std::make_unique<ExtensionActionButton>(
            base::BindRepeating(
                &ExtensionBar::Activate, base::Unretained(this), id,
                ToolbarActionViewModel::InvocationSource::kToolbarButton),
            base::BindRepeating(&ExtensionBar::ShowContextMenuFor,
                                base::Unretained(this), id),
            entry->model.get());
        button->SetTheme(theme_);
        entry->button = AddChildView(std::move(button));
        if (animated && entry->button->layer()) {
          ui::Layer* layer = entry->button->layer();
          const gfx::PointF center(kButtonSize / 2.0f, kButtonSize / 2.0f);
          LayerPose from;
          from.scale = kAppearScale;
          entry->motion.AnimateOpacityFrom(layer, 0, 1, MotionCurve::Of(motion::kTabInsert));
          entry->motion.AnimateTransformFrom(layer, from, LayerPose(),
                                             MotionCurve::Of(motion::kTabInsert), center);
        }
      }
    }
    for (const ToolbarActionsModel::ActionId& id : pinned_ids) {
      if (Entry* entry = Find(id); entry && entry->button) {
        pinned_.push_back(entry->button);
      }
    }
  }
  RefreshButtons();
  InvalidateLayout();
  // 工具栏里的宽度跟着固定的按钮变。
  PreferredSizeChanged();
  if (had_content != HasContent() && content_changed_) {
    content_changed_.Run();
  }
}

void ExtensionBar::RefreshButtons() {
  content::WebContents* contents = ActiveContents(browser_);
  for (const auto& [id, entry] : entries_) {
    if (entry->button) {
      entry->button->Refresh(contents);
    }
  }
  UpdateActiveStates();
}

void ExtensionBar::UpdateActiveStates() {
  for (const auto& [id, entry] : entries_) {
    if (entry->button) {
      entry->button->SetActive(id == popup_id_);
    }
  }
  const Entry* owner = Find(popup_id_);
  menu_button_->SetActive(menu_showing_ || (owner && !owner->button));
}

int ExtensionBar::PreferredWidth() const {
  if (!HasContent()) {
    return 0;
  }
  return static_cast<int>(pinned_.size()) * (kButtonSize + kButtonGap) +
         kButtonSize;
}

void ExtensionBar::Layout(PassKey) {
  const int y = (height() - kButtonSize) / 2;
  if (placement_ == ChromePlacement::kToolbar) {
    // 工具栏模式：固定按钮后紧跟扩展菜单，不留弹性空白。
    int x = 0;
    for (ExtensionActionButton* button : pinned_) {
      button->SetVisible(true);
      button->SetBounds(x, y, kButtonSize, kButtonSize);
      x += kButtonSize + kButtonGap;
    }
    menu_button_->SetBounds(x, y, kButtonSize, kButtonSize);
    return;
  }
  menu_button_->SetBounds(width() - kButtonSize, 0, kButtonSize, kButtonSize);
  int x = 0;
  for (ExtensionActionButton* button : pinned_) {
    // 放不下的藏起来（还在扩展菜单里）。
    const bool fits = x + kButtonSize <= width() - kButtonSize - kButtonGap;
    button->SetVisible(fits);
    button->SetBounds(x, 0, kButtonSize, kButtonSize);
    x += kButtonSize + kButtonGap;
  }
}

views::View* ExtensionBar::PopupAnchorFor(const std::string& id) {
  if (Entry* entry = Find(id);
      entry && entry->button && entry->button->GetVisible()) {
    return entry->button;
  }
  return menu_button_;
}

void ExtensionBar::HideActivePopup() {
  for (const auto& [id, entry] : entries_) {
    if (entry->model->IsShowingPopup()) {
      entry->model->HidePopup();
    }
  }
}

void ExtensionBar::SetPopupOwner(const std::string& id) {
  popup_id_ = id;
  UpdateActiveStates();
}

void ExtensionBar::Activate(const std::string& id,
                            ToolbarActionViewModel::InvocationSource source) {
  Entry* entry = Find(id);
  if (!entry) {
    return;
  }
  // 再点一下开着弹出框的按钮：关上（同 Chrome）。
  if (entry->model->IsShowingPopup()) {
    entry->model->HidePopup();
    return;
  }
  entry->model->ExecuteUserAction(source);
}

void ExtensionBar::ShowMenu(ui::MenuModel* model,
                            views::View* anchor,
                            MenuOptions options) {
  StarryBrowserView* view = StarryBrowserView::From(browser_);
  if (!view || !anchor) {
    return;
  }
  const gfx::Rect rect =
      views::View::ConvertRectToTarget(anchor, view, anchor->GetLocalBounds());
  options.anchor = rect;
  menu_showing_ = true;
  UpdateActiveStates();
  view->ShowMenu(model, rect.bottom_left(),
                 base::BindOnce(
                     [](base::WeakPtr<ExtensionBar> bar) {
                       if (bar) {
                         bar->menu_showing_ = false;
                         bar->UpdateActiveStates();
                       }
                     },
                     weak_factory_.GetWeakPtr()),
                 std::move(options));
}

void ExtensionBar::ShowContextMenuFor(const std::string& id) {
  Entry* entry = Find(id);
  if (!entry) {
    return;
  }
  ui::MenuModel* menu = entry->model->GetContextMenu(
      extensions::ExtensionContextMenuModel::ContextMenuSource::kToolbarAction);
  if (!menu) {
    return;
  }
  ShowMenu(menu, PopupAnchorFor(id), MenuOptions());
}

void ExtensionBar::ShowExtensionsMenu() {
  if (menu_showing_) {
    CloseMenu();
    return;
  }
  content::WebContents* contents = ActiveContents(browser_);
  menu_model_ = std::make_unique<ui::SimpleMenuModel>(this);
  pin_menu_model_ = std::make_unique<ui::SimpleMenuModel>(this);
  menu_ids_.clear();
  menu_model_->AddTitle(L("common.extensions"));
  for (Entry* entry : SortedEntries()) {
    const int index = static_cast<int>(menu_ids_.size());
    menu_ids_.push_back(entry->model->GetId());
    menu_model_->AddItem(kActivateBase + index, entry->model->GetActionName());
    menu_model_->SetIcon(menu_model_->GetItemCount() - 1,
                         entry->model->GetIcon(contents, gfx::Size(16, 16)));
    pin_menu_model_->AddCheckItem(kPinBase + index,
                                  entry->model->GetActionName());
  }
  if (!menu_ids_.empty()) {
    menu_model_->AddSeparator(ui::NORMAL_SEPARATOR);
    menu_model_->AddSubMenuWithIcon(
        kPinMenu, L("extensions.pin.sidebar"), pin_menu_model_.get(),
        ui::ImageModel::FromVectorIcon(icons::kPin, ui::kColorMenuIcon, 14));
  }
  menu_model_->AddItemWithIcon(
      kManage, L("toolbar.extensions.help"),
      ui::ImageModel::FromVectorIcon(icons::kPuzzlepiece, ui::kColorMenuIcon,
                                     14));
  MenuOptions options;
  // 勾「固定到侧栏」里的项时菜单不关（勾一组）。
  options.keeps_open = base::BindRepeating([](int command_id) {
    return command_id >= kPinBase && command_id < kPinMenu;
  });
  ShowMenu(menu_model_.get(), menu_button_, std::move(options));
}

void ExtensionBar::CloseMenu() {
  if (!menu_showing_) {
    return;
  }
  if (StarryBrowserView* view = StarryBrowserView::From(browser_)) {
    if (view->menu_host()) {
      view->menu_host()->Close();
    }
  }
}

bool ExtensionBar::IsCommandIdChecked(int command_id) const {
  const size_t index = static_cast<size_t>(command_id - kPinBase);
  return command_id >= kPinBase && index < menu_ids_.size() && model_ &&
         model_->IsActionPinned(menu_ids_[index]);
}

bool ExtensionBar::IsCommandIdEnabled(int command_id) const {
  const size_t index = static_cast<size_t>(command_id - kPinBase);
  if (command_id >= kPinBase && index < menu_ids_.size()) {
    // 管理员强制固定的取消不了。
    return model_ && !model_->IsActionForcePinned(menu_ids_[index]);
  }
  return true;
}

void ExtensionBar::ExecuteCommand(int command_id, int event_flags) {
  if (command_id == kManage) {
    OpenStarrySettings(browser_, "extensions");
    return;
  }
  if (command_id >= kPinBase && command_id < kPinMenu) {
    const size_t index = static_cast<size_t>(command_id - kPinBase);
    if (index < menu_ids_.size() && model_) {
      const std::string& id = menu_ids_[index];
      model_->SetActionVisibility(id, !model_->IsActionPinned(id));
    }
    return;
  }
  const size_t index = static_cast<size_t>(command_id - kActivateBase);
  if (index < menu_ids_.size()) {
    // 菜单正在淡出：等它关了再弹出（弹出框挂在扩展菜单按钮上）。
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&ExtensionBar::Activate, weak_factory_.GetWeakPtr(),
                       menu_ids_[index],
                       ToolbarActionViewModel::InvocationSource::kMenuEntry));
  }
}

std::string ExtensionBar::DescribeForTesting() const {
  std::vector<std::string> pinned;
  for (ExtensionActionButton* button : pinned_) {
    pinned.push_back(base::UTF16ToUTF8(button->model()->GetActionName()));
  }
  std::vector<std::string> all;
  for (Entry* entry : SortedEntries()) {
    all.push_back(base::UTF16ToUTF8(entry->model->GetActionName()));
  }
  const Entry* owner = Find(popup_id_);
  return base::StrCat(
      {"pinned=", base::JoinString(pinned, ","), " all=",
       base::JoinString(all, ","), " popup=",
       owner ? base::UTF16ToUTF8(owner->model->GetActionName()) : "-"});
}

bool ExtensionBar::ActivateForTesting(size_t index) {
  std::vector<Entry*> sorted = SortedEntries();
  if (index >= sorted.size()) {
    return false;
  }
  Activate(sorted[index]->model->GetId(),
           sorted[index]->button
               ? ToolbarActionViewModel::InvocationSource::kToolbarButton
               : ToolbarActionViewModel::InvocationSource::kMenuEntry);
  return true;
}

void ExtensionBar::OnToolbarActionAdded(
    const ToolbarActionsModel::ActionId& id) {
  Sync(/*animated=*/true);
}

void ExtensionBar::OnToolbarActionRemoved(
    const ToolbarActionsModel::ActionId& id) {
  Sync(/*animated=*/true);
}

void ExtensionBar::OnToolbarActionUpdated(
    const ToolbarActionsModel::ActionId& id) {
  RefreshButtons();
}

void ExtensionBar::OnToolbarModelInitialized() {
  Sync(/*animated=*/false);
}

void ExtensionBar::OnToolbarPinnedActionsChanged() {
  Sync(/*animated=*/true);
}

void ExtensionBar::OnToolbarActionsModelShutdown() {
  model_observation_.Reset();
  model_ = nullptr;
  Sync(/*animated=*/false);
}

void ExtensionBar::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  if (selection.active_tab_changed()) {
    HideActivePopup();
    RefreshButtons();
  }
}

BEGIN_METADATA(ExtensionBar)
END_METADATA

}  // namespace starry
