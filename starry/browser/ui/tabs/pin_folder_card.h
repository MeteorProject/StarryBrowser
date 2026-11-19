// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_TABS_PIN_FOLDER_CARD_H_
#define STARRY_BROWSER_UI_TABS_PIN_FOLDER_CARD_H_

#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/popover/popover_host.h"
#include "starry/browser/ui/tabs/tab_cell.h"
#include "starry/browser/ui/tabs/tab_list_view.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/controls/textfield/textfield_controller.h"

namespace views {
class Textfield;
}

namespace starry {

class ElasticScrollView;
class GlyphButton;
class TabsView;

// 固定标签和常用标签的文件夹卡片，将列表交互转发给所属的 TabsView。
class PinFolderCard : public PopoverContent,
                      public TabCellDelegate,
                      public views::TextfieldController {
  METADATA_HEADER(PinFolderCard, PopoverContent)

 public:
  PinFolderCard(TabsView* owner, std::string id);
  PinFolderCard(const PinFolderCard&) = delete;
  PinFolderCard& operator=(const PinFolderCard&) = delete;
  ~PinFolderCard() override;

  const std::string& id() const { return id_; }
  const std::u16string& title() const { return title_; }
  TabListView* list() { return list_; }
  // 卡片收起了（还在淡出）或者顶部标签行没了：点击、拖动不再交出去。
  void Detach();
  // 卡片多宽（侧栏里同常用图块一样宽）；不设是 264。
  void SetCardWidth(int width) { card_width_ = width; }
  // 改名时 Esc 取消编辑并保留卡片，失焦或关闭卡片时提交。
  void BeginRename();
  bool renaming() const { return field_ != nullptr; }
  // 调试用：输入 text 后确定；空的是取消。
  void FinishRenameForTesting(std::optional<std::u16string> text);
  // 输入框失去焦点：按输入的存。
  void OnFieldBlurred();
  void Update(const std::u16string& title,
              std::vector<TabListRowData> rows,
              const std::vector<TabListView::Item>& items,
              const Theme& theme,
              TabListView::Change change);

  // PopoverContent:
  gfx::Size ContentSize() const override;
  void SetTheme(const Theme& theme) override;
  void WillClose() override;
  bool HandlesEscape() const override;

  // views::View:
  void Layout(PassKey) override;
  void OnPaint(gfx::Canvas* canvas) override;
  bool OnMousePressed(const ui::MouseEvent& event) override;

  // views::TextfieldController:
  bool HandleKeyEvent(views::Textfield* sender,
                      const ui::KeyEvent& key_event) override;

  // TabCellDelegate:
  void TabCellClicked(TabCell* cell, bool reset, bool keep_current) override;
  void TabCellCloseRequested(TabCell* cell, bool button) override;
  void TabCellMuteToggled(TabCell* cell) override;
  void TabFolderAction(const std::string& id,
                       std::optional<gfx::Point> menu_point) override;
  void TabFolderAddCurrent(const std::string& id) override;
  void TabFolderOpenAll(const std::string& id) override;
  void TabFolderToggleTree(const std::string& id) override;
  bool TabListKeyPressed(views::View* item, const ui::KeyEvent& event) override;
  void TabFolderRenameFinished(const std::string& id,
                               std::optional<std::u16string> name) override;
  void TabCellDragStarted(TabCell* cell, const gfx::Point& location) override;
  void TabCellDragged(TabCell* cell, const gfx::Point& location) override;
  void TabCellDragEnded(TabCell* cell, bool cancelled) override;
  void TabRowDragStarted(TabListRow* row, const gfx::Point& location) override;
  void TabRowDragged(TabListRow* row, const gfx::Point& location) override;
  void TabRowDragEnded(TabListRow* row, bool cancelled) override;
  void TabCellContextMenu(TabCell* cell,
                          const gfx::Point& screen_point,
                          ui::mojom::MenuSourceType source) override;

 private:
  void ShowMenu();
  void FinishRename(bool commit);
  gfx::Rect TitleRect() const;

  raw_ptr<TabsView> owner_;
  const std::string id_;
  std::u16string title_;
  int card_width_ = 0;
  Theme theme_ = Theme::DarkBase();
  raw_ptr<GlyphButton> add_button_ = nullptr;
  raw_ptr<GlyphButton> more_button_ = nullptr;
  raw_ptr<ElasticScrollView> scroll_ = nullptr;
  raw_ptr<TabListView> list_ = nullptr;
  raw_ptr<views::Textfield> field_ = nullptr;
  bool finishing_rename_ = false;

  base::WeakPtrFactory<PinFolderCard> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_TABS_PIN_FOLDER_CARD_H_
