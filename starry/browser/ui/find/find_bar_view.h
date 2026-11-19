// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_FIND_FIND_BAR_VIEW_H_
#define STARRY_BROWSER_UI_FIND_FIND_BAR_VIEW_H_

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/find_bar/find_bar.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/controls/textfield/textfield_controller.h"
#include "ui/views/view.h"
#include "ui/views/view_targeter_delegate.h"
#include "ui/views/view_tracker.h"

namespace find_in_page {
class FindNotificationDetails;
}

namespace views {
class Label;
class Textfield;
}

namespace starry {

class FindBarPanel;
class GlassView;
class IconButton;
class StarryFindBar;

// 查找栏（⌘F）：内容卡片右上角宽 360、高 38 的玻璃条，放大镜、输入框、「未找到」，
// 上一个、下一个、关闭。从卡片顶上落下来（motion::kFindBar）。
// 查找本身是 Chrome 的 FindBarController / FindTabHelper（每个标签记着自己找的词）。
//
// 这个视图盖住整个窗口（好按内容卡片摆位置），但只有查找栏那一块接鼠标。
class FindBarView : public views::View,
                    public views::TextfieldController,
                    public views::ViewTargeterDelegate {
  METADATA_HEADER(FindBarView, views::View)

 public:
  FindBarView();
  FindBarView(const FindBarView&) = delete;
  FindBarView& operator=(const FindBarView&) = delete;
  ~FindBarView() override;

  void set_find_bar(StarryFindBar* find_bar) { find_bar_ = find_bar; }
  void SetTheme(const Theme& theme);
  // 内容卡片在父视图里的位置：查找栏放在它右上角。
  void SetCardBounds(const gfx::Rect& card);

  void ShowBar(bool animate, bool focus);
  void HideBar(bool animate);
  bool is_shown() const { return shown_; }
  // 显示着时查找栏的位置（本视图的坐标；媒体岛让开它），没显示时是空的。
  gfx::Rect ShownRect() const;
  // 出现、收起时（媒体岛跟着让位）。
  void SetShownChangedCallback(base::RepeatingClosure callback) {
    shown_changed_ = std::move(callback);
  }
  void FocusAndSelectAll();
  void SetFindTextAndSelectedRange(const std::u16string& text,
                                   const gfx::Range& range);
  std::u16string_view GetFindText() const;
  gfx::Range GetSelectedRange() const;
  void UpdateForResult(const find_in_page::FindNotificationDetails& result,
                       const std::u16string& find_text);
  bool FieldHasFocus() const;
  // 调试用：在查找栏里输入（同用户打字）。
  void TypeForTesting(const std::u16string& text);
  std::u16string_view match_text() const { return match_text_; }

  // views::View:
  void Layout(PassKey) override;

  // views::ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override;

  // views::TextfieldController:
  void ContentsChanged(views::Textfield* sender,
                       const std::u16string& new_contents) override;
  bool HandleKeyEvent(views::Textfield* sender,
                      const ui::KeyEvent& key_event) override;

 private:
  void Find(const std::u16string& text);
  void FindNext(bool reverse);
  void EndSession();
  void SetNotFound(bool not_found);

  raw_ptr<StarryFindBar> find_bar_ = nullptr;
  base::RepeatingClosure shown_changed_;
  Theme theme_ = Theme::DarkBase();
  bool shown_ = false;
  bool not_found_ = false;
  std::u16string match_text_;
  gfx::Rect card_;

  raw_ptr<GlassView> glass_ = nullptr;
  raw_ptr<FindBarPanel> panel_ = nullptr;
  raw_ptr<views::Textfield> field_ = nullptr;
  raw_ptr<views::Label> not_found_label_ = nullptr;
  raw_ptr<IconButton> previous_button_ = nullptr;
  raw_ptr<IconButton> next_button_ = nullptr;
  raw_ptr<IconButton> close_button_ = nullptr;
  LayerMotion motion_;
  int hide_generation_ = 0;

  base::WeakPtrFactory<FindBarView> weak_factory_{this};
};

// Chrome 的 FindBar 接口，界面是 FindBarView（StarryBrowserView::CreateFindBar 建它）。
class StarryFindBar : public FindBar, public FindBarTesting {
 public:
  explicit StarryFindBar(FindBarView* view);
  StarryFindBar(const StarryFindBar&) = delete;
  StarryFindBar& operator=(const StarryFindBar&) = delete;
  ~StarryFindBar() override;

  FindBarView* view() const;

  // FindBar:
  FindBarController* GetFindBarController() const override;
  void SetFindBarController(FindBarController* find_bar_controller) override;
  void Show(bool animate, bool focus) override;
  void Hide(bool animate) override;
  void SetFocusAndSelection() override;
  void ClearResults(
      const find_in_page::FindNotificationDetails& results) override;
  void StopAnimation() override;
  void MoveWindowIfNecessary() override;
  void SetFindTextAndSelectedRange(const std::u16string& find_text,
                                   const gfx::Range& selected_range) override;
  std::u16string_view GetFindText() const override;
  gfx::Range GetSelectedRange() const override;
  void UpdateUIForFindResult(
      const find_in_page::FindNotificationDetails& result,
      const std::u16string& find_text) override;
  void AudibleAlert() override;
  bool IsFindBarVisible() const override;
  void RestoreSavedFocus() override;
  bool HasGlobalFindPasteboard() const override;
  void UpdateFindBarForChangedWebContents() override;
  bool CanPopulateFromSelectedText() override;
  const FindBarTesting* GetFindBarTesting() const override;
  bool HasFocus() const override;
  void CloseOverlappingBubbles() override;
  views::Widget* GetHostWidget() override;

  // FindBarTesting:
  bool GetFindBarWindowInfo(gfx::Point* position,
                            bool* fully_visible) const override;
  std::u16string_view GetFindSelectedText() const override;
  std::u16string_view GetMatchCountText() const override;
  int GetContentsWidth() const override;
  size_t GetAudibleAlertCount() const override;

 private:
  views::ViewTracker view_;
  raw_ptr<FindBarController> controller_ = nullptr;
  size_t audible_alerts_ = 0;
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_FIND_FIND_BAR_VIEW_H_
