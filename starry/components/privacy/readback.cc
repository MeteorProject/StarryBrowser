// Copyright 2026 The Starry Browser Authors

#include "starry/components/privacy/readback.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <utility>

#include "base/notreached.h"
#include "base/numerics/byte_conversions.h"
#include "base/numerics/checked_math.h"

namespace starry::privacy {

namespace {

constexpr size_t kMaxRegions = 4096;

uint64_t Mix(uint64_t value) {
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

size_t SampleBytes(PixelFormat format) {
  switch (format) {
    case PixelFormat::kNone:
    case PixelFormat::kUnorm8:
    case PixelFormat::kSnorm8:
      return 1;
    case PixelFormat::kUnorm16:
    case PixelFormat::kSnorm16:
    case PixelFormat::kFloat16:
    case PixelFormat::kRGB565:
    case PixelFormat::kRGBA4444:
    case PixelFormat::kRGBA5551:
      return 2;
    case PixelFormat::kFloat32:
    case PixelFormat::kRGB10A2:
    case PixelFormat::kRG11B10Float:
      return 4;
  }
  NOTREACHED();
}

uint32_t NoiseInteger(uint32_t value,
                      unsigned bits,
                      unsigned noisy_bits,
                      uint64_t seed) {
  const uint32_t maximum = (uint32_t{1} << bits) - 1;
  if (value == 0 || value == maximum) {
    return value;
  }
  const uint32_t mask = (uint32_t{1} << noisy_bits) - 1;
  const uint32_t bucket = value & ~mask;
  return std::clamp<uint32_t>(
      bucket |
          (static_cast<uint32_t>(Mix(seed ^ bucket ^ (uint64_t{bits} << 40))) &
           mask),
      1, maximum - 1);
}

uint32_t NoiseFloat(uint32_t value,
                    unsigned mantissa_bits,
                    unsigned exponent_bits,
                    unsigned bias,
                    uint64_t seed) {
  const uint32_t mantissa_mask = (uint32_t{1} << mantissa_bits) - 1;
  const uint32_t exponent_mask = (uint32_t{1} << exponent_bits) - 1;
  const uint32_t magnitude_mask =
      (exponent_mask << mantissa_bits) | mantissa_mask;
  const uint32_t magnitude = value & magnitude_mask;
  const uint32_t exponent = (value >> mantissa_bits) & exponent_mask;
  // Preserve zero, exact unity, infinities and NaN payloads.
  if (!magnitude || magnitude == (bias << mantissa_bits) ||
      exponent == exponent_mask) {
    return value;
  }
  // Discard a small bucket of low precision, rather than adding fresh random
  // noise on every read. Around [0, 1], the bucket is about 1/64 wide so an
  // 8-bit conversion cannot simply remove all of the protection.
  const unsigned noisy_bits = std::clamp<int>(
      static_cast<int>(mantissa_bits + bias) - static_cast<int>(exponent) - 6,
      1, mantissa_bits);
  const uint32_t mask = (uint32_t{1} << noisy_bits) - 1;
  const uint32_t bucket = value & ~mask;
  return bucket |
         (static_cast<uint32_t>(Mix(seed ^ bucket ^ 0x677075666c6f6174ULL)) &
          mask);
}

uint32_t NoisePacked(uint32_t value, PixelFormat format, uint64_t seed) {
  auto channel = [&](unsigned offset, unsigned bits, unsigned noise) {
    const uint32_t mask = (uint32_t{1} << bits) - 1;
    return NoiseInteger((value >> offset) & mask, bits, noise, seed) << offset;
  };
  switch (format) {
    case PixelFormat::kRGB565:
      return channel(11, 5, 1) | channel(5, 6, 1) | channel(0, 5, 1);
    case PixelFormat::kRGBA4444:
      return channel(12, 4, 1) | channel(8, 4, 1) | channel(4, 4, 1) |
             channel(0, 4, 1);
    case PixelFormat::kRGBA5551:
      return channel(11, 5, 1) | channel(6, 5, 1) | channel(1, 5, 1) |
             (value & 1);
    case PixelFormat::kRGB10A2:
      return channel(0, 10, 4) | channel(10, 10, 4) | channel(20, 10, 4) |
             (value & 0xc0000000);
    case PixelFormat::kRG11B10Float:
      return NoiseFloat(value & 0x7ff, 6, 5, 15, seed) |
             (NoiseFloat((value >> 11) & 0x7ff, 6, 5, 15, seed) << 11) |
             (NoiseFloat((value >> 22) & 0x3ff, 5, 5, 15, seed) << 22);
    default:
      return value;
  }
}

void ProtectSample(base::span<uint8_t> bytes,
                   PixelFormat format,
                   uint64_t seed) {
  switch (format) {
    case PixelFormat::kNone:
      return;
    case PixelFormat::kUnorm8:
      bytes[0] = static_cast<uint8_t>(NoiseInteger(bytes[0], 8, 2, seed));
      return;
    case PixelFormat::kSnorm8:
      if (bytes[0] != 0 && bytes[0] != 0x7f && bytes[0] != 0x80 &&
          bytes[0] != 0x81) {
        const uint8_t bucket = bytes[0] & ~uint8_t{1};
        bytes[0] = bucket | static_cast<uint8_t>(Mix(seed ^ bucket) & 1);
      }
      return;
    case PixelFormat::kUnorm16:
      bytes.copy_from(
          base::U16ToLittleEndian(static_cast<uint16_t>(NoiseInteger(
              base::U16FromLittleEndian(bytes.first<2>()), 16, 10, seed))));
      return;
    case PixelFormat::kSnorm16: {
      const uint16_t value = base::U16FromLittleEndian(bytes.first<2>());
      if (value != 0 && value != 0x7fff && value != 0x8000 && value != 0x8001) {
        const uint16_t bucket = value & ~uint16_t{511};
        bytes.copy_from(base::U16ToLittleEndian(
            bucket | static_cast<uint16_t>(Mix(seed ^ bucket) & 511)));
      }
      return;
    }
    case PixelFormat::kFloat16:
      bytes.copy_from(base::U16ToLittleEndian(static_cast<uint16_t>(NoiseFloat(
          base::U16FromLittleEndian(bytes.first<2>()), 10, 5, 15, seed))));
      return;
    case PixelFormat::kFloat32:
      bytes.copy_from(base::U32ToLittleEndian(NoiseFloat(
          base::U32FromLittleEndian(bytes.first<4>()), 23, 8, 127, seed)));
      return;
    default:
      if (bytes.size() == 2) {
        bytes.copy_from(
            base::U16ToLittleEndian(static_cast<uint16_t>(NoisePacked(
                base::U16FromLittleEndian(bytes.first<2>()), format, seed))));
      } else {
        bytes.copy_from(base::U32ToLittleEndian(NoisePacked(
            base::U32FromLittleEndian(bytes.first<4>()), format, seed)));
      }
  }
}

// Return the next contiguous pixel row interval at/after a logical byte offset.
std::optional<std::pair<uint64_t, uint64_t>> NextRow(
    const ReadbackLayout& layout,
    uint64_t offset) {
  uint64_t image = offset / layout.image_stride;
  if (image >= layout.images) {
    return std::nullopt;
  }
  const uint64_t within_image = offset % layout.image_stride;
  uint64_t row = within_image / layout.row_stride;
  if (row < layout.rows &&
      within_image % layout.row_stride >= layout.row_bytes) {
    ++row;
  }
  if (row >= layout.rows) {
    ++image;
    row = 0;
  }
  if (image >= layout.images) {
    return std::nullopt;
  }
  const uint64_t start = image * layout.image_stride + row * layout.row_stride;
  return std::pair(start, start + layout.row_bytes);
}

}  // namespace

std::optional<uint64_t> ReadbackLayout::Size() const {
  if (!row_bytes || !rows || !images || row_stride < row_bytes ||
      row_bytes % SampleBytes(format)) {
    return std::nullopt;
  }
  auto last_image = base::CheckMul(row_stride, rows - 1) + row_bytes;
  if (!last_image.IsValid() || image_stride < last_image.ValueOrDie()) {
    return std::nullopt;
  }
  auto total = base::CheckMul(image_stride, images - 1) + last_image;
  return total.IsValid() ? std::optional(total.ValueOrDie()) : std::nullopt;
}

ReadbackBuffer::ReadbackBuffer(uint64_t size) : size_(size) {}
ReadbackBuffer::~ReadbackBuffer() = default;
ReadbackBuffer::ReadbackBuffer(const ReadbackBuffer&) = default;
ReadbackBuffer& ReadbackBuffer::operator=(const ReadbackBuffer&) = default;

void ReadbackBuffer::Reset(uint64_t size) {
  size_ = size;
  overflowed_ = false;
  regions_.clear();
}

bool ReadbackBuffer::empty() const {
  return regions_.empty() && !overflowed_;
}

bool ReadbackBuffer::ContainsPixels(const Region& region) {
  auto row = NextRow(region.layout, region.logical_offset);
  return row && std::max(row->first, region.logical_offset) <
                    region.logical_offset + (region.end - region.begin);
}

void ReadbackBuffer::Overwrite(uint64_t offset, uint64_t size) {
  if (!size || offset > size_ || size > size_ - offset) {
    return;
  }
  if (!offset && size == size_) {
    Reset(size_);
    return;
  }
  const uint64_t end = offset + size;
  std::vector<Region> kept;
  for (const Region& region : regions_) {
    if (region.end <= offset || region.begin >= end) {
      kept.push_back(region);
      continue;
    }
    if (region.begin < offset) {
      Region left = region;
      left.end = offset;
      if (ContainsPixels(left))
        kept.push_back(left);
    }
    if (region.end > end) {
      Region right = region;
      right.logical_offset += end - right.begin;
      right.begin = end;
      if (ContainsPixels(right))
        kept.push_back(right);
    }
  }
  regions_ = std::move(kept);
  Compact();
}

void ReadbackBuffer::WritePixels(uint64_t offset,
                                 const ReadbackLayout& layout) {
  const auto size = layout.Size();
  if (!size || offset > size_ || *size > size_ - offset) {
    return;
  }
  if (layout.row_stride == layout.row_bytes &&
      layout.image_stride == layout.row_stride * layout.rows) {
    Overwrite(offset, *size);
    if (!overflowed_ && layout.format != PixelFormat::kNone) {
      regions_.push_back({offset, offset + *size, 0, layout});
      Compact();
    }
    return;
  }
  if (overflowed_)
    return;
  if (regions_.empty()) {
    if (layout.format != PixelFormat::kNone) {
      regions_.push_back({offset, offset + *size, 0, layout});
    }
    return;
  }
  // Avoid unbounded bookkeeping for pathological, previously fragmented
  // layouts.
  if (layout.rows > 1048576 / layout.images) {
    overflowed_ = true;
    regions_.clear();
    return;
  }
  // Only texel bytes are overwritten; row/layer padding retains its provenance.
  for (uint64_t image = 0; image < layout.images; ++image) {
    for (uint64_t row = 0; row < layout.rows; ++row) {
      Overwrite(offset + image * layout.image_stride + row * layout.row_stride,
                layout.row_bytes);
    }
  }
  if (overflowed_)
    return;
  if (layout.format != PixelFormat::kNone) {
    // Split around existing padding ranges so all stored intervals stay
    // disjoint. One row can usually be represented by one interval; Compact
    // joins adjacent rows into a single strided region where there is no
    // intervening old data.
    for (uint64_t image = 0; image < layout.images; ++image) {
      for (uint64_t row = 0; row < layout.rows; ++row) {
        const uint64_t logical =
            image * layout.image_stride + row * layout.row_stride;
        regions_.push_back({offset + logical,
                            offset + logical + layout.row_bytes, logical,
                            layout});
        if (regions_.size() >= 2 * kMaxRegions) {
          Compact();
          if (overflowed_)
            return;
        }
      }
    }
    Compact();
  }
}

void ReadbackBuffer::CopyFrom(const ReadbackBuffer& source,
                              uint64_t source_offset,
                              uint64_t destination_offset,
                              uint64_t size) {
  if (!size || source_offset > source.size_ ||
      size > source.size_ - source_offset || destination_offset > size_ ||
      size > size_ - destination_offset) {
    return;
  }
  // Take the source snapshot before erasing, including for same-buffer copies.
  const auto source_regions = source.regions_;
  const bool source_overflowed = source.overflowed_;
  Overwrite(destination_offset, size);
  if (source_overflowed) {
    overflowed_ = true;
    regions_.clear();
    return;
  }
  for (const Region& region : source_regions) {
    const uint64_t begin = std::max(source_offset, region.begin);
    const uint64_t end = std::min(source_offset + size, region.end);
    if (begin < end) {
      regions_.push_back({destination_offset + (begin - source_offset),
                          destination_offset + (end - source_offset),
                          region.logical_offset + (begin - region.begin),
                          region.layout});
    }
  }
  Compact();
}

void ReadbackBuffer::Compact() {
  std::sort(regions_.begin(), regions_.end(),
            [](const Region& a, const Region& b) { return a.begin < b.begin; });
  std::vector<Region> compact;
  for (const Region& region : regions_) {
    if (!ContainsPixels(region))
      continue;
    if (!compact.empty()) {
      Region& previous = compact.back();
      const auto logical = base::CheckAdd(previous.logical_offset,
                                          region.begin - previous.begin);
      if (previous.end <= region.begin && previous.layout == region.layout &&
          logical.IsValid() && logical.ValueOrDie() == region.logical_offset) {
        // Only bridge a gap if the shared layout says it contains no texels.
        Region gap{previous.end, region.begin,
                   previous.logical_offset + previous.end - previous.begin,
                   previous.layout};
        if (gap.begin == gap.end || !ContainsPixels(gap)) {
          previous.end = region.end;
          continue;
        }
      }
    }
    compact.push_back(region);
  }
  if (compact.size() > kMaxRegions) {
    overflowed_ = true;
    compact.clear();
  }
  regions_ = std::move(compact);
}

void ReadbackBuffer::Protect(uint64_t offset,
                             base::span<uint8_t> data,
                             uint64_t seed) const {
  Protect(offset, data, seed,
          [](uint64_t, base::span<uint8_t>) { return false; });
}

void ReadbackBuffer::Protect(uint64_t offset,
                             base::span<uint8_t> data,
                             uint64_t seed,
                             ReadOriginal read_original) const {
  if (offset > size_ || data.size() > size_ - offset)
    return;
  if (overflowed_) {
    std::ranges::fill(data, 0);
    return;
  }
  std::array<std::array<uint8_t, 256>, 2> tables;
  std::array<bool, 2> table_ready{};
  const uint64_t end = offset + data.size();
  for (const Region& region : regions_) {
    uint64_t begin = std::max(offset, region.begin);
    const uint64_t region_end = std::min(end, region.end);
    if (begin >= region_end)
      continue;
    const size_t sample_size = SampleBytes(region.layout.format);
    while (begin < region_end) {
      const uint64_t logical = region.logical_offset + begin - region.begin;
      const auto row = NextRow(region.layout, logical);
      if (!row)
        break;
      if (logical < row->first) {
        begin += std::min(row->first - logical, region_end - begin);
        continue;
      }
      const uint64_t row_end =
          begin + std::min(row->second - logical, region_end - begin);
      if (region.layout.format == PixelFormat::kUnorm8 ||
          region.layout.format == PixelFormat::kSnorm8) {
        auto bytes = data.subspan(static_cast<size_t>(begin - offset),
                                  static_cast<size_t>(row_end - begin));
        const size_t table_index =
            region.layout.format == PixelFormat::kUnorm8 ? 0 : 1;
        // Large rows use the same 256-value transform. Small/one-byte reads
        // avoid building a table; both paths produce exactly the same bytes.
        if (bytes.size() >= 256 || table_ready[table_index]) {
          auto& table = tables[table_index];
          if (!table_ready[table_index]) {
            for (size_t i = 0; i < table.size(); ++i) {
              table[i] = static_cast<uint8_t>(i);
              ProtectSample(base::span_from_ref(table[i]), region.layout.format,
                            seed);
            }
            table_ready[table_index] = true;
          }
          for (uint8_t& value : bytes)
            value = table[value];
        } else {
          for (uint8_t& value : bytes) {
            ProtectSample(base::span_from_ref(value), region.layout.format,
                          seed);
          }
        }
        begin = row_end;
        continue;
      }
      while (begin < row_end) {
        const uint64_t in_row =
            region.logical_offset + (begin - region.begin) - row->first;
        const size_t preceding = in_row % sample_size;
        const size_t count =
            std::min<uint64_t>(sample_size - preceding, row_end - begin);
        auto destination =
            data.subspan(static_cast<size_t>(begin - offset), count);
        std::array<uint8_t, 4> original{};
        auto sample = base::span(original).first(sample_size);
        bool complete = false;
        if (!preceding && count == sample_size) {
          sample.copy_from(destination);
          complete = true;
        } else if (begin >= preceding && begin - preceding >= region.begin &&
                   sample_size <= region.end - (begin - preceding)) {
          complete = read_original(begin - preceding, sample);
        }
        if (complete) {
          ProtectSample(sample, region.layout.format, seed);
          destination.copy_from(sample.subspan(preceding, count));
        } else {
          std::ranges::fill(destination, 0);
        }
        begin += count;
      }
    }
  }
}

}  // namespace starry::privacy
