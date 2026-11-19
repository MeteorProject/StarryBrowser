// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_ARCHIVE_ARCHIVE_PAGE_H_
#define STARRY_BROWSER_UI_ARCHIVE_ARCHIVE_PAGE_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "starry/browser/tab_organizer/tab_archive.h"
#include "starry/browser/ui/design/theme.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/controls/textfield/textfield_controller.h"
#include "ui/views/view.h"

class BrowserWindowInterface;

namespace views {
class Textfield;
}

namespace starry {

class ArchiveList;
class ElasticScrollView;
class IconButton;

// 侧栏翻到的「已归档」这一页：点标签区域右上角的
// 归档图标时从右边推进来，盖住常用图块和标签列表的位置（媒体坞、工具按钮还在下面）。
//
// 最上面一行：‹ 回到标签、「已归档」和数量、⤢ 在标签页里打开整页的归档（starry://archive，
// 带着搜索词）。下面是搜索框，再下面按今天、昨天、7 天内、更早分组的列表：网站图标、标题、
// 网站和为什么归档的；悬停时右边是「在后台恢复」和「删除」。点一行恢复并切过去（这一页收
// 起来）。最下面左边是自动归档的设置（点了去设置 › 标签管理），右边是「整页查看」。
//
// 键盘：搜索框里 ↑↓ 选、回车恢复，Esc 先清空搜索、再按一次回到标签。
class ArchivePage : public views::View,
                    public views::TextfieldController,
                    public tab_organizer::TabArchive::Observer {
  METADATA_HEADER(ArchivePage, views::View)

 public:
  explicit ArchivePage(BrowserWindowInterface* browser);
  ArchivePage(const ArchivePage&) = delete;
  ArchivePage& operator=(const ArchivePage&) = delete;
  ~ArchivePage() override;

  void SetTheme(const Theme& theme);
  // 翻过来时：清空搜索、滚到最上面、焦点放在搜索框。
  void Prepare();
  void FocusSearch();
  // 回到标签（SidebarView::ShowArchive(false)）。
  void Close();
  BrowserWindowInterface* browser() const { return browser_; }
  const Theme& theme() const { return theme_; }

  // 调试用：每一行（分组的标题、标签）和搜索词。
  std::string DescribeForTesting() const;
  // 调试用：搜这个词。
  void SetQueryForTesting(const std::u16string& query);
  // 调试用：点第 index 行（只算标签的行）；button 是 restore、remove 或者空的（整行）。
  bool ClickRowForTesting(size_t index, const std::string& button);

  // views::View:
  void Layout(PassKey) override;
  void OnPaint(gfx::Canvas* canvas) override;
  bool OnMousePressed(const ui::MouseEvent& event) override;
  void OnMouseReleased(const ui::MouseEvent& event) override;
  void OnMouseMoved(const ui::MouseEvent& event) override;
  void OnMouseExited(const ui::MouseEvent& event) override;
  ui::Cursor GetCursor(const ui::MouseEvent& event) override;
  bool OnKeyPressed(const ui::KeyEvent& event) override;

  // views::TextfieldController:
  void ContentsChanged(views::Textfield* sender,
                       const std::u16string& new_contents) override;
  bool HandleKeyEvent(views::Textfield* sender,
                      const ui::KeyEvent& key_event) override;

  // tab_organizer::TabArchive::Observer:
  void OnArchiveChanged() override;

 private:
  enum class Link { kNone, kSettings, kFullPage };

  void Reload();
  void OpenFullPage();
  void OpenSettings();
  Link LinkAt(const gfx::Point& point) const;
  gfx::Rect SearchRect() const;
  gfx::Rect FooterRect() const;
  gfx::Rect SettingsLinkRect() const;
  gfx::Rect FullLinkRect() const;
  std::u16string SettingsSummary() const;

  const raw_ptr<BrowserWindowInterface> browser_;
  Theme theme_ = Theme::DarkBase();
  raw_ptr<IconButton> back_ = nullptr;
  raw_ptr<IconButton> full_ = nullptr;
  raw_ptr<views::Textfield> search_ = nullptr;
  raw_ptr<ElasticScrollView> scroll_ = nullptr;
  raw_ptr<ArchiveList> list_ = nullptr;
  Link hovered_link_ = Link::kNone;
  Link pressed_link_ = Link::kNone;
  base::ScopedObservation<tab_organizer::TabArchive,
                          tab_organizer::TabArchive::Observer>
      observation_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_ARCHIVE_ARCHIVE_PAGE_H_
