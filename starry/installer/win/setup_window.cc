// Copyright 2026 The Starry Browser Authors

#include "starry/installer/win/setup_window.h"

#include <windows.h>

#include <d2d1.h>
#include <d2d1_1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellapi.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

#include "starry/common/platform_branding.h"
#include "starry/installer/win/resource.h"
#include "starry/installer/win/strings.h"

namespace starry::installer {

namespace {

using Microsoft::WRL::ComPtr;

constexpr float kWidth = 600.0f;
constexpr float kHeight = 400.0f;
constexpr float kBarWidth = 300.0f;
constexpr float kBarLeft = (kWidth - kBarWidth) / 2;
constexpr float kPi = 3.14159265f;
constexpr float kHorizonRadius = 1200.0f;
constexpr float kGlowRadius = kHorizonRadius + 140.0f;
constexpr UINT kInstallFinishedMessage = WM_APP + 1;
constexpr UINT_PTR kFinishTimer = 1;
// The bar fills in 0.3 s; the next screen follows a moment after.
constexpr double kFinishDelay = 0.32;
constexpr ULONGLONG kRequiredSpace = 1024ull * 1024 * 1024;

// setup.exe's exit codes (InstallStatus in chrome/installer/util/
// util_constants.h) and mini_installer's own (mini_installer/exit_code.h).
enum ExitCode : DWORD {
  kExistingVersionLaunched = 3,
  kHigherVersionExists = 4,
  kUserLevelInstallExists = 5,
  kSystemLevelInstallExists = 6,
  kOsNotSupported = 9,
  kTempDirFailed = 11,
  kUncompressionFailed = 12,
  kInvalidArchive = 13,
  kInsufficientRights = 14,
  kChromeRunning = 16,
  kInstallDirInUse = 28,
  kSameVersionRepairFailed = 31,
  kCpuNotSupported = 54,
  kUnpackingFailed = 57,
  kSetupSingletonFailed = 60,
  kUnableToExtractArchive = 112,
  kUnableToExtractSetupExe = 115,
  kUnableToExtractSetup = 116,
};

enum class Screen {
  kWelcome,
  kOptions,
  kInstalling,
  kDone,
  kError,
  kInstalled,
  kNewer,
};

enum class Hit {
  kNone,
  kClose,
  kMinimize,
  kPrimary,
  kSecondary,
  kBack,
  kCardMe,
  kCardAll,
  kDesktop,
};

enum class ErrorAction { kRetry, kOpen, kClose };

enum TextStyle : size_t {
  kTitle,
  kHeading,
  kBody,
  kSmall,
  kGroup,
  kLink,
  kButton,
  kCardTitle,
  kTag,
  kGlyph,
  kGlyphLarge,
  kBadgeMark,
  kTextStyleCount,
};

struct StyleSpec {
  const wchar_t* family;
  float size;
  DWRITE_FONT_WEIGHT weight;
};

constexpr wchar_t kFont[] = L"Segoe UI";
constexpr wchar_t kIconFont[] = L"Segoe MDL2 Assets";
constexpr std::array<StyleSpec, kTextStyleCount> kStyles = {{
    {kFont, 26.0f, DWRITE_FONT_WEIGHT_BOLD},        // kTitle
    {kFont, 19.0f, DWRITE_FONT_WEIGHT_BOLD},        // kHeading
    {kFont, 13.0f, DWRITE_FONT_WEIGHT_NORMAL},      // kBody
    {kFont, 11.5f, DWRITE_FONT_WEIGHT_NORMAL},      // kSmall
    {kFont, 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD},   // kGroup
    {kFont, 12.5f, DWRITE_FONT_WEIGHT_NORMAL},      // kLink
    {kFont, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD},   // kButton
    {kFont, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD},   // kCardTitle
    {kFont, 10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD},   // kTag
    {kIconFont, 10.0f, DWRITE_FONT_WEIGHT_NORMAL},  // kGlyph
    {kIconFont, 14.0f, DWRITE_FONT_WEIGHT_NORMAL},  // kGlyphLarge
    {kFont, 15.0f, DWRITE_FONT_WEIGHT_BLACK},       // kBadgeMark
}};

constexpr wchar_t kGlyphClose[] = L"\xE8BB";
constexpr wchar_t kGlyphMinimize[] = L"\xE921";
constexpr wchar_t kGlyphBack[] = L"\xE76B";
constexpr wchar_t kGlyphCheck[] = L"\xE73E";
constexpr wchar_t kGlyphContact[] = L"\xE77B";
constexpr wchar_t kGlyphPeople[] = L"\xE716";

constexpr D2D1_COLOR_F Rgba(int r, int g, int b, float a = 1.0f) {
  return D2D1_COLOR_F{r / 255.0f, g / 255.0f, b / 255.0f, a};
}

constexpr D2D1_COLOR_F kOn = Rgba(255, 255, 255);
constexpr D2D1_COLOR_F kSub = Rgba(232, 226, 255, 0.68f);
constexpr D2D1_COLOR_F kTiny = Rgba(232, 226, 255, 0.52f);
constexpr D2D1_COLOR_F kGroupColor = Rgba(232, 226, 255, 0.58f);

struct HitRect {
  D2D1_RECT_F rect;
  Hit hit;
};

struct Star {
  float x;
  float y;
  float size;
  float alpha;
};

struct Horizon {
  float height;
  float glow;
};

float Clamp01(float t) {
  return std::clamp(t, 0.0f, 1.0f);
}

// cubic-bezier(0.2, 0.8, 0.2, 1), Starry's Motion.chromeLayout curve.
float EaseOut(float t) {
  t = Clamp01(t);
  float s = t;
  for (int i = 0; i < 8; ++i) {
    const float u = 1.0f - s;
    const float x = 0.6f * u * u * s + 0.6f * u * s * s + s * s * s;
    const float slope = 0.6f * u * u + 2.4f * s * s;
    if (slope < 1e-4f) {
      break;
    }
    s = Clamp01(s - (x - t) / slope);
  }
  const float u = 1.0f - s;
  return 2.4f * u * u * s + 3.0f * u * s * s + s * s * s;
}

// Unit spring from 0 to 1 (`response` in seconds, `damping` ratio).
float Spring(float t, float response, float damping) {
  if (t <= 0.0f) {
    return 0.0f;
  }
  const float omega = 2.0f * kPi / response;
  if (damping >= 1.0f) {
    return 1.0f - (1.0f + omega * t) * std::exp(-omega * t);
  }
  const float damped = omega * std::sqrt(1.0f - damping * damping);
  return 1.0f - std::exp(-damping * omega * t) *
                    (std::cos(damped * t) +
                     damping * omega / damped * std::sin(damped * t));
}

double Hash(double n) {
  const double x = std::sin(n * 127.1 + 311.7) * 43758.5453;
  return x - std::floor(x);
}

std::array<Star, 70> MakeStars() {
  std::array<Star, 70> stars = {};
  for (size_t i = 0; i < stars.size(); ++i) {
    const double k = static_cast<double>(i);
    Star& star = stars[i];
    star.x = static_cast<float>(Hash(k * 3.17) * kWidth);
    star.y = static_cast<float>(std::pow(Hash(k * 7.73), 1.4) * 0.86 * kHeight);
    star.size = static_cast<float>(0.7 + std::pow(Hash(k * 1.31), 3.0) * 2.0);
    star.alpha = static_cast<float>(0.3 + Hash(k * 9.2) * 0.65);
  }
  return stars;
}

Horizon HorizonFor(Screen screen) {
  switch (screen) {
    case Screen::kOptions:
      return {16.0f, 0.45f};
    case Screen::kInstalling:
      return {34.0f, 0.8f};
    case Screen::kDone:
      return {66.0f, 1.35f};
    case Screen::kError:
      return {22.0f, 0.35f};
    case Screen::kWelcome:
    case Screen::kInstalled:
    case Screen::kNewer:
      return {54.0f, 1.0f};
  }
  return {54.0f, 1.0f};
}

class SetupWindow {
 public:
  SetupWindow(HINSTANCE instance, const InstallOptions& defaults);
  SetupWindow(const SetupWindow&) = delete;
  SetupWindow& operator=(const SetupWindow&) = delete;
  ~SetupWindow();

  int Run();

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd,
                                  UINT message,
                                  WPARAM wparam,
                                  LPARAM lparam);
  LRESULT OnMessage(UINT message, WPARAM wparam, LPARAM lparam);

  double Now() const;
  void Invalidate();
  bool Animating(double now) const;
  Horizon CurrentHorizon(double now) const;
  float IntroScale(double now) const;
  float Progress(double now) const;
  bool Upgrading() const;
  bool NeedsShield() const;
  std::wstring InstallLabel() const;
  void Show(Screen screen, int direction);
  void ShowError(const std::wstring& text,
                 const std::wstring& code,
                 ErrorAction action);

  Hit HitAt(POINT client) const;
  void SetHover(Hit hit);
  void OnClick(Hit hit);
  void StartInstall();
  void OnInstallFinished(const InstallResult& result);
  void Finish();
  void LaunchAndClose(const std::wstring& exe);

  bool CreateWindowAndShow();
  bool CreateDeviceResources();
  void DiscardDeviceResources();
  ComPtr<ID2D1GradientStopCollection> Stops(
      std::initializer_list<D2D1_GRADIENT_STOP> stops);
  ComPtr<ID2D1Bitmap> BitmapFromWic(IWICBitmapSource* source);
  void LoadImages();
  void Render();
  void DrawBackground(double now);
  void DrawScreen(Screen screen,
                  double now,
                  float alpha,
                  float dx,
                  float dy,
                  bool record);
  void DrawWelcome(double now, bool record);
  void DrawOptions(bool record);
  void DrawInstalling(double now);
  void DrawDone(double now, bool record);
  void DrawError(bool record);
  void DrawInstalled(bool newer, double now, bool record);
  void DrawCaptionButtons();
  void DrawCard(D2D1_RECT_F rect,
                const wchar_t* glyph,
                const std::wstring& title,
                const std::wstring& detail,
                bool recommended,
                bool selected,
                Hit hit,
                bool record);
  void DrawButtonRow(const std::wstring& link,
                     const std::wstring& button,
                     float top,
                     bool record);
  void PaintIcon(float cx, float cy, float size, float scale);
  void DrawBadge(float cx, float cy, bool ok, float scale, float alpha);
  ComPtr<IDWriteTextLayout> MakeLayout(const std::wstring& text,
                                       TextStyle style,
                                       float width,
                                       float height);
  float TextWidth(const std::wstring& text, TextStyle style);
  void Label(const std::wstring& text,
             TextStyle style,
             D2D1_RECT_F box,
             D2D1_COLOR_F color,
             DWRITE_TEXT_ALIGNMENT align,
             bool middle = false,
             bool trim = false);
  float PillWidth(const std::wstring& label, bool shield);
  void DrawPill(const std::wstring& label,
                float left,
                float top,
                bool shield,
                Hit hit,
                bool record);
  float LinkWidth(const std::wstring& text, bool back);
  void DrawLink(const std::wstring& text,
                float left,
                float top,
                Hit hit,
                bool record,
                bool back = false);
  void FillShape(D2D1_RECT_F rect, float radius, D2D1_COLOR_F color);
  void StrokeShape(D2D1_RECT_F rect,
                   float radius,
                   D2D1_COLOR_F color,
                   float width);
  void AddHit(D2D1_RECT_F rect, Hit hit, bool record);

  HINSTANCE instance_;
  HWND hwnd_ = nullptr;
  UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
  LARGE_INTEGER frequency_ = {};
  LARGE_INTEGER start_ = {};
  bool elevated_ = false;
  bool reduced_motion_ = false;

  InstallOptions options_;
  InstalledInfo installed_;
  std::wstring payload_version_;
  std::wstring payload_display_version_;
  std::wstring user_dir_;
  std::wstring system_dir_;

  Screen screen_ = Screen::kWelcome;
  Screen previous_ = Screen::kWelcome;
  bool has_previous_ = false;
  int direction_ = 1;
  double opened_at_ = 0;
  double screen_since_ = 0;
  Horizon horizon_from_ = {};
  Horizon horizon_to_ = {};
  double horizon_since_ = 0;
  float horizon_duration_ = 0.9f;

  bool installing_ = false;
  bool finishing_ = false;
  double install_since_ = 0;
  double finish_since_ = 0;
  float finish_from_ = 0;
  InstallResult result_;
  std::wstring error_text_;
  std::wstring error_code_;
  ErrorAction error_action_ = ErrorAction::kRetry;
  bool error_has_log_ = false;
  std::wstring launch_exe_;

  Hit hover_ = Hit::kNone;
  Hit pressed_ = Hit::kNone;
  bool tracking_mouse_ = false;
  std::vector<HitRect> hits_;
  std::array<Star, 70> stars_;

  ComPtr<ID2D1Factory> d2d_;
  ComPtr<IDWriteFactory> dwrite_;
  ComPtr<IWICImagingFactory> wic_;
  std::array<ComPtr<IDWriteTextFormat>, kTextStyleCount> formats_;
  ComPtr<IDWriteInlineObject> ellipsis_;
  ComPtr<ID2D1HwndRenderTarget> target_;
  ComPtr<ID2D1DeviceContext> context_;
  ComPtr<ID2D1SolidColorBrush> brush_;
  ComPtr<ID2D1LinearGradientBrush> sky_;
  ComPtr<ID2D1RadialGradientBrush> sky_top_;
  ComPtr<ID2D1RadialGradientBrush> horizon_glow_;
  ComPtr<ID2D1LinearGradientBrush> bar_;
  ComPtr<ID2D1Layer> layer_;
  ComPtr<ID2D1Bitmap> logo_;
  ComPtr<ID2D1Bitmap> shield_;
};

SetupWindow::SetupWindow(HINSTANCE instance, const InstallOptions& defaults)
    : instance_(instance), options_(defaults), stars_(MakeStars()) {
  QueryPerformanceFrequency(&frequency_);
  QueryPerformanceCounter(&start_);
  elevated_ = IsElevated();
  BOOL animations = TRUE;
  SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
  reduced_motion_ = !animations;
  installed_ = FindInstalled();
  payload_version_ = PayloadVersion();
  payload_display_version_ = PayloadDisplayVersion();
  user_dir_ = InstallDir(false);
  system_dir_ = InstallDir(true);
  if (installed_.found) {
    // Updates go wherever it is installed now.
    options_.system_level = installed_.system_level;
    const int order = CompareVersions(installed_.version, payload_version_);
    if (order == 0) {
      screen_ = Screen::kInstalled;
    } else if (order > 0) {
      screen_ = Screen::kNewer;
    }
  }
}

SetupWindow::~SetupWindow() = default;

int SetupWindow::Run() {
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               d2d_.GetAddressOf())) ||
      FAILED(DWriteCreateFactory(
          DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
          reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())))) {
    return 1;
  }
  // Without WIC there is no icon, which is not worth failing for.
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                   IID_PPV_ARGS(&wic_));
  for (size_t i = 0; i < kStyles.size(); ++i) {
    const StyleSpec& spec = kStyles[i];
    if (FAILED(dwrite_->CreateTextFormat(spec.family, nullptr, spec.weight,
                                         DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, spec.size,
                                         LocaleName(), &formats_[i]))) {
      return 1;
    }
  }
  if (!CreateWindowAndShow()) {
    return 1;
  }
  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return 0;
}

// static
LRESULT CALLBACK SetupWindow::WndProc(HWND hwnd,
                                      UINT message,
                                      WPARAM wparam,
                                      LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const CREATESTRUCTW* create =
        reinterpret_cast<const CREATESTRUCTW*>(lparam);
    SetupWindow* self = static_cast<SetupWindow*>(create->lpCreateParams);
    self->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  SetupWindow* self =
      reinterpret_cast<SetupWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  return self ? self->OnMessage(message, wparam, lparam)
              : DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT SetupWindow::OnMessage(UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_NCCALCSIZE:
      // The whole window is client area; DWM still draws the shadow.
      if (wparam) {
        return 0;
      }
      break;
    case WM_NCACTIVATE:
      // Keep DWM from painting a frame when the window is deactivated.
      return DefWindowProcW(hwnd_, message, wparam, -1);
    case WM_NCHITTEST: {
      POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(hwnd_, &point);
      return HitAt(point) == Hit::kNone ? HTCAPTION : HTCLIENT;
    }
    case WM_SETCURSOR:
      if (LOWORD(lparam) == HTCLIENT) {
        POINT cursor = {};
        GetCursorPos(&cursor);
        ScreenToClient(hwnd_, &cursor);
        const Hit hit = HitAt(cursor);
        if (hit != Hit::kNone && hit != Hit::kClose && hit != Hit::kMinimize) {
          SetCursor(LoadCursorW(nullptr, IDC_HAND));
          return TRUE;
        }
      }
      break;
    case WM_MOUSEMOVE: {
      const POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      SetHover(HitAt(point));
      if (!tracking_mouse_) {
        TRACKMOUSEEVENT track = {};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd_;
        tracking_mouse_ = TrackMouseEvent(&track) != FALSE;
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      tracking_mouse_ = false;
      SetHover(Hit::kNone);
      return 0;
    case WM_LBUTTONDOWN: {
      const POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      pressed_ = HitAt(point);
      SetCapture(hwnd_);
      Invalidate();
      return 0;
    }
    case WM_LBUTTONUP: {
      const POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const Hit hit = HitAt(point);
      const Hit pressed = pressed_;
      pressed_ = Hit::kNone;
      ReleaseCapture();
      Invalidate();
      if (hit != Hit::kNone && hit == pressed) {
        OnClick(hit);
      }
      return 0;
    }
    case WM_KEYDOWN:
      if (wparam == VK_RETURN) {
        for (const HitRect& hit : hits_) {
          if (hit.hit == Hit::kPrimary) {
            OnClick(Hit::kPrimary);
            break;
          }
        }
      } else if (wparam == VK_ESCAPE) {
        OnClick(screen_ == Screen::kOptions ? Hit::kBack : Hit::kClose);
      }
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT paint;
      BeginPaint(hwnd_, &paint);
      Render();
      EndPaint(hwnd_, &paint);
      // WM_TIMER is only delivered while no WM_PAINT is pending, and this
      // loop keeps one pending for the whole install, so the finish timer
      // never fires on its own. Finish from here; the timer only covers a
      // minimized window, which gets no WM_PAINT.
      if (finishing_ && Now() - finish_since_ >= kFinishDelay) {
        KillTimer(hwnd_, kFinishTimer);
        Finish();
      }
      // Render target presents are vsync-paced, so this runs once a frame.
      if (Animating(Now())) {
        Invalidate();
      }
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      if (target_) {
        target_->Resize(D2D1::SizeU(LOWORD(lparam), HIWORD(lparam)));
      }
      return 0;
    case WM_DPICHANGED: {
      dpi_ = HIWORD(wparam);
      const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
      SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                   suggested->right - suggested->left,
                   suggested->bottom - suggested->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      if (target_) {
        target_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
      }
      Invalidate();
      return 0;
    }
    case WM_TIMER:
      if (wparam == kFinishTimer) {
        KillTimer(hwnd_, kFinishTimer);
        if (finishing_) {
          Finish();
        }
      }
      return 0;
    case kInstallFinishedMessage: {
      InstallResult result;
      result.exit_code = static_cast<DWORD>(wparam);
      result.started = (lparam & 1) != 0;
      result.cancelled = (lparam & 2) != 0;
      result.error = static_cast<DWORD>(lparam >> 2);
      OnInstallFinished(result);
      return 0;
    }
    case WM_CLOSE:
      // setup.exe would carry on without us; wait for it.
      if (!installing_) {
        DestroyWindow(hwnd_);
      }
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, message, wparam, lparam);
}

double SetupWindow::Now() const {
  LARGE_INTEGER now = {};
  QueryPerformanceCounter(&now);
  return static_cast<double>(now.QuadPart - start_.QuadPart) /
         static_cast<double>(frequency_.QuadPart);
}

void SetupWindow::Invalidate() {
  InvalidateRect(hwnd_, nullptr, FALSE);
}

bool SetupWindow::Animating(double now) const {
  return installing_ || now - screen_since_ < 1.0 ||
         now - horizon_since_ < horizon_duration_ || now - opened_at_ < 0.8;
}

Horizon SetupWindow::CurrentHorizon(double now) const {
  const float k =
      EaseOut(static_cast<float>((now - horizon_since_) / horizon_duration_));
  return {
      horizon_from_.height + (horizon_to_.height - horizon_from_.height) * k,
      horizon_from_.glow + (horizon_to_.glow - horizon_from_.glow) * k};
}

float SetupWindow::IntroScale(double now) const {
  if (has_previous_ || reduced_motion_) {
    return 1.0f;
  }
  return 0.6f + 0.4f * Spring(static_cast<float>(now - opened_at_) - 0.1f,
                              0.42f, 0.86f);
}

// setup.exe reports no progress: creep to 90% over the usual 25 seconds, then
// fill up in 0.3 s once it is done.
float SetupWindow::Progress(double now) const {
  const float waited =
      Clamp01(static_cast<float>((now - install_since_) / 25.0));
  const float waiting = 0.9f * (1.0f - (1.0f - waited) * (1.0f - waited));
  if (!finishing_) {
    return waiting;
  }
  return finish_from_ +
         (1.0f - finish_from_) *
             EaseOut(static_cast<float>((now - finish_since_) / 0.3));
}

bool SetupWindow::Upgrading() const {
  return installed_.found &&
         CompareVersions(installed_.version, payload_version_) < 0;
}

bool SetupWindow::NeedsShield() const {
  return options_.system_level && !elevated_;
}

std::wstring SetupWindow::InstallLabel() const {
  return Upgrading() ? L("installer.welcome.update")
                     : L("installer.welcome.install");
}

void SetupWindow::Show(Screen screen, int direction) {
  const double now = Now();
  horizon_from_ = CurrentHorizon(now);
  horizon_to_ = HorizonFor(screen);
  horizon_since_ = now;
  horizon_duration_ = reduced_motion_ ? 0.2f : 0.9f;
  previous_ = screen_;
  has_previous_ = true;
  screen_ = screen;
  direction_ = direction;
  screen_since_ = now;
  hover_ = Hit::kNone;
  pressed_ = Hit::kNone;
  Invalidate();
}

void SetupWindow::ShowError(const std::wstring& text,
                            const std::wstring& code,
                            ErrorAction action) {
  error_text_ = text;
  error_code_ = code;
  error_action_ = action;
  Show(Screen::kError, 1);
}

Hit SetupWindow::HitAt(POINT client) const {
  const float scale =
      static_cast<float>(USER_DEFAULT_SCREEN_DPI) / static_cast<float>(dpi_);
  const float x = static_cast<float>(client.x) * scale;
  const float y = static_cast<float>(client.y) * scale;
  // Later rectangles (the caption buttons) sit on top.
  for (size_t i = hits_.size(); i > 0; --i) {
    const HitRect& hit = hits_[i - 1];
    if (x >= hit.rect.left && x < hit.rect.right && y >= hit.rect.top &&
        y < hit.rect.bottom) {
      return hit.hit;
    }
  }
  return Hit::kNone;
}

void SetupWindow::SetHover(Hit hit) {
  if (hover_ != hit) {
    hover_ = hit;
    Invalidate();
  }
}

void SetupWindow::OnClick(Hit hit) {
  switch (hit) {
    case Hit::kNone:
      return;
    case Hit::kClose:
      PostMessageW(hwnd_, WM_CLOSE, 0, 0);
      return;
    case Hit::kMinimize:
      ShowWindow(hwnd_, SW_MINIMIZE);
      return;
    case Hit::kBack:
      Show(Screen::kWelcome, -1);
      return;
    case Hit::kCardMe:
      options_.system_level = false;
      Invalidate();
      return;
    case Hit::kCardAll:
      options_.system_level = true;
      Invalidate();
      return;
    case Hit::kDesktop:
      options_.desktop_shortcut = !options_.desktop_shortcut;
      Invalidate();
      return;
    case Hit::kPrimary:
    case Hit::kSecondary:
      break;
  }
  const bool primary = hit == Hit::kPrimary;
  switch (screen_) {
    case Screen::kWelcome:
      if (primary) {
        StartInstall();
      } else {
        Show(Screen::kOptions, 1);
      }
      return;
    case Screen::kOptions:
      if (primary) {
        StartInstall();
      }
      return;
    case Screen::kInstalling:
      return;
    case Screen::kDone:
      LaunchAndClose(launch_exe_);
      return;
    case Screen::kError:
      if (!primary) {
        OpenInstallerLog();
      } else if (error_action_ == ErrorAction::kRetry) {
        StartInstall();
      } else if (error_action_ == ErrorAction::kOpen) {
        LaunchAndClose(installed_.exe);
      } else {
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
      }
      return;
    case Screen::kInstalled:
      if (primary) {
        LaunchAndClose(installed_.exe);
      } else {
        StartInstall();
      }
      return;
    case Screen::kNewer:
      LaunchAndClose(installed_.exe);
      return;
  }
}

void SetupWindow::StartInstall() {
  std::wstring drive;
  if (!HasFreeSpace(options_.system_level, kRequiredSpace, &drive)) {
    error_has_log_ = false;
    ShowError(Fill(L("installer.error.disk"), "drive", drive), std::wstring(),
              ErrorAction::kRetry);
    return;
  }
  installing_ = true;
  finishing_ = false;
  install_since_ = Now();
  Show(Screen::kInstalling, 1);
  const HWND hwnd = hwnd_;
  const InstallOptions options = options_;
  std::thread([hwnd, options] {
    const HRESULT com = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const InstallResult result = RunInstaller(options);
    if (SUCCEEDED(com)) {
      CoUninitialize();
    }
    const LPARAM flags = (result.started ? 1 : 0) | (result.cancelled ? 2 : 0) |
                         (static_cast<LPARAM>(result.error) << 2);
    PostMessageW(hwnd, kInstallFinishedMessage, result.exit_code, flags);
  }).detach();
}

void SetupWindow::OnInstallFinished(const InstallResult& result) {
  const double now = Now();
  result_ = result;
  finish_from_ = Progress(now);
  finishing_ = true;
  finish_since_ = now;
  SetTimer(hwnd_, kFinishTimer, static_cast<UINT>(kFinishDelay * 1000),
           nullptr);
  Invalidate();
}

void SetupWindow::Finish() {
  installing_ = false;
  finishing_ = false;
  const DWORD code = result_.exit_code;
  if (result_.started && (code == 0 || code == kExistingVersionLaunched)) {
    const InstalledInfo installed = FindInstalled();
    launch_exe_ = installed.found
                      ? installed.exe
                      : (options_.system_level ? system_dir_ : user_dir_) +
                            L"\\Application\\" STARRY_WINDOWS_EXE;
    Show(Screen::kDone, 1);
    return;
  }
  if (!result_.started) {
    error_has_log_ = false;
    if (result_.cancelled) {
      ShowError(L("installer.error.cancelled"), std::wstring(),
                ErrorAction::kRetry);
    } else {
      ShowError(L("installer.error.generic"),
                Fill(L("installer.error.code"), "code",
                     std::to_wstring(result_.error)),
                ErrorAction::kRetry);
    }
    return;
  }
  error_has_log_ = true;
  const std::wstring code_text =
      Fill(L("installer.error.code"), "code", std::to_wstring(code));
  switch (code) {
    case kHigherVersionExists:
      installed_ = FindInstalled();
      if (installed_.found) {
        Show(Screen::kNewer, 1);
      } else {
        ShowError(L("installer.error.generic"), code_text, ErrorAction::kClose);
      }
      return;
    case kUserLevelInstallExists:
      ShowError(L("installer.error.userExists"), code_text,
                ErrorAction::kClose);
      return;
    case kSystemLevelInstallExists:
      installed_ = FindInstalled();
      ShowError(L("installer.error.systemExists"), code_text,
                installed_.found ? ErrorAction::kOpen : ErrorAction::kClose);
      return;
    case kOsNotSupported:
    case kCpuNotSupported:
      ShowError(L("installer.error.os"), code_text, ErrorAction::kClose);
      return;
    case kTempDirFailed:
    case kUncompressionFailed:
    case kInvalidArchive:
    case kUnpackingFailed:
    case kUnableToExtractArchive:
    case kUnableToExtractSetupExe:
    case kUnableToExtractSetup:
      ShowError(L("installer.error.files"), code_text, ErrorAction::kRetry);
      return;
    case kInsufficientRights:
      ShowError(L("installer.error.rights"), code_text, ErrorAction::kRetry);
      return;
    case kChromeRunning:
    case kInstallDirInUse:
    case kSameVersionRepairFailed:
      ShowError(L("installer.error.running"), code_text, ErrorAction::kRetry);
      return;
    case kSetupSingletonFailed:
      ShowError(L("installer.error.busy"), code_text, ErrorAction::kRetry);
      return;
    default:
      ShowError(L("installer.error.generic"), code_text, ErrorAction::kRetry);
      return;
  }
}

void SetupWindow::LaunchAndClose(const std::wstring& exe) {
  LaunchBrowser(exe);
  DestroyWindow(hwnd_);
}

bool SetupWindow::CreateWindowAndShow() {
  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = &SetupWindow::WndProc;
  window_class.hInstance = instance_;
  window_class.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_STARRY_SETUP));
  window_class.hIconSm = static_cast<HICON>(
      LoadImageW(instance_, MAKEINTRESOURCEW(IDI_STARRY_SETUP), IMAGE_ICON,
                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                 LR_DEFAULTCOLOR));
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = kWindowClass;
  if (!RegisterClassExW(&window_class)) {
    return false;
  }

  POINT cursor = {};
  GetCursorPos(&cursor);
  MONITORINFO monitor = {};
  monitor.cbSize = sizeof(monitor);
  GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor);
  const RECT work = monitor.rcWork;
  const std::wstring title = L("installer.window.title");
  hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, title.c_str(),
                          WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                          work.left, work.top, 1, 1, nullptr, nullptr,
                          instance_, this);
  if (!hwnd_) {
    return false;
  }
  dpi_ = GetDpiForWindow(hwnd_);
  const int width = MulDiv(static_cast<int>(kWidth), static_cast<int>(dpi_),
                           USER_DEFAULT_SCREEN_DPI);
  const int height = MulDiv(static_cast<int>(kHeight), static_cast<int>(dpi_),
                            USER_DEFAULT_SCREEN_DPI);
  const MARGINS margins = {1, 1, 1, 1};
  DwmExtendFrameIntoClientArea(hwnd_, &margins);
  const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
  DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners,
                        sizeof(corners));
  const BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                        sizeof(dark));
  SetWindowPos(hwnd_, nullptr, work.left + (work.right - work.left - width) / 2,
               work.top + (work.bottom - work.top - height) / 2, width, height,
               SWP_NOZORDER | SWP_FRAMECHANGED);

  opened_at_ = Now();
  screen_since_ = opened_at_;
  horizon_to_ = HorizonFor(screen_);
  horizon_from_ = {
      reduced_motion_ ? horizon_to_.height : horizon_to_.height - 160.0f,
      horizon_to_.glow};
  horizon_since_ = opened_at_;
  ShowWindow(hwnd_, SW_SHOWNORMAL);
  UpdateWindow(hwnd_);
  return true;
}

ComPtr<ID2D1GradientStopCollection> SetupWindow::Stops(
    std::initializer_list<D2D1_GRADIENT_STOP> stops) {
  ComPtr<ID2D1GradientStopCollection> collection;
  target_->CreateGradientStopCollection(
      stops.begin(), static_cast<UINT32>(stops.size()), &collection);
  return collection;
}

bool SetupWindow::CreateDeviceResources() {
  if (target_) {
    return true;
  }
  RECT client = {};
  GetClientRect(hwnd_, &client);
  const D2D1_SIZE_U size =
      D2D1::SizeU(static_cast<UINT32>(client.right - client.left),
                  static_cast<UINT32>(client.bottom - client.top));
  const float dpi = static_cast<float>(dpi_);
  if (FAILED(d2d_->CreateHwndRenderTarget(
          D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                       D2D1::PixelFormat(), dpi, dpi),
          D2D1::HwndRenderTargetProperties(hwnd_, size), &target_))) {
    return false;
  }
  target_.As(&context_);
  target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  target_->CreateSolidColorBrush(kOn, &brush_);
  target_->CreateLinearGradientBrush(
      D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0),
                                          D2D1::Point2F(0, kHeight)),
      Stops({{0.0f, Rgba(14, 11, 38)},
             {0.45f, Rgba(23, 18, 56)},
             {1.0f, Rgba(34, 26, 77)}})
          .Get(),
      &sky_);
  target_->CreateRadialGradientBrush(
      D2D1::RadialGradientBrushProperties(D2D1::Point2F(kWidth / 2, 0),
                                          D2D1::Point2F(0, 0), kWidth * 0.9f,
                                          kHeight * 0.6f),
      Stops({{0.0f, Rgba(36, 27, 86)},
             {0.7f, Rgba(36, 27, 86, 0.0f)},
             {1.0f, Rgba(36, 27, 86, 0.0f)}})
          .Get(),
      &sky_top_);
  target_->CreateRadialGradientBrush(
      D2D1::RadialGradientBrushProperties(
          D2D1::Point2F(0, 0), D2D1::Point2F(0, 0), kGlowRadius, kGlowRadius),
      Stops({{0.0f, Rgba(254, 121, 113, 0.5f)},
             {kHorizonRadius / kGlowRadius, Rgba(254, 121, 113, 0.5f)},
             {1.0f, Rgba(254, 121, 113, 0.0f)}})
          .Get(),
      &horizon_glow_);
  target_->CreateLinearGradientBrush(
      D2D1::LinearGradientBrushProperties(
          D2D1::Point2F(kBarLeft, 0), D2D1::Point2F(kBarLeft + kBarWidth, 0)),
      Stops({{0.0f, Rgba(177, 77, 121)}, {1.0f, Rgba(254, 121, 113)}}).Get(),
      &bar_);
  target_->CreateLayer(&layer_);
  LoadImages();
  return brush_ && sky_ && sky_top_ && horizon_glow_ && bar_ && layer_;
}

void SetupWindow::DiscardDeviceResources() {
  shield_.Reset();
  logo_.Reset();
  layer_.Reset();
  bar_.Reset();
  horizon_glow_.Reset();
  sky_top_.Reset();
  sky_.Reset();
  brush_.Reset();
  context_.Reset();
  target_.Reset();
}

ComPtr<ID2D1Bitmap> SetupWindow::BitmapFromWic(IWICBitmapSource* source) {
  ComPtr<IWICFormatConverter> converter;
  ComPtr<ID2D1Bitmap> bitmap;
  if (SUCCEEDED(wic_->CreateFormatConverter(&converter)) &&
      SUCCEEDED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA,
                                      WICBitmapDitherTypeNone, nullptr, 0.0,
                                      WICBitmapPaletteTypeMedianCut))) {
    target_->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap);
  }
  return bitmap;
}

void SetupWindow::LoadImages() {
  if (!wic_) {
    return;
  }
  HRSRC resource =
      FindResourceW(instance_, MAKEINTRESOURCEW(IDR_STARRY_LOGO), RT_RCDATA);
  HGLOBAL loaded = resource ? LoadResource(instance_, resource) : nullptr;
  void* data = loaded ? LockResource(loaded) : nullptr;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  if (data && SUCCEEDED(wic_->CreateStream(&stream)) &&
      SUCCEEDED(stream->InitializeFromMemory(
          static_cast<BYTE*>(data), SizeofResource(instance_, resource))) &&
      SUCCEEDED(wic_->CreateDecoderFromStream(
          stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) &&
      SUCCEEDED(decoder->GetFrame(0, &frame))) {
    logo_ = BitmapFromWic(frame.Get());
  }
  SHSTOCKICONINFO shield = {};
  shield.cbSize = sizeof(shield);
  if (SUCCEEDED(SHGetStockIconInfo(SIID_SHIELD, SHGSI_ICON | SHGSI_SMALLICON,
                                   &shield))) {
    ComPtr<IWICBitmap> icon;
    if (SUCCEEDED(wic_->CreateBitmapFromHICON(shield.hIcon, &icon))) {
      shield_ = BitmapFromWic(icon.Get());
    }
    DestroyIcon(shield.hIcon);
  }
}

void SetupWindow::Render() {
  if (!CreateDeviceResources()) {
    return;
  }
  const double now = Now();
  hits_.clear();
  target_->BeginDraw();
  target_->SetTransform(D2D1::Matrix3x2F::Identity());
  DrawBackground(now);

  const float t = static_cast<float>(now - screen_since_);
  const float direction = static_cast<float>(direction_);
  if (has_previous_) {
    const float fade = reduced_motion_ ? 0.2f : 0.16f;
    if (t < fade) {
      DrawScreen(
          previous_, now, 1.0f - t / fade,
          reduced_motion_ ? 0.0f : -28.0f * direction * EaseOut(t / fade), 0.0f,
          false);
    }
  }
  float alpha = 1.0f;
  float dx = 0.0f;
  float dy = 0.0f;
  if (reduced_motion_) {
    alpha = Clamp01(t / 0.2f);
  } else if (has_previous_) {
    alpha = Clamp01((t - 0.05f) / 0.26f);
    dx = 32.0f * direction * (1.0f - Spring(t - 0.05f, 0.42f, 0.86f));
  } else {
    alpha = Clamp01((t - 0.15f) / 0.3f);
    dy = 14.0f * (1.0f - Spring(t - 0.15f, 0.42f, 0.86f));
  }
  DrawScreen(screen_, now, alpha, dx, dy, true);
  target_->SetTransform(D2D1::Matrix3x2F::Identity());
  DrawCaptionButtons();
  if (target_->EndDraw() == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
    DiscardDeviceResources();
  }
}

void SetupWindow::DrawBackground(double now) {
  const D2D1_RECT_F all = D2D1::RectF(0, 0, kWidth, kHeight);
  target_->FillRectangle(all, sky_.Get());
  target_->FillRectangle(all, sky_top_.Get());

  const float stars =
      reduced_motion_ ? 1.0f
                      : Clamp01(static_cast<float>((now - opened_at_) / 0.5));
  for (const Star& star : stars_) {
    brush_->SetColor(Rgba(255, 255, 255, star.alpha * stars));
    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(star.x, star.y),
                                       star.size / 2, star.size / 2),
                         brush_.Get());
  }

  const Horizon horizon = CurrentHorizon(now);
  const D2D1_POINT_2F center =
      D2D1::Point2F(kWidth / 2, kHeight - horizon.height + kHorizonRadius);
  const D2D1_POINT_2F glow_center = D2D1::Point2F(center.x, center.y - 24);
  horizon_glow_->SetCenter(glow_center);
  horizon_glow_->SetOpacity(Clamp01(horizon.glow * 0.72f));
  target_->FillEllipse(D2D1::Ellipse(glow_center, kGlowRadius, kGlowRadius),
                       horizon_glow_.Get());
  brush_->SetColor(Rgba(177, 77, 121, 0.75f));
  target_->FillEllipse(
      D2D1::Ellipse(center, kHorizonRadius + 10, kHorizonRadius + 10),
      brush_.Get());
  brush_->SetColor(Rgba(254, 121, 113));
  target_->FillEllipse(D2D1::Ellipse(center, kHorizonRadius, kHorizonRadius),
                       brush_.Get());
}

void SetupWindow::DrawScreen(Screen screen,
                             double now,
                             float alpha,
                             float dx,
                             float dy,
                             bool record) {
  target_->SetTransform(D2D1::Matrix3x2F::Translation(dx, dy));
  const bool fading = alpha < 0.999f;
  if (fading) {
    target_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                             D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), alpha),
                       layer_.Get());
  }
  switch (screen) {
    case Screen::kWelcome:
      DrawWelcome(now, record);
      break;
    case Screen::kOptions:
      DrawOptions(record);
      break;
    case Screen::kInstalling:
      DrawInstalling(now);
      break;
    case Screen::kDone:
      DrawDone(now, record);
      break;
    case Screen::kError:
      DrawError(record);
      break;
    case Screen::kInstalled:
      DrawInstalled(false, now, record);
      break;
    case Screen::kNewer:
      DrawInstalled(true, now, record);
      break;
  }
  if (fading) {
    target_->PopLayer();
  }
}

void SetupWindow::DrawWelcome(double now, bool record) {
  PaintIcon(kWidth / 2, 106, 88, IntroScale(now));
  Label(STARRY_PRODUCT_FULLNAME_W, kTitle, D2D1::RectF(0, 166, kWidth, 202),
        kOn, DWRITE_TEXT_ALIGNMENT_CENTER);
  const std::wstring detail =
      Upgrading() ? Fill(Fill(L("installer.welcome.upgrade"), "installed",
                              installed_.display_version),
                         "version", payload_display_version_)
                  : Fill(L("installer.welcome.version"), "version",
                         payload_display_version_);
  Label(detail, kBody, D2D1::RectF(0, 204, kWidth, 224), kSub,
        DWRITE_TEXT_ALIGNMENT_CENTER);
  const std::wstring label = InstallLabel();
  const bool shield = NeedsShield();
  DrawPill(label, (kWidth - PillWidth(label, shield)) / 2, 250, shield,
           Hit::kPrimary, record);
  const std::wstring options = L("installer.welcome.options");
  DrawLink(options, (kWidth - LinkWidth(options, false)) / 2, 300,
           Hit::kSecondary, record);
}

void SetupWindow::DrawOptions(bool record) {
  constexpr float kLeft = 40.0f;
  constexpr float kRight = kWidth - 40.0f;
  Label(L("installer.options.title"), kHeading,
        D2D1::RectF(kLeft, 34, kRight, 62), kOn, DWRITE_TEXT_ALIGNMENT_LEADING);
  Label(L("installer.options.who"), kGroup, D2D1::RectF(kLeft, 78, kRight, 96),
        kGroupColor, DWRITE_TEXT_ALIGNMENT_LEADING);
  DrawCard(D2D1::RectF(kLeft, 100, 295, 176), kGlyphContact,
           L("installer.options.me"), L("installer.options.meDetail"), true,
           !options_.system_level, Hit::kCardMe, record);
  DrawCard(D2D1::RectF(305, 100, kRight, 176), kGlyphPeople,
           L("installer.options.all"), L("installer.options.allDetail"), false,
           options_.system_level, Hit::kCardAll, record);
  Label(Fill(L("installer.options.location"), "path",
             options_.system_level ? system_dir_ : user_dir_),
        kSmall, D2D1::RectF(kLeft, 185, kRight, 203),
        Rgba(232, 226, 255, 0.55f), DWRITE_TEXT_ALIGNMENT_LEADING, false, true);

  Label(L("installer.options.shortcuts"), kGroup,
        D2D1::RectF(kLeft, 221, kRight, 239), kGroupColor,
        DWRITE_TEXT_ALIGNMENT_LEADING);
  const D2D1_RECT_F box = D2D1::RectF(kLeft, 247, kLeft + 16, 263);
  if (options_.desktop_shortcut) {
    FillShape(box, 4, kOn);
    Label(kGlyphCheck, kGlyph, box, Rgba(26, 20, 64),
          DWRITE_TEXT_ALIGNMENT_CENTER, true);
  } else {
    StrokeShape(box, 4, Rgba(255, 255, 255, 0.5f), 1.5f);
  }
  const std::wstring desktop = L("installer.options.desktop");
  const float width = TextWidth(desktop, kBody);
  Label(desktop, kBody, D2D1::RectF(kLeft + 25, 245, kLeft + 29 + width, 265),
        kOn, DWRITE_TEXT_ALIGNMENT_LEADING, true);
  AddHit(D2D1::RectF(kLeft - 4, 241, kLeft + 31 + width, 269), Hit::kDesktop,
         record);

  DrawLink(L("installer.options.back"), kLeft, 347, Hit::kBack, record, true);
  const std::wstring label = InstallLabel();
  const bool shield = NeedsShield();
  DrawPill(label, kRight - PillWidth(label, shield), 338, shield, Hit::kPrimary,
           record);
}

void SetupWindow::DrawInstalling(double now) {
  PaintIcon(kWidth / 2, 130, 68, 1.0f);
  Label(L("installer.installing.title"), kHeading,
        D2D1::RectF(0, 184, kWidth, 210), kOn, DWRITE_TEXT_ALIGNMENT_CENTER);
  FillShape(D2D1::RectF(kBarLeft, 226, kBarLeft + kBarWidth, 230), 2,
            Rgba(255, 255, 255, 0.12f));
  const float filled = kBarWidth * Progress(now);
  FillShape(D2D1::RectF(kBarLeft - 3, 223, kBarLeft + filled + 3, 233), 5,
            Rgba(254, 121, 113, 0.16f));
  target_->FillRoundedRectangle(
      D2D1::RoundedRect(D2D1::RectF(kBarLeft, 226, kBarLeft + filled, 230), 2,
                        2),
      bar_.Get());
  Label(L("installer.installing.hint"), kSmall,
        D2D1::RectF(0, 242, kWidth, 260), kTiny, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void SetupWindow::DrawDone(double now, bool record) {
  const float t = static_cast<float>(now - screen_since_) - 0.15f;
  const float scale =
      reduced_motion_ ? 1.0f : 0.5f + 0.5f * Spring(t, 0.42f, 0.6f);
  const float alpha = reduced_motion_ ? 1.0f : Clamp01(t / 0.12f);
  PaintIcon(kWidth / 2, 110, 88, 1.0f);
  DrawBadge(kWidth / 2 + 33, 143, true, scale, alpha);
  Label(L("installer.done.title"), kTitle, D2D1::RectF(0, 170, kWidth, 206),
        kOn, DWRITE_TEXT_ALIGNMENT_CENTER);
  Label(L("installer.done.detail"), kBody, D2D1::RectF(0, 208, kWidth, 228),
        kSub, DWRITE_TEXT_ALIGNMENT_CENTER);
  DrawButtonRow(std::wstring(), L("installer.done.start"), 254, record);
}

void SetupWindow::DrawError(bool record) {
  PaintIcon(kWidth / 2, 104, 76, 1.0f);
  DrawBadge(kWidth / 2 + 27, 131, false, 1.0f, 1.0f);
  Label(L("installer.error.title"), kHeading, D2D1::RectF(0, 160, kWidth, 186),
        kOn, DWRITE_TEXT_ALIGNMENT_CENTER);
  Label(error_text_, kBody, D2D1::RectF(70, 190, kWidth - 70, 230), kSub,
        DWRITE_TEXT_ALIGNMENT_CENTER);
  if (!error_code_.empty()) {
    Label(error_code_, kSmall, D2D1::RectF(0, 232, kWidth, 250), kTiny,
          DWRITE_TEXT_ALIGNMENT_CENTER);
  }
  std::wstring button;
  switch (error_action_) {
    case ErrorAction::kRetry:
      button = L("installer.error.retry");
      break;
    case ErrorAction::kOpen:
      button = L("installer.installed.open");
      break;
    case ErrorAction::kClose:
      button = L("installer.error.close");
      break;
  }
  DrawButtonRow(error_has_log_ ? L("installer.error.log") : std::wstring(),
                button, 262, record);
}

void SetupWindow::DrawInstalled(bool newer, double now, bool record) {
  PaintIcon(kWidth / 2, 110, 88, IntroScale(now));
  if (newer) {
    Label(L("installer.newer.title"), kHeading,
          D2D1::RectF(0, 172, kWidth, 198), kOn, DWRITE_TEXT_ALIGNMENT_CENTER);
    Label(Fill(Fill(L("installer.newer.detail"), "installed",
                    installed_.display_version),
               "version", payload_display_version_),
          kBody, D2D1::RectF(70, 204, kWidth - 70, 240), kSub,
          DWRITE_TEXT_ALIGNMENT_CENTER);
    DrawButtonRow(std::wstring(), L("installer.installed.open"), 254, record);
    return;
  }
  Label(L("installer.installed.title"), kTitle,
        D2D1::RectF(0, 170, kWidth, 206), kOn, DWRITE_TEXT_ALIGNMENT_CENTER);
  Label(Fill(L("installer.installed.detail"), "version",
             installed_.display_version),
        kBody, D2D1::RectF(0, 208, kWidth, 228), kSub,
        DWRITE_TEXT_ALIGNMENT_CENTER);
  DrawButtonRow(L("installer.installed.reinstall"),
                L("installer.installed.open"), 254, record);
}

void SetupWindow::DrawCaptionButtons() {
  const D2D1_RECT_F minimize = D2D1::RectF(kWidth - 92, 0, kWidth - 46, 32);
  const D2D1_RECT_F close = D2D1::RectF(kWidth - 46, 0, kWidth, 32);
  const bool can_close = !installing_;
  const bool close_hot = can_close && hover_ == Hit::kClose;
  if (hover_ == Hit::kMinimize) {
    FillShape(minimize, 0, Rgba(255, 255, 255, 0.08f));
  }
  if (close_hot) {
    FillShape(close, 0, Rgba(196, 43, 28));
  }
  Label(kGlyphMinimize, kGlyph, minimize, Rgba(255, 255, 255, 0.72f),
        DWRITE_TEXT_ALIGNMENT_CENTER, true);
  Label(kGlyphClose, kGlyph, close,
        close_hot ? kOn : Rgba(255, 255, 255, can_close ? 0.72f : 0.3f),
        DWRITE_TEXT_ALIGNMENT_CENTER, true);
  AddHit(minimize, Hit::kMinimize, true);
  AddHit(close, Hit::kClose, true);
}

void SetupWindow::DrawCard(D2D1_RECT_F rect,
                           const wchar_t* glyph,
                           const std::wstring& title,
                           const std::wstring& detail,
                           bool recommended,
                           bool selected,
                           Hit hit,
                           bool record) {
  const bool hot = record && hover_ == hit;
  FillShape(rect, 10,
            Rgba(255, 255, 255, selected ? 0.12f : (hot ? 0.09f : 0.06f)));
  if (selected) {
    StrokeShape(rect, 10, Rgba(255, 255, 255, 0.88f), 1.5f);
  } else {
    StrokeShape(rect, 10, Rgba(255, 255, 255, 0.12f), 1.0f);
  }
  Label(
      glyph, kGlyphLarge,
      D2D1::RectF(rect.left + 14, rect.top + 12, rect.left + 32, rect.top + 30),
      kOn, DWRITE_TEXT_ALIGNMENT_LEADING, true);
  const float title_width = TextWidth(title, kCardTitle);
  const float title_left = rect.left + 36;
  Label(title, kCardTitle,
        D2D1::RectF(title_left, rect.top + 12, title_left + title_width + 4,
                    rect.top + 30),
        kOn, DWRITE_TEXT_ALIGNMENT_LEADING, true);
  if (recommended) {
    const std::wstring tag = L("installer.options.recommended");
    const float tag_left = title_left + title_width + 7;
    const D2D1_RECT_F tag_rect =
        D2D1::RectF(tag_left, rect.top + 14,
                    tag_left + TextWidth(tag, kTag) + 12, rect.top + 28);
    FillShape(tag_rect, 6, Rgba(254, 121, 113, 0.22f));
    Label(tag, kTag, tag_rect, Rgba(255, 194, 189),
          DWRITE_TEXT_ALIGNMENT_CENTER, true);
  }
  Label(detail, kSmall,
        D2D1::RectF(rect.left + 14, rect.top + 36, rect.right - 14,
                    rect.bottom - 4),
        Rgba(232, 226, 255, 0.62f), DWRITE_TEXT_ALIGNMENT_LEADING);
  AddHit(rect, hit, record);
}

void SetupWindow::DrawButtonRow(const std::wstring& link,
                                const std::wstring& button,
                                float top,
                                bool record) {
  const float button_width = PillWidth(button, false);
  const float link_width = link.empty() ? 0.0f : LinkWidth(link, false);
  const float gap = link.empty() ? 0.0f : 18.0f;
  const float left = (kWidth - link_width - gap - button_width) / 2;
  if (!link.empty()) {
    DrawLink(link, left, top + 9, Hit::kSecondary, record);
  }
  DrawPill(button, left + link_width + gap, top, false, Hit::kPrimary, record);
}

void SetupWindow::PaintIcon(float cx, float cy, float size, float scale) {
  if (!logo_) {
    return;
  }
  // The PNG keeps the app icon's margin around the rounded square.
  const float half = size * 1.24f * scale / 2;
  const D2D1_RECT_F rect =
      D2D1::RectF(cx - half, cy - half, cx + half, cy + half);
  if (context_) {
    context_->DrawBitmap(logo_.Get(), &rect, 1.0f,
                         D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr,
                         nullptr);
  } else {
    target_->DrawBitmap(logo_.Get(), rect, 1.0f,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
  }
}

void SetupWindow::DrawBadge(float cx,
                            float cy,
                            bool ok,
                            float scale,
                            float alpha) {
  if (alpha <= 0.0f || scale <= 0.0f) {
    return;
  }
  D2D1_MATRIX_3X2_F saved;
  target_->GetTransform(&saved);
  const D2D1_POINT_2F center = D2D1::Point2F(cx, cy);
  target_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, center) *
                        *D2D1::Matrix3x2F::ReinterpretBaseType(&saved));
  brush_->SetColor(Rgba(26, 20, 64, alpha));
  target_->FillEllipse(D2D1::Ellipse(center, 15, 15), brush_.Get());
  brush_->SetColor(ok ? Rgba(74, 222, 128, alpha) : Rgba(245, 185, 78, alpha));
  target_->FillEllipse(D2D1::Ellipse(center, 12, 12), brush_.Get());
  const D2D1_RECT_F mark = D2D1::RectF(cx - 12, cy - 12, cx + 12, cy + 12);
  if (ok) {
    Label(kGlyphCheck, kGlyphLarge, mark, Rgba(11, 61, 30, alpha),
          DWRITE_TEXT_ALIGNMENT_CENTER, true);
  } else {
    Label(L"!", kBadgeMark, mark, Rgba(61, 38, 0, alpha),
          DWRITE_TEXT_ALIGNMENT_CENTER, true);
  }
  target_->SetTransform(saved);
}

ComPtr<IDWriteTextLayout> SetupWindow::MakeLayout(const std::wstring& text,
                                                  TextStyle style,
                                                  float width,
                                                  float height) {
  ComPtr<IDWriteTextLayout> layout;
  dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                            formats_[style].Get(), std::max(width, 1.0f),
                            std::max(height, 1.0f), &layout);
  return layout;
}

float SetupWindow::TextWidth(const std::wstring& text, TextStyle style) {
  ComPtr<IDWriteTextLayout> layout = MakeLayout(text, style, 4096, 256);
  DWRITE_TEXT_METRICS metrics = {};
  if (!layout || FAILED(layout->GetMetrics(&metrics))) {
    return 0.0f;
  }
  return std::ceil(metrics.widthIncludingTrailingWhitespace);
}

void SetupWindow::Label(const std::wstring& text,
                        TextStyle style,
                        D2D1_RECT_F box,
                        D2D1_COLOR_F color,
                        DWRITE_TEXT_ALIGNMENT align,
                        bool middle,
                        bool trim) {
  ComPtr<IDWriteTextLayout> layout =
      MakeLayout(text, style, box.right - box.left, box.bottom - box.top);
  if (!layout) {
    return;
  }
  layout->SetTextAlignment(align);
  if (middle) {
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  }
  if (trim) {
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!ellipsis_) {
      dwrite_->CreateEllipsisTrimmingSign(formats_[style].Get(), &ellipsis_);
    }
    const DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0,
                                      0};
    layout->SetTrimming(&trimming, ellipsis_.Get());
  }
  brush_->SetColor(color);
  target_->DrawTextLayout(D2D1::Point2F(box.left, box.top), layout.Get(),
                          brush_.Get());
}

float SetupWindow::PillWidth(const std::wstring& label, bool shield) {
  return TextWidth(label, kButton) + 56 + (shield && shield_ ? 22 : 0);
}

void SetupWindow::DrawPill(const std::wstring& label,
                           float left,
                           float top,
                           bool shield,
                           Hit hit,
                           bool record) {
  const float width = PillWidth(label, shield);
  const D2D1_RECT_F rect = D2D1::RectF(left, top, left + width, top + 36);
  const bool hot = record && hover_ == hit;
  const bool down = hot && pressed_ == hit;
  FillShape(rect, 18,
            down ? Rgba(220, 220, 226) : (hot ? Rgba(236, 236, 240) : kOn));
  float text_left = left + 28;
  if (shield && shield_) {
    target_->DrawBitmap(
        shield_.Get(),
        D2D1::RectF(text_left, top + 10, text_left + 16, top + 26), 1.0f,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    text_left += 22;
  }
  Label(label, kButton,
        D2D1::RectF(text_left, top, left + width - 24, top + 36),
        Rgba(24, 24, 27), DWRITE_TEXT_ALIGNMENT_LEADING, true);
  AddHit(rect, hit, record);
}

float SetupWindow::LinkWidth(const std::wstring& text, bool back) {
  return TextWidth(text, kLink) + (back ? 15 : 0);
}

void SetupWindow::DrawLink(const std::wstring& text,
                           float left,
                           float top,
                           Hit hit,
                           bool record,
                           bool back) {
  const float width = LinkWidth(text, back);
  const D2D1_COLOR_F color = record && hover_ == hit
                                 ? Rgba(232, 226, 255, 0.95f)
                                 : Rgba(232, 226, 255, 0.72f);
  float x = left;
  if (back) {
    Label(kGlyphBack, kGlyph, D2D1::RectF(x, top, x + 12, top + 18), color,
          DWRITE_TEXT_ALIGNMENT_LEADING, true);
    x += 15;
  }
  Label(text, kLink, D2D1::RectF(x, top, left + width + 4, top + 18), color,
        DWRITE_TEXT_ALIGNMENT_LEADING, true);
  AddHit(D2D1::RectF(left - 6, top - 6, left + width + 6, top + 24), hit,
         record);
}

void SetupWindow::FillShape(D2D1_RECT_F rect,
                            float radius,
                            D2D1_COLOR_F color) {
  brush_->SetColor(color);
  if (radius > 0) {
    target_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius),
                                  brush_.Get());
  } else {
    target_->FillRectangle(rect, brush_.Get());
  }
}

void SetupWindow::StrokeShape(D2D1_RECT_F rect,
                              float radius,
                              D2D1_COLOR_F color,
                              float width) {
  const float inset = width / 2;
  brush_->SetColor(color);
  target_->DrawRoundedRectangle(
      D2D1::RoundedRect(D2D1::RectF(rect.left + inset, rect.top + inset,
                                    rect.right - inset, rect.bottom - inset),
                        radius - inset, radius - inset),
      brush_.Get(), width);
}

void SetupWindow::AddHit(D2D1_RECT_F rect, Hit hit, bool record) {
  if (record) {
    hits_.push_back({rect, hit});
  }
}

}  // namespace

int RunSetupWindow(HINSTANCE instance, const InstallOptions& defaults) {
  SetupWindow window(instance, defaults);
  return window.Run();
}

}  // namespace starry::installer
