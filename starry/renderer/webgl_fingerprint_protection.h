// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_RENDERER_WEBGL_FINGERPRINT_PROTECTION_H_
#define STARRY_RENDERER_WEBGL_FINGERPRINT_PROTECTION_H_

#include <cstddef>
#include <cstdint>

#include "third_party/blink/renderer/modules/modules_export.h"
#include "third_party/blink/renderer/platform/graphics/gpu/webgl_image_conversion.h"

namespace gpu::gles2 {
class GLES2Interface;
}
namespace blink {
class ExecutionContext;
class WebGLBuffer;
class WebGLTransformFeedback;
}  // namespace blink

namespace starry {

MODULES_EXPORT bool WebGLReadbackIsValid(gpu::gles2::GLES2Interface* gl,
                                         bool webgl2,
                                         unsigned format,
                                         unsigned type);
MODULES_EXPORT bool TrackWebGLPixelRead(blink::ExecutionContext* context,
                                        blink::WebGLBuffer* buffer,
                                        unsigned format,
                                        unsigned type);
MODULES_EXPORT void ProtectWebGLPixels(
    blink::ExecutionContext* context,
    int width,
    int height,
    unsigned format,
    unsigned type,
    const blink::WebGLImageConversion::PixelStoreParams& pack,
    void* destination,
    size_t size);
MODULES_EXPORT void RecordWebGLPixels(
    blink::ExecutionContext* context,
    blink::WebGLBuffer* buffer,
    uint64_t offset,
    int width,
    int height,
    unsigned format,
    unsigned type,
    const blink::WebGLImageConversion::PixelStoreParams& pack);
MODULES_EXPORT void ResetWebGLBuffer(blink::ExecutionContext* context,
                                     blink::WebGLBuffer* buffer);
MODULES_EXPORT void OverwriteWebGLBuffer(blink::ExecutionContext* context,
                                         blink::WebGLBuffer* buffer,
                                         uint64_t offset,
                                         uint64_t size);
MODULES_EXPORT void CopyWebGLBuffer(blink::ExecutionContext* context,
                                    blink::WebGLBuffer* source,
                                    blink::WebGLBuffer* destination,
                                    uint64_t source_offset,
                                    uint64_t destination_offset,
                                    uint64_t size);
MODULES_EXPORT void ProtectWebGLBuffer(blink::ExecutionContext* context,
                                       blink::WebGLBuffer* buffer,
                                       gpu::gles2::GLES2Interface* gl,
                                       unsigned target,
                                       uint64_t offset,
                                       void* destination,
                                       size_t size);
MODULES_EXPORT void WebGLTransformFeedbackFinished(
    blink::ExecutionContext* context,
    blink::WebGLTransformFeedback* feedback);

}  // namespace starry

#endif  // STARRY_RENDERER_WEBGL_FINGERPRINT_PROTECTION_H_
