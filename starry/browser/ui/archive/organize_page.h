// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_ARCHIVE_ORGANIZE_PAGE_H_
#define STARRY_BROWSER_UI_ARCHIVE_ORGANIZE_PAGE_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "components/tabs/public/tab_interface.h"
#include "starry/browser/tab_organizer/tab_grouper.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/motion/layer_loop.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"

class BrowserWindowInterface;

namespace starry {

class ElasticScrollView;
class IconButton;
class OrganizeList;
class PillButton;

// 打开一键整理：侧栏在时侧栏翻到整理页（自动隐藏收着
// 时先滑出来），顶部标签布局下是内容卡片右上角的卡片。无痕窗口里没有。
void OpenOrganize(BrowserWindowInterface* browser);

// 「整理标签页」这一页：AI 按主题把这个窗口的普通标签分成几组，先给用户看、改，再放进固定区
// 的文件夹。侧栏里同归档页（SidebarView::ShowOrganize），顶部标签布局下装在卡片里。
//
// 打开时开始问模型（「正在按主题分组…」，下面几块淡淡地呼吸的占位）；回来以后每组一张小卡
// 片：文件夹图标、名字（新文件夹的名字点一下就地改）、「新文件夹」或「已有的文件夹」、右上
// ✕ 去掉这一组；下面是放进去的标签，悬停时右边 ✕ 把它留在列表里，点一下切过去。最后是
// 「留在列表里 · N 个」（点开看是哪些）。最下面「重新整理」和「放进 N 个文件夹」。出错时写
// 原因：没设置模型、找不到本机智能体时是「去设置」，别的是「重试」。
//
// 键盘：Esc 回去（正在改名时先取消改名），回车放进去。
class OrganizePage : public views::View, public TabStripModelObserver {
  METADATA_HEADER(OrganizePage, views::View)

 public:
  enum class State { kIdle, kThinking, kResult, kError };

  // 预览里的一组（可以改名、去掉标签）。
  struct Group {
    Group();
    Group(const Group&);
    Group& operator=(const Group&);
    ~Group();

    std::u16string name;
    // 已有的文件夹；空的是新建。
    std::string folder;
    std::vector<tabs::TabHandle> tabs;
  };

  // in_card：装在顶部标签布局的卡片里（没有返回，右上角是关闭）。
  OrganizePage(BrowserWindowInterface* browser, bool in_card);
  OrganizePage(const OrganizePage&) = delete;
  OrganizePage& operator=(const OrganizePage&) = delete;
  ~OrganizePage() override;

  void SetTheme(const Theme& theme);
  // 翻过来时：正在问就接着等；刚问过（2 分钟内）就留着结果，不然重新问。
  void Prepare();
  // 翻走了：停下正在问的（下次翻过来重新问）。
  void Closed();
  // 回去（侧栏回到标签、卡片收起）。
  void Close();
  // 重新问一次。
  void Regroup();
  // 按预览放进文件夹（整批能撤销），然后回去。
  void Apply();
  void RenameGroup(size_t group, const std::u16string& name);
  void DropGroup(size_t group);
  void DropTab(size_t group, size_t tab);
  void OpenSettings();

  BrowserWindowInterface* browser() const { return browser_; }
  const Theme& theme() const { return theme_; }
  State state() const { return state_; }
  const std::vector<Group>& groups() const { return groups_; }
  const std::vector<tabs::TabHandle>& leftover() const { return leftover_; }
  bool in_card() const { return in_card_; }

  // 调试用：状态、每一组和里面的标签、留下的。
  std::string DescribeForTesting() const;
  // 调试用：改名（同点名字、输入、回车）。
  bool RenameForTesting(size_t group, const std::u16string& name);

  // views::View:
  void Layout(PassKey) override;
  void OnPaint(gfx::Canvas* canvas) override;
  bool OnKeyPressed(const ui::KeyEvent& event) override;
  bool SkipDefaultKeyEventProcessing(const ui::KeyEvent& event) override;
  void VisibilityChanged(views::View* starting_from, bool is_visible) override;

  // TabStripModelObserver:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;

 private:
  void Start();
  void OnGrouped(tab_organizer::TabGrouper::Result result);
  // 关掉、固定了、挪走了的标签拿掉，空了的组拿掉。
  void Prune();
  // 状态、组变了：按钮、列表、提示跟着变。
  void Update();
  std::u16string ErrorText() const;
  bool NeedsSettings() const;
  // 用户在预览里去掉了这一组里的这个标签：记住这个网站不要放进这个文件夹。
  void NoteDropped(const Group& group, const tabs::TabHandle& handle);
  size_t TabCount() const;

  const raw_ptr<BrowserWindowInterface> browser_;
  const bool in_card_;
  Theme theme_ = Theme::DarkBase();
  State state_ = State::kIdle;
  std::string error_;
  std::vector<Group> groups_;
  std::vector<tabs::TabHandle> leftover_;
  // 问了几个标签。
  size_t asked_ = 0;
  base::TimeTicks planned_;
  std::unique_ptr<tab_organizer::TabGrouper> grouper_;

  raw_ptr<IconButton> close_ = nullptr;
  raw_ptr<ElasticScrollView> scroll_ = nullptr;
  raw_ptr<OrganizeList> list_ = nullptr;
  // 正在问的时候淡淡地呼吸的占位。
  raw_ptr<views::View> skeleton_ = nullptr;
  LayerLoop pulse_;
  raw_ptr<PillButton> regroup_ = nullptr;
  raw_ptr<PillButton> apply_ = nullptr;
  raw_ptr<PillButton> cancel_ = nullptr;
  raw_ptr<PillButton> retry_ = nullptr;
  raw_ptr<PillButton> settings_ = nullptr;
  base::WeakPtrFactory<OrganizePage> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_ARCHIVE_ORGANIZE_PAGE_H_
