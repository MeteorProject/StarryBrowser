// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_RENDERER_WEBGPU_FINGERPRINT_PROTECTION_H_
#define STARRY_RENDERER_WEBGPU_FINGERPRINT_PROTECTION_H_

#include <cstddef>
#include <cstdint>
#include <optional>

#include "third_party/blink/renderer/modules/modules_export.h"
#include "third_party/blink/renderer/platform/graphics/gpu/webgpu_cpp.h"
#include "third_party/blink/renderer/platform/heap/collection_support/heap_vector.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {
template <typename T>
class DawnObject;
class DawnObjectImpl;
class ExecutionContext;
class GPUBuffer;
class GPUCommandBuffer;
class GPUCommandEncoder;
class GPUMemoryHeapInfo;
class GPUQueue;
class GPUTexture;
}  // namespace blink

namespace starry {

MODULES_EXPORT const blink::String& WebGPUIdentity(const blink::String& value);
MODULES_EXPORT const std::optional<uint32_t>& WebGPUDriverVersion(
    const std::optional<uint32_t>& value);
MODULES_EXPORT const blink::HeapVector<blink::Member<blink::GPUMemoryHeapInfo>>&
WebGPUMemoryHeaps(
    const blink::HeapVector<blink::Member<blink::GPUMemoryHeapInfo>>& value);

MODULES_EXPORT void RecordWebGPUTextureCopy(
    blink::GPUCommandEncoder* encoder,
    blink::GPUTexture* source,
    blink::GPUBuffer* destination,
    const wgpu::TexelCopyTextureInfo& texture,
    const wgpu::TexelCopyBufferLayout& layout,
    const wgpu::Extent3D& extent);
MODULES_EXPORT void RecordWebGPUBufferCopy(
    blink::GPUCommandEncoder* encoder,
    blink::DawnObject<wgpu::Buffer>* source,
    uint64_t source_offset,
    blink::DawnObject<wgpu::Buffer>* destination,
    uint64_t destination_offset,
    uint64_t size);
MODULES_EXPORT void RecordWebGPUBufferClear(
    blink::GPUCommandEncoder* encoder,
    blink::DawnObject<wgpu::Buffer>* buffer,
    uint64_t offset,
    std::optional<uint64_t> size);
MODULES_EXPORT void RecordWebGPUQueryResolve(
    blink::GPUCommandEncoder* encoder,
    blink::DawnObject<wgpu::QuerySet>* query,
    uint32_t first,
    uint32_t count,
    blink::DawnObject<wgpu::Buffer>* destination,
    uint64_t offset);
MODULES_EXPORT void InvalidateWebGPUCommands(blink::GPUCommandEncoder* encoder);
MODULES_EXPORT void RegisterWebGPUPass(blink::GPUCommandEncoder* encoder,
                                       blink::DawnObjectImpl* pass);
MODULES_EXPORT void RecordWebGPUShaderWrite(blink::DawnObjectImpl* pass);
MODULES_EXPORT void FinishWebGPUCommands(blink::GPUCommandEncoder* encoder,
                                         blink::GPUCommandBuffer* buffer);
MODULES_EXPORT void SubmitWebGPUCommands(
    blink::GPUQueue* queue,
    const blink::HeapVector<blink::Member<blink::GPUCommandBuffer>>& buffers);
MODULES_EXPORT void OverwriteWebGPUBuffer(blink::GPUQueue* queue,
                                          blink::GPUBuffer* buffer,
                                          uint64_t offset,
                                          uint64_t size);
MODULES_EXPORT void BeginWebGPUMapping(blink::GPUBuffer* buffer,
                                       uint32_t mode,
                                       uint64_t offset,
                                       uint64_t size);
MODULES_EXPORT void ProtectWebGPUMapping(blink::GPUBuffer* buffer,
                                         uint64_t offset,
                                         void* data,
                                         size_t size);
MODULES_EXPORT void ResetWebGPUMapping(blink::GPUBuffer* buffer);
MODULES_EXPORT void DestroyWebGPUBuffer(blink::GPUBuffer* buffer);

}  // namespace starry

#endif  // STARRY_RENDERER_WEBGPU_FINGERPRINT_PROTECTION_H_
