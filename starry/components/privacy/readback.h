// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMPONENTS_PRIVACY_READBACK_H_
#define STARRY_COMPONENTS_PRIVACY_READBACK_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/function_ref.h"

namespace starry::privacy {

// Only normalized/floating-point color data is protected. Integer textures,
// query results, CPU uploads and general compute buffers retain exact values.
enum class PixelFormat {
  kNone,
  kUnorm8,
  kSnorm8,
  kUnorm16,
  kSnorm16,
  kFloat16,
  kFloat32,
  kRGB565,
  kRGBA4444,
  kRGBA5551,
  kRGB10A2,
  kRG11B10Float,
};

struct ReadbackLayout {
  PixelFormat format = PixelFormat::kNone;
  uint64_t row_bytes = 0;
  uint64_t row_stride = 0;
  uint64_t rows = 0;
  uint64_t image_stride = 0;
  uint64_t images = 1;

  std::optional<uint64_t> Size() const;
  bool operator==(const ReadbackLayout&) const = default;
};

// Byte provenance for a GPU buffer, independent of Blink and graphics APIs.
// Ranges are disjoint; padding is never classified as pixels. Copies keep the
// source layout even when copied into another buffer or split across calls.
class ReadbackBuffer {
 public:
  explicit ReadbackBuffer(uint64_t size = 0);
  ~ReadbackBuffer();
  ReadbackBuffer(const ReadbackBuffer&);
  ReadbackBuffer& operator=(const ReadbackBuffer&);

  void Reset(uint64_t size);
  void Overwrite(uint64_t offset, uint64_t size);
  void WritePixels(uint64_t offset, const ReadbackLayout& layout);
  void CopyFrom(const ReadbackBuffer& source,
                uint64_t source_offset,
                uint64_t destination_offset,
                uint64_t size);
  bool empty() const;

  // For a partial scalar read (e.g. one byte of a WebGL float), the callback
  // reads the original complete scalar. Missing fragments are cleared rather
  // than exposing unprotected data. WebGPU mappings are scalar-aligned.
  using ReadOriginal = base::FunctionRef<bool(uint64_t, base::span<uint8_t>)>;
  void Protect(uint64_t offset,
               base::span<uint8_t> data,
               uint64_t seed,
               ReadOriginal read_original) const;
  void Protect(uint64_t offset, base::span<uint8_t> data, uint64_t seed) const;

 private:
  struct Region {
    uint64_t begin;
    uint64_t end;
    uint64_t logical_offset;
    ReadbackLayout layout;
  };

  void Compact();
  static bool ContainsPixels(const Region& region);

  uint64_t size_ = 0;
  // Bound adversarial fragmentation. Overflowed readbacks fail closed until
  // the buffer is completely rewritten; unprotected browsing is unaffected.
  bool overflowed_ = false;
  std::vector<Region> regions_;
};

}  // namespace starry::privacy

#endif  // STARRY_COMPONENTS_PRIVACY_READBACK_H_
