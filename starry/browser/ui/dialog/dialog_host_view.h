// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_DIALOG_DIALOG_HOST_VIEW_H_
#define STARRY_BROWSER_UI_DIALOG_DIALOG_HOST_VIEW_H_

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "starry/browser/ui/design/controls.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/controls/textfield/textfield_controller.h"
#include "ui/views/view.h"
#include "ui/views/view_tracker.h"

namespace gfx {
struct VectorIcon;
}

namespace starry {

class DialogCard;

// 一个对话框要显示的内容配置。
struct DialogSpec {
  // 图标块的颜色：强调色、警示色、危险色的浅底。
  enum class Tone { kAccent, kWarning, kDanger };
  struct Button {
    std::u16string title;
    ButtonVariant variant = ButtonVariant::kTertiary;
    // Esc 按的是它。
    bool cancel = false;
  };

  DialogSpec();
  DialogSpec(const DialogSpec&);
  DialogSpec& operator=(const DialogSpec&);
  ~DialogSpec();

  Tone tone = Tone::kAccent;
  // 没有时不画图标块。
  raw_ptr<const gfx::VectorIcon> icon = nullptr;
  std::u16string title;
  std::u16string message;
  // 有值时显示输入框，值是初始的文字。
  std::optional<std::u16string> field;
  // 从左到右；最后一个是默认按钮（回车）。
  std::vector<Button> buttons;
  // 没有默认按钮：回车不回答（网站权限这类，用户正打着字时弹出来不会被顺手允许）。
  bool no_default_button = false;
  // 刚弹出来的这一会儿不接受点按钮（Esc 和取消除外），防止网页趁用户点别处时弹出来骗到
  // 一下点击（同 Chrome 权限气泡的 input protection）。
  base::TimeDelta input_protection;
  // 自己画内容的对话框（DialogRequest::CreateContentView）想要的大小，窗口小时缩小。
  gfx::Size content_size;
};

// 一个排队等着显示、或者正显示着的对话框。DialogHostView 拥有它。
class DialogRequest {
 public:
  virtual ~DialogRequest() = default;
  virtual const DialogSpec& spec() const = 0;
  // 用户点了第 button 个按钮（Esc 是 cancel 的那个）；input 是输入框的文字。
  virtual void Answer(size_t button, const std::u16string& input) = 0;
  // 没有回答就被拿掉了（标签关了、窗口关了）。
  virtual void Dismissed() = 0;
  // 不是空的：卡片里只放这个视图（铺满、圆角裁掉），没有标题、正文和按钮，大小是
  // spec().content_size。内容自己关掉对话框（DialogHostView::Remove）。里面可以是网页：
  // 卡片只淡入淡出，不缩放（macOS 上缩放 WebView 的祖先会把网页改成缩放后的大小）。
  virtual std::unique_ptr<views::View> CreateContentView();
};

// 按了哪个按钮（没回答就被拿掉时是 nullopt）和输入框里的文字。
using DialogAnswerCallback =
    base::OnceCallback<void(std::optional<size_t> button,
                            const std::u16string& input)>;

// 一般的对话框：答案交给 callback（可以是空的）。
std::unique_ptr<DialogRequest> MakeDialogRequest(DialogSpec spec,
                                                 DialogAnswerCallback callback);

// 每个窗口一个对话框层：压暗的遮罩（深色 42%、浅色 20%，点它卡片动一下）和居中的玻璃卡片
// （宽 440，高不超过 640）。卡片从上到下：40 的图标块和 16 半粗的标题、13 的正文（太长时在
// 中间滚动）、胶囊输入框、右对齐的胶囊按钮。回车是默认按钮，Esc 是取消，Tab 和 ←→ 在按钮
// 之间移动键盘选中。同一时间显示一个，其余排队。
class DialogHostView : public views::View, public views::TextfieldController {
  METADATA_HEADER(DialogHostView, views::View)

 public:
  DialogHostView();
  DialogHostView(const DialogHostView&) = delete;
  DialogHostView& operator=(const DialogHostView&) = delete;
  ~DialogHostView() override;

  void Present(std::unique_ptr<DialogRequest> request);
  // 拿掉（排着队的或正显示的），不回答，也不调用 Dismissed。request 晚一点删。
  void Remove(DialogRequest* request);
  bool is_presenting() const { return current_ != nullptr; }
  DialogRequest* current() const { return current_.get(); }
  // 正显示的输入框里的文字。
  std::u16string CurrentInput() const;
  void SetTheme(const Theme& theme);

  // 按第 index 个按钮（卡片上的按钮、调试步骤）。
  void PressButton(size_t index);
  void TypeForTesting(const std::u16string& text);

  // views::View:
  void Layout(PassKey) override;
  bool OnMousePressed(const ui::MouseEvent& event) override;
  bool OnKeyPressed(const ui::KeyEvent& event) override;
  bool SkipDefaultKeyEventProcessing(const ui::KeyEvent& event) override;

  // views::TextfieldController:
  bool HandleKeyEvent(views::Textfield* sender,
                      const ui::KeyEvent& key_event) override;

 private:
  void ShowNext();
  void Respond(size_t button);
  void RespondDefault();
  void RespondCancel();
  void MoveKeyboardSelection(int delta);
  void HideCard(bool then_next);
  void Nudge();

  Theme theme_ = Theme::DarkBase();
  std::deque<std::unique_ptr<DialogRequest>> queue_;
  std::unique_ptr<DialogRequest> current_;
  raw_ptr<views::View> backdrop_ = nullptr;
  raw_ptr<DialogCard> card_ = nullptr;
  LayerMotion backdrop_motion_;
  std::optional<size_t> keyboard_selection_;
  // 正显示的对话框弹出来的时刻（input_protection 用）。
  base::TimeTicks shown_at_;
  views::ViewTracker previous_focus_;
  int generation_ = 0;

  base::WeakPtrFactory<DialogHostView> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_DIALOG_DIALOG_HOST_VIEW_H_
