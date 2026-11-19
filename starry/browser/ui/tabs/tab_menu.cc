// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/tabs/tab_menu.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/tab_list/tab_list_interface.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/recently_audible_helper.h"
#include "chrome/browser/ui/tabs/tab_data.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_muted_utils.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/url_formatter/url_fixer.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/tab_organizer/organizer_memory.h"
#include "starry/browser/tab_organizer/tab_grouper.h"
#include "starry/browser/ui/archive/organize_page.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/internal_pages.h"
#include "starry/browser/ui/split/split_tabs.h"
#include "starry/browser/ui/tabs/favorite_folders.h"
#include "starry/browser/ui/tabs/pin_folders.h"
#include "starry/browser/ui/tabs/pinned_tabs.h"
#include "starry/browser/ui/tabs/tab_tree.h"
#include "starry/common/internal_urls.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/models/image_model.h"
#include "ui/color/color_id.h"
#include "ui/menus/simple_menu_model.h"
#include "url/gurl.h"

namespace starry {

namespace {

ui::ImageModel Icon(const gfx::VectorIcon& icon) {
  // 颜色由菜单按自己的主题画（MenuHostView），这里的颜色只是占位。
  return ui::ImageModel::FromVectorIcon(icon, ui::kColorMenuIcon, 14);
}

void EditPinnedAddress(tabs::TabInterface* tab,
                       const std::u16string& input,
                       bool invalid) {
  auto* browser = tab->GetBrowserWindowInterface();
  auto* view = browser ? StarryBrowserView::From(browser) : nullptr;
  if (!view) {
    return;
  }
  DialogSpec spec;
  spec.title = L("tabs.editPin");
  spec.field = input;
  if (invalid) {
    spec.message = L("bookmarks.editor.error.invalidURL");
  }
  spec.buttons = {{L("common.cancel"), ButtonVariant::kTertiary, true},
                  {L("common.save"), ButtonVariant::kFilled}};
  view->PresentDialog(
      std::move(spec),
      base::BindOnce(
          [](tabs::TabHandle handle, std::optional<size_t> button,
             const std::u16string& text) {
            auto* target = handle.Get();
            if (button != 1 || !target || !target->IsPinned() ||
                !target->GetBrowserWindowInterface()) {
              return;
            }
            auto url = url_formatter::FixupURL(base::UTF16ToUTF8(text), {});
            if (!url.is_valid() || IsBlankTabUrl(url) ||
                !(url.SchemeIsHTTPOrHTTPS() || url.SchemeIsFile() ||
                  InternalPageForUrl(url) != InternalPage::kNone)) {
              EditPinnedAddress(target, text, true);
              return;
            }
            pinned_tabs::SetPinnedURL(target, url);
          },
          tab->GetHandle()));
}

bool IsCapturing(const tabs::TabData& data) {
  if (!data.alert_state) {
    return false;
  }
  switch (*data.alert_state) {
    case tabs::TabAlert::kMediaRecording:
    case tabs::TabAlert::kTabCapturing:
    case tabs::TabAlert::kDesktopCapturing:
    case tabs::TabAlert::kAudioRecording:
    case tabs::TabAlert::kVideoRecording:
      return true;
    default:
      return false;
  }
}

}  // namespace

TabMenu::TabMenu(BrowserWindowInterface* browser, tabs::TabInterface* tab)
    : browser_(browser), tab_(tab->GetHandle()) {
  Build(tab);
}

TabMenu::~TabMenu() = default;

void TabMenu::Build(tabs::TabInterface* tab) {
  content::WebContents* contents = tab->GetContents();
  const GURL& url = contents->GetLastCommittedURL().is_empty()
                        ? contents->GetVisibleURL()
                        : contents->GetLastCommittedURL();
  const bool blank = IsBlankTabUrl(url);
  const bool internal = InternalPageForUrl(url) != InternalPage::kNone ||
                        IsInternalURL(url) || url.SchemeIs("devtools");
  const bool sleeping = contents->WasDiscarded();
  TabStripModel* model = browser_->GetTabStripModel();

  auto add = [this](int command, const std::u16string& title,
                    const gfx::VectorIcon& icon, bool enabled) {
    model_.AddItemWithIcon(command, title, Icon(icon));
    if (!enabled) {
      disabled_.insert(command);
    }
  };

  // 按网站的规则提示放进文件夹：放进去、不提示。
  if (const std::optional<tab_organizer::Suggestion> suggestion =
          tab_organizer::SuggestFolder(tab)) {
    add(kSuggestAccept,
        L("organize.suggest.accept", {{"folder", suggestion->name}}),
        icons::kFolder, true);
    add(kSuggestDismiss,
        L("organize.suggest.dismiss", {{"folder", suggestion->name}}),
        icons::kXmark, true);
    model_.AddSeparator(ui::NORMAL_SEPARATOR);
  }
  add(kReload, L("common.reload"), icons::kArrowClockwise, !blank);
  if (sleeping) {
    add(kWake, L("tabs.wake"), icons::kSunMax, true);
  } else {
    add(kSleep, L("tabs.sleep"), icons::kMoonZzz, !blank && !internal);
  }
  add(kCopyLink, L("tabs.copyLink"), icons::kLink,
      !blank && !internal && url.is_valid());
  add(kDuplicate, L("tabs.duplicate"), icons::kPlusSquareOnSquare, !blank);
  if (tab->IsPinned()) {
    add(kUnpin, L("tabs.unpin"), icons::kPinSlash, true);
  } else {
    add(kPin, L("tabs.pin"), icons::kPin, !blank);
  }
  const bool favorite = pinned_tabs::IsFavorite(tab);
  if (!favorite) {
    add(kFavorite, L("tabs.addFavorite"), icons::kStar,
        !blank && pinned_tabs::HasRoomForFavorite(browser_));
  }
  if (favorite || !pinned_tabs::Folder(tab).empty()) {
    add(kMoveToPins, L("tabs.moveToPins"), icons::kPin, true);
  }
  folder_model_.AddItem(kNewFolder, L("tabs.newFolder"));
  for (const auto& folder : pin_folders::Read(browser_->GetProfile())) {
    folder_model_.AddItem(kFirstFolder + folders_.size(), folder.path);
    folders_.push_back(folder.id);
  }
  model_.AddSubMenuWithIcon(kFolderMenu, L("tabs.moveToFolder"), &folder_model_,
                            Icon(icons::kFolder));
  if (blank) {
    disabled_.insert(kFolderMenu);
  }
  // 文件夹里的固定标签：这个网站的新标签提示放进这个文件夹（按网站的规则）。
  if (tab->IsPinned() && !favorite && !pinned_tabs::Folder(tab).empty() &&
      !browser_->GetProfile()->IsOffTheRecord()) {
    const std::string site = tab_organizer::SiteOf(url);
    if (!site.empty()) {
      rule_site_ = site;
      rule_folder_ = pinned_tabs::Folder(tab);
      const std::optional<tab_organizer::SiteRule> rule =
          tab_organizer::RuleFor(browser_->GetProfile(), url);
      rule_on_ = rule && rule->site == site && rule->folder == rule_folder_;
      model_.AddCheckItem(kSuggestRule,
                          L("organize.rule.menu", {{"site", site}}));
    }
  }
  // 常用图块里的文件夹：放进去的标签变成常用的。满了的、已经在里面的不能选。
  favorite_model_.AddItem(kNewFavoriteFolder, L("tabs.newFavoriteFolder"));
  if (!favorite_folders::CanCreate(browser_, tab)) {
    disabled_.insert(kNewFavoriteFolder);
  }
  const std::string tile_folder = pinned_tabs::FavoriteFolder(tab);
  if (!tile_folder.empty()) {
    favorite_model_.AddItem(kFavoriteRoot, L("tabs.favoritesSection"));
    if (!pinned_tabs::HasRoomForFavorite(browser_)) {
      disabled_.insert(kFavoriteRoot);
    }
  }
  const std::vector<pin_folders::Folder> tile_folders =
      favorite_folders::Read(browser_->GetProfile());
  if (!tile_folders.empty()) {
    favorite_model_.AddSeparator(ui::NORMAL_SEPARATOR);
  }
  for (const pin_folders::Folder& folder : tile_folders) {
    const int command =
        kFirstFavoriteFolder + static_cast<int>(favorite_folders_.size());
    favorite_model_.AddItem(command, folder.name);
    if ((favorite && folder.id == tile_folder) ||
        !pinned_tabs::HasRoomForFavorite(browser_, folder.id)) {
      disabled_.insert(command);
    }
    favorite_folders_.push_back(folder.id);
  }
  model_.AddSubMenuWithIcon(kFavoriteFolderMenu, L("tabs.moveToFavoriteFolder"),
                            &favorite_model_, Icon(icons::kStar));
  if (blank) {
    disabled_.insert(kFavoriteFolderMenu);
  }
  // 一键整理：AI 按主题把普通标签放进文件夹。
  if (!tab->IsPinned() && !browser_->GetProfile()->IsOffTheRecord()) {
    add(kOrganize, L("organize.menu"), icons::kSparkle,
        tab_organizer::OrganizableTabs(browser_).size() >= 2);
  }

  if (tab->IsPinned()) {
    const auto saved = pinned_tabs::PinnedUrl(tab);
    add(kReset, L("tabs.resetPin"), icons::kArrowClockwise,
        saved.is_valid() && saved != url);
    add(kReplace, L("tabs.replacePin"), icons::kLink, !blank && saved != url);
    add(kRename, L("tabs.renamePin"), icons::kPencil, true);
    add(kEdit, L("tabs.editPin"), icons::kPencil, true);
  }
  // 正在播放音频、近期有音频或处于静音状态时才显示静音项。
  if (contents->IsAudioMuted()) {
    add(kUnmute, L("tabs.unmute"), icons::kSpeakerWave, true);
  } else if (RecentlyAudibleHelper* audible =
                 RecentlyAudibleHelper::FromWebContents(contents);
             contents->IsCurrentlyAudible() ||
             (audible && audible->WasRecentlyAudible())) {
    add(kMute, L("tabs.mute"), icons::kSpeakerSlash, true);
  }

  // 树状标签页（侧栏里的普通标签）。
  const int index = model->GetIndexOfTab(tab);
  const bool tree = !tab->IsPinned() && index != TabStripModel::kNoTab &&
                    tab_tree::WindowTree::From(model) &&
                    tab_tree::Shown(browser_->GetProfile());
  const tab_tree::Node node =
      tree ? tab_tree::Compute(model)[index] : tab_tree::Node();
  if (tree) {
    model_.AddSeparator(ui::NORMAL_SEPARATOR);
    add(kTreeNewBelow, L("tabs.tree.newBelow"), icons::kPlus, true);
    if (node.children > 0 && node.collapsed) {
      add(kTreeExpand, L("tabs.tree.expandGroup"), icons::kChevronDown, true);
    } else if (node.children > 0) {
      add(kTreeCollapse, L("tabs.tree.collapseGroup"), icons::kChevronRight,
          true);
    }
    if (tab_tree::CanCollapseOthers(model, index)) {
      add(kTreeCollapseOthers, L("tabs.tree.collapseOthers"),
          icons::kChevronsToLine, true);
    }
    if (tab_tree::WindowTree::From(model)->CanHoist(index)) {
      add(kTreeHoist, L("tabs.tree.hoist"),
          icons::kArrowUpLeftAndArrowDownRight, true);
    }
    if (tab_tree::CanOutdent(model, index)) {
      add(kTreeOutdent, L("tabs.tree.outdent"), icons::kDecreaseIndent, true);
    }
    if (tab_tree::CanIndent(model, index)) {
      add(kTreeIndent, L("tabs.tree.indent"), icons::kIncreaseIndent, true);
    }
  }
  model_.AddSeparator(ui::NORMAL_SEPARATOR);

  // 分屏操作。
  if (tab->IsSplit()) {
    add(kSplitRemove, L("split.remove"), icons::kSplitRemove, true);
    add(kSplitDissolve, L("split.dissolve"), icons::kRectangle, true);
  } else {
    tabs::TabInterface* current = browser_->GetActiveTabInterface();
    const bool possible =
        current &&
        split::Evaluate(model, tab, current, split::Edge::kRight).preview;
    const std::array<std::pair<int, split::Edge>, 4> edges = {{
        {kSplitRight, split::Edge::kRight},
        {kSplitLeft, split::Edge::kLeft},
        {kSplitBottom, split::Edge::kBottom},
        {kSplitTop, split::Edge::kTop},
    }};
    for (const auto& [command, edge] : edges) {
      split_model_.AddItemWithIcon(command, split::PlacementTitle(edge),
                                   Icon(split::PlacementIcon(edge)));
    }
    model_.AddSubMenuWithIcon(kSplitRight + 100, L("tabs.splitWithCurrent"),
                              &split_model_, Icon(icons::kSplit2x1));
    if (!possible) {
      disabled_.insert(kSplitRight + 100);
    }
  }
  model_.AddSeparator(ui::NORMAL_SEPARATOR);

  bool has_other_unpinned = false;
  for (int i = model->IndexOfFirstNonPinnedTab(); i < model->count(); ++i) {
    if (model->GetTabAtIndex(i) != tab) {
      has_other_unpinned = true;
      break;
    }
  }
  add(kCloseOthers, L("tabs.closeOthers"), icons::kXmarkSquare,
      has_other_unpinned);
  if (tree && node.children > 0) {
    // ⌘⇧T 一次整组恢复（连同结构）。
    add(kTreeCloseGroup,
        L("tabs.tree.closeGroup", {{"count", node.descendants + 1}}),
        icons::kXmark, true);
    add(kTreeCloseChildren, L("tabs.tree.closeChildren"), icons::kXmark, true);
    add(kTreeMoveToNewWindow, L("tabs.tree.moveToNewWindow"), icons::kMacWindow,
        true);
  }
  if (tab->IsPinned()) {
    add(kClose, L("tabs.closeKeepPin"), icons::kMinus, true);
    add(kArchive, L("tabs.archive"), icons::kTrash, true);
  } else {
    add(kClose, L("tabs.close"), icons::kXmark, true);
  }
}

bool TabMenu::IsCommandIdEnabled(int command_id) const {
  return !disabled_.contains(command_id);
}

bool TabMenu::IsCommandIdChecked(int command_id) const {
  return command_id == kSuggestRule && rule_on_;
}

void TabMenu::ExecuteCommand(int command_id, int event_flags) {
  tabs::TabInterface* tab = tab_.Get();
  if (!tab) {
    return;
  }
  TabStripModel* model = browser_->GetTabStripModel();
  const int index = model->GetIndexOfTab(tab);
  if (index == TabStripModel::kNoTab) {
    return;
  }
  content::WebContents* contents = tab->GetContents();
  if (command_id >= kFirstFavoriteFolder &&
      static_cast<size_t>(command_id - kFirstFavoriteFolder) <
          favorite_folders_.size()) {
    favorite_folders::Place(
        browser_, pin_folders::Child{tab_, std::string()},
        favorite_folders_[command_id - kFirstFavoriteFolder]);
    return;
  }
  if (command_id >= kFirstFolder &&
      static_cast<size_t>(command_id - kFirstFolder) < folders_.size()) {
    const std::string& folder = folders_[command_id - kFirstFolder];
    tab_organizer::NoteManualMove(
        tab,
        tab->IsPinned() && !pinned_tabs::IsFavorite(tab)
            ? pinned_tabs::Folder(tab)
            : std::string(),
        folder);
    pinned_tabs::SetLocation(tab, false, folder);
    return;
  }
  switch (command_id) {
    case kTreeNewBelow:
      tab_tree::NewTabBelow(browser_, index);
      break;
    case kTreeCollapse:
    case kTreeExpand:
      tab_tree::SetCollapsed(model, index, command_id == kTreeCollapse,
                             /*all=*/false);
      break;
    case kTreeCollapseOthers:
      tab_tree::CollapseOthers(model, index);
      break;
    case kTreeHoist:
      if (tab_tree::WindowTree* tree = tab_tree::WindowTree::From(model)) {
        tree->Hoist(index);
      }
      break;
    case kTreeOutdent:
      tab_tree::Outdent(model, index);
      break;
    case kTreeIndent:
      tab_tree::Indent(model, index);
      break;
    case kTreeCloseGroup:
      tab_tree::CloseBranch(model, index);
      break;
    case kTreeCloseChildren:
      tab_tree::CloseChildren(model, index);
      break;
    case kTreeMoveToNewWindow:
      tab_tree::MoveBranchToNewWindow(browser_, index);
      break;
    case kNewFolder:
      pin_folders::Create(browser_, {}, tab_);
      break;
    case kNewFavoriteFolder:
      favorite_folders::Create(browser_, tab_);
      break;
    case kFavoriteRoot:
      favorite_folders::Place(browser_, pin_folders::Child{tab_, std::string()},
                              std::string());
      break;
    case kFavorite:
    case kMoveToPins:
    case kPin:
      pinned_tabs::SetLocation(tab, command_id == kFavorite, {});
      break;
    case kReset:
      pinned_tabs::Reset(tab, false);
      break;
    case kReplace:
      pinned_tabs::SetPinnedURL(tab, contents->GetVisibleURL());
      break;
    case kRename:
      pinned_tabs::Rename(tab);
      break;
    case kEdit:
      EditPinnedAddress(
          tab, base::UTF8ToUTF16(pinned_tabs::PinnedUrl(tab).spec()), false);
      break;
    case kReload:
      contents->GetController().Reload(content::ReloadType::NORMAL,
                                       /*check_for_repost=*/true);
      break;
    case kSleep:
      Sleep(tab);
      break;
    case kWake:
      // 切过去就醒了（Chrome 切到休眠的标签时重新载入）。
      model->ActivateTabAt(index);
      break;
    case kCopyLink: {
      const GURL& url = contents->GetLastCommittedURL();
      ui::ScopedClipboardWriter(ui::ClipboardBuffer::kCopyPaste)
          .WriteText(base::UTF8ToUTF16(url.spec()));
      break;
    }
    case kDuplicate:
      if (TabListInterface* list = TabListInterface::From(browser_)) {
        list->DuplicateTab(tab_);
      }
      break;
    case kUnpin:
      model->SetTabPinned(index, false);
      break;
    case kMute:
    case kUnmute:
      SetTabAudioMuted(contents, command_id == kMute,
                       TabMutedReason::kAudioIndicator, std::string());
      break;
    case kCloseOthers:
      CloseOthers(tab);
      break;
    case kOrganize:
      OpenOrganize(browser_);
      return;
    case kSuggestAccept:
      // 标签要挪到固定区：晚一步做，菜单收起以后再改列表。
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(
                         [](tabs::TabHandle handle) {
                           if (tabs::TabInterface* tab = handle.Get()) {
                             tab_organizer::AcceptSuggestion(tab);
                           }
                         },
                         tab_));
      return;
    case kSuggestDismiss:
      tab_organizer::DismissSuggestion(tab);
      return;
    case kSuggestRule:
      if (rule_on_) {
        tab_organizer::RemoveRule(browser_->GetProfile(), rule_site_);
      } else {
        tab_organizer::AddRule(browser_->GetProfile(), rule_site_, rule_folder_,
                               /*learned=*/false);
      }
      return;
    case kClose:
      if (pinned_tabs::Close(browser_, tab)) {
        break;
      }
      [[fallthrough]];
    case kArchive:
      pinned_tabs::Archive(tab, /*to_tab_archive=*/true);
      break;
    case kSplitRemove:
      split::RemoveFromSplit(browser_, tab);
      break;
    case kSplitDissolve:
      split::ShowOnly(browser_, tab);
      break;
    case kSplitRight:
    case kSplitLeft:
    case kSplitBottom:
    case kSplitTop: {
      const split::Edge edge = command_id == kSplitRight  ? split::Edge::kRight
                               : command_id == kSplitLeft ? split::Edge::kLeft
                               : command_id == kSplitBottom
                                   ? split::Edge::kBottom
                                   : split::Edge::kTop;
      split::Apply(browser_,
                   split::Evaluate(model, tab,
                                   browser_->GetActiveTabInterface(), edge));
      break;
    }
  }
}

void TabMenu::Sleep(tabs::TabInterface* tab) {
  content::WebContents* contents = tab->GetContents();
  // 正在播放声音或使用摄像头/麦克风的标签页不允许休眠。
  if (contents->IsCurrentlyAudible() ||
      IsCapturing(tabs::TabData::FromTabInterface(tab))) {
    ShowSleepFailed(L("tabSleep.reason.media"));
    return;
  }
  TabStripModel* model = browser_->GetTabStripModel();
  if (tab->IsActivated()) {
    // 当前的标签：先换到最近用过的另一个（醒着的优先），没有就开一个新标签页。
    int next = TabStripModel::kNoTab;
    bool next_awake = false;
    base::TimeTicks next_time;
    for (int i = 0; i < model->count(); ++i) {
      tabs::TabInterface* other = model->GetTabAtIndex(i);
      if (other == tab) {
        continue;
      }
      content::WebContents* other_contents = other->GetContents();
      const bool awake = !other_contents->WasDiscarded();
      const base::TimeTicks time = other_contents->GetLastActiveTimeTicks();
      if (next == TabStripModel::kNoTab || (awake && !next_awake) ||
          (awake == next_awake && time > next_time)) {
        next = i;
        next_awake = awake;
        next_time = time;
      }
    }
    if (next != TabStripModel::kNoTab) {
      model->ActivateTabAt(next);
    } else {
      chrome::AddTabAt(browser_, GURL(), -1, /*foreground=*/true);
    }
  }
  TabListInterface* list = TabListInterface::From(browser_);
  if (!list || !list->DiscardTab(tab_)) {
    ShowSleepFailed(L("tabSleep.reason.busy"));
  }
}

void TabMenu::CloseOthers(tabs::TabInterface* tab) {
  TabStripModel* model = browser_->GetTabStripModel();
  // 不关固定标签。先选中留下的这个：不然关到当前标签时可能选中它的打开者。
  tabs::TabInterface* active = browser_->GetActiveTabInterface();
  if (!active || !active->IsPinned()) {
    model->ActivateTabAt(model->GetIndexOfTab(tab));
  }
  std::vector<tabs::TabHandle> others;
  for (int i = model->IndexOfFirstNonPinnedTab(); i < model->count(); ++i) {
    tabs::TabInterface* other = model->GetTabAtIndex(i);
    if (other != tab) {
      others.push_back(other->GetHandle());
    }
  }
  for (const tabs::TabHandle& handle : others) {
    tabs::TabInterface* other = handle.Get();
    const int index = other ? model->GetIndexOfTab(other) : TabStripModel::kNoTab;
    if (index != TabStripModel::kNoTab) {
      model->CloseWebContentsAt(index,
                                TabCloseTypes::CLOSE_USER_GESTURE |
                                    TabCloseTypes::CLOSE_CREATE_HISTORICAL_TAB);
    }
  }
}

void TabMenu::ShowSleepFailed(const std::u16string& message) {
  StarryBrowserView* view = StarryBrowserView::From(browser_);
  if (!view) {
    return;
  }
  DialogSpec spec;
  spec.tone = DialogSpec::Tone::kWarning;
  spec.icon = &icons::kMoonFill;
  spec.title = L("window.sleepFailed.title");
  spec.message = message;
  spec.buttons.push_back({L("common.ok"), ButtonVariant::kFilled, /*cancel=*/true});
  view->PresentDialog(std::move(spec), DialogAnswerCallback());
}

}  // namespace starry
