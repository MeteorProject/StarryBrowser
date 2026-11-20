// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_MEDIA_MEDIA_HUB_H_
#define STARRY_BROWSER_MEDIA_MEDIA_HUB_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/containers/flat_set.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/supports_user_data.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/unguessable_token.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/media_session/public/cpp/media_position.h"
#include "services/media_session/public/mojom/audio_focus.mojom.h"
#include "services/media_session/public/mojom/media_controller.mojom.h"
#include "services/media_session/public/mojom/media_session.mojom.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/gfx/image/image_skia.h"

class Profile;

namespace content {
class WebContents;
}

namespace starry {

class MediaHub;
class MediaLevels;

// 有界面在看某个媒体的实时声音：拿着它时那个标签的回环流开着（MediaHub::WatchLevels）。
class MediaLevelWatch {
 public:
  MediaLevelWatch(base::WeakPtr<MediaHub> hub,
                  const base::UnguessableToken& id);
  MediaLevelWatch(const MediaLevelWatch&) = delete;
  MediaLevelWatch& operator=(const MediaLevelWatch&) = delete;
  ~MediaLevelWatch();

  const base::UnguessableToken& id() const { return id_; }

 private:
  base::WeakPtr<MediaHub> hub_;
  base::UnguessableToken id_;
};

// 一个媒体（某个标签里在放、或者暂停着的音视频，对应一个
// MediaSession）的快照。界面 直接读，MediaHub 在内容变了时通知。
struct MediaEntry {
  MediaEntry();
  MediaEntry(const MediaEntry&);
  MediaEntry& operator=(const MediaEntry&);
  ~MediaEntry();

  // 是不是有这个操作（网页没注册的按钮不显示）。
  bool Can(media_session::mojom::MediaSessionAction action) const {
    return actions.contains(action);
  }
  // 现在放到哪里（按播放速度往后推算）；不知道时为空。
  std::optional<base::TimeDelta> CurrentPosition() const;
  // 0…1；不知道总长（直播）时为空。
  std::optional<double> Progress() const;
  // 所在的标签；标签关掉了时为空。
  content::WebContents* GetWebContents() const;

  // Audio focus 的 request id，一个标签一个。
  base::UnguessableToken id;
  // 网页给的标题；网页没给时是标签的标题。
  std::u16string title;
  std::u16string artist;
  std::u16string album;
  // 来源：网页给的 source_title（一般是网站的名字），没有时是域名。
  std::u16string source;
  gfx::ImageSkia artwork;
  gfx::ImageSkia favicon;
  // 封面的主色，用来给卡片着色（同 Starry Player 从封面取色）；没有封面时为空。
  std::optional<SkColor> artwork_color;
  bool playing = false;
  bool muted = false;
  bool in_picture_in_picture = false;
  bool has_video = false;
  std::optional<media_session::MediaPosition> position;
  base::flat_set<media_session::mojom::MediaSessionAction> actions;
  // 上一次开始播放、暂停的时候：排序用，暂停太久的不再显示。
  base::TimeTicks last_active;
};

// 一个用户资料里所有的媒体（同 Chrome
// 全局媒体控件的数据，但不经过它那一层：它只能接 一个界面）。直接接
// media_session 服务：AudioFocusManager 告诉有哪些会话，每个会话 一个
// MediaController 拿标题、封面、进度、可用的操作，也用它发操作。焦点会话（媒体
// 键、系统「正在播放」控制的那个）跟着 CreateActiveMediaController。
//
// 挂在 Profile 上（SupportsUserData），无痕窗口的用户资料单独一个。
class MediaHub : public media_session::mojom::AudioFocusObserver,
                 public base::SupportsUserData::Data {
 public:
  class Observer : public base::CheckedObserver {
   public:
    // 有媒体出现、消失，或者哪一个的内容（标题、播放状态、进度、封面）变了。同一轮里
    // 的多次变化合成一次。
    virtual void OnMediaChanged() = 0;
  };

  // 暂停超过这么久的不再显示（标签里的播放器还在）。
  static constexpr base::TimeDelta kPausedLifetime = base::Minutes(30);

  // 没有时建一个。
  static MediaHub* Get(Profile* profile);

  explicit MediaHub(Profile* profile);
  MediaHub(const MediaHub&) = delete;
  MediaHub& operator=(const MediaHub&) = delete;
  ~MediaHub() override;

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // 要显示的媒体：在放的在前（焦点会话最前），暂停的按最近活动排。只算放出过
  // 声音的（静音的预览、静音自动播放的视频不算，画中画里的除外）。
  std::vector<const MediaEntry*> Entries() const;
  const MediaEntry* Find(const base::UnguessableToken& id) const;
  // 最该显示的那个：Entries() 的第一个。焦点会话只在它在放、或者都没在放时
  // 排第一，暂停着的焦点会话不压过别处正在放的。
  const MediaEntry* Primary() const;
  // 收起后的坞、标签行的胶囊显示的那个：先挑在放、又不在画中画里的（画中画的画面上已经
  // 有它的控件），没有时同 Primary()。
  const MediaEntry* PrimaryOutsidePictureInPicture() const;

  void Perform(const base::UnguessableToken& id,
               media_session::mojom::MediaSessionAction action);
  void TogglePlayPause(const base::UnguessableToken& id);
  void SeekTo(const base::UnguessableToken& id, base::TimeDelta time);
  void SetMuted(const base::UnguessableToken& id, bool muted);
  // 切到这个媒体所在的标签；在别的窗口里时把那个窗口拿到前面。
  void ShowTab(const base::UnguessableToken& id);

  // 实时的声音：拿着返回的对象时开着回环流（没人拿了过 1 秒停）。
  std::unique_ptr<MediaLevelWatch> WatchLevels(
      const base::UnguessableToken& id);
  // 现在的声音；没开着时为空。
  const MediaLevels* Levels(const base::UnguessableToken& id) const;

  // media_session::mojom::AudioFocusObserver:
  void OnFocusGained(
      media_session::mojom::AudioFocusRequestStatePtr state) override;
  void OnFocusLost(
      media_session::mojom::AudioFocusRequestStatePtr state) override;
  void OnRequestIdReleased(const base::UnguessableToken& request_id) override;

 private:
  friend class MediaLevelWatch;
  class Session;
  class ActiveWatcher;

  void Unwatch(const base::UnguessableToken& id);

  void OnInitialRequests(
      std::vector<media_session::mojom::AudioFocusRequestStatePtr> requests);
  void AddSession(const base::UnguessableToken& id);
  void RemoveSession(const base::UnguessableToken& id);
  void SetActive(const std::optional<base::UnguessableToken>& id);
  Session* FindSession(const base::UnguessableToken& id) const;
  // 内容变了：下一轮消息循环通知一次。
  void NotifySoon();
  void Notify();
  // 下一个暂停的媒体到期时再通知一次（让它从列表里消失）。
  void ScheduleExpiry();

  const raw_ptr<Profile> profile_;
  base::UnguessableToken source_id_;
  mojo::Remote<media_session::mojom::AudioFocusManager> audio_focus_;
  mojo::Remote<media_session::mojom::MediaControllerManager>
      controller_manager_;
  mojo::Receiver<media_session::mojom::AudioFocusObserver> observer_receiver_{
      this};
  std::map<base::UnguessableToken, std::unique_ptr<Session>> sessions_;
  std::unique_ptr<ActiveWatcher> active_watcher_;
  std::optional<base::UnguessableToken> active_id_;
  base::ObserverList<Observer> observers_;
  bool notify_pending_ = false;
  base::OneShotTimer expiry_timer_;
  base::WeakPtrFactory<MediaHub> weak_factory_{this};
};

}  // namespace starry

#endif  // STARRY_BROWSER_MEDIA_MEDIA_HUB_H_
