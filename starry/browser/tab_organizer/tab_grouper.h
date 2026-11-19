// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_TAB_ORGANIZER_TAB_GROUPER_H_
#define STARRY_BROWSER_TAB_ORGANIZER_TAB_GROUPER_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/types/expected.h"
#include "components/tabs/public/tab_interface.h"

class BrowserWindowInterface;
class Profile;

namespace starry::tab_organizer {

class OrganizerModel;

// 一键整理的分组阶段：AI 把一个窗口里的普通标签按主题
// 分成几组，各自放进一个文件夹（已有的优先）。这里挑标签、写给模型的话、读回答，整理出一
// 份「分组的计划」；预览在侧栏的整理页（OrganizePage），放进去是
// TabOrganizer::MoveToFolders。给模型的只有标题（遮掉邮箱、长数字这类）和网站，已有的
// 文件夹的名字和里面的几个标题。
struct GroupPlan {
  GroupPlan();
  GroupPlan(const GroupPlan&);
  GroupPlan& operator=(const GroupPlan&);
  ~GroupPlan();

  struct Group {
    Group();
    Group(const Group&);
    Group& operator=(const Group&);
    ~Group();

    std::u16string name;
    // 放进已有的文件夹：它的 ID；新建的是空的。
    std::string folder;
    std::vector<tabs::TabHandle> tabs;
  };
  std::vector<Group> groups;
  // 问过模型、没分进哪一组的（留在列表里），按列表里的顺序。
  std::vector<tabs::TabHandle> leftover;
};

// 能整理的标签有这么多时「新标签页」那一行一直显示「✦ 整理 N」。
inline constexpr size_t kOrganizeHintTabs = 15;
// 要不要一直显示「✦ 整理 N」：设置里开着（prefs::kOrganizeHint）、选了模型、能整理的有
// kOrganizeHintTabs 个以上。
bool OrganizeHintWanted(Profile* profile, size_t organizable);

// 一次最多给模型多少个标签（多的按列表里的顺序留下）。
inline constexpr size_t kMaxGroupedTabs = 300;
// 最多分几组。
inline constexpr size_t kMaxGroups = 10;
// 文件夹的名字最多几个字。
inline constexpr size_t kMaxFolderName = 40;

// 这个窗口里能一键整理的标签，按列表里的顺序：普通标签（不固定的）里的网页（http、https、
// file），「不整理的网站」以外的；无痕窗口里没有。
std::vector<tabs::TabInterface*> OrganizableTabs(
    BrowserWindowInterface* browser);

// 告诉模型的已有文件夹（这个窗口里显示的）：第 i 个在给模型的话里叫 F<i+1>。
struct PromptFolder {
  std::string id;
  std::u16string name;
  // 嵌在别的文件夹里的写成「工作 / 设计」。
  std::u16string path;
};
std::vector<PromptFolder> PromptFolders(BrowserWindowInterface* browser);

// 给模型的说明、这一次的输入（JSON：界面语言、最多几组、已有的文件夹 F1……和里面的
// 几个标题、标签 1……的标题和网站）。
std::string GroupingSystemPrompt();
std::string GroupingPrompt(BrowserWindowInterface* browser,
                           const std::vector<tabs::TabInterface*>& tabs,
                           const std::vector<PromptFolder>& folders);

// 读模型的回答 {"groups":[{"name":"…","folder":"F1"|null,"tabs":[1,2]}]}（前后有别的
// 字、包在 ``` 里也行）：序号对不上的、重复的不要；新文件夹至少 2 个标签；和已有的文件夹
// 同名的放进那个文件夹；同一个文件夹的几组并成一组；每组里按列表的顺序。读不懂时为空。
struct ParsedGroup {
  ParsedGroup();
  ParsedGroup(const ParsedGroup&);
  ParsedGroup& operator=(const ParsedGroup&);
  ~ParsedGroup();

  std::u16string name;
  std::string folder;
  // 标签在问模型的那些里的下标。
  std::vector<size_t> tabs;
};
std::optional<std::vector<ParsedGroup>> ParseGroups(
    const std::string& text,
    const std::vector<PromptFolder>& folders,
    size_t tab_count);

// 发给模型之前遮掉标题里的邮箱、长数字、像密钥的长串。
std::u16string MaskTitle(const std::u16string& title);
// 模型给的名字（文件夹、主题）：一行、去掉两边的引号、最多 max 个字。
std::u16string CleanLabel(const std::string& raw, size_t max);

// 跑一次：挑标签、问 AI 整理用的模型、读回答。出错的原因：tooFew（能整理的不到 2 个）、
// badAnswer（读不懂）和 OrganizerModel 的（notConfigured、localAgentMissing、timeout、
// cancelled、failed: …）。
class TabGrouper {
 public:
  using Result = base::expected<GroupPlan, std::string>;
  using Callback = base::OnceCallback<void(Result)>;

  explicit TabGrouper(BrowserWindowInterface* browser);
  TabGrouper(const TabGrouper&) = delete;
  TabGrouper& operator=(const TabGrouper&) = delete;
  ~TabGrouper();

  // 只能跑一次。回调可能同步来（标签不够、没设置模型）。
  void Start(Callback callback);
  // 停下：回调收到 cancelled。
  void Cancel();
  bool running() const { return !callback_.is_null(); }
  // 问了几个标签。
  size_t tab_count() const { return tabs_.size(); }

 private:
  void OnAnswer(base::expected<std::string, std::string> result);
  // 按记住的习惯改一遍（organizer_memory.h）：不要的拿出来，有规则的网站放进它的文件夹，
  // 再去掉太小的组。
  void ApplyMemory(GroupPlan* plan) const;
  void Finish(Result result);

  const raw_ptr<BrowserWindowInterface> browser_;
  std::vector<tabs::TabHandle> tabs_;
  std::vector<PromptFolder> folders_;
  std::unique_ptr<OrganizerModel> model_;
  Callback callback_;
  base::WeakPtrFactory<TabGrouper> weak_factory_{this};
};

}  // namespace starry::tab_organizer

#endif  // STARRY_BROWSER_TAB_ORGANIZER_TAB_GROUPER_H_
