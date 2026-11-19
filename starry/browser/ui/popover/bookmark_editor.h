// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_POPOVER_BOOKMARK_EDITOR_H_
#define STARRY_BROWSER_UI_POPOVER_BOOKMARK_EDITOR_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "components/bookmarks/browser/base_bookmark_model_observer.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "starry/browser/ui/popover/popover_host.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/views/controls/textfield/textfield_controller.h"
#include "url/gurl.h"

class BrowserWindowInterface;

namespace views {
class Label;
class Textfield;
}  // namespace views

namespace starry {

class BookmarkFolderButton;
class PillButton;

// 加书签（⌘D、菜单栏的「为此页添加书签」）之后的小卡片，替代 Chrome 的工具栏书签气泡：
// 星形图块与「已添加书签」（已存在时为「编辑书签」）、网址、名称输入框、文件夹选择器，
// 底部为「移除」和「完成」。回车、「完成」或点击外部都会保存名称，Esc 取消修改；书签在
// 别处被删掉时自动关闭。
class BookmarkEditor : public PopoverContent,
                       public views::TextfieldController,
                       public bookmarks::BaseBookmarkModelObserver,
                       public ui::SimpleMenuModel::Delegate {
  METADATA_HEADER(BookmarkEditor, PopoverContent)

 public:
  BookmarkEditor(BrowserWindowInterface* browser,
                 PopoverHost* host,
                 const GURL& url,
                 bool already_bookmarked);
  BookmarkEditor(const BookmarkEditor&) = delete;
  BookmarkEditor& operator=(const BookmarkEditor&) = delete;
  ~BookmarkEditor() override;

  // 调试步骤：改名字、按「完成」「移除」。
  void SetNameForTesting(const std::u16string& name);
  void DoneForTesting() { Done(); }
  void RemoveForTesting() { Remove(); }
  std::u16string DescribeForTesting() const;

  // PopoverContent:
  gfx::Size ContentSize() const override;
  void SetTheme(const Theme& theme) override;
  views::View* InitialFocus() override;
  void WillClose() override;

  // views::View:
  void Layout(PassKey) override;
  void OnPaint(gfx::Canvas* canvas) override;

  // views::TextfieldController:
  bool HandleKeyEvent(views::Textfield* sender,
                      const ui::KeyEvent& key_event) override;

  // bookmarks::BaseBookmarkModelObserver:
  void BookmarkModelChanged() override;

  // ui::SimpleMenuModel::Delegate（文件夹菜单）:
  bool IsCommandIdChecked(int command_id) const override;
  void ExecuteCommand(int command_id, int event_flags) override;

 private:
  const bookmarks::BookmarkNode* Node() const;
  void UpdateFolder();
  void ShowFolderMenu();
  void Done();
  void Remove();

  const raw_ptr<BrowserWindowInterface> browser_;
  const raw_ptr<PopoverHost> host_;
  raw_ptr<bookmarks::BookmarkModel> model_ = nullptr;
  int64_t node_id_ = -1;
  Theme theme_ = Theme::DarkBase();
  // Esc：关掉时不改名字。
  bool keep_title_ = false;

  raw_ptr<views::Label> title_ = nullptr;
  raw_ptr<views::Label> url_label_ = nullptr;
  raw_ptr<views::Textfield> field_ = nullptr;
  raw_ptr<views::Label> folder_label_ = nullptr;
  raw_ptr<BookmarkFolderButton> folder_button_ = nullptr;
  raw_ptr<PillButton> remove_button_ = nullptr;
  raw_ptr<PillButton> done_button_ = nullptr;

  std::unique_ptr<ui::SimpleMenuModel> folder_menu_;
  std::vector<int64_t> folder_ids_;

  base::ScopedObservation<bookmarks::BookmarkModel,
                          bookmarks::BookmarkModelObserver>
      observation_{this};
  base::WeakPtrFactory<BookmarkEditor> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_POPOVER_BOOKMARK_EDITOR_H_
