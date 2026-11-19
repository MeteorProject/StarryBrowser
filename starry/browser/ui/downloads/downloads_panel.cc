// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/downloads/downloads_panel.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/scoped_observation.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/cancelable_task_tracker.h"
#include "base/task/current_thread.h"
#include "build/build_config.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/download/download_prefs.h"
#include "chrome/browser/icon_manager.h"
#include "chrome/browser/platform_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/singleton_tabs.h"
#include "chrome/common/webui_url_constants.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "starry/browser/downloads/download.h"
#include "starry/browser/downloads/download_info.h"
#include "starry/browser/downloads/download_service.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/design/controls.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/downloads/download_progress_bar.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/glass/glass_view.h"
#include "starry/browser/ui/menu/menu_host_view.h"
#include "starry/browser/ui/motion/layer_motion.h"
#include "starry/browser/ui/motion/motion.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/image_model.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "ui/color/color_id.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/image/image.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/text_elider.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/event_monitor.h"
#include "ui/views/view_targeter.h"
#include "ui/views/widget/widget.h"

namespace starry {

namespace {

constexpr int kCardWidth = 360;
constexpr int kHeaderHeight = 46;
constexpr int kFooterHeight = 36;
constexpr int kListInset = 6;
constexpr int kRowSpacing = 2;
constexpr int kRowHeight = 50;
constexpr int kMaxListHeight = 380;
constexpr int kEmptyHeight = 110;
constexpr int kAnchorGap = 8;
constexpr int kWindowInset = 8;
constexpr size_t kLimit = 30;

enum Command {
  kOpen = 1,
  kShowInFolder,
  kPause,
  kResume,
  kCancel,
  kRetry,
  kCopyLink,
  kOpenReferrer,
  kRemove,
  kKeep,
  kDiscard,
};

DownloadService* ServiceFor(BrowserWindowInterface* browser) {
  return DownloadService::Get(browser->GetProfile());
}

content::WebContents* ActiveContents(BrowserWindowInterface* browser) {
  tabs::TabInterface* tab = browser->GetActiveTabInterface();
  return tab ? tab->GetContents() : nullptr;
}

void OpenInNewTab(BrowserWindowInterface* browser, const GURL& url) {
  NavigateParams params(browser, url, ui::PAGE_TRANSITION_LINK);
  params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  Navigate(&params);
}

// 单项下载操作（弹出框按钮和右键菜单共用）。
void Act(BrowserWindowInterface* browser, Download* item, int command) {
  switch (command) {
    case kOpen:
      item->Open();
      break;
    case kShowInFolder:
      platform_util::ShowItemInFolder(browser->GetProfile(),
                                      item->RevealPath());
      break;
    case kPause:
      item->Pause();
      break;
    case kResume:
      item->Resume();
      break;
    case kCancel:
      item->Cancel();
      break;
    case kRetry:
      if (DownloadService* service = ServiceFor(browser)) {
        service->Retry(item, ActiveContents(browser));
      }
      break;
    case kCopyLink:
      ui::ScopedClipboardWriter(ui::ClipboardBuffer::kCopyPaste)
          .WriteText(base::UTF8ToUTF16(item->Url().spec()));
      break;
    case kOpenReferrer:
      if (const GURL referrer = item->ReferrerUrl(); referrer.is_valid()) {
        OpenInNewTab(browser, referrer);
      }
      break;
    case kRemove:
    case kDiscard:
      item->Remove();
      break;
    case kKeep:
      item->KeepDangerous();
      break;
  }
}

// 卡片的底：比菜单再实一点的面板色和一圈细边（玻璃在下面）。
class PanelBackground : public views::View {
  METADATA_HEADER(PanelBackground, views::View)

 public:
  PanelBackground() { SetCanProcessEventsWithinSubtree(false); }

  void SetTheme(const Theme& theme) {
    theme_ = theme;
    SchedulePaint();
  }

  void OnPaint(gfx::Canvas* canvas) override {
    const gfx::RectF bounds(GetLocalBounds());
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    fill.setColor(SkColorSetA(theme_.surface_panel,
                              theme_.is_dark ? 0.66f * 255 : 0.76f * 255));
    canvas->DrawRoundRect(bounds, radius::kCard, fill);
    cc::PaintFlags line;
    line.setAntiAlias(true);
    line.setColor(theme_.OnSurface(0.07f));
    // 顶上一条、底上一条分隔线。
    canvas->DrawRect(gfx::RectF(0, kHeaderHeight, bounds.width(), 1), line);
    canvas->DrawRect(gfx::RectF(0, bounds.height() - kFooterHeight - 1,
                                bounds.width(), 1),
                     line);
    cc::PaintFlags border;
    border.setAntiAlias(true);
    border.setStyle(cc::PaintFlags::kStroke_Style);
    border.setStrokeWidth(1);
    border.setColor(theme_.OnSurface(0.1f));
    gfx::RectF stroke = bounds;
    stroke.Inset(0.5f);
    canvas->DrawRoundRect(stroke, radius::kCard - 0.5f, border);
  }

 private:
  Theme theme_ = Theme::DarkBase();
};

BEGIN_METADATA(PanelBackground)
END_METADATA

// 顶上：「下载」和一行概况，右边打开下载文件夹、清除已结束的。
class PanelHeader : public views::View {
  METADATA_HEADER(PanelHeader, views::View)

 public:
  PanelHeader(BrowserWindowInterface* browser, const Theme& theme)
      : theme_(theme) {
    folder_ = AddChildView(std::make_unique<IconButton>(
        base::BindRepeating(
            [](BrowserWindowInterface* browser) {
              Profile* profile = browser->GetProfile();
              if (DownloadPrefs* prefs =
                      DownloadPrefs::FromBrowserContext(profile)) {
                platform_util::OpenItem(profile, prefs->DownloadPath(),
                                        platform_util::OPEN_FOLDER,
                                        platform_util::OpenOperationCallback());
              }
            },
            browser),
        icons::kFolder, L("downloads.openFolder"), 26, 12));
    clear_ = AddChildView(std::make_unique<IconButton>(
        base::BindRepeating(
            [](BrowserWindowInterface* browser) {
              if (DownloadService* service = ServiceFor(browser)) {
                service->ClearInactive();
              }
            },
            browser),
        icons::kCheckmarkCircle, L("downloads.clearInactive"), 26, 12));
    for (IconButton* button : {folder_.get(), clear_.get()}) {
      button->SetTheme(theme);
      button->SetVariant(ButtonVariant::kGhost);
    }
  }

  void Set(const std::u16string& detail, bool can_clear) {
    detail_ = detail;
    clear_->SetEnabled(can_clear);
    SchedulePaint();
  }

  void Layout(PassKey) override {
    const int y = (height() - 26) / 2;
    clear_->SetBounds(width() - 8 - 26, y, 26, 26);
    folder_->SetBounds(clear_->x() - 8 - 26, y, 26, 26);
  }

  void OnPaint(gfx::Canvas* canvas) override {
    const gfx::FontList& title = UiFont(13, gfx::Font::Weight::SEMIBOLD);
    const gfx::FontList& detail = UiFont(11);
    const int text_width = folder_->x() - 14 - 8;
    const int block =
        title.GetHeight() + (detail_.empty() ? 0 : 1 + detail.GetHeight());
    int y = (height() - block) / 2;
    canvas->DrawStringRectWithFlags(
        L("common.downloads"), title, theme_.on_surface,
        gfx::Rect(14, y, text_width, title.GetHeight()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
    if (!detail_.empty()) {
      y += title.GetHeight() + 1;
      canvas->DrawStringRectWithFlags(
          detail_, detail, theme_.on_surface_variant,
          gfx::Rect(14, y, text_width, detail.GetHeight()),
          gfx::Canvas::NO_SUBPIXEL_RENDERING);
    }
  }

 private:
  Theme theme_;
  std::u16string detail_;
  raw_ptr<IconButton> folder_ = nullptr;
  raw_ptr<IconButton> clear_ = nullptr;
};

BEGIN_METADATA(PanelHeader)
END_METADATA

// 底下：「显示全部下载」，右边是快捷键和箭头；悬停时一层浅底。
class PanelFooter : public views::Button {
  METADATA_HEADER(PanelFooter, views::Button)

 public:
  PanelFooter(PressedCallback callback, const Theme& theme)
      : views::Button(std::move(callback)), theme_(theme) {
    SetAccessibleName(L("downloads.panel.showAll"));
  }

  void SetCount(size_t count) {
    title_ = count > kLimit
                 ? L("downloads.panel.showAllCount", {{"count", count}})
                 : L("downloads.panel.showAll");
    SchedulePaint();
  }

  void PaintButtonContents(gfx::Canvas* canvas) override {
    if (GetState() == STATE_HOVERED || GetState() == STATE_PRESSED) {
      canvas->FillRect(GetLocalBounds(), theme_.OnSurface(0.05f));
    }
    const gfx::FontList& font = UiFont(12, gfx::Font::Weight::MEDIUM);
    const gfx::FontList& small = UiFont(11);
#if BUILDFLAG(IS_MAC)
    const std::u16string shortcut = u"⇧⌘J";
#else
    const std::u16string shortcut = u"Ctrl+J";
#endif
    const int arrow_x = width() - 14 - 10;
    canvas->Save();
    canvas->Translate(gfx::Vector2d(arrow_x, (height() - 10) / 2));
    gfx::PaintVectorIcon(canvas, icons::kChevronRight, 10,
                         theme_.OnSurface(0.85f));
    canvas->Restore();
    const int shortcut_width = gfx::GetStringWidth(shortcut, small);
    const int shortcut_x = arrow_x - 6 - shortcut_width;
    canvas->DrawStringRectWithFlags(
        shortcut, small, theme_.on_surface_variant,
        gfx::Rect(shortcut_x, (height() - small.GetHeight()) / 2,
                  shortcut_width, small.GetHeight()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
    canvas->DrawStringRectWithFlags(
        title_, font, theme_.OnSurface(0.85f),
        gfx::Rect(14, (height() - font.GetHeight()) / 2, shortcut_x - 20,
                  font.GetHeight()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

  void StateChanged(ButtonState old_state) override { SchedulePaint(); }

 private:
  Theme theme_;
  std::u16string title_ = L("downloads.panel.showAll");
};

BEGIN_METADATA(PanelFooter)
END_METADATA

// 没有下载时：一个下载图标和「还没有下载」。
class PanelEmpty : public views::View {
  METADATA_HEADER(PanelEmpty, views::View)

 public:
  explicit PanelEmpty(const Theme& theme) : theme_(theme) {
    SetCanProcessEventsWithinSubtree(false);
  }

  void OnPaint(gfx::Canvas* canvas) override {
    const gfx::FontList& font = UiFont(12);
    const int block = 26 + 8 + font.GetHeight();
    const int y = (height() - block) / 2;
    canvas->Save();
    canvas->Translate(gfx::Vector2d((width() - 26) / 2, y));
    gfx::PaintVectorIcon(canvas, icons::kArrowDownCircle, 26,
                         SkColorSetA(theme_.on_surface_variant, 0.6f * 255));
    canvas->Restore();
    canvas->DrawStringRectWithFlags(
        L("downloads.empty.title"), font, theme_.on_surface_variant,
        gfx::Rect(0, y + 26 + 8, width(), font.GetHeight()),
        gfx::Canvas::TEXT_ALIGN_CENTER | gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

 private:
  Theme theme_;
};

BEGIN_METADATA(PanelEmpty)
END_METADATA

// 单条下载行：图标（进行中时外面一圈进度）、文件名、细进度条（下载中、排队时；Starry 分段
// 下载时按段画）、状态；右边是操作按钮。悬停时一层浅底；双击打开；下载好的可以拖出去；右键
// 是菜单。
class PanelRow : public views::View {
  METADATA_HEADER(PanelRow, views::View)

 public:
  PanelRow(DownloadsPanelHost* host, Download* item)
      : host_(host), guid_(item->Guid()) {
    // 自己的图层：列表变了时滑到新位置（FLIP）、新的一行淡入。
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    primary_ = AddChildView(std::make_unique<IconButton>(
        base::BindRepeating(&PanelRow::PressButton, base::Unretained(this),
                            /*primary=*/true),
        icons::kPause, std::u16string(), 24, 10));
    secondary_ = AddChildView(std::make_unique<IconButton>(
        base::BindRepeating(&PanelRow::PressButton, base::Unretained(this),
                            /*primary=*/false),
        icons::kXmark, std::u16string(), 24, 9));
    for (IconButton* button : {primary_.get(), secondary_.get()}) {
      button->SetTheme(host->theme());
      button->SetVariant(ButtonVariant::kGhost);
    }
    Update(item);
  }

  const std::string& guid() const { return guid_; }
  const DownloadProgressBar& bar() const { return bar_; }
  LayerMotion& motion() { return motion_; }
  // 「文件名 | 状态」，分段的进度条后面加「 | segments=4 shown=0.42」（段数和画到的
  // 比例）。
  std::u16string TextForTesting() const {
    std::u16string text = base::StrCat({filename_, u" | ", status_});
    if (bar_.PieceCountForTesting() > 1) {
      base::StrAppend(
          &text,
          {u" | segments=", base::NumberToString16(bar_.PieceCountForTesting()),
           u" shown=", base::NumberToString16(bar_.ShownFractionForTesting())});
    }
    return text;
  }

  void Update(Download* item) {
    state_ = item->State();
    dangerous_ = item->IsDangerous();
    missing_ = item->FileMissing();
    filename_ = item->Filename();
    status_ = DownloadStatusText(*item);
    progress_ = DownloadProgress(*item);
    path_ = item->TargetPath();
    bar_.Update(*item, /*animate=*/updated_);
    updated_ = true;
    if (dangerous_) {
      SetButtons(&icons::kCheckmark, L("downloads.keep"), kKeep, &icons::kXmark,
                 L("downloads.discard"), kDiscard);
    } else {
      switch (state_) {
        case DownloadState::kQueued:
        case DownloadState::kRunning:
          SetButtons(&icons::kPause, L("common.pause"), kPause,
                     &icons::kXmark, L("downloads.cancel"), kCancel);
          break;
        case DownloadState::kPaused:
          SetButtons(&icons::kPlay, L("downloads.resume"), kResume,
                     &icons::kXmark, L("downloads.cancel"), kCancel);
          break;
        case DownloadState::kInterrupted:
          SetButtons(&icons::kArrowClockwise,
                     item->CanResume() ? L("downloads.resumeDownload")
                                       : L("common.retry"),
                     kRetry, nullptr, u"", 0);
          break;
        case DownloadState::kCancelled:
          SetButtons(&icons::kArrowClockwise, L("downloads.redownload"), kRetry,
                     nullptr,
                     u"", 0);
          break;
        case DownloadState::kComplete:
          SetButtons(&icons::kMagnifyingGlass, l10n::ShowInFolderLabel(),
                     kShowInFolder, nullptr, u"", 0);
          primary_->SetEnabled(!missing_);
          break;
      }
    }
    UpdateHoverButtons();
    LoadIcon();
    SetTooltipText(filename_);
    InvalidateLayout();
    SchedulePaint();
  }

  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    return gfx::Size(kCardWidth - 2 * kListInset, kRowHeight);
  }

  void Layout(PassKey) override {
    const int y = (height() - 24) / 2;
    int right = width() - 8;
    if (secondary_->GetVisible()) {
      secondary_->SetBounds(right - 24, y, 24, 24);
      right -= 24;
    }
    primary_->SetBounds(right - 24, y, 24, 24);
  }

  void OnPaint(gfx::Canvas* canvas) override {
    const Theme& theme = host_->theme();
    if (hovered_) {
      cc::PaintFlags flags;
      flags.setAntiAlias(true);
      flags.setColor(theme.OnSurface(0.05f));
      canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()), radius::kMenu,
                            flags);
    }
    // 图标区 36 × 36：进行中时外面一圈进度（线宽 2），图标 24；结束的图标 30。
    const bool active = IsActive(state_) && !dangerous_;
    const gfx::RectF area(8, (height() - 36) / 2.0f, 36, 36);
    if (active) {
      const SkColor tint = state_ == DownloadState::kRunning
                               ? theme.accent
                               : theme.on_surface_variant;
      cc::PaintFlags track;
      track.setAntiAlias(true);
      track.setStyle(cc::PaintFlags::kStroke_Style);
      track.setStrokeWidth(2);
      track.setColor(theme.OnSurface(0.12f));
      const gfx::PointF center = area.CenterPoint();
      canvas->DrawCircle(center, 17, track);
      cc::PaintFlags arc = track;
      arc.setStrokeCap(cc::PaintFlags::kRound_Cap);
      arc.setColor(tint);
      SkPathBuilder path;
      path.addArc(SkRect::MakeLTRB(center.x() - 17, center.y() - 17,
                                   center.x() + 17, center.y() + 17),
                  -90, 360 * std::max(0.02, progress_.value_or(0.02)));
      canvas->DrawPath(path.detach(), arc);
    }
    const int icon_size = active ? 24 : 30;
    const gfx::Rect icon_rect(
        static_cast<int>(area.CenterPoint().x()) - icon_size / 2,
        static_cast<int>(area.CenterPoint().y()) - icon_size / 2, icon_size,
        icon_size);
    if (!icon_.IsEmpty()) {
      const gfx::ImageSkia& image = *icon_.ToImageSkia();
      canvas->DrawImageInt(image, 0, 0, image.width(), image.height(),
                           icon_rect.x(), icon_rect.y(), icon_rect.width(),
                           icon_rect.height(), /*filter=*/true);
    } else {
      gfx::Rect doc = icon_rect;
      doc.Inset(gfx::Insets::VH(icon_size / 10, icon_size / 5));
      cc::PaintFlags flags;
      flags.setAntiAlias(true);
      flags.setStyle(cc::PaintFlags::kStroke_Style);
      flags.setStrokeWidth(1.2f);
      flags.setColor(theme.on_surface_variant);
      canvas->DrawRoundRect(gfx::RectF(doc), 3, flags);
    }

    // 文字：文件名 12 中等（文件没了、取消了的灰）、细进度条（3 pt）、状态 11。
    const int text_x = 8 + 36 + 10;
    int text_right = primary_->x() - 8;
    if (!primary_->GetVisible() && !secondary_->GetVisible()) {
      text_right = width() - 8;
    }
    const int text_width = std::max(0, text_right - text_x);
    const gfx::FontList& name_font = UiFont(12, gfx::Font::Weight::MEDIUM);
    const gfx::FontList& status_font = UiFont(11);
    const bool bar = (state_ == DownloadState::kRunning ||
                      state_ == DownloadState::kQueued) &&
                     !dangerous_;
    const int block = name_font.GetHeight() + 3 + (bar ? 3 + 3 : 0) +
                      status_font.GetHeight();
    int y = (height() - block) / 2;
    const bool dim = missing_ || state_ == DownloadState::kCancelled;
    canvas->DrawStringRectWithFlags(
        gfx::ElideText(filename_, name_font, text_width, gfx::ELIDE_MIDDLE),
        name_font, dim ? theme.on_surface_variant : theme.on_surface,
        gfx::Rect(text_x, y, text_width, name_font.GetHeight()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
    y += name_font.GetHeight() + 3;
    if (bar) {
      bar_.Paint(canvas, gfx::RectF(text_x, y, text_width, 3), theme,
                 theme.surface_panel);
      y += 3 + 3;
    }
    canvas->DrawStringRectWithFlags(
        status_, status_font,
        state_ == DownloadState::kInterrupted || dangerous_
            ? theme.danger()
            : theme.on_surface_variant,
        gfx::Rect(text_x, y, text_width, status_font.GetHeight()),
        gfx::Canvas::NO_SUBPIXEL_RENDERING);
  }

  void OnMouseEntered(const ui::MouseEvent& event) override {
    hovered_ = true;
    UpdateHoverButtons();
    SchedulePaint();
  }

  void OnMouseExited(const ui::MouseEvent& event) override {
    hovered_ = false;
    UpdateHoverButtons();
    SchedulePaint();
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    if (event.IsRightMouseButton()) {
      gfx::Point point = event.location();
      views::View::ConvertPointToTarget(this, host_, &point);
      if (Download* item = Item()) {
        host_->ShowItemMenu(item, point);
      }
      return true;
    }
    if (!event.IsLeftMouseButton()) {
      return false;
    }
    press_point_ = event.location();
    if (event.GetClickCount() == 2 || (event.flags() & ui::EF_IS_DOUBLE_CLICK)) {
      if (Download* item = Item();
          item && state_ == DownloadState::kComplete && !missing_) {
        item->Open();
      }
    }
    return true;
  }

  bool OnMouseDragged(const ui::MouseEvent& event) override {
    Download* item = Item();
    if (!item || state_ != DownloadState::kComplete || missing_ ||
        !ExceededDragThreshold(event.location() - press_point_)) {
      return true;
    }
    // 拖出已完成的文件本身（可拖到访达或其他应用）。
    const gfx::Image* icon = icon_.IsEmpty() ? nullptr : &icon_;
    DragDownload(*item, icon, GetWidget()->GetNativeView());
    return false;
  }

 private:
  Download* Item() const {
    DownloadService* service = ServiceFor(host_->browser());
    return service ? service->Find(guid_) : nullptr;
  }

  void SetButtons(const gfx::VectorIcon* primary,
                  const std::u16string& primary_tip,
                  int primary_command,
                  const gfx::VectorIcon* secondary,
                  const std::u16string& secondary_tip,
                  int secondary_command) {
    primary_->SetIcon(*primary);
    primary_->SetTooltipText(primary_tip);
    primary_->SetEnabled(true);
    primary_command_ = primary_command;
    secondary_->SetVisible(secondary != nullptr);
    if (secondary) {
      secondary_->SetIcon(*secondary);
      secondary_->SetTooltipText(secondary_tip);
      secondary_command_ = secondary_command;
    }
  }

  // 已完成或已取消的条目，仅在悬停时显示操作按钮。
  void UpdateHoverButtons() {
    const bool hover_only = !dangerous_ && (state_ == DownloadState::kComplete ||
                                            state_ == DownloadState::kCancelled);
    primary_->SetVisible(!hover_only || hovered_);
  }

  void PressButton(bool primary) {
    if (Download* item = Item()) {
      Act(host_->browser(), item, primary ? primary_command_ : secondary_command_);
    }
  }

  void LoadIcon() {
    if (path_.empty() || path_ == icon_path_) {
      return;
    }
    icon_path_ = path_;
    IconManager* manager = g_browser_process->icon_manager();
    // 按窗口的缩放取图标（还没放进窗口时按 2 倍）。
    const float scale =
        GetWidget() ? GetWidget()->GetLayer()->device_scale_factor() : 2.0f;
    if (gfx::Image* cached = manager->LookupIconFromFilepath(
            path_, IconLoader::NORMAL, scale)) {
      icon_ = *cached;
      return;
    }
    tracker_.TryCancelAll();
    manager->LoadIcon(path_, IconLoader::NORMAL, scale,
                      base::BindOnce(
                          [](base::WeakPtr<PanelRow> row, gfx::Image image) {
                            if (row) {
                              row->icon_ = std::move(image);
                              row->SchedulePaint();
                            }
                          },
                          weak_factory_.GetWeakPtr()),
                      &tracker_);
  }

  const raw_ptr<DownloadsPanelHost> host_;
  const std::string guid_;
  raw_ptr<IconButton> primary_ = nullptr;
  raw_ptr<IconButton> secondary_ = nullptr;
  int primary_command_ = 0;
  int secondary_command_ = 0;
  DownloadState state_ = DownloadState::kRunning;
  bool dangerous_ = false;
  bool missing_ = false;
  bool hovered_ = false;
  std::u16string filename_;
  std::u16string status_;
  std::optional<double> progress_;
  base::FilePath path_;
  base::FilePath icon_path_;
  gfx::Image icon_;
  gfx::Point press_point_;
  base::CancelableTaskTracker tracker_;
  LayerMotion motion_;
  DownloadProgressBar bar_{
      base::BindRepeating(&views::View::SchedulePaint, base::Unretained(this))};
  // 已经显示过一次：之后的进度变化有过渡。
  bool updated_ = false;
  base::WeakPtrFactory<PanelRow> weak_factory_{this};
};

BEGIN_METADATA(PanelRow)
END_METADATA

}  // namespace

// 卡片：玻璃、底、顶上的概况、列表（太长时滚动）、底下的「显示全部下载」。
class DownloadsPanelCard : public GlassView, public DownloadService::Observer {
  METADATA_HEADER(DownloadsPanelCard, GlassView)

 public:
  explicit DownloadsPanelCard(DownloadsPanelHost* host) : host_(host) {
    const Theme& theme = host->theme();
    SetCornerRadius(radius::kCard, true);
    GlassStyle style;
    style.dark = theme.is_dark;
    style.luma = theme.is_dark ? 0.1f : 0.9f;
    SetStyle(style);
    background_ = AddChildView(std::make_unique<PanelBackground>());
    background_->SetTheme(theme);
    header_ = AddChildView(std::make_unique<PanelHeader>(host->browser(), theme));
    scroll_ = AddChildView(std::make_unique<views::ScrollView>(
        views::ScrollView::ScrollWithLayers::kEnabled));
    scroll_->SetBackgroundColor(std::nullopt);
    scroll_->SetHorizontalScrollBarMode(
        views::ScrollView::ScrollBarMode::kDisabled);
    scroll_->SetDrawOverflowIndicator(false);
    list_ = scroll_->SetContents(std::make_unique<views::View>());
    empty_ = AddChildView(std::make_unique<PanelEmpty>(theme));
    footer_ = AddChildView(std::make_unique<PanelFooter>(
        base::BindRepeating(
            [](DownloadsPanelHost* host) {
              BrowserWindowInterface* browser = host->browser();
              host->Close();
              ShowSingletonTabOverwritingNTP(
                  browser, GURL(chrome::kChromeUIDownloadsURL));
            },
            base::Unretained(host)),
        theme));
    if (DownloadService* service = ServiceFor(host->browser())) {
      service_ = service;
      observation_.Observe(service);
    }
    Rebuild();
  }

  LayerMotion& motion() { return motion_; }

  int ContentHeight() const {
    const int list = rows_.empty()
                         ? kEmptyHeight
                         : std::min(kMaxListHeight, ListHeight());
    return kHeaderHeight + 1 + list + 1 + kFooterHeight;
  }

  void Place(const gfx::Rect& card) {
    SetGlassBounds(card);
    const gfx::Rect rect = glass_rect();
    background_->SetBoundsRect(rect);
    header_->SetBounds(rect.x(), rect.y(), rect.width(), kHeaderHeight);
    const int list_top = rect.y() + kHeaderHeight + 1;
    const int list_height = rect.height() - kHeaderHeight - 2 - kFooterHeight;
    empty_->SetVisible(rows_.empty());
    scroll_->SetVisible(!rows_.empty());
    empty_->SetBounds(rect.x(), list_top, rect.width(), list_height);
    scroll_->SetBounds(rect.x(), list_top, rect.width(), list_height);
    list_->SetBounds(0, 0, rect.width(), ListHeight());
    int y = kListInset;
    const bool animate = animate_rows_ && !motion::ReduceMotion();
    animate_rows_ = false;
    for (PanelRow* row : rows_) {
      const gfx::Rect old = row->bounds();
      const gfx::Rect target(kListInset, y, rect.width() - 2 * kListInset,
                             kRowHeight);
      row->SetBoundsRect(target);
      // 列表变化（新增下载或清除已结束项）时，其余行从原位置平滑滑动到新位置。
      if (animate && !old.IsEmpty() && old.origin() != target.origin()) {
        row->motion().ShiftAndAnimate(row->layer(),
                                      gfx::Vector2dF(old.origin() - target.origin()),
                                      LayerPose(),
                                      MotionCurve::Of(motion::kListEdit));
      }
      y += kRowHeight + kRowSpacing;
    }
    footer_->SetBounds(rect.x(), rect.bottom() - kFooterHeight, rect.width(),
                       kFooterHeight);
  }

  std::vector<std::u16string> RowTexts() const {
    std::vector<std::u16string> texts;
    for (const PanelRow* row : rows_) {
      texts.push_back(row->TextForTesting());
    }
    return texts;
  }

  // DownloadService::Observer:
  void OnDownloadAdded(Download* download) override { Rebuild(); }
  void OnDownloadUpdated(Download* download) override {
    for (PanelRow* row : rows_) {
      if (row->guid() == download->Guid()) {
        row->Update(download);
        break;
      }
    }
    UpdateSummary();
  }
  void OnDownloadRemoved(const std::string& guid) override {
    // 在别人的通知里：下一轮再重建。
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&DownloadsPanelCard::Rebuild,
                                  weak_factory_.GetWeakPtr()));
  }
  void OnDownloadServiceShutdown() override {
    observation_.Reset();
    service_ = nullptr;
  }

 private:
  int ListHeight() const {
    const int count = static_cast<int>(rows_.size());
    return 2 * kListInset + count * kRowHeight +
           std::max(0, count - 1) * kRowSpacing;
  }

  std::vector<Download*> Items() const {
    return service_ ? service_->Downloads() : std::vector<Download*>();
  }

  void Rebuild() {
    std::vector<Download*> items = Items();
    total_ = items.size();
    if (items.size() > kLimit) {
      items.resize(kLimit);
    }
    // 留下还在的行（悬停、图标不丢），按新的顺序排。
    std::vector<raw_ptr<PanelRow, VectorExperimental>> rows;
    for (Download* item : items) {
      auto it = std::ranges::find_if(
          rows_, [&](PanelRow* row) { return row->guid() == item->Guid(); });
      if (it != rows_.end()) {
        (*it)->Update(item);
        rows.push_back(*it);
        rows_.erase(it);
      } else {
        PanelRow* row =
            list_->AddChildView(std::make_unique<PanelRow>(host_, item));
        // 新的一行淡入、从 0.98 放大（第一次建列表时不动）。
        if (built_ && !motion::ReduceMotion()) {
          LayerPose from;
          from.scale = 0.98f;
          const MotionCurve curve = MotionCurve::Of(motion::kListEdit);
          row->motion().AnimateOpacityFrom(row->layer(), 0, 1, curve);
          row->motion().AnimateTransformFrom(
              row->layer(), from, LayerPose(), curve,
              gfx::PointF(kCardWidth / 2.0f, kRowHeight / 2.0f));
        }
        rows.push_back(row);
      }
    }
    for (PanelRow* row : rows_) {
      list_->RemoveChildViewT(row);
    }
    rows_ = std::move(rows);
    animate_rows_ = built_;
    built_ = true;
    UpdateSummary();
    host_->InvalidateLayout();
  }

  // 顶部的概况文案：有在下载的是「2 个正在下载 · 1.2 MB/s」，有暂停、失败的是
  // 「1 个未完成」，否则是「5 个下载」。
  void UpdateSummary() {
    std::vector<Download*> items = Items();
    int running = 0;
    int active = 0;
    int64_t speed = 0;
    bool any_inactive = false;
    for (Download* item : items) {
      const DownloadState state = item->State();
      if (state == DownloadState::kRunning) {
        ++running;
        speed += item->Speed();
      }
      if (IsActive(state)) {
        ++active;
      } else {
        any_inactive = true;
      }
    }
    std::u16string detail;
    if (running > 0) {
      detail = L("downloads.panel.running",
                 {{"count", running}, {"speed", FormatSpeed(speed)}});
    } else if (active > 0) {
      detail = L("downloads.panel.incomplete", {{"count", active}});
    } else if (!items.empty()) {
      detail = L("downloads.panel.count", {{"count", items.size()}});
    }
    header_->Set(detail, any_inactive);
    footer_->SetCount(total_);
  }

  const raw_ptr<DownloadsPanelHost> host_;
  raw_ptr<PanelBackground> background_ = nullptr;
  raw_ptr<PanelHeader> header_ = nullptr;
  raw_ptr<views::ScrollView> scroll_ = nullptr;
  raw_ptr<views::View> list_ = nullptr;
  raw_ptr<PanelEmpty> empty_ = nullptr;
  raw_ptr<PanelFooter> footer_ = nullptr;
  std::vector<raw_ptr<PanelRow, VectorExperimental>> rows_;
  // 列表建过一次了（之后的变化才有动画）；下一次排版时把挪了位置的行滑过去。
  bool built_ = false;
  bool animate_rows_ = false;
  size_t total_ = 0;
  raw_ptr<DownloadService> service_ = nullptr;
  base::ScopedObservation<DownloadService, DownloadService::Observer>
      observation_{this};
  LayerMotion motion_;
  base::WeakPtrFactory<DownloadsPanelCard> weak_factory_{this};
};

BEGIN_METADATA(DownloadsPanelCard)
END_METADATA

// views::View 也有 OnEvent，所以窗口的事件由这个小对象转给 OnWindowEvent。
class DownloadsPanelHost::WindowWatcher : public ui::EventObserver {
 public:
  explicit WindowWatcher(DownloadsPanelHost* host) : host_(host) {}
  void OnEvent(const ui::Event& event) override { host_->OnWindowEvent(event); }

 private:
  const raw_ptr<DownloadsPanelHost> host_;
};

DownloadsPanelHost::DownloadsPanelHost(BrowserWindowInterface* browser)
    : browser_(browser), watcher_(std::make_unique<WindowWatcher>(this)) {
  SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
  SetVisible(false);
}

DownloadsPanelHost::~DownloadsPanelHost() {
  event_monitor_.reset();
}

base::CallbackListSubscription DownloadsPanelHost::AddShowingChangedCallback(
    base::RepeatingClosure callback) {
  return showing_changed_.Add(std::move(callback));
}

void DownloadsPanelHost::SetTheme(const Theme& theme) {
  theme_ = theme;
  // 主题变了（深浅切换）：开着的卡片按新主题重建。
  if (is_showing()) {
    const gfx::Rect anchor = anchor_;
    RemoveCard();
    Show(anchor);
  }
}

void DownloadsPanelHost::Show(const gfx::Rect& anchor) {
  if (closing_) {
    RemoveCard();
  }
  if (card_) {
    return;
  }
  anchor_ = anchor;
  SetVisible(true);
  card_ = AddChildView(std::make_unique<DownloadsPanelCard>(this));
  PlaceCard();

  // 从按钮那个角（左下）由 0.95 放大、淡入（同菜单）。
  LayerPose from;
  if (!motion::ReduceMotion()) {
    from.scale = motion::kMenuOpenScale;
  }
  const gfx::Rect glass = card_->glass_rect();
  card_->motion().AnimateTransformFrom(
      card_->layer(), from, LayerPose(), MotionCurve::Of(motion::kMenuOpen),
      gfx::PointF(glass.x(), glass.bottom()));
  card_->motion().AnimateOpacityFrom(card_->layer(), 0, 1,
                                     MotionCurve::EaseOut(motion::kMenuFadeIn));
  if (GetWidget()) {
    event_monitor_ = views::EventMonitor::CreateWindowMonitor(
        watcher_.get(), GetWidget()->GetNativeWindow(),
        {ui::EventType::kMousePressed, ui::EventType::kKeyPressed});
  }
  showing_changed_.Notify();
}

void DownloadsPanelHost::Close() {
  if (!card_ || closing_) {
    return;
  }
  closing_ = true;
  event_monitor_.reset();
  if (menu_model_) {
    if (StarryBrowserView* view = StarryBrowserView::From(browser_);
        view && view->menu_host()) {
      view->menu_host()->Close();
    }
  }
  card_->SetCanProcessEventsWithinSubtree(false);
  card_->motion().AnimateOpacity(
      card_->layer(), 0, MotionCurve::EaseOut(motion::kMenuFadeOut),
      base::BindOnce(
          [](base::WeakPtr<DownloadsPanelHost> host) {
            if (host && host->closing_) {
              host->RemoveCard();
            }
          },
          weak_factory_.GetWeakPtr()));
  showing_changed_.Notify();
}

void DownloadsPanelHost::RemoveCard() {
  if (card_) {
    RemoveChildViewT(card_.ExtractAsDangling());
  }
  card_ = nullptr;
  closing_ = false;
  event_monitor_.reset();
  SetVisible(false);
}

void DownloadsPanelHost::Layout(PassKey) {
  if (card_ && !closing_) {
    PlaceCard();
  }
}

// 按钮上面、左边对齐；高度变了（多了、少了一行）时底边不动，往上长。
void DownloadsPanelHost::PlaceCard() {
  gfx::Rect visible = GetLocalBounds();
  visible.Inset(kWindowInset);
  const int width = std::min(kCardWidth, visible.width());
  const int height = std::min(card_->ContentHeight(), visible.height());
  int x = std::clamp(anchor_.x(), visible.x(),
                     std::max(visible.x(), visible.right() - width));
  int y = anchor_.y() - kAnchorGap - height;
  if (y < visible.y()) {
    // 上面放不下（按钮在窗口上半部）：放在按钮下面。
    y = std::min(anchor_.bottom() + kAnchorGap, visible.bottom() - height);
  }
  card_->Place(gfx::Rect(x, y, width, height));
}

bool DownloadsPanelHost::DoesIntersectRect(const views::View* target,
                                           const gfx::Rect& rect) const {
  return card_ && !closing_ && card_->glass_bounds().Intersects(rect);
}

void DownloadsPanelHost::OnWindowEvent(const ui::Event& event) {
  if (!is_showing()) {
    return;
  }
  // 菜单开着时（行的右键菜单）按键、点击都是菜单的。
  if (StarryBrowserView* view = StarryBrowserView::From(browser_);
      view && view->menu_host() && view->menu_host()->is_showing()) {
    return;
  }
  if (event.IsKeyEvent()) {
    if (event.AsKeyEvent()->key_code() == ui::VKEY_ESCAPE) {
      Close();
    }
    return;
  }
  if (!event.IsMouseEvent() || !event_monitor_) {
    return;
  }
  gfx::Point point = event_monitor_->GetLastMouseLocation();
  views::View::ConvertPointFromScreen(this, &point);
  // 点在卡片上是卡片的；点在下载按钮上由按钮自己关（不然关了又开）。
  if (card_->glass_bounds().Contains(point) || anchor_.Contains(point)) {
    return;
  }
  Close();
}

void DownloadsPanelHost::ShowItemMenu(Download* item, const gfx::Point& point) {
  StarryBrowserView* view = StarryBrowserView::From(browser_);
  if (!view) {
    return;
  }
  menu_guid_ = item->Guid();
  menu_model_ = std::make_unique<ui::SimpleMenuModel>(this);
  const auto add = [&](int command, const std::u16string& title,
                       const gfx::VectorIcon& icon) {
    menu_model_->AddItemWithIcon(
        command, title, ui::ImageModel::FromVectorIcon(icon, ui::kColorMenuIcon, 14));
  };
  const DownloadState state = item->State();
  if (state == DownloadState::kComplete) {
    add(kOpen, L("common.open"), icons::kArrowUpRight);
    add(kShowInFolder, l10n::ShowInFolderLabel(), icons::kFolder);
  } else if (item->IsStarry() && IsActive(state)) {
    // 未完成的下载：在文件管理器中定位临时文件。
    add(kShowInFolder, l10n::ShowInFolderLabel(), icons::kFolder);
  }
  switch (state) {
    case DownloadState::kQueued:
    case DownloadState::kRunning:
      add(kPause, L("common.pause"), icons::kPause);
      add(kCancel, L("downloads.cancel"), icons::kXmark);
      break;
    case DownloadState::kPaused:
      add(kResume, L("downloads.resume"), icons::kPlay);
      add(kCancel, L("downloads.cancel"), icons::kXmark);
      break;
    case DownloadState::kInterrupted:
      add(kRetry, item->CanResume() ? L("downloads.resume") : L("common.retry"),
          icons::kArrowClockwise);
      break;
    case DownloadState::kCancelled:
    case DownloadState::kComplete:
      add(kRetry, L("downloads.redownload"), icons::kArrowDownCircle);
      break;
  }
  menu_model_->AddSeparator(ui::NORMAL_SEPARATOR);
  add(kCopyLink, L("downloads.copyDownloadLink"), icons::kLink);
  if (item->ReferrerUrl().is_valid()) {
    add(kOpenReferrer, L("downloads.openReferrer"), icons::kGlobe);
  }
  menu_model_->AddSeparator(ui::NORMAL_SEPARATOR);
  add(kRemove, L("downloads.removeFromList"), icons::kMinusCircle);

  gfx::Point in_view = point;
  views::View::ConvertPointToTarget(this, view, &in_view);
  view->ShowMenu(menu_model_.get(), in_view,
                 base::BindOnce(
                     [](base::WeakPtr<DownloadsPanelHost> host) {
                       if (host) {
                         // 模型留到下一轮（菜单在关的动画里还会读它）。
                         base::SequencedTaskRunner::GetCurrentDefault()
                             ->DeleteSoon(FROM_HERE,
                                          std::move(host->menu_model_));
                       }
                     },
                     weak_factory_.GetWeakPtr()));
}

Download* DownloadsPanelHost::MenuItem() const {
  DownloadService* service = ServiceFor(browser_);
  return service ? service->Find(menu_guid_) : nullptr;
}

bool DownloadsPanelHost::IsCommandIdEnabled(int command_id) const {
  Download* item = MenuItem();
  if (!item) {
    return false;
  }
  if (command_id == kOpen || command_id == kShowInFolder) {
    return !item->FileMissing();
  }
  return true;
}

void DownloadsPanelHost::ExecuteCommand(int command_id, int event_flags) {
  if (Download* item = MenuItem()) {
    Act(browser_, item, command_id);
  }
}

std::vector<std::u16string> DownloadsPanelHost::RowTextsForTesting() const {
  return card_ ? card_->RowTexts() : std::vector<std::u16string>();
}

gfx::Rect DownloadsPanelHost::CardBoundsForTesting() const {
  return card_ ? card_->glass_bounds() : gfx::Rect();
}

BEGIN_METADATA(DownloadsPanelHost)
END_METADATA

}  // namespace starry
