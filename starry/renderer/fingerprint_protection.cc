// Copyright 2026 The Starry Browser Authors

#include "starry/renderer/fingerprint_protection.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <vector>

#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/memory/read_only_shared_memory_region.h"
#include "base/no_destructor.h"
#include "base/numerics/byte_conversions.h"
#include "base/synchronization/lock.h"
#include "build/build_config.h"
#include "cc/paint/paint_image.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "starry/common/fingerprint_protection.h"
#include "starry/common/fingerprint_values.h"
#include "starry/components/privacy/mojom/fingerprint_protection.mojom.h"
#include "third_party/blink/public/common/device_memory/approximated_device_memory.h"
#include "third_party/blink/public/common/fingerprinting_protection/noise_token.h"
#include "third_party/blink/public/common/thread_safe_browser_interface_broker_proxy.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_binding_for_core.h"
#include "third_party/blink/renderer/core/canvas_interventions/noise_hash.h"
#include "third_party/blink/renderer/core/canvas_interventions/noise_helper.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/frame/frame.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/frame/settings.h"
#include "third_party/blink/renderer/core/layout/adjust_for_absolute_zoom.h"
#include "third_party/blink/renderer/core/page/chrome_client.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/platform/graphics/static_bitmap_image.h"
#include "third_party/blink/renderer/platform/graphics/unaccelerated_static_bitmap_image.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_request.h"
#include "third_party/blink/renderer/platform/network/http_names.h"
#include "third_party/blink/renderer/platform/supplementable.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/platform/wtf/wtf.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/display/screen_info.h"
#include "ui/gfx/geometry/size_conversions.h"
#include "ui/gfx/geometry/size_f.h"
#include "ui/gfx/skia_span_util.h"
#include "url/origin.h"
#include "v8/include/v8-isolate.h"

namespace starry {

namespace {

// Keep at most one modest snapshot per context. Repeated reads, including tiny
// tiled reads of one canvas, reuse the same protected full image.
constexpr size_t kMaxCachedBytes = 8 * 1024 * 1024;

bool HasFingerprintService() {
  static const bool enabled = base::CommandLine::ForCurrentProcess()->HasSwitch(
      kFingerprintServiceSwitch);
  return enabled;
}

// Every context in a renderer belongs to the same profile, so the browser
// hands all of them the same read-only region. Map it once for the process;
// any thread may read it.
std::atomic<const FingerprintPolicyVersion*> g_policy_version{nullptr};

const FingerprintPolicyVersion* ProcessPolicyVersion() {
  return g_policy_version.load(std::memory_order_acquire);
}

void MapProcessPolicyVersion(base::ReadOnlySharedMemoryRegion region) {
  if (ProcessPolicyVersion() || !region.IsValid()) {
    return;
  }
  static base::NoDestructor<base::Lock> lock;
  base::AutoLock hold(*lock);
  static base::NoDestructor<base::ReadOnlySharedMemoryMapping> mapping;
  if (mapping->IsValid()) {
    return;
  }
  *mapping = region.Map();
  if (mapping->IsValid()) {
    g_policy_version.store(mapping->GetMemoryAs<FingerprintPolicyVersion>(),
                           std::memory_order_release);
  }
}

// Screen, window and media queries are read by most documents, often before
// any script runs (the preload scanner evaluates media). The browser's answer
// for a document depends only on its top-level origin, so documents in this
// process share it until the policy changes. Main thread only.
struct WindowPolicyCache {
  static constexpr size_t kMaxOrigins = 64;

  uint32_t generation = 0;
  std::map<url::Origin, std::optional<uint64_t>> seeds;
};

WindowPolicyCache& WindowCache() {
  DCHECK(blink::IsMainThread());
  static base::NoDestructor<WindowPolicyCache> cache;
  return *cache;
}

blink::ExecutionContext* ReadContext(blink::ExecutionContext* owner) {
  if (owner && !owner->IsContextDestroyed()) {
    return owner;
  }
  // A page can retain a canvas after removing its iframe, or create a document
  // with no browsing context. Reading that canvas must still use the caller's
  // protection policy instead of exposing raw pixels through a null owner.
  auto* isolate = v8::Isolate::TryGetCurrent();
  if (isolate && isolate->InContext()) {
    if (auto* window = blink::EnteredDOMWindow(isolate);
        window && !window->IsContextDestroyed()) {
      return window;
    }
    auto* current = blink::CurrentExecutionContext(isolate);
    if (current && !current->IsContextDestroyed()) {
      return current;
    }
  }
  return owner;
}

SkColorType ReadbackColorType(SkColorType source) {
  switch (source) {
    case kAlpha_8_SkColorType:
    case kRGB_565_SkColorType:
    case kARGB_4444_SkColorType:
    case kRGBA_8888_SkColorType:
    case kRGB_888x_SkColorType:
    case kBGRA_8888_SkColorType:
    case kGray_8_SkColorType:
    case kSRGBA_8888_SkColorType:
    case kR8_unorm_SkColorType:
      return kRGBA_8888_SkColorType;
    case kRGBA_F16Norm_SkColorType:
    case kRGBA_F16_SkColorType:
      return kRGBA_F16_SkColorType;
    default:
      // Preserve the precision and extended range of HDR/10/16-bit images.
      return kRGBA_F32_SkColorType;
  }
}

// Float canvases must not be silently quantized to 8-bit sRGB. Change a low
// mantissa bit of finite, nonzero RGB samples, leaving alpha and empty pixels
// intact. Select the bit by exponent (about 1/256 when representable), so a
// subsequent 8-bit read/PNG conversion does not simply round away the noise.
// A pixel's original values and the site seed determine its noise.
uint64_t MixCanvasBits(uint64_t value) {
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

void NoiseFloatPixels(base::span<uint8_t> pixels,
                      uint64_t seed,
                      bool half_float) {
  const size_t channel_bytes = half_float ? 2 : 4;
  const size_t pixel_bytes = channel_bytes * 4;
  for (size_t offset = 0; offset < pixels.size(); offset += pixel_bytes) {
    auto pixel = pixels.subspan(offset, pixel_bytes);
    uint64_t hash = seed;
    if (half_float) {
      if ((base::U16FromLittleEndian(pixel.subspan<6, 2>()) & 0x7fff) == 0) {
        continue;
      }
      hash = MixCanvasBits(hash ^ base::U64FromLittleEndian(pixel.first<8>()));
    } else {
      if ((base::U32FromLittleEndian(pixel.subspan<12, 4>()) & 0x7fffffff) ==
          0) {
        continue;
      }
      hash = MixCanvasBits(hash ^ base::U64FromLittleEndian(pixel.first<8>()));
      hash = MixCanvasBits(hash ^
                           base::U64FromLittleEndian(pixel.subspan<8, 8>()));
    }
    for (size_t channel = 0; channel < 3; ++channel) {
      auto value = pixel.subspan(channel * channel_bytes, channel_bytes);
      // Mix all component bits before selecting channels. Looking only at low
      // FNV bits made aligned half-float inputs share the same channel choices,
      // occasionally changing only an HDR channel that an 8-bit read clips
      // away.
      const bool flip = ((hash >> channel) & 1) != 0;
      if (half_float) {
        uint16_t bits = base::U16FromLittleEndian(value.first<2>());
        if ((bits & 0x7fff) != 0 && (bits & 0x7c00) != 0x7c00) {
          const int exponent = (bits >> 10) & 0x1f;
          const uint16_t mask = static_cast<uint16_t>(flip)
                                << std::clamp(17 - exponent, 0, 9);
          value.copy_from(base::U16ToLittleEndian(bits ^ mask));
        }
      } else {
        uint32_t bits = base::U32FromLittleEndian(value.first<4>());
        if ((bits & 0x7fffffff) != 0 && (bits & 0x7f800000) != 0x7f800000) {
          const int exponent = (bits >> 23) & 0xff;
          const uint32_t mask = static_cast<uint32_t>(flip)
                                << std::clamp(142 - exponent, 0, 22);
          value.copy_from(base::U32ToLittleEndian(bits ^ mask));
        }
      }
    }
  }
}

scoped_refptr<blink::StaticBitmapImage> NoiseImage(
    const scoped_refptr<blink::StaticBitmapImage>& image,
    uint64_t seed) {
  cc::PaintImage source = image->PaintImageForCurrentFrame();
  if (!source) {
    return nullptr;
  }
  SkImageInfo info = source.GetSkImageInfo();
  if (info.isEmpty() || static_cast<uint64_t>(info.width()) * info.height() >
                            std::numeric_limits<int>::max() / 4) {
    return nullptr;
  }
  info = info.makeColorType(ReadbackColorType(info.colorType()))
             .makeAlphaType(kUnpremul_SkAlphaType);
  SkBitmap bitmap;
  if (!bitmap.tryAllocPixels(info, info.minRowBytes()) ||
      !source.readPixels(info, bitmap.getPixels(), bitmap.rowBytes(), 0, 0)) {
    return nullptr;
  }
  auto pixels = gfx::SkPixmapToWritableSpan(bitmap.pixmap());
  if (info.colorType() == kRGBA_8888_SkColorType) {
    // Chromium's canvas-interventions algorithm gives identical output for the
    // same input and seed. Preserve alpha so exports do not gain translucency.
    std::vector<uint8_t> alpha(pixels.size() / 4);
    for (size_t i = 0; i < alpha.size(); ++i) {
      alpha[i] = pixels[i * 4 + 3];
    }
    blink::NoisePixels(blink::NoiseHash(blink::NoiseToken(seed)), pixels,
                       info.width(), info.height());
    for (size_t i = 0; i < alpha.size(); ++i) {
      pixels[i * 4 + 3] = alpha[i];
    }
  } else {
    NoiseFloatPixels(pixels, seed, info.colorType() == kRGBA_F16_SkColorType);
  }
  bitmap.setImmutable();
  return blink::UnacceleratedStaticBitmapImage::Create(
      bitmap.asImage(), image->Orientation(), image->GetHdrMetadata());
}

class FingerprintState final
    : public blink::GarbageCollected<FingerprintState>,
      public blink::Supplement<blink::ExecutionContext> {
 public:
  static const char kSupplementName[];

  explicit FingerprintState(blink::ExecutionContext& context)
      : blink::Supplement<blink::ExecutionContext>(context) {}

  static FingerprintState& From(blink::ExecutionContext& context) {
    auto* state =
        blink::Supplement<blink::ExecutionContext>::From<FingerprintState>(
            context);
    if (!state) {
      state = blink::MakeGarbageCollected<FingerprintState>(context);
      ProvideTo(context, state);
    }
    state->Initialize(context);
    return *state;
  }

  bool enabled() const { return seed_.has_value(); }
  std::optional<uint64_t> seed() const { return seed_; }

  scoped_refptr<blink::StaticBitmapImage> Protect(
      scoped_refptr<blink::StaticBitmapImage> image) {
    if (!seed_ || !image) {
      return image;
    }
    const auto content_id =
        image->PaintImageForCurrentFrame().GetContentIdForFrame(
            cc::PaintImage::kDefaultFrameIndex);
    if (cached_image_ && content_id != cc::PaintImage::kInvalidContentId &&
        cached_content_ == content_id) {
      return cached_image_;
    }
    cached_image_ = nullptr;
    cached_content_ = cc::PaintImage::kInvalidContentId;
    auto protected_image = NoiseImage(image, *seed_);
    if (protected_image && protected_image->PaintImageForCurrentFrame()
                                   .GetSkImageInfo()
                                   .computeMinByteSize() <= kMaxCachedBytes) {
      cached_content_ = content_id;
      cached_image_ = protected_image;
    }
    return protected_image;
  }

  void Trace(blink::Visitor* visitor) const override {
    blink::Supplement<blink::ExecutionContext>::Trace(visitor);
  }

 private:
  void Initialize(blink::ExecutionContext& context) {
    const blink::SecurityOrigin* top_origin = context.GetSecurityOrigin();
    auto* window = blink::DynamicTo<blink::LocalDOMWindow>(&context);
    if (window) {
      if (window->document() && document_ != window->document()->Token()) {
        // LocalDOMWindow can be reused by a navigation. Reset document state;
        // a BFCache restore keeps its document but still checks policy changes.
        document_ = window->document()->Token();
        initialized_ = false;
        seed_.reset();
        cached_image_ = nullptr;
      }
      if (window->GetFrame()) {
        top_origin = window->GetFrame()
                         ->Top()
                         ->GetSecurityContext()
                         ->GetSecurityOrigin();
      }
    }
    const FingerprintPolicyVersion* version = ProcessPolicyVersion();
    if (initialized_ && version &&
        version->generation.load(std::memory_order_acquire) == generation_) {
      return;
    }
    initialized_ = true;
    seed_.reset();
    cached_image_ = nullptr;
    if (context.IsContextDestroyed()) {
      return;
    }
    const url::Origin top = top_origin->ToUrlOrigin();
    if (version) {
      const uint32_t generation =
          version->generation.load(std::memory_order_acquire);
      if (!version->enabled.load(std::memory_order_relaxed)) {
        // Nothing in this profile is protected right now.
        generation_ = generation;
        return;
      }
      if (window) {
        WindowPolicyCache& cache = WindowCache();
        if (cache.generation == generation) {
          if (auto it = cache.seeds.find(top); it != cache.seeds.end()) {
            generation_ = generation;
            seed_ = it->second;
            return;
          }
        }
      }
    }
    mojo::Remote<mojom::FingerprintProtection> remote;
    auto* broker = blink::Platform::Current()->GetBrowserInterfaceBroker();
    if (!broker) {
      return;
    }
    broker->GetInterface(remote.BindNewPipeAndPassReceiver());
    bool enabled = false;
    uint64_t seed = 0;
    base::ReadOnlySharedMemoryRegion policy_version;
    if (!remote->GetSeed(context.GetExecutionContextToken(), top, &enabled,
                         &seed, &generation_, &policy_version)) {
      return;
    }
    MapProcessPolicyVersion(std::move(policy_version));
    if (enabled) {
      seed_ = seed;
    }
    if (window && ProcessPolicyVersion()) {
      WindowPolicyCache& cache = WindowCache();
      if (cache.generation != generation_ ||
          cache.seeds.size() >= WindowPolicyCache::kMaxOrigins) {
        cache.generation = generation_;
        cache.seeds.clear();
      }
      cache.seeds[top] = seed_;
    }
  }

  bool initialized_ = false;
  std::optional<blink::DocumentToken> document_;
  std::optional<uint64_t> seed_;
  uint32_t generation_ = 0;
  cc::PaintImage::ContentId cached_content_ = cc::PaintImage::kInvalidContentId;
  scoped_refptr<blink::StaticBitmapImage> cached_image_;
};

const char FingerprintState::kSupplementName[] = "StarryFingerprintState";

bool FrameProtected(blink::LocalFrame& frame) {
  return frame.DomWindow() && frame.GetPage() &&
         FingerprintSeed(frame.DomWindow()).has_value();
}

float DeviceScaleFactor(blink::LocalFrame& frame) {
  const float scale = frame.GetPage()
                          ->GetChromeClient()
                          .GetScreenInfo(frame)
                          .device_scale_factor;
  return scale > 0 ? scale : 1;
}

float LayoutZoomFactor(blink::LocalFrame& frame) {
  const float zoom = frame.LayoutZoomFactor();
  return zoom > 0 ? zoom : 1;
}

// What a U.S. keyboard layout reports. macOS adds the ISO section key (§);
// the other platforms report the ANSI keys.
constexpr std::pair<const char*, const char*> kUSKeyboardLayout[] = {
    {"Backquote", "`"},    {"Digit1", "1"},     {"Digit2", "2"},
    {"Digit3", "3"},       {"Digit4", "4"},     {"Digit5", "5"},
    {"Digit6", "6"},       {"Digit7", "7"},     {"Digit8", "8"},
    {"Digit9", "9"},       {"Digit0", "0"},     {"Minus", "-"},
    {"Equal", "="},        {"KeyQ", "q"},       {"KeyW", "w"},
    {"KeyE", "e"},         {"KeyR", "r"},       {"KeyT", "t"},
    {"KeyY", "y"},         {"KeyU", "u"},       {"KeyI", "i"},
    {"KeyO", "o"},         {"KeyP", "p"},       {"BracketLeft", "["},
    {"BracketRight", "]"}, {"Backslash", "\\"}, {"KeyA", "a"},
    {"KeyS", "s"},         {"KeyD", "d"},       {"KeyF", "f"},
    {"KeyG", "g"},         {"KeyH", "h"},       {"KeyJ", "j"},
    {"KeyK", "k"},         {"KeyL", "l"},       {"Semicolon", ";"},
    {"Quote", "'"},        {"KeyZ", "z"},       {"KeyX", "x"},
    {"KeyC", "c"},         {"KeyV", "v"},       {"KeyB", "b"},
    {"KeyN", "n"},         {"KeyM", "m"},       {"Comma", ","},
    {"Period", "."},       {"Slash", "/"},
};

}  // namespace

bool ShouldProtectFingerprint(blink::ExecutionContext* context) {
  return FingerprintSeed(context).has_value();
}

bool ShouldProtectFrame(blink::LocalFrame* frame) {
  return frame && FrameProtected(*frame);
}

blink::ExecutionContext* FingerprintExecutionContext(
    blink::ExecutionContext* context) {
  return HasFingerprintService() ? ReadContext(context) : nullptr;
}

std::optional<uint64_t> FingerprintSeed(blink::ExecutionContext* context) {
  if (!HasFingerprintService()) {
    return std::nullopt;
  }
  context = ReadContext(context);
  return context ? FingerprintState::From(*context).seed() : std::nullopt;
}

scoped_refptr<blink::StaticBitmapImage> ProtectCanvasReadback(
    blink::ExecutionContext* context,
    scoped_refptr<blink::StaticBitmapImage> image) {
  if (!HasFingerprintService()) {
    return image;
  }
  context = ReadContext(context);
  return context ? FingerprintState::From(*context).Protect(std::move(image))
                 : image;
}

std::optional<gfx::Size> ProtectedScreenSize(blink::LocalFrame& frame,
                                             bool update_layout) {
  if (!FrameProtected(frame)) {
    return std::nullopt;
  }
  blink::LocalFrameView* view = frame.View();
  if (!view) {
    return gfx::Size();
  }
  if (update_layout) {
    // The same guarantees as innerWidth (LocalDOMWindow::GetViewportSize): a
    // main frame's first layout sets its size, a parent's layout an iframe's.
    blink::Page* page = frame.GetPage();
    if (frame.IsMainFrame() &&
        (page->GetSettings().GetViewportEnabled() || !view->DidFirstLayout())) {
      frame.GetDocument()->UpdateStyleAndLayout(
          blink::DocumentUpdateReason::kJavaScript);
    }
    if (auto* parent =
            blink::DynamicTo<blink::LocalFrame>(frame.Tree().Parent())) {
      parent->GetDocument()->UpdateStyleAndLayout(
          blink::DocumentUpdateReason::kJavaScript);
    }
    view = frame.View();
    if (!view) {
      return gfx::Size();
    }
  }
  // innerWidth / innerHeight: frame geometry is in physical pixels, scaled by
  // the display scale and the page zoom.
  const float zoom = LayoutZoomFactor(frame);
  return gfx::Size(
      blink::AdjustForAbsoluteZoom::AdjustInt(view->Size().width(), zoom),
      blink::AdjustForAbsoluteZoom::AdjustInt(view->Size().height(), zoom));
}

std::optional<gfx::PointF> ProtectedScreenPoint(
    blink::LocalFrame& frame,
    const gfx::PointF& client_point) {
  if (!FrameProtected(frame)) {
    return std::nullopt;
  }
  return client_point;
}

float ProtectedMovementScale(const blink::LocalDOMWindow* window) {
  blink::LocalFrame* frame = window ? window->GetFrame() : nullptr;
  if (!ShouldProtectFrame(frame)) {
    return 1;
  }
  // Screen positions are DIPs, protected screen coordinates CSS pixels. Keep
  // exactly 1 without page zoom: movement truncates scaled positions.
  const float scale = DeviceScaleFactor(*frame) / LayoutZoomFactor(*frame);
  return std::abs(scale - 1) < 1e-4f ? 1 : scale;
}

double ProtectDevicePixelRatio(blink::LocalFrame* frame, double dpr) {
  return ShouldProtectFrame(frame)
             ? ProtectedDevicePixelRatio(DeviceScaleFactor(*frame))
             : dpr;
}

gfx::Size ProtectDevicePixelSize(blink::LocalFrame* frame,
                                 const gfx::Size& size) {
  if (!ShouldProtectFrame(frame)) {
    return size;
  }
  // Device pixels are layout units: CSS pixels times the layout zoom.
  const double scale = ProtectedDevicePixelRatio(DeviceScaleFactor(*frame)) /
                       LayoutZoomFactor(*frame);
  if (std::abs(scale - 1) < 1e-6) {
    return size;
  }
  return gfx::ToRoundedSize(gfx::ScaleSize(gfx::SizeF(size), scale));
}

unsigned ProtectHardwareConcurrency(blink::ExecutionContext* context,
                                    unsigned cores) {
  return ShouldProtectFingerprint(context) ? ProtectedHardwareConcurrency(cores)
                                           : cores;
}

float ProtectDeviceMemory(blink::ExecutionContext* context, float gigabytes) {
  return ShouldProtectFingerprint(context) ? ProtectedDeviceMemory(gigabytes)
                                           : gigabytes;
}

blink::UserAgentMetadata ProtectUserAgentMetadata(
    blink::ExecutionContext* context,
    blink::UserAgentMetadata metadata) {
  if (ShouldProtectFingerprint(context)) {
    ReduceUserAgentMetadata(metadata);
  }
  return metadata;
}

void ProtectUserAgentMetadata(
    blink::ExecutionContext* context,
    std::optional<blink::UserAgentMetadata>& metadata) {
  if (metadata && ShouldProtectFingerprint(context)) {
    ReduceUserAgentMetadata(*metadata);
  }
}

void ProtectClientHints(blink::ExecutionContext* context,
                        blink::ResourceRequest& request) {
  const auto replace = [&](const blink::AtomicString& name,
                           const blink::AtomicString& value) {
    if (!request.HttpHeaderField(name).IsNull()) {
      request.SetHttpHeaderField(name, value);
    }
  };
  const blink::AtomicString* names[] = {
      &blink::http_names::kRtt_DEPRECATED,
      &blink::http_names::kDownlink_DEPRECATED,
      &blink::http_names::kEct_DEPRECATED,
      &blink::http_names::kDeviceMemory_DEPRECATED,
      &blink::http_names::kDeviceMemory,
  };
  if (std::ranges::all_of(names, [&](const blink::AtomicString* name) {
        return request.HttpHeaderField(*name).IsNull();
      })) {
    return;
  }
  if (!ShouldProtectFingerprint(context)) {
    return;
  }
  const blink::AtomicString memory(blink::String::Number(ProtectedDeviceMemory(
      blink::ApproximatedDeviceMemory::GetApproximatedDeviceMemory())));
  replace(blink::http_names::kDeviceMemory_DEPRECATED, memory);
  replace(blink::http_names::kDeviceMemory, memory);
  replace(blink::http_names::kRtt_DEPRECATED,
          blink::AtomicString(blink::String::Number(kProtectedRttMs)));
  replace(blink::http_names::kDownlink_DEPRECATED,
          blink::AtomicString(blink::String::Number(kProtectedDownlinkMbps)));
  replace(blink::http_names::kEct_DEPRECATED,
          blink::AtomicString(kProtectedEffectiveType));
}

void ProtectKeyboardLayoutMap(
    blink::ExecutionContext* context,
    blink::HashMap<blink::String, blink::String>& layout) {
  if (!ShouldProtectFingerprint(context)) {
    return;
  }
  layout.clear();
  for (const auto& [code, key] : kUSKeyboardLayout) {
    layout.Set(blink::String(code), blink::String(key));
  }
#if BUILDFLAG(IS_MAC)
  static constexpr UChar kSectionSign[] = {0x00a7, 0};
  layout.Set(blink::String("IntlBackslash"), blink::String(kSectionSign));
#endif
}

}  // namespace starry
