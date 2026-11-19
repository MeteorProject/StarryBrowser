// Copyright 2026 The Starry Browser Authors

#include "starry/renderer/webgl_fingerprint_protection.h"

#include <optional>

#include "base/containers/span.h"
#include "base/numerics/checked_math.h"
#include "gpu/command_buffer/client/gles2_interface.h"
#include "starry/components/privacy/readback.h"
#include "starry/renderer/fingerprint_protection.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/modules/webgl/webgl_buffer.h"
#include "third_party/blink/renderer/modules/webgl/webgl_transform_feedback.h"
#include "third_party/blink/renderer/platform/heap/collection_support/heap_hash_map.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/supplementable.h"

namespace starry {
namespace {

using privacy::PixelFormat;
using privacy::ReadbackBuffer;
using privacy::ReadbackLayout;

PixelFormat Format(unsigned format, unsigned type) {
  switch (format) {
    case GL_RGBA:
    case GL_BGRA_EXT:
    case GL_RGB:
    case GL_RED:
    case GL_RG:
    case GL_ALPHA:
    case GL_LUMINANCE:
    case GL_LUMINANCE_ALPHA:
      break;
    default:
      return PixelFormat::kNone;
  }
  switch (type) {
    case GL_UNSIGNED_BYTE:
      return PixelFormat::kUnorm8;
    case GL_BYTE:
      return PixelFormat::kSnorm8;
    case GL_UNSIGNED_SHORT:
      return PixelFormat::kUnorm16;
    case GL_SHORT:
      return PixelFormat::kSnorm16;
    case GL_HALF_FLOAT:
    case GL_HALF_FLOAT_OES:
      return PixelFormat::kFloat16;
    case GL_FLOAT:
      return PixelFormat::kFloat32;
    case GL_UNSIGNED_SHORT_5_6_5:
      return PixelFormat::kRGB565;
    case GL_UNSIGNED_SHORT_4_4_4_4:
      return PixelFormat::kRGBA4444;
    case GL_UNSIGNED_SHORT_5_5_5_1:
      return PixelFormat::kRGBA5551;
    case GL_UNSIGNED_INT_2_10_10_10_REV:
      return PixelFormat::kRGB10A2;
    case GL_UNSIGNED_INT_10F_11F_11F_REV:
      return PixelFormat::kRG11B10Float;
    default:
      return PixelFormat::kNone;
  }
}

struct PackedReadback {
  ReadbackLayout layout;
  uint64_t skip;
  uint64_t size;
};

std::optional<PackedReadback> Layout(
    int width,
    int height,
    unsigned format,
    unsigned type,
    const blink::WebGLImageConversion::PixelStoreParams& pack) {
  if (width <= 0 || height <= 0 || pack.alignment <= 0 || pack.row_length < 0) {
    return std::nullopt;
  }
  unsigned bytes = 0, skip = 0, components = 0, component_bytes = 0;
  if (blink::WebGLImageConversion::ComputeImageSizeInBytes(
          format, type, width, height, 1, pack, &bytes, nullptr, &skip) !=
          GL_NO_ERROR ||
      !blink::WebGLImageConversion::ComputeFormatAndTypeParameters(
          format, type, &components, &component_bytes)) {
    return std::nullopt;
  }
  const uint64_t pixel_bytes = uint64_t{components} * component_bytes;
  const uint64_t row_bytes =
      uint64_t{static_cast<unsigned>(width)} * pixel_bytes;
  const uint64_t row_pixels = pack.row_length ? pack.row_length : width;
  const uint64_t stride = (row_pixels * pixel_bytes + pack.alignment - 1) /
                          pack.alignment * pack.alignment;
  ReadbackLayout layout{Format(format, type),          row_bytes, stride,
                        static_cast<uint64_t>(height), bytes,     1};
  // Overlapping pack rows write a dense union; scalar protection is independent
  // of row order, so this remains consistent with non-overlapping readbacks.
  if (stride < row_bytes) {
    layout = {Format(format, type), bytes, bytes, 1, bytes, 1};
  }
  if (layout.Size() != bytes)
    return std::nullopt;
  return PackedReadback{layout, skip, uint64_t{bytes} + skip};
}

class BufferState final : public blink::GarbageCollected<BufferState> {
 public:
  explicit BufferState(uint64_t size) : pixels(size) {}
  void Trace(blink::Visitor*) const {}
  ReadbackBuffer pixels;
};

class WebGLReadbacks final : public blink::GarbageCollected<WebGLReadbacks>,
                             public blink::Supplement<blink::ExecutionContext> {
 public:
  static const char kSupplementName[];
  explicit WebGLReadbacks(blink::ExecutionContext& context)
      : blink::Supplement<blink::ExecutionContext>(context) {}

  static WebGLReadbacks* Get(blink::ExecutionContext* context, bool create) {
    context = FingerprintExecutionContext(context);
    if (!context)
      return nullptr;
    auto* state =
        blink::Supplement<blink::ExecutionContext>::From<WebGLReadbacks>(
            *context);
    if (!state && create) {
      state = blink::MakeGarbageCollected<WebGLReadbacks>(*context);
      ProvideTo(*context, state);
    }
    return state;
  }

  BufferState* Find(blink::WebGLBuffer* buffer) {
    auto it = buffers.find(buffer);
    return it == buffers.end() ? nullptr : it->value.Get();
  }

  BufferState& Ensure(blink::WebGLBuffer* buffer) {
    if (auto* found = Find(buffer))
      return *found;
    auto* state = blink::MakeGarbageCollected<BufferState>(buffer->GetSize());
    buffers.Set(buffer, state);
    return *state;
  }

  void Trace(blink::Visitor* visitor) const override {
    visitor->Trace(buffers);
    blink::Supplement<blink::ExecutionContext>::Trace(visitor);
  }

  blink::HeapHashMap<blink::WeakMember<blink::WebGLBuffer>,
                     blink::Member<BufferState>>
      buffers;
};
const char WebGLReadbacks::kSupplementName[] = "StarryWebGLReadbacks";

}  // namespace

bool WebGLReadbackIsValid(gpu::gles2::GLES2Interface* gl,
                          bool webgl2,
                          unsigned format,
                          unsigned type) {
  // Do not change a destination on a rejected GL read. These state queries do
  // not consume the caller's error flags, and run before queuing the pixel
  // transfer so PBO reads can still complete asynchronously.
  if (gl->CheckFramebufferStatus(webgl2 ? GL_READ_FRAMEBUFFER
                                        : GL_FRAMEBUFFER) !=
      GL_FRAMEBUFFER_COMPLETE)
    return false;
  if (webgl2) {
    GLint read_buffer = 0;
    gl->GetIntegerv(GL_READ_BUFFER, &read_buffer);
    if (read_buffer == GL_NONE)
      return false;
  }
  GLint native_format = 0, native_type = 0;
  gl->GetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &native_format);
  gl->GetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &native_type);
  if (format == static_cast<unsigned>(native_format) &&
      type == static_cast<unsigned>(native_type))
    return true;
  const bool integer = native_format == GL_RGBA_INTEGER ||
                       native_format == GL_RG_INTEGER ||
                       native_format == GL_RED_INTEGER;
  if (integer) {
    const bool is_signed = native_type == GL_INT || native_type == GL_SHORT ||
                           native_type == GL_BYTE;
    return format == GL_RGBA_INTEGER &&
           type == (is_signed ? GL_INT : GL_UNSIGNED_INT);
  }
  const bool floating = native_type == GL_FLOAT ||
                        native_type == GL_HALF_FLOAT ||
                        native_type == GL_HALF_FLOAT_OES ||
                        native_type == GL_UNSIGNED_INT_10F_11F_11F_REV ||
                        native_type == GL_UNSIGNED_INT_5_9_9_9_REV;
  return format == GL_RGBA && type == (floating ? GL_FLOAT : GL_UNSIGNED_BYTE);
}

bool TrackWebGLPixelRead(blink::ExecutionContext* context,
                         blink::WebGLBuffer* buffer,
                         unsigned format,
                         unsigned type) {
  if (!FingerprintExecutionContext(context))
    return false;
  if (Format(format, type) != PixelFormat::kNone)
    return true;
  auto* state = WebGLReadbacks::Get(context, false);
  return state && state->Find(buffer);
}

void ProtectWebGLPixels(
    blink::ExecutionContext* context,
    int width,
    int height,
    unsigned format,
    unsigned type,
    const blink::WebGLImageConversion::PixelStoreParams& pack,
    void* destination,
    size_t size) {
  const auto seed = FingerprintSeed(context);
  if (!seed)
    return;
  const auto packed = Layout(width, height, format, type, pack);
  if (!packed || packed->size > size || !destination ||
      packed->layout.format == PixelFormat::kNone)
    return;
  ReadbackBuffer pixels(size);
  pixels.WritePixels(packed->skip, packed->layout);
  // ReadPixelsHelper already validated the ArrayBufferView and destination
  // offset; Layout additionally verifies that every written pixel fits.
  auto data =
      UNSAFE_BUFFERS(base::span(static_cast<uint8_t*>(destination), size));
  pixels.Protect(0, data, *seed);
}

void RecordWebGLPixels(
    blink::ExecutionContext* context,
    blink::WebGLBuffer* buffer,
    uint64_t offset,
    int width,
    int height,
    unsigned format,
    unsigned type,
    const blink::WebGLImageConversion::PixelStoreParams& pack) {
  const auto packed = Layout(width, height, format, type, pack);
  if (!buffer || !packed || offset > static_cast<uint64_t>(buffer->GetSize()) ||
      packed->size > static_cast<uint64_t>(buffer->GetSize()) - offset)
    return;
  auto* state =
      WebGLReadbacks::Get(context, packed->layout.format != PixelFormat::kNone);
  if (!state)
    return;
  if (packed->layout.format != PixelFormat::kNone || state->Find(buffer)) {
    state->Ensure(buffer).pixels.WritePixels(offset + packed->skip,
                                             packed->layout);
  }
}

void ResetWebGLBuffer(blink::ExecutionContext* context,
                      blink::WebGLBuffer* buffer) {
  if (auto* state = WebGLReadbacks::Get(context, false))
    state->buffers.erase(buffer);
}

void OverwriteWebGLBuffer(blink::ExecutionContext* context,
                          blink::WebGLBuffer* buffer,
                          uint64_t offset,
                          uint64_t size) {
  if (auto* state = WebGLReadbacks::Get(context, false)) {
    if (auto* data = state->Find(buffer))
      data->pixels.Overwrite(offset, size);
  }
}

void CopyWebGLBuffer(blink::ExecutionContext* context,
                     blink::WebGLBuffer* source,
                     blink::WebGLBuffer* destination,
                     uint64_t source_offset,
                     uint64_t destination_offset,
                     uint64_t size) {
  auto* state = WebGLReadbacks::Get(context, false);
  if (!state || !source || !destination)
    return;
  if (auto* source_data = state->Find(source)) {
    state->Ensure(destination)
        .pixels.CopyFrom(source_data->pixels, source_offset, destination_offset,
                         size);
  } else if (auto* destination_data = state->Find(destination)) {
    destination_data->pixels.Overwrite(destination_offset, size);
  }
}

void ProtectWebGLBuffer(blink::ExecutionContext* context,
                        blink::WebGLBuffer* buffer,
                        gpu::gles2::GLES2Interface* gl,
                        unsigned target,
                        uint64_t offset,
                        void* destination,
                        size_t size) {
  const auto seed = FingerprintSeed(context);
  auto* state = WebGLReadbacks::Get(context, false);
  auto* data = state ? state->Find(buffer) : nullptr;
  if (!seed || !data || !destination || !size)
    return;
  // ValidateGetBufferSubData checked the destination and source bounds.
  auto bytes =
      UNSAFE_BUFFERS(base::span(static_cast<uint8_t*>(destination), size));
  data->pixels.Protect(
      offset, bytes, *seed, [&](uint64_t start, base::span<uint8_t> sample) {
        if (start > static_cast<uint64_t>(buffer->GetSize()) ||
            sample.size() > static_cast<uint64_t>(buffer->GetSize()) - start)
          return false;
        gl->GetBufferSubDataCHROMIUM(target, start, sample.size(),
                                     sample.data());
        return true;
      });
}

void WebGLTransformFeedbackFinished(blink::ExecutionContext* context,
                                    blink::WebGLTransformFeedback* feedback) {
  auto* state = WebGLReadbacks::Get(context, false);
  if (!state || !feedback)
    return;
  // Transform feedback is general shader output, not a pixel transfer. Preserve
  // its exact values even when an application recycles a former pixel buffer.
  for (auto& buffer : feedback->bound_indexed_transform_feedback_buffers()) {
    if (buffer)
      state->buffers.erase(buffer.Get());
  }
}

}  // namespace starry
