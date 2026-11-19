// Copyright 2026 The Starry Browser Authors

#include "starry/renderer/webgpu_fingerprint_protection.h"

#include <algorithm>
#include <limits>

#include "base/containers/span.h"
#include "base/numerics/checked_math.h"
#include "starry/components/privacy/readback.h"
#include "starry/renderer/fingerprint_protection.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/modules/webgpu/dawn_object.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_buffer.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_command_buffer.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_command_encoder.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_device.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_memory_heap_info.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_queue.h"
#include "third_party/blink/renderer/modules/webgpu/gpu_texture.h"
#include "third_party/blink/renderer/modules/webgpu/texture_utils.h"
#include "third_party/blink/renderer/platform/heap/collection_support/heap_hash_map.h"
#include "third_party/blink/renderer/platform/heap/collection_support/heap_hash_set.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/supplementable.h"

namespace starry {
namespace {

using privacy::PixelFormat;
using privacy::ReadbackBuffer;
using privacy::ReadbackLayout;

PixelFormat Format(wgpu::TextureFormat format) {
  switch (format) {
    case wgpu::TextureFormat::R8Unorm:
    case wgpu::TextureFormat::RG8Unorm:
    case wgpu::TextureFormat::RGBA8Unorm:
    case wgpu::TextureFormat::RGBA8UnormSrgb:
    case wgpu::TextureFormat::BGRA8Unorm:
    case wgpu::TextureFormat::BGRA8UnormSrgb:
      return PixelFormat::kUnorm8;
    case wgpu::TextureFormat::R8Snorm:
    case wgpu::TextureFormat::RG8Snorm:
    case wgpu::TextureFormat::RGBA8Snorm:
      return PixelFormat::kSnorm8;
    case wgpu::TextureFormat::R16Unorm:
    case wgpu::TextureFormat::RG16Unorm:
    case wgpu::TextureFormat::RGBA16Unorm:
      return PixelFormat::kUnorm16;
    case wgpu::TextureFormat::R16Snorm:
    case wgpu::TextureFormat::RG16Snorm:
    case wgpu::TextureFormat::RGBA16Snorm:
      return PixelFormat::kSnorm16;
    case wgpu::TextureFormat::R16Float:
    case wgpu::TextureFormat::RG16Float:
    case wgpu::TextureFormat::RGBA16Float:
      return PixelFormat::kFloat16;
    case wgpu::TextureFormat::R32Float:
    case wgpu::TextureFormat::RG32Float:
    case wgpu::TextureFormat::RGBA32Float:
      return PixelFormat::kFloat32;
    case wgpu::TextureFormat::RGB10A2Unorm:
      return PixelFormat::kRGB10A2;
    case wgpu::TextureFormat::RG11B10Ufloat:
      return PixelFormat::kRG11B10Float;
    default:
      return PixelFormat::kNone;
  }
}

struct BlockInfo {
  uint64_t width;
  uint64_t height;
  uint64_t bytes;
};

std::optional<BlockInfo> Block(wgpu::TextureFormat format,
                               wgpu::TextureAspect aspect) {
  // Reuse Blink's format table instead of maintaining a second one. Ordinary
  // formats take a single probe. Compressed WebGPU blocks are at most 12x12.
  for (uint32_t width = 1; width <= 12; ++width) {
    for (uint32_t height = 1; height <= 12; ++height) {
      const auto bytes = blink::EstimateWriteTextureBytesUpperBound(
          {}, {width, height, 1}, format, aspect);
      if (bytes)
        return BlockInfo{width, height, bytes};
    }
  }
  return std::nullopt;
}

std::optional<ReadbackLayout> Layout(const wgpu::TexelCopyBufferLayout& copy,
                                     const wgpu::Extent3D& extent,
                                     wgpu::TextureFormat format,
                                     wgpu::TextureAspect aspect) {
  if (!extent.width || !extent.height || !extent.depthOrArrayLayers)
    return std::nullopt;
  const auto block = Block(format, aspect);
  if (!block || extent.width % block->width || extent.height % block->height ||
      copy.offset % block->bytes ||
      (aspect != wgpu::TextureAspect::All && copy.offset % 4))
    return std::nullopt;
  const uint64_t row_bytes = (extent.width / block->width) * block->bytes;
  const uint64_t rows = extent.height / block->height;
  if (copy.bytesPerRow == wgpu::kCopyStrideUndefined &&
      (rows > 1 || extent.depthOrArrayLayers > 1))
    return std::nullopt;
  const uint64_t stride = copy.bytesPerRow == wgpu::kCopyStrideUndefined
                              ? row_bytes
                              : copy.bytesPerRow;
  if (stride < row_bytes ||
      (copy.bytesPerRow != wgpu::kCopyStrideUndefined && stride % 256))
    return std::nullopt;
  if (copy.rowsPerImage == wgpu::kCopyStrideUndefined &&
      extent.depthOrArrayLayers > 1)
    return std::nullopt;
  const uint64_t rows_per_image =
      copy.rowsPerImage == wgpu::kCopyStrideUndefined ? rows
                                                      : copy.rowsPerImage;
  auto image_stride = base::CheckMul(stride, rows_per_image);
  if (!image_stride.IsValid())
    return std::nullopt;
  ReadbackLayout layout{Format(format),
                        row_bytes,
                        stride,
                        rows,
                        image_stride.ValueOrDie(),
                        extent.depthOrArrayLayers};
  const uint64_t total =
      blink::EstimateWriteTextureBytesUpperBound(copy, extent, format, aspect);
  if (layout.Size() != total)
    return std::nullopt;
  return layout;
}

class BufferState final : public blink::GarbageCollected<BufferState> {
 public:
  explicit BufferState(uint64_t size) : pixels(size) {}
  void Trace(blink::Visitor*) const {}
  ReadbackBuffer pixels;
  bool destroyed = false;
  bool mapping = false;
  bool mapping_read = false;
  bool mapping_processed = false;
  std::optional<uint64_t> mapping_seed;
  uint64_t mapping_offset = 0;
  uint64_t mapping_size = 0;
};

enum class OperationKind { kTextureCopy, kBufferCopy, kClear, kShaderWrite };

struct Operation final : public blink::GarbageCollected<Operation> {
  OperationKind kind = OperationKind::kClear;
  blink::Member<blink::GPUBuffer> source;
  blink::Member<blink::GPUBuffer> destination;
  blink::Member<blink::GPUTexture> texture;
  uint64_t source_offset = 0;
  uint64_t destination_offset = 0;
  uint64_t size = 0;
  ReadbackLayout layout;

  void Trace(blink::Visitor* visitor) const {
    visitor->Trace(source);
    visitor->Trace(destination);
    visitor->Trace(texture);
  }
};

class Commands final : public blink::GarbageCollected<Commands> {
 public:
  void Trace(blink::Visitor* visitor) const { visitor->Trace(operations); }
  void Append(Operation* op) {
    if (overflowed)
      return;
    if (operations.size() >= 65536) {
      operations.clear();
      overflowed = true;
      return;
    }
    operations.push_back(op);
  }
  blink::HeapVector<blink::Member<Operation>> operations;
  bool finished = false;
  bool submitted = false;
  bool overflowed = false;
  bool invalid = false;
};

class WebGPUReadbacks final
    : public blink::GarbageCollected<WebGPUReadbacks>,
      public blink::Supplement<blink::ExecutionContext> {
 public:
  static const char kSupplementName[];
  explicit WebGPUReadbacks(blink::ExecutionContext& context)
      : blink::Supplement<blink::ExecutionContext>(context) {}

  static WebGPUReadbacks* Get(blink::ExecutionContext* context, bool create) {
    context = FingerprintExecutionContext(context);
    if (!context)
      return nullptr;
    auto* state =
        blink::Supplement<blink::ExecutionContext>::From<WebGPUReadbacks>(
            *context);
    if (!state && create) {
      state = blink::MakeGarbageCollected<WebGPUReadbacks>(*context);
      ProvideTo(*context, state);
    }
    return state;
  }

  BufferState* Find(blink::GPUBuffer* buffer) {
    auto it = buffers.find(buffer);
    return it == buffers.end() ? nullptr : it->value.Get();
  }
  BufferState& Ensure(blink::GPUBuffer* buffer) {
    if (auto* state = Find(buffer))
      return *state;
    auto* state = blink::MakeGarbageCollected<BufferState>(buffer->size());
    buffers.Set(buffer, state);
    return *state;
  }
  Commands& Stream(blink::GPUCommandEncoder* encoder) {
    auto it = encoders.find(encoder);
    if (it != encoders.end())
      return *it->value;
    auto* commands = blink::MakeGarbageCollected<Commands>();
    encoders.Set(encoder, commands);
    return *commands;
  }

  void Apply(Operation& op, blink::GPUDevice* device) {
    if (op.kind == OperationKind::kShaderWrite) {
      // Storage outputs are arbitrary computations. A real draw/dispatch takes
      // these buffers out of the pixel-transfer classification; do not corrupt
      // compute results when an application reuses an intermediate buffer.
      for (auto& entry : buffers) {
        if (entry.key && entry.key->device() == device &&
            (entry.key->usage() & uint32_t(wgpu::BufferUsage::Storage))) {
          entry.value->pixels.Reset(entry.key->size());
        }
      }
      return;
    }
    if (!op.destination)
      return;
    auto* dst = Find(op.destination.Get());
    if ((dst && dst->destroyed) || op.destination->GetHandle().GetMapState() !=
                                       wgpu::BufferMapState::Unmapped)
      return;
    switch (op.kind) {
      case OperationKind::kTextureCopy:
        if (!op.texture || op.texture->IsDestroyed())
          return;
        if (op.layout.format != PixelFormat::kNone || dst) {
          Ensure(op.destination.Get())
              .pixels.WritePixels(op.destination_offset, op.layout);
        }
        break;
      case OperationKind::kBufferCopy: {
        auto* src = Find(op.source.Get());
        if (!op.source || (src && src->destroyed) ||
            op.source->GetHandle().GetMapState() !=
                wgpu::BufferMapState::Unmapped)
          return;
        if (src && !src->pixels.empty()) {
          Ensure(op.destination.Get())
              .pixels.CopyFrom(src->pixels, op.source_offset,
                               op.destination_offset, op.size);
        } else if (dst) {
          dst->pixels.Overwrite(op.destination_offset, op.size);
        }
        break;
      }
      case OperationKind::kClear:
        if (dst)
          dst->pixels.Overwrite(op.destination_offset, op.size);
        break;
      case OperationKind::kShaderWrite:
        break;
    }
  }

  void Trace(blink::Visitor* visitor) const override {
    visitor->Trace(buffers);
    visitor->Trace(encoders);
    visitor->Trace(command_buffers);
    visitor->Trace(passes);
    visitor->Trace(empty_heaps);
    visitor->Trace(overflowed_devices);
    blink::Supplement<blink::ExecutionContext>::Trace(visitor);
  }

  blink::HeapHashMap<blink::WeakMember<blink::GPUBuffer>,
                     blink::Member<BufferState>>
      buffers;
  blink::HeapHashMap<blink::WeakMember<blink::GPUCommandEncoder>,
                     blink::Member<Commands>>
      encoders;
  blink::HeapHashMap<blink::WeakMember<blink::GPUCommandBuffer>,
                     blink::Member<Commands>>
      command_buffers;
  blink::HeapHashMap<blink::WeakMember<blink::DawnObjectImpl>,
                     blink::Member<Commands>>
      passes;
  blink::HeapVector<blink::Member<blink::GPUMemoryHeapInfo>> empty_heaps;
  blink::HeapHashSet<blink::WeakMember<blink::GPUDevice>> overflowed_devices;
};
const char WebGPUReadbacks::kSupplementName[] = "StarryWebGPUReadbacks";

WebGPUReadbacks* State(blink::DawnObjectImpl* object, bool create) {
  return object ? WebGPUReadbacks::Get(object->device()->GetExecutionContext(),
                                       create)
                : nullptr;
}

bool BufferRange(blink::GPUBuffer* buffer, uint64_t offset, uint64_t size) {
  return buffer && offset <= buffer->size() && size <= buffer->size() - offset;
}

}  // namespace

const blink::String& WebGPUIdentity(const blink::String& value) {
  return ShouldProtectFingerprint(nullptr) ? blink::g_empty_string : value;
}

const std::optional<uint32_t>& WebGPUDriverVersion(
    const std::optional<uint32_t>& value) {
  static constexpr std::optional<uint32_t> hidden;
  return ShouldProtectFingerprint(nullptr) ? hidden : value;
}

const blink::HeapVector<blink::Member<blink::GPUMemoryHeapInfo>>&
WebGPUMemoryHeaps(
    const blink::HeapVector<blink::Member<blink::GPUMemoryHeapInfo>>& value) {
  if (ShouldProtectFingerprint(nullptr)) {
    if (auto* state = WebGPUReadbacks::Get(nullptr, true))
      return state->empty_heaps;
  }
  return value;
}

void RecordWebGPUTextureCopy(blink::GPUCommandEncoder* encoder,
                             blink::GPUTexture* source,
                             blink::GPUBuffer* destination,
                             const wgpu::TexelCopyTextureInfo& texture,
                             const wgpu::TexelCopyBufferLayout& copy,
                             const wgpu::Extent3D& extent) {
  if (!source || !destination || source->device() != encoder->device() ||
      destination->device() != encoder->device() || source->IsDestroyed() ||
      !(uint32_t(source->Usage()) & uint32_t(wgpu::TextureUsage::CopySrc)) ||
      !(destination->usage() & uint32_t(wgpu::BufferUsage::CopyDst)) ||
      source->sampleCount() != 1 ||
      texture.mipLevel >= source->mipLevelCount() || texture.mipLevel >= 32) {
    InvalidateWebGPUCommands(encoder);
    return;
  }
  if (!extent.width || !extent.height || !extent.depthOrArrayLayers)
    return;
  const auto block = Block(source->Format(), texture.aspect);
  if (!block || texture.origin.x % block->width ||
      texture.origin.y % block->height) {
    InvalidateWebGPUCommands(encoder);
    return;
  }
  const uint64_t width =
      (uint64_t{std::max(1u, source->width() >> texture.mipLevel)} +
       block->width - 1) /
      block->width * block->width;
  const uint64_t height =
      (uint64_t{std::max(1u, source->height() >> texture.mipLevel)} +
       block->height - 1) /
      block->height * block->height;
  const uint32_t depth =
      source->Dimension() == wgpu::TextureDimension::e3D
          ? std::max(1u, source->depthOrArrayLayers() >> texture.mipLevel)
          : source->depthOrArrayLayers();
  if (texture.origin.x > width || extent.width > width - texture.origin.x ||
      texture.origin.y > height || extent.height > height - texture.origin.y ||
      texture.origin.z > depth ||
      extent.depthOrArrayLayers > depth - texture.origin.z) {
    InvalidateWebGPUCommands(encoder);
    return;
  }
  const auto layout = Layout(copy, extent, source->Format(), texture.aspect);
  if (!layout || !BufferRange(destination, copy.offset, *layout->Size())) {
    InvalidateWebGPUCommands(encoder);
    return;
  }
  auto* state = State(encoder, true);
  if (!state)
    return;
  auto& stream = state->Stream(encoder);
  if (stream.finished)
    return;
  auto* op = blink::MakeGarbageCollected<Operation>();
  op->kind = OperationKind::kTextureCopy;
  op->destination = destination;
  op->texture = source;
  op->destination_offset = copy.offset;
  op->layout = *layout;
  stream.Append(op);
}

void RecordWebGPUBufferCopy(blink::GPUCommandEncoder* encoder,
                            blink::DawnObject<wgpu::Buffer>* source_object,
                            uint64_t source_offset,
                            blink::DawnObject<wgpu::Buffer>* destination_object,
                            uint64_t destination_offset,
                            uint64_t size) {
  // WebIDL supplies GPUBuffer objects; upstream uses the DawnObject base in its
  // inline signatures to avoid pulling the GPUBuffer definition into headers.
  auto* source = static_cast<blink::GPUBuffer*>(source_object);
  auto* destination = static_cast<blink::GPUBuffer*>(destination_object);
  if (!BufferRange(source, source_offset, size) ||
      !BufferRange(destination, destination_offset, size) ||
      source == destination || source->device() != encoder->device() ||
      destination->device() != encoder->device() ||
      (source_offset | destination_offset | size) % 4 ||
      !(source->usage() & uint32_t(wgpu::BufferUsage::CopySrc)) ||
      !(destination->usage() & uint32_t(wgpu::BufferUsage::CopyDst))) {
    InvalidateWebGPUCommands(encoder);
    return;
  }
  auto* state = State(encoder, true);
  if (!state)
    return;
  auto& stream = state->Stream(encoder);
  if (stream.finished)
    return;
  auto* op = blink::MakeGarbageCollected<Operation>();
  op->kind = OperationKind::kBufferCopy;
  op->source = source;
  op->destination = destination;
  op->source_offset = source_offset;
  op->destination_offset = destination_offset;
  op->size = size;
  stream.Append(op);
}

void RecordWebGPUBufferClear(blink::GPUCommandEncoder* encoder,
                             blink::DawnObject<wgpu::Buffer>* object,
                             uint64_t offset,
                             std::optional<uint64_t> size) {
  auto* buffer = static_cast<blink::GPUBuffer*>(object);
  if (!buffer || offset > buffer->size()) {
    InvalidateWebGPUCommands(encoder);
    return;
  }
  const uint64_t length = size.value_or(buffer->size() - offset);
  if (!BufferRange(buffer, offset, length) || (offset | length) % 4 ||
      buffer->device() != encoder->device() ||
      !(buffer->usage() & uint32_t(wgpu::BufferUsage::CopyDst))) {
    InvalidateWebGPUCommands(encoder);
    return;
  }
  auto* state = State(encoder, true);
  if (!state)
    return;
  auto& stream = state->Stream(encoder);
  if (stream.finished)
    return;
  auto* op = blink::MakeGarbageCollected<Operation>();
  op->kind = OperationKind::kClear;
  op->destination = buffer;
  op->destination_offset = offset;
  op->size = length;
  stream.Append(op);
}

void RecordWebGPUQueryResolve(blink::GPUCommandEncoder* encoder,
                              blink::DawnObject<wgpu::QuerySet>* query,
                              uint32_t first,
                              uint32_t count,
                              blink::DawnObject<wgpu::Buffer>* destination,
                              uint64_t offset) {
  auto* buffer = static_cast<blink::GPUBuffer*>(destination);
  if (!query || !buffer || query->device() != encoder->device() ||
      buffer->device() != encoder->device() || offset % 256 ||
      first > query->GetHandle().GetCount() ||
      count > query->GetHandle().GetCount() - first ||
      !BufferRange(buffer, offset, uint64_t{count} * 8) ||
      !(buffer->usage() & uint32_t(wgpu::BufferUsage::QueryResolve))) {
    InvalidateWebGPUCommands(encoder);
    return;
  }
  if (auto* state = State(encoder, true)) {
    auto& stream = state->Stream(encoder);
    if (stream.finished)
      return;
    auto* op = blink::MakeGarbageCollected<Operation>();
    op->kind = OperationKind::kClear;
    op->destination = buffer;
    op->destination_offset = offset;
    op->size = uint64_t{count} * 8;
    stream.Append(op);
  }
}

void InvalidateWebGPUCommands(blink::GPUCommandEncoder* encoder) {
  if (auto* state = State(encoder, true)) {
    auto& stream = state->Stream(encoder);
    if (!stream.finished)
      stream.invalid = true;
  }
}

void RegisterWebGPUPass(blink::GPUCommandEncoder* encoder,
                        blink::DawnObjectImpl* pass) {
  if (auto* state = State(encoder, true)) {
    auto& stream = state->Stream(encoder);
    if (!stream.finished)
      state->passes.Set(pass, &stream);
  }
}

void RecordWebGPUShaderWrite(blink::DawnObjectImpl* pass) {
  auto* state = State(pass, false);
  if (!state)
    return;
  auto it = state->passes.find(pass);
  if (it == state->passes.end() || it->value->finished)
    return;
  auto& ops = it->value->operations;
  if (!ops.empty() && ops.back()->kind == OperationKind::kShaderWrite)
    return;
  auto* op = blink::MakeGarbageCollected<Operation>();
  op->kind = OperationKind::kShaderWrite;
  it->value->Append(op);
}

void FinishWebGPUCommands(blink::GPUCommandEncoder* encoder,
                          blink::GPUCommandBuffer* buffer) {
  auto* state = State(encoder, false);
  if (!state)
    return;
  auto& stream = state->Stream(encoder);
  if (stream.finished)
    return;
  stream.finished = true;
  state->command_buffers.Set(buffer, &stream);
}

void SubmitWebGPUCommands(
    blink::GPUQueue* queue,
    const blink::HeapVector<blink::Member<blink::GPUCommandBuffer>>& buffers) {
  auto* state = State(queue, false);
  if (!state)
    return;
  blink::HeapHashSet<blink::Member<blink::GPUCommandBuffer>> seen;
  for (const auto& buffer : buffers) {
    if (!buffer)
      return;
    auto it = state->command_buffers.find(buffer.Get());
    if (buffer->device() != queue->device() || seen.Contains(buffer) ||
        (it != state->command_buffers.end() &&
         (it->value->submitted || it->value->invalid)))
      return;
    seen.insert(buffer);
  }
  for (const auto& buffer : buffers) {
    auto it = state->command_buffers.find(buffer.Get());
    if (it == state->command_buffers.end() || it->value->submitted)
      continue;
    auto& stream = *it->value;
    stream.submitted = true;
    if (stream.overflowed)
      state->overflowed_devices.insert(buffer->device());
    for (auto& op : stream.operations)
      state->Apply(*op, queue->device());
    stream.operations.clear();
  }
}

void OverwriteWebGPUBuffer(blink::GPUQueue* queue,
                           blink::GPUBuffer* buffer,
                           uint64_t offset,
                           uint64_t size) {
  if (!BufferRange(buffer, offset, size) || (offset | size) % 4 ||
      buffer->device() != queue->device() ||
      !(buffer->usage() & uint32_t(wgpu::BufferUsage::CopyDst)) ||
      buffer->GetHandle().GetMapState() != wgpu::BufferMapState::Unmapped)
    return;
  if (auto* state = State(buffer, false)) {
    if (auto* data = state->Find(buffer); data && !data->destroyed) {
      data->pixels.Overwrite(offset, size);
    }
  }
}

void BeginWebGPUMapping(blink::GPUBuffer* buffer,
                        uint32_t mode,
                        uint64_t offset,
                        uint64_t size) {
  if (buffer->GetHandle().GetMapState() != wgpu::BufferMapState::Unmapped ||
      !BufferRange(buffer, offset, size) || offset % 8 || size % 4 ||
      (mode != uint32_t(wgpu::MapMode::Read) &&
       mode != uint32_t(wgpu::MapMode::Write)) ||
      (mode == uint32_t(wgpu::MapMode::Read) &&
       !(buffer->usage() & uint32_t(wgpu::BufferUsage::MapRead))) ||
      (mode == uint32_t(wgpu::MapMode::Write) &&
       !(buffer->usage() & uint32_t(wgpu::BufferUsage::MapWrite))))
    return;
  auto* state = State(buffer, true);
  if (!state)
    return;
  auto& data = state->Ensure(buffer);
  if (data.destroyed)
    return;
  data.mapping = true;
  data.mapping_read = mode == uint32_t(wgpu::MapMode::Read);
  data.mapping_processed = false;
  data.mapping_seed.reset();
  data.mapping_offset = offset;
  data.mapping_size = size;
}

void ProtectWebGPUMapping(blink::GPUBuffer* buffer,
                          uint64_t offset,
                          void* original,
                          size_t size) {
  auto* state = State(buffer, false);
  auto* data = state ? state->Find(buffer) : nullptr;
  if (!data || !data->mapping || data->destroyed || !original || !size)
    return;
  if (!data->mapping_read) {
    data->pixels.Overwrite(offset, size);
    return;
  }
  if (!data->mapping_processed) {
    data->mapping_processed = true;
    data->mapping_seed =
        FingerprintSeed(buffer->device()->GetExecutionContext());
  }
  if (!data->mapping_seed)
    return;
  // Like GPUBuffer::GetMappedRangeImpl, this is Dawn wire's mutable CPU shadow,
  // not GPU storage. READ mappings are never uploaded on unmap. Transform it
  // only for each newly exposed range (Blink rejects overlapping ranges), so
  // disjoint getMappedRange calls share a consistent policy snapshot and all
  // of Blink's original overlap checks / detachment behavior stay intact.
  auto bytes =
      UNSAFE_BUFFERS(base::span(static_cast<uint8_t*>(original), size));
  if (state->overflowed_devices.Contains(buffer->device())) {
    std::ranges::fill(bytes, 0);
  } else {
    data->pixels.Protect(offset, bytes, *data->mapping_seed);
  }
}

void ResetWebGPUMapping(blink::GPUBuffer* buffer) {
  if (auto* state = State(buffer, false)) {
    if (auto* data = state->Find(buffer)) {
      data->mapping = false;
      data->mapping_processed = false;
      data->mapping_seed.reset();
    }
  }
}

void DestroyWebGPUBuffer(blink::GPUBuffer* buffer) {
  if (auto* state = State(buffer, true)) {
    auto& data = state->Ensure(buffer);
    data.destroyed = true;
    data.mapping = false;
    data.pixels.Reset(buffer->size());
  }
}

}  // namespace starry
