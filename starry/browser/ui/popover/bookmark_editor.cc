// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/popover/bookmark_editor.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "components/bookmarks/browser/bookmark_node.h"
#include "components/bookmarks/browser/bookmark_utils.h"
#include "components/bookmarks/common/bookmark_metrics.h"
#include "components/url_formatter/url_formatter.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/ui/design/controls.h"
#include "starry/browser/ui/design/icons.h"
#include "starry/browser/ui/design/metrics.h"
#include "starry/browser/ui/design/typography.h"
#include "starry/browser/ui/frame/starry_browser_view.h"
#include "starry/browser/ui/menu/menu_host_view.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/text_constants.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/controls/label.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/controls/textfield/textfield.h"

namespace starry {

namespace {

using bookmarks::BookmarkNode;

constexpr bookmarks::metrics::BookmarkEditSource kUserEdit =
    bookmarks::metrics::BookmarkEditSource::kUser;

constexpr int kWidth = 320;
constexpr int kPadding = 16;
constexpr int kTileSize = 36;
constexpr int kFieldHeight = 34;
constexpr int kFieldPadding = 14;
constexpr int kFolderHeight = 28;
constexpr int kButtonHeight = 32;

// 固定的几个文件夹用 Starry 的叫法（同书签页 bookmarks_handler.cc）。
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

std::u16string FolderPath(const BookmarkNode* folder) {
  std::u16string path = FolderTitle(folder);
  for (const BookmarkNode* parent = folder->parent();
       parent && !parent->is_root(); parent = parent->parent()) {
    path = base::StrCat({FolderTitle(parent), u" › ", path});
  }
  return path;
}

void CollectFolders(const BookmarkNode* folder,
                    std::vector<const BookmarkNode*>& out) {
  out.push_back(folder);
  for (const auto& child : folder->children()) {
    if (child->is_folder()) {
      CollectFolders(child.get(), out);
    }
  }
}

// 名称输入框：聚焦时父视图重画胶囊的底和描边。
class PillTextfield : public views::Textfield {
  METADATA_HEADER(PillTextfield, views::Textfield)

 public:
  void OnFocus() override {
    views::Textfield::OnFocus();
    parent()->SchedulePaint();
  }
  void OnBlur() override {
    views::Textfield::OnBlur();
    parent()->SchedulePaint();
  }
};

BEGIN_METADATA(PillTextfield)
END_METADATA

}  // namespace

// 文件夹的胶囊菜单按钮（同设置页的 menu-button）：当前文件夹加上下箭头。
class BookmarkFolderButton : public views::Button {
  METADATA_HEADER(BookmarkFolderButton, views::Button)

 public:
  explicit BookmarkFolderButton(PressedCallback callback)
      : views::Button(std::move(callback)) {}

  void Set(const std::u16string& text, const Theme& theme) {
    text_ = text;
    theme_ = theme;
    SetTooltipText(text);
    SchedulePaint();
  }

  int PreferredWidth() const {
    return std::min(200, gfx::GetStringWidth(text_, UiFont(13)) + 12 + 6 +
                             9 + 10);
  }

  void PaintButtonContents(gfx::Canvas* canvas) override {
    cc::PaintFlags fill;
    fill.setAntiAlias(true);
    const bool hovered = GetState() == STATE_HOVERED || GetState() == STATE_PRESSED;
    fill.setColor(SkColorSetA(theme_.primary, hovered ? 0.12f * 255 : 0.08f * 255));
    canvas->DrawRoundRect(gfx::RectF(GetLocalBounds()), height() / 2.0f, fill);
    const int chevron = 9;
    const gfx::Rect text_rect(12, 0, std::max(0, width() - 12 - 6 - chevron - 10),
                              height());
    canvas->DrawStringRectWithFlags(text_, UiFont(13), theme_.on_surface,
                                    text_rect, gfx::Canvas::NO_SUBPIXEL_RENDERING);
    canvas->Save();
    canvas->Translate(gfx::Vector2d(width() - 10 - chevron, (height() - chevron) / 2));
    gfx::PaintVectorIcon(canvas, icons::kChevronUpChevronDown, chevron,
                         theme_.on_surface_variant);
    canvas->Restore();
  }

  void StateChanged(ButtonState old_state) override { SchedulePaint(); }

 private:
  std::u16string text_;
  Theme theme_ = Theme::DarkBase();
};

BEGIN_METADATA(BookmarkFolderButton)
END_METADATA

BookmarkEditor::BookmarkEditor(BrowserWindowInterface* browser,
                               PopoverHost* host,
                               const GURL& url,
                               bool already_bookmarked)
    : browser_(browser), host_(host) {
  model_ = BookmarkModelFactory::GetForBrowserContext(browser->GetProfile());
  const BookmarkNode* node =
      model_ ? model_->GetMostRecentlyAddedUserNodeForURL(url)
             : nullptr;
  if (node) {
    node_id_ = node->id();
    observation_.Observe(model_.get());
  }

  title_ = AddChildView(std::make_unique<views::Label>(
      already_bookmarked ? L("bookmarks.editor.title") : L("bookmarks.popover.added")));
  title_->SetFontList(UiFont(14, gfx::Font::Weight::SEMIBOLD));
  title_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  title_->SetAutoColorReadabilityEnabled(false);

  url_label_ = AddChildView(std::make_unique<views::Label>(
      url_formatter::FormatUrl(url, url_formatter::kFormatUrlOmitDefaults,
                               base::UnescapeRule::SPACES, nullptr, nullptr,
                               nullptr)));
  url_label_->SetFontList(UiFont(12));
  url_label_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  url_label_->SetElideBehavior(gfx::ELIDE_TAIL);
  url_label_->SetAutoColorReadabilityEnabled(false);

  field_ = AddChildView(std::make_unique<PillTextfield>());
  field_->SetController(this);
  field_->SetBackgroundEnabled(false);
  field_->SetBorder(nullptr);
  field_->SetFontList(UiFont(13));
  field_->SetPlaceholderText(L("bookmarks.editor.name"));
  field_->GetViewAccessibility().SetName(L("bookmarks.popover.nameField"));
  if (node) {
    field_->SetText(node->GetTitle());
  }

  folder_label_ = AddChildView(std::make_unique<views::Label>(L("bookmarks.popover.folder")));
  folder_label_->SetFontList(UiFont(13));
  folder_label_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  folder_label_->SetAutoColorReadabilityEnabled(false);
  folder_button_ = AddChildView(std::make_unique<BookmarkFolderButton>(
      base::BindRepeating(&BookmarkEditor::ShowFolderMenu,
                          base::Unretained(this))));

  remove_button_ = AddChildView(std::make_unique<PillButton>(
      base::BindRepeating(&BookmarkEditor::Remove, base::Unretained(this)),
      L("common.remove"), ButtonVariant::kTertiary));
  done_button_ = AddChildView(std::make_unique<PillButton>(
      base::BindRepeating(&BookmarkEditor::Done, base::Unretained(this)),
      L("common.done"), ButtonVariant::kFilled));
  UpdateFolder();
}

BookmarkEditor::~BookmarkEditor() = default;

const BookmarkNode* BookmarkEditor::Node() const {
  return model_ && node_id_ >= 0
             ? bookmarks::GetBookmarkNodeByID(model_, node_id_)
             : nullptr;
}

gfx::Size BookmarkEditor::ContentSize() const {
  return gfx::Size(kWidth, kPadding + kTileSize + 14 + kFieldHeight + 10 +
                               kFolderHeight + 16 + kButtonHeight + kPadding);
}

void BookmarkEditor::SetTheme(const Theme& theme) {
  theme_ = theme;
  title_->SetEnabledColor(theme.on_surface);
  url_label_->SetEnabledColor(theme.on_surface_variant);
  field_->SetColor(theme.on_surface);
  folder_label_->SetEnabledColor(theme.on_surface_variant);
  remove_button_->SetTheme(theme);
  done_button_->SetTheme(theme);
  UpdateFolder();
  SchedulePaint();
}

views::View* BookmarkEditor::InitialFocus() {
  field_->SelectAll(false);
  return field_;
}

void BookmarkEditor::UpdateFolder() {
  const BookmarkNode* node = Node();
  folder_button_->Set(node ? FolderPath(node->parent()) : std::u16string(),
                      theme_);
  folder_button_->SetEnabled(node != nullptr);
  InvalidateLayout();
}

void BookmarkEditor::Layout(PassKey) {
  const int inner = width() - 2 * kPadding;
  const int text_x = kPadding + kTileSize + 12;
  const int text_width = std::max(0, width() - kPadding - text_x);
  title_->SetBounds(text_x, kPadding, text_width, 20);
  url_label_->SetBounds(text_x, kPadding + 20, text_width, 16);

  int y = kPadding + kTileSize + 14;
  field_->SetBounds(kPadding + kFieldPadding, y + 5,
                    std::max(0, inner - 2 * kFieldPadding), kFieldHeight - 10);
  y += kFieldHeight + 10;

  const int folder_width = folder_button_->PreferredWidth();
  folder_button_->SetBounds(width() - kPadding - folder_width, y, folder_width,
                            kFolderHeight);
  folder_label_->SetBounds(kPadding + 2, y, 80, kFolderHeight);
  y += kFolderHeight + 16;

  const gfx::Size remove = remove_button_->GetPreferredSize();
  remove_button_->SetBounds(kPadding, y, remove.width(), kButtonHeight);
  const gfx::Size done = done_button_->GetPreferredSize();
  done_button_->SetBounds(width() - kPadding - done.width(), y, done.width(),
                          kButtonHeight);
}

void BookmarkEditor::OnPaint(gfx::Canvas* canvas) {
  // 星形图块：强调色的浅底（同对话框的图标块）。
  cc::PaintFlags tile;
  tile.setAntiAlias(true);
  tile.setColor(SkColorSetA(theme_.accent, theme_.is_dark ? 0.16f * 255
                                                          : 0.1f * 255));
  const gfx::RectF tile_rect(kPadding, kPadding, kTileSize, kTileSize);
  canvas->DrawRoundRect(tile_rect, 10, tile);
  const int star = 17;
  canvas->Save();
  canvas->Translate(gfx::Vector2d(kPadding + (kTileSize - star) / 2,
                                  kPadding + (kTileSize - star) / 2));
  gfx::PaintVectorIcon(canvas, icons::kStarFill, star, theme_.accent);
  canvas->Restore();

  // 名称输入框的胶囊（同对话框的输入框）。
  const bool focused = field_->HasFocus();
  const gfx::RectF field(kPadding, kPadding + kTileSize + 14,
                         width() - 2 * kPadding, kFieldHeight);
  cc::PaintFlags fill;
  fill.setAntiAlias(true);
  fill.setColor(theme_.OnSurface(focused ? 0.08f : 0.06f));
  canvas->DrawRoundRect(field, kFieldHeight / 2.0f, fill);
  if (focused) {
    cc::PaintFlags outline;
    outline.setAntiAlias(true);
    outline.setStyle(cc::PaintFlags::kStroke_Style);
    outline.setStrokeWidth(1);
    outline.setColor(theme_.OnSurface(0.16f));
    gfx::RectF stroke = field;
    stroke.Inset(0.5f);
    canvas->DrawRoundRect(stroke, kFieldHeight / 2.0f - 0.5f, outline);
  }
}

bool BookmarkEditor::HandleKeyEvent(views::Textfield* sender,
                                    const ui::KeyEvent& key_event) {
  if (key_event.type() != ui::EventType::kKeyPressed) {
    return false;
  }
  if (key_event.key_code() == ui::VKEY_RETURN) {
    Done();
    return true;
  }
  if (key_event.key_code() == ui::VKEY_ESCAPE) {
    keep_title_ = true;
    host_->Close();
    return true;
  }
  return false;
}

void BookmarkEditor::WillClose() {
  if (keep_title_) {
    return;
  }
  const BookmarkNode* node = Node();
  if (!node) {
    return;
  }
  std::u16string title(field_->GetText());
  if (title != node->GetTitle()) {
    model_->SetTitle(node, title, kUserEdit);
  }
}

void BookmarkEditor::Done() {
  host_->Close();
}

void BookmarkEditor::Remove() {
  if (const BookmarkNode* node = Node()) {
    keep_title_ = true;
    model_->Remove(node, kUserEdit, FROM_HERE);
  }
  host_->Close();
}

void BookmarkEditor::BookmarkModelChanged() {
  if (!Node()) {
    // 别处删掉了这个书签。
    keep_title_ = true;
    host_->Close();
    return;
  }
  UpdateFolder();
}

void BookmarkEditor::ShowFolderMenu() {
  const BookmarkNode* node = Node();
  StarryBrowserView* view = StarryBrowserView::From(browser_);
  if (!node || !view) {
    return;
  }
  std::vector<const BookmarkNode*> folders;
  for (const auto& permanent : model_->root_node()->children()) {
    if (permanent->IsVisible() || !permanent->children().empty()) {
      CollectFolders(permanent.get(), folders);
    }
  }
  folder_menu_ = std::make_unique<ui::SimpleMenuModel>(this);
  folder_ids_.clear();
  for (const BookmarkNode* folder : folders) {
    folder_menu_->AddCheckItem(static_cast<int>(folder_ids_.size()) + 1,
                               FolderPath(folder));
    folder_ids_.push_back(folder->id());
  }
  const gfx::Rect rect = views::View::ConvertRectToTarget(
      folder_button_, view, folder_button_->GetLocalBounds());
  MenuOptions options;
  options.anchor = rect;
  view->ShowMenu(folder_menu_.get(), rect.bottom_left(), base::DoNothing(),
                 std::move(options));
}

bool BookmarkEditor::IsCommandIdChecked(int command_id) const {
  const BookmarkNode* node = Node();
  const size_t index = static_cast<size_t>(command_id - 1);
  return node && index < folder_ids_.size() &&
         node->parent()->id() == folder_ids_[index];
}

void BookmarkEditor::ExecuteCommand(int command_id, int event_flags) {
  const BookmarkNode* node = Node();
  const size_t index = static_cast<size_t>(command_id - 1);
  if (!node || index >= folder_ids_.size()) {
    return;
  }
  const BookmarkNode* folder =
      bookmarks::GetBookmarkNodeByID(model_, folder_ids_[index]);
  if (folder && folder != node->parent()) {
    model_->Move(node, folder, folder->children().size());
  }
}

void BookmarkEditor::SetNameForTesting(const std::u16string& name) {
  field_->SetText(name);
}

std::u16string BookmarkEditor::DescribeForTesting() const {
  const BookmarkNode* node = Node();
  return base::StrCat({title_->GetText(), u" name=", field_->GetText(),
                       u" folder=",
                       node ? FolderPath(node->parent()) : u"-"});
}

BEGIN_METADATA(BookmarkEditor)
END_METADATA

}  // namespace starry
