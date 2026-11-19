// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_WEBUI_BOOKMARKS_HANDLER_H_
#define STARRY_BROWSER_UI_WEBUI_BOOKMARKS_HANDLER_H_

#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "components/bookmarks/browser/base_bookmark_model_observer.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "content/public/browser/web_ui_message_handler.h"

class Profile;

namespace starry {

// 书签页（pages/bookmarks/）的数据：基于 Chrome 的 BookmarkModel。按文件夹分组，每组
// 包含一个文件夹里的网址（子文件夹各自成组，名称为完整路径），按树顺序排列。无痕窗口
// 读取原始用户资料的书签。
//
//   getBookmarks(callbackId)
//     回 {loaded, folders: [{id, path, items: [{id, title, url}]}]}；空的文件夹不列。
//   updateBookmark(callbackId, id, title, address)
//     address 按地址栏的规则补全（可以省略 https://）；地址栏会当成搜索的回
//     {error: 'invalid'}，否则回 {ok}。
//   removeBookmark(id)
//   moveBookmark(id, beforeId, folderId)：挪到 beforeId 前面；beforeId 是 null 时放到
//     folderId 这个文件夹的最后。
//   addBookmark(url, title, beforeId, folderId)：同上；两个都是 null 时放到 Chrome 默认的
//     文件夹（最近加过书签的那个）。
// 书签变了（别处加了、改了、删了）时发 bookmarks-changed（带 getBookmarks 的结果）。
class BookmarksHandler : public content::WebUIMessageHandler,
                         public bookmarks::BaseBookmarkModelObserver {
 public:
  explicit BookmarksHandler(Profile* profile);
  BookmarksHandler(const BookmarksHandler&) = delete;
  BookmarksHandler& operator=(const BookmarksHandler&) = delete;
  ~BookmarksHandler() override;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;
  void OnJavascriptAllowed() override;
  void OnJavascriptDisallowed() override;

  // bookmarks::BaseBookmarkModelObserver:
  void BookmarkModelChanged() override;
  void BookmarkNodeFaviconChanged(const bookmarks::BookmarkNode* node) override {}

 private:
  bookmarks::BookmarkModel* model();
  const bookmarks::BookmarkNode* FindNode(const base::Value& id);
  base::DictValue Snapshot();

  void HandleGetBookmarks(const base::ListValue& args);
  void HandleUpdateBookmark(const base::ListValue& args);
  void HandleRemoveBookmark(const base::ListValue& args);
  void HandleMoveBookmark(const base::ListValue& args);
  void HandleAddBookmark(const base::ListValue& args);

  const raw_ptr<Profile> profile_;
  base::OneShotTimer changed_timer_;
  base::ScopedObservation<bookmarks::BookmarkModel,
                          bookmarks::BookmarkModelObserver>
      observation_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_WEBUI_BOOKMARKS_HANDLER_H_
