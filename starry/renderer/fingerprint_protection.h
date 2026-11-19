// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_RENDERER_FINGERPRINT_PROTECTION_H_
#define STARRY_RENDERER_FINGERPRINT_PROTECTION_H_

#include <cstdint>
#include <optional>

#include "base/memory/scoped_refptr.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/platform/wtf/hash_map.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/size.h"

namespace blink {
class ExecutionContext;
class LocalDOMWindow;
class LocalFrame;
class ResourceRequest;
class StaticBitmapImage;
}  // namespace blink

namespace starry {

// Compiled into Blink core. All policy and readback work runs on the context's
// own thread, including OffscreenCanvas in workers.
CORE_EXPORT bool ShouldProtectFingerprint(blink::ExecutionContext* context);
CORE_EXPORT std::optional<uint64_t> FingerprintSeed(
    blink::ExecutionContext* context);
CORE_EXPORT blink::ExecutionContext* FingerprintExecutionContext(
    blink::ExecutionContext* context);
// For hooks that have a frame (null-safe; a frame without a window is not
// protected).
CORE_EXPORT bool ShouldProtectFrame(blink::LocalFrame* frame);

// Noise a separate, complete snapshot before cropping or encoding. Never
// modify the canvas's backing store, rendered appearance, or a shared image.
// On allocation/readback failure return null instead of exposing raw pixels.
CORE_EXPORT scoped_refptr<blink::StaticBitmapImage> ProtectCanvasReadback(
    blink::ExecutionContext* context,
    scoped_refptr<blink::StaticBitmapImage> image);

// Screen and window geometry: a
// protected frame reports its own viewport as the screen, the available
// screen and the outer window, placed at 0,0. Sizes are CSS pixels, exactly
// innerWidth / innerHeight, so page zoom cannot be recovered from screen and
// window sizes (see ProtectDevicePixelRatio). Null when the frame is not
// protected. `update_layout` gives script getters the same layout guarantees
// as innerWidth; media query evaluation passes false.
CORE_EXPORT std::optional<gfx::Size> ProtectedScreenSize(
    blink::LocalFrame& frame,
    bool update_layout = false);
// The screen position of a native mouse, pointer, wheel or touch event at
// `client_point` on that screen: the client point itself.
CORE_EXPORT std::optional<gfx::PointF> ProtectedScreenPoint(
    blink::LocalFrame& frame,
    const gfx::PointF& client_point);
// Scale for movementX/Y, which Chromium takes from screen positions in DIPs,
// so movement matches the protected screen coordinates under page zoom. 1 when
// the window is not protected.
CORE_EXPORT float ProtectedMovementScale(const blink::LocalDOMWindow* window);

// devicePixelRatio as pages observe it: window.devicePixelRatio, resolution
// media queries, srcset and image-set() selection, ResizeObserver device-pixel
// boxes and DPR client hints. The display scale bucketed to 1 or 2, without
// page zoom; `dpr` when the frame is not protected. Rendering keeps the real
// ratio, so layout precision can still reveal it.
CORE_EXPORT double ProtectDevicePixelRatio(blink::LocalFrame* frame,
                                           double dpr);
// A size in the frame's device pixels, rescaled to the protected ratio.
CORE_EXPORT gfx::Size ProtectDevicePixelSize(blink::LocalFrame* frame,
                                             const gfx::Size& size);

CORE_EXPORT unsigned ProtectHardwareConcurrency(
    blink::ExecutionContext* context,
    unsigned cores);
CORE_EXPORT float ProtectDeviceMemory(blink::ExecutionContext* context,
                                      float gigabytes);
// navigator.userAgentData and the client hints Blink adds to subresources.
CORE_EXPORT blink::UserAgentMetadata ProtectUserAgentMetadata(
    blink::ExecutionContext* context,
    blink::UserAgentMetadata metadata);
CORE_EXPORT void ProtectUserAgentMetadata(
    blink::ExecutionContext* context,
    std::optional<blink::UserAgentMetadata>& metadata);
// Replaces RTT / Downlink / ECT / Device-Memory client hints already added to
// `request`. DPR is protected where Blink computes it.
CORE_EXPORT void ProtectClientHints(blink::ExecutionContext* context,
                                    blink::ResourceRequest& request);
// navigator.keyboard.getLayoutMap(): a U.S. layout.
CORE_EXPORT void ProtectKeyboardLayoutMap(
    blink::ExecutionContext* context,
    blink::HashMap<blink::String, blink::String>& layout);

}  // namespace starry

#endif  // STARRY_RENDERER_FINGERPRINT_PROTECTION_H_
