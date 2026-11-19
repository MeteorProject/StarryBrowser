// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/media/media_player_view.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "cc/paint/paint_flags.h"
#include "content/public/browser/media_session.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/media/media_parts.h"
#include "starry/browser/ui/media/media_pip.h"
#include "starry/browser/ui/media/media_waveform.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/button/button.h"

namespace starry {

namespace {

using media_session::mojom::MediaSessionAction;

constexpr int kHeaderHeight = 24;
constexpr int kHeaderGap = 10;
constexpr int kArt = 64;
constexpr float kArtRadius = 12;
constexpr int kArtGap = 12;
constexpr int kTimesHeight = 14;
constexpr int kOthersGap = 6;
constexpr int kRowHeight = 44;
constexpr int kSmallButton = 28;
constexpr int kPlayButton = 44;
constexpr int kSkipButton = 34;
constexpr int kControlSpacing = 14;
// 紧凑的样子里下面那一行按钮之间最多隔这么多（宽的时候不摊开，聚在中间）。
constexpr int kUtilitySpacing = 20;
// 其余的媒体：视频的封面是宽的；一行不到这么宽时和音乐一样用方的，标题多留一点。
constexpr int kWideRowArt = 260;

// 上面一块（不算其余的媒体）里各部分从哪里开始。
struct Metrics {
  // 对称频谱。
  int wave_top;
  int wave_height;
  int seek_top;
  // 上一首、播放暂停、下一首那一行。
  int controls_top;
  int controls_height;
  // 紧凑的样子下面多一行小按钮；完整的样子没有（是 0）。
  int utility_top;
  int main_height;
};

constexpr Metrics MetricsFor(bool compact) {
  Metrics m{};
  if (compact) {
    m.wave_top = 0;
    m.wave_height = 26;
    m.seek_top = m.wave_top + m.wave_height + 6;
    m.controls_top = m.seek_top + MediaSeekBar::kHeight + kTimesHeight + 2;
    m.controls_height = 46;
    m.utility_top = m.controls_top + m.controls_height + 2;
    m.main_height = m.utility_top + kSmallButton;
  } else {
    // 封面下面是对称频谱。
    m.wave_top = kHeaderHeight + kHeaderGap + kArt + 12;
    m.wave_height = 28;
    m.seek_top = m.wave_top + m.wave_height + 8;
    m.controls_top = m.seek_top + MediaSeekBar::kHeight + kTimesHeight + 4;
    m.controls_height = 48;
    m.utility_top = 0;
    m.main_height = m.controls_top + m.controls_height;
  }
  return m;
}

std::u16string Subtitle(const MediaEntry& entry) {
  if (!entry.artist.empty() && !entry.album.empty()) {
    return entry.artist + u" · " + entry.album;
  }
  return entry.artist.empty() ? entry.source : entry.artist;
}

}  // namespace

MediaPlayerView::Options::Options() = default;
MediaPlayerView::Options::Options(const Options&) = default;
MediaPlayerView::Options& MediaPlayerView::Options::operator=(const Options&) =
    default;
MediaPlayerView::Options::~Options() = default;

// 其余的媒体一行一个：封面 32、标题、来源，右边播放暂停。点一行换到上面。
class MediaPlayerView::Row : public views::Button {
  METADATA_HEADER(Row, views::Button)

 public:
  explicit Row(MediaPlayerView* owner)
      : views::Button(
            base::BindRepeating(&Row::Pressed, base::Unretained(this))),
        owner_(owner) {
    SetAnimationDuration(base::Milliseconds(180));
    toggle_ = AddChildView(std::make_unique<MediaButton>(
        base::BindRepeating(
            [](Row* row) {
              row->owner_->hub_->TogglePlayPause(row->entry_.id);
            },
            base::Unretained(this)),
        MediaGlyph::kPause, L("media.pause"), kSmallButton, 11,
        MediaButton::Style::kTonal));
  }

  void Set(const MediaEntry& entry, const Theme& theme) {
    entry_ = entry;
    theme_ = theme;
    toggle_->SetTheme(theme);
    toggle_->SetGlyph(entry.playing ? MediaGlyph::kPause : MediaGlyph::kPlay,
                      L(entry.playing ? "media.pause" : "media.play"));
    GetViewAccessibility().SetName(entry.title);
    SetTooltipText(entry.title);
    SchedulePaint();
  }

  // views::Button:
  void Layout(PassKey) override {
    toggle_->SetBounds(width() - kSmallButton - 4,
                       (height() - kSmallButton) / 2, kSmallButton,
                       kSmallButton);
  }

  void PaintButtonContents(gfx::Canvas* canvas) override {
    const float hover = static_cast<float>(hover_animation().GetCurrentValue());
    if (hover > 0) {
      cc::PaintFlags flags;
      flags.setAntiAlias(true);
      flags.setColor(theme_.OnSurface(0.07f * hover));
      canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()), 10, flags);
    }
    const Theme art_theme = MediaThemeFor(&entry_, theme_);
    const int art_width = entry_.has_video && width() >= kWideRowArt ? 48 : 32;
    PaintArtwork(canvas, gfx::RectF(4, (height() - 32) / 2.0f, art_width, 32),
                 7, entry_, art_theme);
    const int x = 4 + art_width + 10;
    const int right = toggle_->x() - 8;
    DrawMediaText(canvas, entry_.title, 12, /*semibold=*/false,
                  theme_.on_surface, gfx::Rect(x, 6, right - x, 17));
    DrawMediaText(canvas, Subtitle(entry_), 11, false,
                  theme_.on_surface_variant, gfx::Rect(x, 23, right - x, 15));
  }

 private:
  void Pressed() { owner_->Select(entry_.id); }

  const raw_ptr<MediaPlayerView> owner_;
  raw_ptr<MediaButton> toggle_ = nullptr;
  MediaEntry entry_;
  Theme theme_ = Theme::DarkBase();
};

BEGIN_METADATA(MediaPlayerView, Row)
END_METADATA

MediaPlayerView::MediaPlayerView(MediaHub* hub, Options options)
    : hub_(hub), options_(std::move(options)) {
  if (options_.placement_glyph) {
    // 紧凑的样子里它和静音、画中画排在一行，一样大。
    placement_ = AddChildView(std::make_unique<MediaButton>(
        options_.placement, *options_.placement_glyph,
        options_.placement_tooltip, options_.compact ? kSmallButton : 24,
        options_.compact ? 15 : 14));
  }
  if (options_.collapse && !options_.compact) {
    collapse_ = AddChildView(std::make_unique<MediaButton>(
        options_.collapse, MediaGlyph::kCollapse, L("media.collapse"), 24, 14));
  }
  wave_ = AddChildView(
      std::make_unique<MediaWaveform>(hub_, MediaWaveform::Style::kSpectrum));
  wave_->SetBar(2.2f, 1.8f);
  seek_ = AddChildView(std::make_unique<MediaSeekBar>(
      base::BindRepeating(&MediaPlayerView::Seek, base::Unretained(this))));
  mute_ = AddChildView(std::make_unique<MediaButton>(
      base::BindRepeating(
          [](MediaPlayerView* view) {
            if (const MediaEntry* entry = view->Selected()) {
              view->hub_->SetMuted(entry->id, !entry->muted);
            }
          },
          base::Unretained(this)),
      MediaGlyph::kSpeaker, L("tabs.mute"), kSmallButton, 15));
  previous_ = AddChildView(std::make_unique<MediaButton>(
      views::Button::PressedCallback(), MediaGlyph::kPrevious,
      L("media.previous"), kSkipButton, 17));
  play_ = AddChildView(std::make_unique<MediaButton>(
      base::BindRepeating(
          [](MediaPlayerView* view) {
            if (const MediaEntry* entry = view->Selected()) {
              view->hub_->TogglePlayPause(entry->id);
            }
          },
          base::Unretained(this)),
      MediaGlyph::kPause, L("media.pause"), kPlayButton, 17,
      MediaButton::Style::kFilled));
  next_ = AddChildView(std::make_unique<MediaButton>(
      views::Button::PressedCallback(), MediaGlyph::kNext, L("media.next"),
      kSkipButton, 17));
  pip_ = AddChildView(std::make_unique<MediaButton>(
      base::BindRepeating(
          [](MediaPlayerView* view) {
            const MediaEntry* entry = view->Selected();
            if (!entry) {
              return;
            }
            if (entry->in_picture_in_picture) {
              view->Perform(MediaSessionAction::kExitPictureInPicture);
              return;
            }
            // 和切走标签时一样先放到岛上；当前标签里的视频是桌面小窗。
            MediaPip::EnterFromBrowserUi(
                content::MediaSession::GetWebContentsFromRequestId(entry->id));
          },
          base::Unretained(this)),
      MediaGlyph::kPictureInPicture, L("media.pip"), kSmallButton, 16));
  show_tab_ = AddChildView(std::make_unique<MediaButton>(
      base::BindRepeating(
          [](MediaPlayerView* view) {
            if (const MediaEntry* entry = view->Selected()) {
              // 切过去以后岛可能会收起来：不在按钮自己的回调里做。
              base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
                  FROM_HERE, base::BindOnce(&MediaHub::ShowTab,
                                            base::Unretained(view->hub_.get()),
                                            entry->id));
            }
          },
          base::Unretained(this)),
      MediaGlyph::kShowTab, L("media.showTab"), kSmallButton, 15));
  observation_.Observe(hub_);
  Refresh();
}

MediaPlayerView::~MediaPlayerView() = default;

void MediaPlayerView::SetTheme(const Theme& theme) {
  if (window_theme_ == theme) {
    return;
  }
  window_theme_ = theme;
  Refresh();
}

void MediaPlayerView::Select(const base::UnguessableToken& id) {
  const std::optional<base::UnguessableToken> before = selected();
  selected_ = id;
  Refresh();
  if (options_.selection_changed && selected() != before) {
    options_.selection_changed.Run();
  }
}

std::optional<base::UnguessableToken> MediaPlayerView::selected() const {
  const MediaEntry* entry = Selected();
  return entry ? std::optional(entry->id) : std::nullopt;
}

void MediaPlayerView::SetExclude(
    std::optional<base::UnguessableToken> exclude) {
  if (options_.exclude == exclude) {
    return;
  }
  options_.exclude = exclude;
  Refresh();
}

void MediaPlayerView::SetPlacement(MediaGlyph glyph,
                                   const std::u16string& tooltip) {
  if (placement_) {
    placement_->SetGlyph(glyph, tooltip);
  }
}

const MediaEntry* MediaPlayerView::Selected() const {
  if (entries_.empty()) {
    return nullptr;
  }
  if (selected_) {
    for (const MediaEntry& entry : entries_) {
      if (entry.id == *selected_) {
        return &entry;
      }
    }
  }
  return &entries_.front();
}

int MediaPlayerView::ContentHeight() const {
  return HeightForOthers(
      std::min<int>(max_others_, std::max<int>(0, entries_.size() - 1)));
}

int MediaPlayerView::MainHeight() const {
  return MetricsFor(options_.compact).main_height;
}

int MediaPlayerView::HeightForOthers(int others) const {
  return MainHeight() +
         (others > 0 ? kOthersGap * 2 + 1 + others * kRowHeight : 0);
}

double MediaPlayerView::WavePaintRate() {
  return wave_->TakePaintRate();
}

void MediaPlayerView::SetMaxOthers(int max_others) {
  max_others = std::clamp(max_others, 0, kMaxOthers);
  if (max_others_ == max_others) {
    return;
  }
  max_others_ = max_others;
  Refresh();
}

void MediaPlayerView::OnMediaChanged() {
  Refresh();
}

void MediaPlayerView::Refresh() {
  entries_.clear();
  const MediaEntry* primary = hub_->Primary();
  for (const MediaEntry* entry : hub_->Entries()) {
    if (options_.exclude && entry->id == *options_.exclude) {
      continue;
    }
    entries_.push_back(*entry);
  }
  // 没有选过（或者选的那个没了）时跟着焦点会话。
  const bool selected_alive =
      selected_ && std::any_of(entries_.begin(), entries_.end(),
                               [this](const MediaEntry& entry) {
                                 return entry.id == *selected_;
                               });
  if (!selected_alive) {
    selected_.reset();
    if (primary) {
      for (const MediaEntry& entry : entries_) {
        if (entry.id == primary->id) {
          selected_ = entry.id;
        }
      }
    }
  }
  const MediaEntry* entry = Selected();
  media_theme_ = MediaThemeFor(entry, window_theme_);

  for (MediaButton* button :
       {placement_.get(), collapse_.get(), mute_.get(), previous_.get(),
        play_.get(), next_.get(), pip_.get(), show_tab_.get()}) {
    if (button) {
      button->SetTheme(media_theme_);
    }
  }
  if (entry) {
    mute_->SetGlyph(entry->muted ? MediaGlyph::kMuted : MediaGlyph::kSpeaker,
                    L(entry->muted ? "tabs.unmute" : "tabs.mute"));
    play_->SetGlyph(entry->playing ? MediaGlyph::kPause : MediaGlyph::kPlay,
                    L(entry->playing ? "media.pause" : "media.play"));
    const bool track_previous = entry->Can(MediaSessionAction::kPreviousTrack);
    const bool seek_back = entry->Can(MediaSessionAction::kSeekBackward);
    previous_->SetGlyph(
        track_previous ? MediaGlyph::kPrevious : MediaGlyph::kBack10,
        L(track_previous ? "media.previous" : "media.seekBackward"));
    previous_->SetCallback(base::BindRepeating(
        &MediaPlayerView::Perform, base::Unretained(this),
        track_previous ? MediaSessionAction::kPreviousTrack
                       : MediaSessionAction::kSeekBackward));
    previous_->SetEnabled(track_previous || seek_back);
    const bool track_next = entry->Can(MediaSessionAction::kNextTrack);
    const bool seek_forward = entry->Can(MediaSessionAction::kSeekForward);
    next_->SetGlyph(track_next ? MediaGlyph::kNext : MediaGlyph::kForward10,
                    L(track_next ? "media.next" : "media.seekForward"));
    next_->SetCallback(
        base::BindRepeating(&MediaPlayerView::Perform, base::Unretained(this),
                            track_next ? MediaSessionAction::kNextTrack
                                       : MediaSessionAction::kSeekForward));
    next_->SetEnabled(track_next || seek_forward);
    // 按钮说清楚画面会去哪：放到岛上是「画中画」，只能去桌面时是「弹到桌面」。
    const bool desktop =
        !entry->in_picture_in_picture &&
        !MediaPip::CanShowInline(
            content::MediaSession::GetWebContentsFromRequestId(entry->id));
    pip_->SetGlyph(entry->in_picture_in_picture
                       ? MediaGlyph::kExitPictureInPicture
                   : desktop ? MediaGlyph::kPopOut
                             : MediaGlyph::kPictureInPicture,
                   L(entry->in_picture_in_picture ? "media.pipExit"
                     : desktop                    ? "media.popOut"
                                                  : "media.pip"));
    pip_->SetVisible(entry->has_video &&
                     (entry->in_picture_in_picture ||
                      entry->Can(MediaSessionAction::kEnterPictureInPicture)));
    seek_->Set(entry->Progress(), media_theme_.accent, media_theme_);
    seek_->SetEnabled(entry->Can(MediaSessionAction::kSeekTo));
    wave_->Set(*entry, media_theme_.accent, media_theme_.primary,
               media_theme_.OnSurface(0.25f));
  }

  // 其余的媒体。
  std::vector<const MediaEntry*> others;
  for (const MediaEntry& other : entries_) {
    if (!entry || other.id != entry->id) {
      others.push_back(&other);
    }
  }
  if (others.size() > static_cast<size_t>(max_others_)) {
    others.resize(max_others_);
  }
  while (rows_.size() < others.size()) {
    rows_.push_back(AddChildView(std::make_unique<Row>(this)));
  }
  while (rows_.size() > others.size()) {
    Row* row = rows_.back();
    rows_.pop_back();
    RemoveChildViewT(row);
  }
  for (size_t i = 0; i < others.size(); ++i) {
    rows_[i]->Set(*others[i], window_theme_);
  }

  UpdateTicker();
  const int height = ContentHeight();
  if (height != last_height_) {
    last_height_ = height;
    PreferredSizeChanged();
    if (options_.size_changed) {
      options_.size_changed.Run();
    }
  }
  InvalidateLayout();
  SchedulePaint();
}

void MediaPlayerView::UpdateTicker() {
  const MediaEntry* entry = Selected();
  const bool wants =
      GetVisible() && entry && entry->playing && entry->position.has_value();
  if (!wants) {
    ticker_.Stop();
    return;
  }
  if (!ticker_.IsRunning()) {
    ticker_.Start(FROM_HERE, base::Milliseconds(250),
                  base::BindRepeating(
                      [](MediaPlayerView* view) {
                        const MediaEntry* current = view->Selected();
                        if (current && !view->seek_->dragging()) {
                          view->seek_->Set(current->Progress(),
                                           view->media_theme_.accent,
                                           view->media_theme_);
                        }
                        view->SchedulePaint();
                      },
                      base::Unretained(this)));
  }
}

void MediaPlayerView::Seek(double fraction) {
  const MediaEntry* entry = Selected();
  if (!entry || !entry->position) {
    return;
  }
  hub_->SeekTo(entry->id, entry->position->duration() * fraction);
}

void MediaPlayerView::Perform(MediaSessionAction action) {
  if (const MediaEntry* entry = Selected()) {
    hub_->Perform(entry->id, action);
  }
}

gfx::Size MediaPlayerView::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  return gfx::Size(300, ContentHeight());
}

void MediaPlayerView::Layout(PassKey) {
  const int w = width();
  const Metrics m = MetricsFor(options_.compact);
  wave_->SetBounds(0, m.wave_top, w, m.wave_height);
  seek_->SetBounds(0, m.seek_top, w, MediaSeekBar::kHeight);
  const int cy = m.controls_top + m.controls_height / 2;
  // 三个大按钮在中间；窄的时候挨近一点，两边不出去。
  const int spacing = std::clamp((w - kPlayButton - 2 * kSkipButton) / 2 - 2, 4,
                                 kControlSpacing);
  play_->SetBounds(w / 2 - kPlayButton / 2, cy - kPlayButton / 2, kPlayButton,
                   kPlayButton);
  previous_->SetBounds(play_->x() - spacing - kSkipButton, cy - kSkipButton / 2,
                       kSkipButton, kSkipButton);
  next_->SetBounds(play_->bounds().right() + spacing, cy - kSkipButton / 2,
                   kSkipButton, kSkipButton);
  if (options_.compact) {
    // 下面一行小按钮，聚在中间；放不开时挨近。
    std::vector<MediaButton*> buttons = {mute_.get()};
    if (pip_->GetVisible()) {
      buttons.push_back(pip_.get());
    }
    buttons.push_back(show_tab_.get());
    if (placement_) {
      buttons.push_back(placement_.get());
    }
    const int n = static_cast<int>(buttons.size());
    const int gap =
        n > 1 ? std::clamp((w - n * kSmallButton) / (n - 1), 0, kUtilitySpacing)
              : 0;
    int x = (w - n * kSmallButton - (n - 1) * gap) / 2;
    for (MediaButton* button : buttons) {
      button->SetBounds(x, m.utility_top, kSmallButton, kSmallButton);
      x += kSmallButton + gap;
    }
  } else {
    int header_right = w;
    if (collapse_) {
      collapse_->SetBounds(header_right - 24, 0, 24, 24);
      header_right -= 28;
    }
    if (placement_) {
      placement_->SetBounds(header_right - 24, 0, 24, 24);
    }
    mute_->SetBounds(-4, cy - kSmallButton / 2, kSmallButton, kSmallButton);
    show_tab_->SetBounds(w - kSmallButton + 4, cy - kSmallButton / 2,
                         kSmallButton, kSmallButton);
    pip_->SetBounds(show_tab_->x() - kSmallButton, cy - kSmallButton / 2,
                    kSmallButton, kSmallButton);
  }
  int y = m.main_height + kOthersGap * 2 + 1;
  for (Row* row : rows_) {
    row->SetBounds(-4, y, w + 8, kRowHeight);
    y += kRowHeight;
  }
}

void MediaPlayerView::OnPaint(gfx::Canvas* canvas) {
  const MediaEntry* entry = Selected();
  if (!entry) {
    return;
  }
  const int w = width();
  const Theme& theme = media_theme_;
  const Metrics m = MetricsFor(options_.compact);
  if (!options_.compact) {
    PaintHeader(canvas, *entry);
  }

  // 时间：左边放到哪，右边还剩多少；直播只写「直播」。
  const int times_y = m.seek_top + MediaSeekBar::kHeight;
  const std::optional<base::TimeDelta> position = entry->CurrentPosition();
  if (entry->Progress()) {
    const base::TimeDelta duration = entry->position->duration();
    DrawMediaText(canvas, FormatMediaTime(*position), 10, false,
                  theme.on_surface_variant, gfx::Rect(0, times_y, w / 2, 14));
    DrawMediaText(canvas, u"-" + FormatMediaTime(duration - *position), 10,
                  false, theme.on_surface_variant,
                  gfx::Rect(w / 2, times_y, w / 2, 14), /*align_right=*/true);
  } else if (position) {
    DrawMediaText(canvas, L("media.live"), 10, true, theme.danger(),
                  gfx::Rect(0, times_y, w / 2, 14));
  }

  // 其余的媒体上面一条细线。
  if (!rows_.empty()) {
    cc::PaintFlags line;
    line.setColor(theme.OnSurface(0.08f));
    canvas->DrawRect(gfx::RectF(0, m.main_height + kOthersGap, w, 1), line);
  }
}

void MediaPlayerView::PaintHeader(gfx::Canvas* canvas,
                                  const MediaEntry& selected) {
  const MediaEntry* entry = &selected;
  const int w = width();
  const Theme& theme = media_theme_;
  // 标题行：「正在播放」和个数。
  const std::u16string header = L("media.nowPlaying");
  const gfx::FontList& header_font = UiFont(12, gfx::Font::Weight::SEMIBOLD);
  const int header_width = gfx::GetStringWidth(header, header_font);
  canvas->DrawStringRectWithFlags(
      header, header_font, theme.on_surface_variant,
      gfx::Rect(0, 0, header_width + 2, kHeaderHeight),
      gfx::Canvas::NO_SUBPIXEL_RENDERING);
  if (entries_.size() > 1) {
    const std::u16string count = base::NumberToString16(entries_.size());
    const gfx::FontList& count_font = UiFont(10, gfx::Font::Weight::SEMIBOLD);
    const int count_width =
        std::max(16, gfx::GetStringWidth(count, count_font) + 10);
    const gfx::Rect chip(header_width + 8, (kHeaderHeight - 16) / 2,
                         count_width, 16);
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(theme.OnSurface(0.12f));
    canvas->DrawRoundRect(gfx::RectF(chip), 8, flags);
    canvas->DrawStringRectWithFlags(
        count, count_font, theme.on_surface, chip,
        gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

  // 封面和文字。
  const int top = kHeaderHeight + kHeaderGap;
  PaintArtwork(canvas, gfx::RectF(0, top, kArt, kArt), kArtRadius, *entry,
               theme);
  const int x = kArt + kArtGap;
  DrawMediaText(canvas, entry->title, 15, /*semibold=*/true, theme.on_surface,
                gfx::Rect(x, top + 4, w - x, 20));
  DrawMediaText(canvas, Subtitle(*entry), 12, false, theme.on_surface_variant,
                gfx::Rect(x, top + 26, w - x, 16));
  int source_x = x;
  if (!entry->favicon.isNull()) {
    canvas->DrawImageInt(entry->favicon, 0, 0, entry->favicon.width(),
                         entry->favicon.height(), x, top + 47, 12, 12, true);
    source_x += 17;
  }
  DrawMediaText(canvas, entry->source, 11, false, theme.on_surface_variant,
                gfx::Rect(source_x, top + 45, w - source_x, 15));
}

void MediaPlayerView::VisibilityChanged(views::View* starting_from,
                                        bool is_visible) {
  UpdateTicker();
}

BEGIN_METADATA(MediaPlayerView)
END_METADATA

}  // namespace starry
