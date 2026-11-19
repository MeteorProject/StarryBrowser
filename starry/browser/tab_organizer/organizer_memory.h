// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_TAB_ORGANIZER_ORGANIZER_MEMORY_H_
#define STARRY_BROWSER_TAB_ORGANIZER_ORGANIZER_MEMORY_H_

#include <optional>
#include <string>
#include <vector>

#include "components/tabs/public/tab_interface.h"

class GURL;
class Profile;

// 记住用户的整理习惯，存在设置里
// （prefs::kOrganizerMemory），所有窗口共用：
// - 按网站的规则：「github.com → 开发」。用户自己加的（设置、右键菜单），和学到的：同一个
//   网站的标签第二次被用户放进同一个文件夹（拖进去、右键「移到文件夹」、接受建议、一键
//   整理放进去的那一批算一次）时记成规则。规则只用来提示（标签行右边的「→ 开发」）和一键
//   整理，不会自己挪标签。
// - 不要的：用户在整理页里去掉的标签、一组，点了「不提示」的建议，把标签从文件夹里挪走的。
//   这个网站和那个文件夹（新文件夹按名字）30 天内一键整理不再放在一起、学到的规则不再提示；
//   学到的规则被拒绝两次就删掉。
// - 用户在整理页里改的名字（最近 10 个）：一键整理时告诉模型。
namespace starry::tab_organizer {

struct SiteRule {
  // 网站（小写的主机名，去掉 www.，含子域名）。
  std::string site;
  // pin_folders 的文件夹 ID。
  std::string folder;
  bool learned = false;
};

// 网页的网站（http、https），别的是空的。
std::string SiteOf(const GURL& url);
// 新文件夹（还没有 ID）在「不要的」里的写法。
std::string NameKey(const std::u16string& name);

// 所有规则，按网站排序。文件夹没了的不算。
std::vector<SiteRule> ReadRules(Profile* profile);
// 这个网址的规则（主机名或者上一级的域名，取最长的那条）；没有是空的。
std::optional<SiteRule> RuleFor(Profile* profile, const GURL& url);
// 加一条（同一个网站只有一条，新的换掉旧的）。site 可以写成网址。
void AddRule(Profile* profile,
             const std::string& site,
             const std::string& folder,
             bool learned);
void RemoveRule(Profile* profile, const std::string& site);

// 用户把这个网页放进了文件夹：记一次（够了就学成规则）。
void NotePlacement(Profile* profile,
                   const GURL& url,
                   const std::string& folder);
// 用户手动挪了一个标签：从 from 挪到 to（空的是不在文件夹里）。to 记一次放进去；离开
// from 记一次不要。
void NoteManualMove(tabs::TabInterface* tab,
                    const std::string& from,
                    const std::string& to);
// 用户不要这个网站和这个文件夹（folder 是文件夹的 ID 或者 NameKey）。
void NoteRejection(Profile* profile,
                   const GURL& url,
                   const std::string& folder);
// 30 天内不要过（folder 的 ID 和名字都看）。
bool IsRejected(Profile* profile,
                const GURL& url,
                const std::string& folder,
                const std::u16string& name);
// 这个网站 30 天内不要过的文件夹（ID 或者 NameKey）。
std::vector<std::string> RejectedFor(Profile* profile, const GURL& url);
// 用户在整理页里起的名字。
void NoteName(Profile* profile, const std::u16string& name);
std::vector<std::u16string> PreferredNames(Profile* profile);
// 忘掉学到的（学到的规则、放过的次数、不要的、名字）；用户自己加的规则留着。
void Forget(Profile* profile);

// 提示把这个标签放进哪个文件夹：设置里开着提示（prefs::kOrganizeSuggest）、普通标签、
// 网页有规则、文件夹还在；这个标签上点过「不提示」的没有。
struct Suggestion {
  std::string folder;
  std::u16string name;
  // 文件夹的颜色（pin_folders::kColors 里的），没选是空的。
  std::string color;
};
std::optional<Suggestion> SuggestFolder(tabs::TabInterface* tab);
// 放进建议的文件夹（记一次放进去）。没有建议时什么都不做。
bool AcceptSuggestion(tabs::TabInterface* tab);
// 这个标签不再提示（记一次不要）。
void DismissSuggestion(tabs::TabInterface* tab);

// 调试用：规则、放过的次数、不要的、名字。
std::string DescribeMemoryForTesting(Profile* profile);

}  // namespace starry::tab_organizer

#endif  // STARRY_BROWSER_TAB_ORGANIZER_ORGANIZER_MEMORY_H_
