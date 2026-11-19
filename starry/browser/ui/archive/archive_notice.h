// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_ARCHIVE_ARCHIVE_NOTICE_H_
#define STARRY_BROWSER_UI_ARCHIVE_ARCHIVE_NOTICE_H_

#include <string>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "starry/browser/tab_organizer/tab_archive.h"
#include "starry/browser/tab_organizer/tab_organizer.h"

class BrowserWindowInterface;

namespace starry {

// 自动整理以后「刚归档 N 个标签页 · 撤销」的提示，
// 一个窗口一份（StarryBrowserView 拥有）。只算这个窗口里归档的标签：一批里没有这个窗口的
// 不提示。
//
// 窗口到前台后，在内容卡片左下角显示提示条（ArchiveToast）。
//
// 一键整理放进文件夹以后的「整理进 N 个文件夹 · 撤销」也用它（kind() 是 kOrganize，提示条
// 上没有「查看」）。
//
// 什么时候收：被看到以后 8 秒（鼠标停在上面时不算，离开后再等 3 秒）；撤销了、又整理了
// 一批（换成新的）；点了提示以外的地方（Dismiss）；这一批都从归档里拿掉了（恢复了、删了，
// 拿掉一部分时个数跟着变）。一直没被看到的，整理过了 12 小时就不再提示。
class ArchiveNotice : public tab_organizer::TabOrganizer::Observer,
                      public tab_organizer::TabArchive::Observer {
 public:
  enum class Kind { kArchive, kOrganize };

  // 这个窗口的（窗口还在建、StarryBrowserView::From 还拿不到时也行）；没有是空的。
  static ArchiveNotice* From(BrowserWindowInterface* browser);

  explicit ArchiveNotice(BrowserWindowInterface* browser);
  ArchiveNotice(const ArchiveNotice&) = delete;
  ArchiveNotice& operator=(const ArchiveNotice&) = delete;
  ~ArchiveNotice() override;

  bool active() const { return !batch_id_.empty(); }
  Kind kind() const { return kind_; }
  // 归档：这一批里有几个是这个窗口的；整理：放进了几个文件夹。
  int count() const { return count_; }
  // 提示条上写的（不含「撤销」）。
  std::u16string ToastText() const;
  const std::string& batch_id() const { return batch_id_; }
  bool shown() const { return shown_; }

  // 鼠标停在提示上：不倒数。
  void Hold(bool hold);
  void Undo();
  void Dismiss();

  // 提示的内容或可见性变了。
  base::CallbackListSubscription AddChangedCallback(
      base::RepeatingClosure callback);

  // 调试用：当作这个窗口刚整理过一批（count 个）。
  void ShowForTesting(const std::string& batch_id, int count);
  std::string DescribeForTesting() const;

  // tab_organizer::TabOrganizer::Observer:
  void OnBatchDone(const tab_organizer::Batch& batch) override;
  void OnBatchUndone(const tab_organizer::Batch& batch) override;

  // tab_organizer::TabArchive::Observer:
  void OnArchiveChanged() override;

 private:
  void Start(Kind kind,
             const std::string& batch_id,
             std::vector<std::string> ids,
             int count,
             base::Time time);
  // 每半秒看一次窗口是否到了前台。
  void Check();
  void Clear();
  void Notify();

  const raw_ptr<BrowserWindowInterface> browser_;
  Kind kind_ = Kind::kArchive;
  std::string batch_id_;
  // 这一批里这个窗口的标签在归档里的 ID（调试用的假的一批没有）。
  std::vector<std::string> ids_;
  int count_ = 0;
  base::Time time_;
  bool shown_ = false;
  bool held_ = false;
  base::RepeatingTimer check_timer_;
  base::OneShotTimer dismiss_timer_;
  base::RepeatingClosureList changed_;
  base::ScopedObservation<tab_organizer::TabOrganizer,
                          tab_organizer::TabOrganizer::Observer>
      observation_{this};
  base::ScopedObservation<tab_organizer::TabArchive,
                          tab_organizer::TabArchive::Observer>
      archive_observation_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_ARCHIVE_ARCHIVE_NOTICE_H_
