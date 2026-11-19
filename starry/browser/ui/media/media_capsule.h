// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_MEDIA_MEDIA_CAPSULE_H_
#define STARRY_BROWSER_UI_MEDIA_MEDIA_CAPSULE_H_

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/timer/timer.h"
#include "components/prefs/pref_change_registrar.h"
#include "starry/browser/media/media_hub.h"
#include "starry/browser/ui/design/theme.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/controls/button/button.h"

class BrowserWindowInterface;

namespace starry {

class MediaWaveform;

// 顶部标签布局下，媒体岛收起后停靠在标签行右端（助手按钮左侧）的胶囊：展示封面
// （外圈进度）、标题与媒体数量。点击向下弹出播放器（MediaPlayerView）。
class MediaCapsule : public views::Button, public MediaHub::Observer {
  METADATA_HEADER(MediaCapsule, views::Button)

 public:
  static constexpr int kWidth = 200;
  static constexpr int kHeight = 30;

  explicit MediaCapsule(BrowserWindowInterface* browser);
  MediaCapsule(const MediaCapsule&) = delete;
  MediaCapsule& operator=(const MediaCapsule&) = delete;
  ~MediaCapsule() override;

  void SetTheme(const Theme& theme);
  // 顶部标签布局时才有。
  void SetStripActive(bool active);
  // 该不该占位置；变了时调用 presence_changed（标签行重新排）。
  bool ShouldShow() const { return should_show_; }
  void SetPresenceChangedCallback(base::RepeatingClosure callback);
  // 打开下面的播放器（调试步骤也用）。
  void ShowPlayer();
  // 媒体岛飞到了：淡入（没有飞过来的窗口等一会儿自己淡入）。
  void Reveal();

  // MediaHub::Observer:
  void OnMediaChanged() override;

  // views::Button:
  void Layout(PassKey) override;
  void PaintButtonContents(gfx::Canvas* canvas) override;

 private:
  void Update();
  void OnDockedChanged();

  const raw_ptr<BrowserWindowInterface> browser_;
  const raw_ptr<MediaHub> hub_;
  Theme theme_ = Theme::DarkBase();
  Theme media_theme_ = Theme::DarkBase();
  MediaEntry entry_;
  bool has_entry_ = false;
  size_t count_ = 0;
  bool strip_active_ = false;
  bool should_show_ = false;
  base::RepeatingClosure presence_changed_;
  raw_ptr<MediaWaveform> meter_ = nullptr;
  LayerMotion fade_;
  base::OneShotTimer reveal_timer_;
  base::RepeatingTimer ticker_;
  PrefChangeRegistrar prefs_;
  base::ScopedObservation<MediaHub, MediaHub::Observer> observation_{this};
  base::WeakPtrFactory<MediaCapsule> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_UI_MEDIA_MEDIA_CAPSULE_H_
