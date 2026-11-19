// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/webui/bookmarks_handler.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/autocomplete/autocomplete_classifier_factory.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/bookmarks/browser/bookmark_node.h"
#include "components/bookmarks/browser/bookmark_utils.h"
#include "components/bookmarks/common/bookmark_metrics.h"
#include "components/omnibox/browser/autocomplete_classifier.h"
#include "components/omnibox/browser/autocomplete_match.h"
#include "starry/browser/l10n/l10n.h"
#include "third_party/metrics_proto/omnibox_event.pb.h"
#include "content/public/browser/web_ui.h"
#include "url/gurl.h"

namespace starry {

namespace {

using bookmarks::BookmarkNode;

// 一连串的变化（拖动、导入）合成一次通知。
constexpr base::TimeDelta kChangedDelay = base::Milliseconds(60);

constexpr bookmarks::metrics::BookmarkEditSource kUserEdit =
    bookmarks::metrics::BookmarkEditSource::kUser;

// 固定的几个文件夹用 Starry 的叫法。
std::u16string FolderTitle(const BookmarkNode* node) {
  switch (node->type()) {
    case BookmarkNode::BOOKMARK_BAR:
      return L("bookmarks.folder.bookmarkBar");
    case BookmarkNode::OTHER_NODE:
      return L("bookmarks.folder.other");
    case BookmarkNode::MOBILE:
      return L("bookmarks.folder.mobile");
    default:
      return node->GetTitle().empty() ? L("bookmarks.folder.untitled") : node->GetTitle();
  }
}

std::string NodeId(const BookmarkNode* node) {
  return base::NumberToString(node->id());
}

// 一个文件夹：它自己的网址成一组，子文件夹按顺序跟在后面。
void AppendFolder(const BookmarkNode* folder,
                  const std::u16string& path,
                  base::ListValue& out) {
  base::ListValue items;
  for (const auto& child : folder->children()) {
    if (!child->is_url()) {
      continue;
    }
    base::DictValue item;
    item.Set("id", NodeId(child.get()));
    item.Set("title", child->GetTitle());
    item.Set("url", child->url().spec());
    items.Append(std::move(item));
  }
  if (!items.empty()) {
    base::DictValue entry;
    entry.Set("id", NodeId(folder));
    entry.Set("path", path);
    entry.Set("items", std::move(items));
    out.Append(std::move(entry));
  }
  for (const auto& child : folder->children()) {
    if (child->is_folder()) {
      AppendFolder(child.get(),
                   base::StrCat({path, u" › ", FolderTitle(child.get())}), out);
    }
  }
}

}  // namespace

BookmarksHandler::BookmarksHandler(Profile* profile)
    : profile_(profile->GetOriginalProfile()) {}

BookmarksHandler::~BookmarksHandler() = default;

bookmarks::BookmarkModel* BookmarksHandler::model() {
  return BookmarkModelFactory::GetForBrowserContext(profile_);
}

void BookmarksHandler::RegisterMessages() {
  const auto add = [this](const char* name, auto method) {
    web_ui()->RegisterMessageCallback(
        name, base::BindRepeating(method, base::Unretained(this)));
  };
  add("getBookmarks", &BookmarksHandler::HandleGetBookmarks);
  add("updateBookmark", &BookmarksHandler::HandleUpdateBookmark);
  add("removeBookmark", &BookmarksHandler::HandleRemoveBookmark);
  add("moveBookmark", &BookmarksHandler::HandleMoveBookmark);
  add("addBookmark", &BookmarksHandler::HandleAddBookmark);
}

void BookmarksHandler::OnJavascriptAllowed() {
  if (bookmarks::BookmarkModel* bookmarks = model()) {
    observation_.Observe(bookmarks);
  }
}

void BookmarksHandler::OnJavascriptDisallowed() {
  observation_.Reset();
  changed_timer_.Stop();
}

void BookmarksHandler::BookmarkModelChanged() {
  if (changed_timer_.IsRunning()) {
    return;
  }
  changed_timer_.Start(FROM_HERE, kChangedDelay,
                       base::BindOnce(
                           [](BookmarksHandler* handler) {
                             if (handler->IsJavascriptAllowed()) {
                               handler->FireWebUIListener(
                                   "bookmarks-changed",
                                   base::Value(handler->Snapshot()));
                             }
                           },
                           base::Unretained(this)));
}

base::DictValue BookmarksHandler::Snapshot() {
  base::DictValue reply;
  bookmarks::BookmarkModel* bookmarks = model();
  const bool loaded = bookmarks && bookmarks->loaded();
  reply.Set("loaded", loaded);
  base::ListValue folders;
  if (loaded) {
    // 根下面是书签栏、其他书签、移动设备书签（登录了同步的账号时还有账号的那一套）。
    for (const auto& permanent : bookmarks->root_node()->children()) {
      if (permanent->IsVisible() || !permanent->children().empty()) {
        AppendFolder(permanent.get(), FolderTitle(permanent.get()), folders);
      }
    }
  }
  reply.Set("folders", std::move(folders));
  return reply;
}

const BookmarkNode* BookmarksHandler::FindNode(const base::Value& id) {
  bookmarks::BookmarkModel* bookmarks = model();
  if (!bookmarks || !bookmarks->loaded() || !id.is_string()) {
    return nullptr;
  }
  int64_t value = 0;
  if (!base::StringToInt64(id.GetString(), &value)) {
    return nullptr;
  }
  return bookmarks::GetBookmarkNodeByID(bookmarks, value);
}

void BookmarksHandler::HandleGetBookmarks(const base::ListValue& args) {
  AllowJavascript();
  ResolveJavascriptCallback(args[0], base::Value(Snapshot()));
}

void BookmarksHandler::HandleUpdateBookmark(const base::ListValue& args) {
  AllowJavascript();
  base::DictValue reply;
  const BookmarkNode* node = args.size() > 3 ? FindNode(args[1]) : nullptr;
  if (!node || !node->is_url() || !args[2].is_string() ||
      !args[3].is_string()) {
    reply.Set("error", "missing");
    ResolveJavascriptCallback(args[0], base::Value(std::move(reply)));
    return;
  }
  // 按地址栏的规则判断：会被识别为搜索的内容不算有效网址。
  const std::u16string address(base::TrimWhitespace(
      base::UTF8ToUTF16(args[3].GetString()), base::TRIM_ALL));
  AutocompleteMatch match;
  if (!address.empty()) {
    AutocompleteClassifierFactory::GetForProfile(profile_)->Classify(
        address, /*in_keyword_mode=*/false,
        /*allow_exact_keyword_match=*/false,
        ::metrics::OmniboxEventProto::OTHER, &match,
        /*alternate_nav_url=*/nullptr);
  }
  const GURL& url = match.destination_url;
  if (address.empty() || !url.is_valid() ||
      AutocompleteMatch::IsSearchType(match.type)) {
    reply.Set("error", "invalid");
    ResolveJavascriptCallback(args[0], base::Value(std::move(reply)));
    return;
  }
  const std::u16string title(base::TrimWhitespace(
      base::UTF8ToUTF16(args[2].GetString()), base::TRIM_ALL));
  bookmarks::BookmarkModel* bookmarks = model();
  bookmarks->SetTitle(node, title, kUserEdit);
  bookmarks->SetURL(node, url, kUserEdit);
  reply.Set("ok", true);
  ResolveJavascriptCallback(args[0], base::Value(std::move(reply)));
}

void BookmarksHandler::HandleRemoveBookmark(const base::ListValue& args) {
  const BookmarkNode* node = args.empty() ? nullptr : FindNode(args[0]);
  if (!node || node->is_permanent_node()) {
    return;
  }
  model()->Remove(node, kUserEdit, FROM_HERE);
}

void BookmarksHandler::HandleMoveBookmark(const base::ListValue& args) {
  if (args.size() < 3) {
    return;
  }
  const BookmarkNode* node = FindNode(args[0]);
  if (!node || node->is_permanent_node()) {
    return;
  }
  if (const BookmarkNode* before = FindNode(args[1])) {
    if (before != node) {
      model()->Move(node, before->parent(),
                    *before->parent()->GetIndexOf(before));
    }
    return;
  }
  if (const BookmarkNode* folder = FindNode(args[2]);
      folder && folder->is_folder() &&
      !bookmarks::IsDescendantOf(folder, node)) {
    model()->Move(node, folder, folder->children().size());
  }
}

void BookmarksHandler::HandleAddBookmark(const base::ListValue& args) {
  bookmarks::BookmarkModel* bookmarks = model();
  if (!bookmarks || !bookmarks->loaded() || args.size() < 4 ||
      !args[0].is_string()) {
    return;
  }
  const GURL url(args[0].GetString());
  if (!url.is_valid() || !(url.SchemeIsHTTPOrHTTPS() || url.SchemeIsFile())) {
    return;
  }
  const std::u16string title = args[1].is_string()
                                   ? base::UTF8ToUTF16(args[1].GetString())
                                   : std::u16string();
  const BookmarkNode* parent = nullptr;
  size_t index = 0;
  if (const BookmarkNode* before = FindNode(args[2])) {
    parent = before->parent();
    index = *parent->GetIndexOf(before);
  } else if (const BookmarkNode* folder = FindNode(args[3]);
             folder && folder->is_folder()) {
    parent = folder;
    index = folder->children().size();
  } else {
    parent = bookmarks::GetParentForNewNodes(bookmarks, url);
    index = parent->children().size();
  }
  // 没有名字时用网址的主机名（网页打开后 Chrome 不会再补标题）。
  bookmarks->AddURL(parent, index,
                    title.empty() ? base::UTF8ToUTF16(url.host()) : title, url,
                    /*meta_info=*/nullptr, /*creation_time=*/std::nullopt,
                    /*uuid=*/std::nullopt, /*added_by_user=*/true);
}

}  // namespace starry
