// Copyright 2026 The Starry Browser Authors

#include "starry/browser/media/media_hub.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/media_session.h"
#include "content/public/browser/media_session_service.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "services/media_session/public/cpp/media_metadata.h"
#include "services/media_session/public/cpp/media_session_service.h"
#include "services/media_session/public/cpp/util.h"
#include "starry/browser/media/media_levels.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/base_window.h"
#include "ui/gfx/color_analysis.h"
#include "url/gurl.h"

namespace starry {

namespace {

const char kUserDataKey[] = "starry_media_hub";

using media_session::mojom::MediaAudioVideoState;
using media_session::mojom::MediaPictureInPictureState;
using media_session::mojom::MediaPlaybackState;
using media_session::mojom::MediaSessionAction;
using media_session::mojom::MediaSessionImageType;

// 封面：至少 64，要 512（卡片上最大 72 pt，2 倍屏够用）。网站图标 16 起。
constexpr int kArtworkMinSize = 64;
constexpr int kArtworkDesiredSize = 512;
constexpr int kFaviconMinSize = 16;
constexpr int kFaviconDesiredSize = 64;
// 失去焦点后等这么久才拿掉：网页换下一个视频时会先失去、再拿回焦点，不闪一下。
constexpr base::TimeDelta kLostGrace = base::Seconds(2);
// 没人看了过这么久才关回环流：展开、收起、换位置时不反复开关。
constexpr base::TimeDelta kLevelsLinger = base::Seconds(1);

}  // namespace

MediaLevelWatch::MediaLevelWatch(base::WeakPtr<MediaHub> hub,
                                 const base::UnguessableToken& id)
    : hub_(std::move(hub)), id_(id) {}

MediaLevelWatch::~MediaLevelWatch() {
  if (hub_) {
    hub_->Unwatch(id_);
  }
}

MediaEntry::MediaEntry() = default;
MediaEntry::MediaEntry(const MediaEntry&) = default;
MediaEntry& MediaEntry::operator=(const MediaEntry&) = default;
MediaEntry::~MediaEntry() = default;

std::optional<base::TimeDelta> MediaEntry::CurrentPosition() const {
  if (!position) {
    return std::nullopt;
  }
  return position->GetPosition();
}

std::optional<double> MediaEntry::Progress() const {
  if (!position || position->duration().is_zero() ||
      position->duration().is_max()) {
    return std::nullopt;
  }
  return std::clamp(position->GetPosition() / position->duration(), 0.0, 1.0);
}

content::WebContents* MediaEntry::GetWebContents() const {
  return content::MediaSession::GetWebContentsFromRequestId(id);
}

// 一个会话：拿它的 MediaController，听内容和封面的变化；也看着它的标签有没有
// 出声（WebContentsObserver）。
class MediaHub::Session
    : public media_session::mojom::MediaControllerObserver,
      public media_session::mojom::MediaControllerImageObserver,
      public content::WebContentsObserver {
 public:
  Session(MediaHub* hub,
          const base::UnguessableToken& id,
          mojo::Remote<media_session::mojom::MediaController> controller)
      : content::WebContentsObserver(
            content::MediaSession::GetWebContentsFromRequestId(id)),
        hub_(hub) {
    entry_.id = id;
    entry_.last_active = base::TimeTicks::Now();
    Bind(std::move(controller));
    RefreshText();
  }
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  ~Session() override = default;

  // 换一个 MediaController 重新听。焦点丢了又拿回来时要换：原来那个归
  // AudioFocusManager 焦点栈里的那一行所有，失去焦点时跟着那一行一起没了，
  // 留着它就再也收不到这个标签的标题、播放状态，操作也发不过去（YouTube
  // 换广告、换下一个视频、悬停预览时都会这样丢一下焦点）。
  void Bind(mojo::Remote<media_session::mojom::MediaController> controller) {
    observer_receiver_.reset();
    artwork_receiver_.reset();
    favicon_receiver_.reset();
    controller_ = std::move(controller);
    controller_->AddObserver(observer_receiver_.BindNewPipeAndPassRemote());
    controller_->ObserveImages(MediaSessionImageType::kArtwork, kArtworkMinSize,
                               kArtworkDesiredSize,
                               artwork_receiver_.BindNewPipeAndPassRemote());
    controller_->ObserveImages(MediaSessionImageType::kSourceIcon,
                               kFaviconMinSize, kFaviconDesiredSize,
                               favicon_receiver_.BindNewPipeAndPassRemote());
  }

  const MediaEntry& entry() const { return entry_; }
  const mojo::Remote<media_session::mojom::MediaController>& controller()
      const {
    return controller_;
  }

  // 列表里显示不显示：能控制、有东西可写、放出过声音（画中画里的除外），暂停着的
  // 不能太久。
  bool Visible(base::TimeTicks now) const {
    if (!controllable_ || lost_ || entry_.title.empty()) {
      return false;
    }
    if (!heard_ && !entry_.in_picture_in_picture) {
      return false;
    }
    return entry_.playing || now - entry_.last_active < kPausedLifetime;
  }

  void SetLost(bool lost) { lost_ = lost; }
  bool lost() const { return lost_; }

  // 实时的声音：有人看着时开着。
  void AddWatcher() {
    ++watchers_;
    stop_timer_.Stop();
    EnsureLevels();
  }
  void RemoveWatcher() {
    if (--watchers_ > 0) {
      return;
    }
    watchers_ = 0;
    stop_timer_.Start(
        FROM_HERE, kLevelsLinger,
        base::BindOnce(&Session::StopLevels, base::Unretained(this)));
  }
  const MediaLevels* levels() const { return levels_.get(); }

  // 网页没给的字用标签的标题、域名补上（标签的标题会变，每次通知前重新取）。
  void RefreshText() {
    content::WebContents* contents = entry_.GetWebContents();
    const std::u16string host =
        contents ? base::UTF8ToUTF16(contents->GetLastCommittedURL().host())
                 : std::u16string();
    if (hide_metadata_) {
      entry_.title = host;
      entry_.artist.clear();
      entry_.album.clear();
      entry_.source = host;
      return;
    }
    entry_.title = metadata_.title;
    if (entry_.title.empty() && contents) {
      entry_.title = contents->GetTitle();
    }
    entry_.artist = metadata_.artist;
    entry_.album = metadata_.album;
    entry_.source =
        metadata_.source_title.empty() ? host : metadata_.source_title;
  }

  // media_session::mojom::MediaControllerObserver:
  void MediaSessionInfoChanged(
      media_session::mojom::MediaSessionInfoPtr info) override {
    if (!info) {
      controllable_ = false;
      hub_->NotifySoon();
      return;
    }
    const bool playing = info->playback_state == MediaPlaybackState::kPlaying;
    if (playing != entry_.playing) {
      entry_.last_active = base::TimeTicks::Now();
    }
    entry_.playing = playing;
    controllable_ = info->is_controllable;
    hide_metadata_ = info->hide_metadata;
    entry_.muted = info->muted;
    entry_.in_picture_in_picture =
        info->picture_in_picture_state ==
        MediaPictureInPictureState::kInPictureInPicture;
    entry_.has_video = false;
    if (info->audio_video_states) {
      for (MediaAudioVideoState state : *info->audio_video_states) {
        if (state == MediaAudioVideoState::kAudioVideo ||
            state == MediaAudioVideoState::kVideoOnly) {
          entry_.has_video = true;
        }
      }
    }
    RefreshText();
    MaybeHeard();
    hub_->NotifySoon();
  }

  void MediaSessionMetadataChanged(
      const std::optional<media_session::MediaMetadata>& metadata) override {
    metadata_ = metadata.value_or(media_session::MediaMetadata());
    RefreshText();
    hub_->NotifySoon();
  }

  void MediaSessionActionsChanged(
      const std::vector<MediaSessionAction>& actions) override {
    entry_.actions =
        base::flat_set<MediaSessionAction>(actions.begin(), actions.end());
    hub_->NotifySoon();
  }

  void MediaSessionChanged(
      const std::optional<base::UnguessableToken>& request_id) override {}

  void MediaSessionPositionChanged(
      const std::optional<media_session::MediaPosition>& position) override {
    entry_.position = position;
    hub_->NotifySoon();
  }

  // media_session::mojom::MediaControllerImageObserver:
  void MediaControllerImageChanged(MediaSessionImageType type,
                                   const SkBitmap& bitmap) override {
    const bool empty = bitmap.drawsNothing();
    const gfx::ImageSkia image =
        empty ? gfx::ImageSkia() : gfx::ImageSkia::CreateFrom1xBitmap(bitmap);
    if (type == MediaSessionImageType::kArtwork) {
      entry_.artwork = image;
      entry_.artwork_color =
          empty ? std::nullopt
                : std::optional<SkColor>(
                      color_utils::CalculateKMeanColorOfBitmap(bitmap));
    } else {
      entry_.favicon = image;
    }
    hub_->NotifySoon();
  }

  void MediaControllerChapterImageChanged(int chapter_index,
                                          const SkBitmap& bitmap) override {}

  // content::WebContentsObserver:
  void OnAudioStateChanged(bool audible) override {
    if (MaybeHeard()) {
      hub_->NotifySoon();
    }
  }

 private:
  // 在放、标签又在出声（标签上的小喇叭，按真实的输出电平）：记下这个会话放出过
  // 声音。静音的视频也要音频焦点（Chrome 只看标签有没有静音），比如 YouTube
  // 鼠标停在推荐上时的静音预览、网页里静音自动播放的视频，它们从没出过声，不当作
  // 在放的媒体。之后再静音（岛上的静音按钮）的照样显示。
  bool MaybeHeard() {
    if (heard_ || !entry_.playing || !web_contents() ||
        !web_contents()->IsCurrentlyAudible()) {
      return false;
    }
    heard_ = true;
    return true;
  }

  void EnsureLevels() {
    if (levels_ || watchers_ == 0) {
      return;
    }
    content::WebContents* contents = entry_.GetWebContents();
    if (!contents) {
      return;
    }
    levels_ = std::make_unique<MediaLevels>(contents, base::RepeatingClosure());
  }

  void StopLevels() { levels_.reset(); }

  const raw_ptr<MediaHub> hub_;
  MediaEntry entry_;
  media_session::MediaMetadata metadata_;
  bool controllable_ = false;
  bool hide_metadata_ = false;
  bool lost_ = false;
  bool heard_ = false;
  int watchers_ = 0;
  std::unique_ptr<MediaLevels> levels_;
  base::OneShotTimer stop_timer_;
  mojo::Remote<media_session::mojom::MediaController> controller_;
  mojo::Receiver<media_session::mojom::MediaControllerObserver>
      observer_receiver_{this};
  mojo::Receiver<media_session::mojom::MediaControllerImageObserver>
      artwork_receiver_{this};
  mojo::Receiver<media_session::mojom::MediaControllerImageObserver>
      favicon_receiver_{this};
};

// 跟着焦点会话（媒体键、系统的「正在播放」控制的那个）。
class MediaHub::ActiveWatcher
    : public media_session::mojom::MediaControllerObserver {
 public:
  ActiveWatcher(MediaHub* hub,
                media_session::mojom::MediaControllerManager* manager)
      : hub_(hub) {
    manager->CreateActiveMediaController(
        controller_.BindNewPipeAndPassReceiver());
    controller_->AddObserver(receiver_.BindNewPipeAndPassRemote());
  }

  // media_session::mojom::MediaControllerObserver:
  void MediaSessionInfoChanged(
      media_session::mojom::MediaSessionInfoPtr info) override {}
  void MediaSessionMetadataChanged(
      const std::optional<media_session::MediaMetadata>& metadata) override {}
  void MediaSessionActionsChanged(
      const std::vector<MediaSessionAction>& actions) override {}
  void MediaSessionChanged(
      const std::optional<base::UnguessableToken>& request_id) override {
    hub_->SetActive(request_id);
  }
  void MediaSessionPositionChanged(
      const std::optional<media_session::MediaPosition>& position) override {}

 private:
  const raw_ptr<MediaHub> hub_;
  mojo::Remote<media_session::mojom::MediaController> controller_;
  mojo::Receiver<media_session::mojom::MediaControllerObserver> receiver_{this};
};

// static
MediaHub* MediaHub::Get(Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  auto* hub = static_cast<MediaHub*>(profile->GetUserData(kUserDataKey));
  if (!hub) {
    auto owned = std::make_unique<MediaHub>(profile);
    hub = owned.get();
    profile->SetUserData(kUserDataKey, std::move(owned));
  }
  return hub;
}

MediaHub::MediaHub(Profile* profile)
    : profile_(profile),
      source_id_(content::MediaSession::GetSourceId(profile)) {
  media_session::MediaSessionService& service =
      content::GetMediaSessionService();
  service.BindAudioFocusManager(audio_focus_.BindNewPipeAndPassReceiver());
  service.BindMediaControllerManager(
      controller_manager_.BindNewPipeAndPassReceiver());
  audio_focus_->AddSourceObserver(
      source_id_, observer_receiver_.BindNewPipeAndPassRemote());
  audio_focus_->GetSourceFocusRequests(
      source_id_,
      base::BindOnce(&MediaHub::OnInitialRequests, weak_factory_.GetWeakPtr()));
  active_watcher_ =
      std::make_unique<ActiveWatcher>(this, controller_manager_.get());
}

MediaHub::~MediaHub() = default;

void MediaHub::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void MediaHub::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

std::vector<const MediaEntry*> MediaHub::Entries() const {
  const base::TimeTicks now = base::TimeTicks::Now();
  std::vector<const MediaEntry*> entries;
  for (const auto& [id, session] : sessions_) {
    if (session->Visible(now)) {
      entries.push_back(&session->entry());
    }
  }
  std::stable_sort(entries.begin(), entries.end(),
                   [this](const MediaEntry* a, const MediaEntry* b) {
                     if (a->playing != b->playing) {
                       return a->playing;
                     }
                     const bool a_active = active_id_ && a->id == *active_id_;
                     const bool b_active = active_id_ && b->id == *active_id_;
                     if (a_active != b_active) {
                       return a_active;
                     }
                     return a->last_active > b->last_active;
                   });
  return entries;
}

const MediaEntry* MediaHub::Find(const base::UnguessableToken& id) const {
  Session* session = FindSession(id);
  return session && session->Visible(base::TimeTicks::Now()) ? &session->entry()
                                                             : nullptr;
}

const MediaEntry* MediaHub::Primary() const {
  // Entries() 的第一个：在放的里焦点会话优先，都没在放时焦点会话优先。不直接
  // 拿焦点会话：暂停着的它（比如刚才悬停过的预览）不能压过别的标签里正在放的歌。
  const std::vector<const MediaEntry*> entries = Entries();
  return entries.empty() ? nullptr : entries.front();
}

const MediaEntry* MediaHub::PrimaryOutsidePictureInPicture() const {
  const std::vector<const MediaEntry*> entries = Entries();
  for (const MediaEntry* entry : entries) {
    if (entry->playing && !entry->in_picture_in_picture) {
      return entry;
    }
  }
  return entries.empty() ? nullptr : entries.front();
}

void MediaHub::Perform(const base::UnguessableToken& id,
                       MediaSessionAction action) {
  if (Session* session = FindSession(id)) {
    media_session::PerformMediaSessionAction(action, session->controller());
  }
}

void MediaHub::TogglePlayPause(const base::UnguessableToken& id) {
  Session* session = FindSession(id);
  if (!session) {
    return;
  }
  if (session->entry().playing) {
    session->controller()->Suspend();
  } else {
    session->controller()->Resume();
  }
}

void MediaHub::SeekTo(const base::UnguessableToken& id, base::TimeDelta time) {
  if (Session* session = FindSession(id)) {
    session->controller()->SeekTo(time);
  }
}

void MediaHub::SetMuted(const base::UnguessableToken& id, bool muted) {
  if (Session* session = FindSession(id)) {
    session->controller()->SetMute(muted);
  }
}

void MediaHub::ShowTab(const base::UnguessableToken& id) {
  content::WebContents* contents =
      content::MediaSession::GetWebContentsFromRequestId(id);
  tabs::TabInterface* tab =
      contents ? tabs::TabInterface::MaybeGetFromContents(contents) : nullptr;
  BrowserWindowInterface* browser =
      tab ? tab->GetBrowserWindowInterface() : nullptr;
  if (!browser) {
    return;
  }
  TabStripModel* model = browser->GetTabStripModel();
  const int index = model ? model->GetIndexOfTab(tab) : TabStripModel::kNoTab;
  if (index != TabStripModel::kNoTab) {
    model->ActivateTabAt(index);
  }
  if (ui::BaseWindow* window = browser->GetWindow()) {
    if (window->IsMinimized()) {
      window->Restore();
    }
    window->Show();
    window->Activate();
  }
}

std::unique_ptr<MediaLevelWatch> MediaHub::WatchLevels(
    const base::UnguessableToken& id) {
  Session* session = FindSession(id);
  if (!session) {
    return nullptr;
  }
  session->AddWatcher();
  return std::make_unique<MediaLevelWatch>(weak_factory_.GetWeakPtr(), id);
}

void MediaHub::Unwatch(const base::UnguessableToken& id) {
  if (Session* session = FindSession(id)) {
    session->RemoveWatcher();
  }
}

const MediaLevels* MediaHub::Levels(const base::UnguessableToken& id) const {
  Session* session = FindSession(id);
  return session ? session->levels() : nullptr;
}

void MediaHub::OnFocusGained(
    media_session::mojom::AudioFocusRequestStatePtr state) {
  if (state && state->request_id) {
    AddSession(*state->request_id);
  }
}

void MediaHub::OnFocusLost(
    media_session::mojom::AudioFocusRequestStatePtr state) {
  if (!state || !state->request_id) {
    return;
  }
  Session* session = FindSession(*state->request_id);
  if (!session) {
    return;
  }
  session->SetLost(true);
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<MediaHub> hub, base::UnguessableToken id) {
            if (!hub) {
              return;
            }
            Session* lost = hub->FindSession(id);
            if (lost && lost->lost()) {
              hub->RemoveSession(id);
            }
          },
          weak_factory_.GetWeakPtr(), *state->request_id),
      kLostGrace);
}

void MediaHub::OnRequestIdReleased(const base::UnguessableToken& request_id) {
  RemoveSession(request_id);
}

void MediaHub::OnInitialRequests(
    std::vector<media_session::mojom::AudioFocusRequestStatePtr> requests) {
  for (const auto& state : requests) {
    if (state && state->request_id) {
      AddSession(*state->request_id);
    }
  }
}

void MediaHub::AddSession(const base::UnguessableToken& id) {
  Session* session = FindSession(id);
  if (session && !session->lost() && session->controller().is_connected()) {
    return;
  }
  mojo::Remote<media_session::mojom::MediaController> controller;
  controller_manager_->CreateMediaControllerForSession(
      controller.BindNewPipeAndPassReceiver(), id);
  if (session) {
    // 失去焦点后又拿回来（还在等着拿掉的那 2 秒里）：留着会话的状态，换新的
    // MediaController（见 Session::Bind）。
    session->SetLost(false);
    session->Bind(std::move(controller));
  } else {
    sessions_[id] = std::make_unique<Session>(this, id, std::move(controller));
  }
  NotifySoon();
}

void MediaHub::RemoveSession(const base::UnguessableToken& id) {
  if (sessions_.erase(id)) {
    NotifySoon();
  }
}

void MediaHub::SetActive(const std::optional<base::UnguessableToken>& id) {
  if (active_id_ == id) {
    return;
  }
  active_id_ = id;
  NotifySoon();
}

MediaHub::Session* MediaHub::FindSession(
    const base::UnguessableToken& id) const {
  const auto it = sessions_.find(id);
  return it == sessions_.end() ? nullptr : it->second.get();
}

void MediaHub::NotifySoon() {
  if (notify_pending_) {
    return;
  }
  notify_pending_ = true;
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&MediaHub::Notify, weak_factory_.GetWeakPtr()));
}

void MediaHub::Notify() {
  notify_pending_ = false;
  for (auto& [id, session] : sessions_) {
    session->RefreshText();
  }
  ScheduleExpiry();
  for (Observer& observer : observers_) {
    observer.OnMediaChanged();
  }
}

void MediaHub::ScheduleExpiry() {
  const base::TimeTicks now = base::TimeTicks::Now();
  std::optional<base::TimeTicks> next;
  for (const auto& [id, session] : sessions_) {
    const MediaEntry& entry = session->entry();
    if (entry.playing || !session->Visible(now)) {
      continue;
    }
    const base::TimeTicks expiry = entry.last_active + kPausedLifetime;
    if (!next || expiry < *next) {
      next = expiry;
    }
  }
  if (!next) {
    expiry_timer_.Stop();
    return;
  }
  expiry_timer_.Start(
      FROM_HERE, *next - now + base::Milliseconds(10),
      base::BindOnce(&MediaHub::NotifySoon, weak_factory_.GetWeakPtr()));
}

}  // namespace starry
