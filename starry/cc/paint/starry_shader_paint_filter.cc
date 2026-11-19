// Copyright 2026 The Starry Browser Authors

#include "starry/cc/paint/starry_shader_paint_filter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>

#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/notreached.h"
#include "base/numerics/checked_math.h"
#include "cc/paint/paint_op_reader.h"
#include "cc/paint/paint_op_writer.h"
#include "starry/cc/paint/starry_glass_sksl.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkSamplingOptions.h"
#include "third_party/skia/include/core/SkShader.h"
#include "third_party/skia/include/core/SkString.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/core/SkTileMode.h"
#include "third_party/skia/include/effects/SkImageFilters.h"
#include "third_party/skia/include/effects/SkRuntimeEffect.h"

namespace cc {

namespace {

using Shader = StarryShaderPaintFilter::Shader;
using Spec = StarryShaderPaintFilter::Spec;

constexpr std::string_view kSourceChild = "src";
constexpr std::string_view kMaterialChild = "material";
constexpr std::string_view kBlurChildPrefix = "blur";

sk_sp<SkRuntimeEffect> Compile(const char* name, const char* sksl) {
  SkRuntimeEffect::Result result =
      SkRuntimeEffect::MakeForShader(SkString(sksl));
  if (!result.effect) {
    LOG(ERROR) << "[starry] shader " << name
               << " does not compile: " << result.errorText.c_str();
    return nullptr;
  }
  // uniform 块按 float 数组传，只能有 float 类的 uniform。
  for (const SkRuntimeEffect::Uniform& uniform : result.effect->uniforms()) {
    using Type = SkRuntimeEffect::Uniform::Type;
    if (uniform.type != Type::kFloat && uniform.type != Type::kFloat2 &&
        uniform.type != Type::kFloat3 && uniform.type != Type::kFloat4) {
      LOG(ERROR) << "[starry] shader " << name << ": uniform "
                 << std::string(uniform.name) << " is not a float type";
      return nullptr;
    }
  }
  if (result.effect->uniformSize() >
      StarryShaderPaintFilter::kMaxUniformFloats * sizeof(float)) {
    LOG(ERROR) << "[starry] shader " << name << ": too many uniforms";
    return nullptr;
  }
  return result.effect;
}

// SkRuntimeEffect 不可变，可以在浏览器进程的界面线程和 GPU 进程的合成线程上共用。
SkRuntimeEffect* EffectFor(Shader shader) {
  switch (shader) {
    case Shader::kGlass: {
      static base::NoDestructor<sk_sp<SkRuntimeEffect>> effect(
          Compile("glass", starry_shaders::kGlass));
      return effect->get();
    }
  }
  NOTREACHED();
}

bool IsValid(const Spec& spec) {
  SkRuntimeEffect* effect = EffectFor(spec.shader);
  if (!effect) {
    return false;
  }
  if (spec.uniforms.size() * sizeof(float) != effect->uniformSize()) {
    return false;
  }
  if (spec.blur_sigmas.size() > StarryShaderPaintFilter::kMaxBlurs) {
    return false;
  }
  for (float sigma : spec.blur_sigmas) {
    if (!std::isfinite(sigma) || sigma < 0 ||
        sigma > StarryShaderPaintFilter::kMaxSigma) {
      return false;
    }
  }
  if (!std::isfinite(spec.sample_radius) || spec.sample_radius < 0 ||
      spec.sample_radius > StarryShaderPaintFilter::kMaxSampleRadius) {
    return false;
  }
  if (!spec.crop.isFinite() || !spec.output_crop.isFinite()) {
    return false;
  }
  if (!std::isfinite(spec.scale) || spec.scale <= 0 ||
      spec.scale > StarryShaderPaintFilter::kMaxScale) {
    return false;
  }
  // uniform 里的 NaN、无穷大不拦：只会让这块玻璃画错，着色器取样的范围由
  // sample_radius 限定。
  return true;
}

sk_sp<SkImageFilter> Build(const Spec& spec, float scale_x, float scale_y) {
  if (!IsValid(spec)) {
    return nullptr;
  }
  SkRuntimeEffect* effect = EffectFor(spec.shader);
  SkRuntimeEffectBuilder builder(
      sk_ref_sp(effect),
      SkData::MakeWithCopy(spec.uniforms.data(), effect->uniformSize()));

  builder.uniform("pixelScale") = SkV2{scale_x, scale_y};

  // 输入本身：整个输入，或者裁过、范围外镜像的输入。
  sk_sp<SkImageFilter> source;
  if (!spec.crop.isEmpty()) {
    SkRect crop = SkRect::MakeLTRB(
        spec.crop.left() * scale_x, spec.crop.top() * scale_y,
        spec.crop.right() * scale_x, spec.crop.bottom() * scale_y);
    source = SkImageFilters::Crop(crop, SkTileMode::kMirror, nullptr);
  }

  // Keep an anisotropic surface transform on the exact Gaussian path. Mip
  // levels use square texels; normal device and animation scales are uniform.
  const bool use_mips = spec.downsample_blurs && scale_x == scale_y;
  static base::NoDestructor<sk_sp<SkRuntimeEffect>> downsample_effect(Compile(
      "glass_downsample",
      "uniform shader src; half4 main(float2 p) { return src.eval(p); }"));
  if (use_mips && !*downsample_effect) {
    return nullptr;
  }
  // Resolve each bilinear half-size pass before reducing again. Otherwise
  // consecutive transforms can collapse into a single undersampled draw.
  std::vector<sk_sp<SkImageFilter>> levels{source};
  auto mip = [&](int level) {
    while (static_cast<int>(levels.size()) <= level) {
      auto reduced = SkImageFilters::MatrixTransform(
          SkMatrix::Scale(0.5f, 0.5f), SkSamplingOptions(SkFilterMode::kLinear),
          levels.back());
      reduced = SkImageFilters::RuntimeShader(
          SkRuntimeShaderBuilder(*downsample_effect), 0, "src", reduced,
          /*restrictOutputToInputBounds=*/true);
      if (!spec.crop.isEmpty()) {
        const float scale = std::ldexp(1.0f, -static_cast<int>(levels.size()));
        const SkRect crop =
            SkRect::MakeLTRB(spec.crop.left() * scale_x * scale,
                             spec.crop.top() * scale_y * scale,
                             spec.crop.right() * scale_x * scale,
                             spec.crop.bottom() * scale_y * scale);
        // Give all consumers the same bounded level so the filter cache can
        // reuse it even when their sampling footprints differ.
        reduced = SkImageFilters::Crop(crop, SkTileMode::kMirror, reduced);
      }
      levels.push_back(std::move(reduced));
    }
    return levels[level];
  };

  std::array<sk_sp<SkImageFilter>, StarryShaderPaintFilter::kMaxBlurs> low;
  std::array<sk_sp<SkImageFilter>, StarryShaderPaintFilter::kMaxBlurs> high;
  for (size_t i = 0; i < std::min(spec.blur_sigmas.size(), size_t{5}); ++i) {
    const float sigma = spec.blur_sigmas[i];
    float blend = 0;
    low[i] = source;
    if (sigma > 0) {
      if (use_mips) {
        // A box-filtered mip with bilinear reconstruction has approximately
        // sigma = half its texel width. Blend adjacent levels by variance to
        // keep the requested blur width, including while a shape is resized.
        const float lod = std::clamp(
            std::log2(std::max(1.0f, 2 * sigma * scale_x)), 0.0f, 16.0f);
        const int level = static_cast<int>(std::floor(lod));
        const float ratio =
            std::max(1.0f, 2 * sigma * scale_x) / std::ldexp(1.0f, level);
        blend = std::clamp((ratio * ratio - 1) / 3, 0.0f, 1.0f);
        const float texel_width = std::ldexp(1.0f, level);
        auto lower = mip(level);
        if (spec.scale >= 1.5f && blend > 0) {
          // At high DPR the final material covers many more fragments. Resolve
          // the blend at the finer mip's resolution so each material lookup
          // only samples one texture. At 1x, direct sampling avoids extra
          // passes.
          static base::NoDestructor<sk_sp<SkRuntimeEffect>> blend_effect(
              Compile(
                  "glass_mip_blend",
                  "uniform shader lo; uniform shader hi; uniform float amount;"
                  "half4 main(float2 p) { return mix(lo.eval(p), hi.eval(p), "
                  "half(amount)); }"));
          if (!*blend_effect) {
            return nullptr;
          }
          SkRuntimeShaderBuilder blend_builder(*blend_effect);
          blend_builder.uniform("amount") = blend;
          std::array<std::string_view, 2> children{"lo", "hi"};
          const std::array<sk_sp<SkImageFilter>, 2> levels_to_blend{
              lower,
              SkImageFilters::MatrixTransform(
                  SkMatrix::Scale(2, 2),
                  SkSamplingOptions(SkFilterMode::kLinear), mip(level + 1))};
          lower = SkImageFilters::RuntimeShader(
              blend_builder, 0, children.data(), levels_to_blend.data(), 2,
              /*restrictOutputToInputBounds=*/true);
          blend = 0;
        }
        low[i] = SkImageFilters::MatrixTransform(
            SkMatrix::Scale(texel_width, texel_width),
            SkSamplingOptions(SkFilterMode::kLinear), lower);
        high[i] =
            blend > 0
                ? SkImageFilters::MatrixTransform(
                      SkMatrix::Scale(2 * texel_width, 2 * texel_width),
                      SkSamplingOptions(SkFilterMode::kLinear), mip(level + 1))
                : low[i];
      } else {
        for (size_t j = 0; j < i; ++j) {
          if (spec.blur_sigmas[j] == sigma) {
            low[i] = low[j];
            break;
          }
        }
        if (low[i] == source) {
          low[i] = SkImageFilters::Blur(sigma * scale_x, sigma * scale_y,
                                        SkTileMode::kClamp, source);
        }
      }
    }
    if (!use_mips || sigma == 0) {
      high[i] = low[i];
    }
    if (i < 5) {
      builder.uniform("blurMix" + std::to_string(i)) = blend;
    }
  }

  std::vector<std::string_view> names;
  std::vector<sk_sp<SkImageFilter>> inputs;
  for (const SkRuntimeEffect::Child& child : effect->children()) {
    sk_sp<SkImageFilter> input = source;
    // `material` is only read by the compositor's direct path (DrawMaterial).
    if (child.name != kSourceChild && child.name != kMaterialChild) {
      const bool upper = child.name.starts_with("blurHi");
      const std::string_view prefix = upper ? "blurHi" : kBlurChildPrefix;
      if (!child.name.starts_with(prefix) ||
          child.name.size() != prefix.size() + 1) {
        return nullptr;
      }
      const size_t index = static_cast<size_t>(child.name.back() - '0');
      if (index >= low.size()) {
        return nullptr;
      }
      if (index < spec.blur_sigmas.size()) {
        input = upper ? high[index] : low[index];
      }
    }
    names.push_back(child.name);
    inputs.push_back(std::move(input));
  }
  auto result = SkImageFilters::RuntimeShader(
      builder, spec.sample_radius * std::max(scale_x, scale_y), names.data(),
      inputs.data(), static_cast<int>(names.size()));
  if (!spec.output_crop.isEmpty()) {
    const SkRect output = SkRect::MakeLTRB(spec.output_crop.left() * scale_x,
                                           spec.output_crop.top() * scale_y,
                                           spec.output_crop.right() * scale_x,
                                           spec.output_crop.bottom() * scale_y);
    result = SkImageFilters::Crop(output, std::move(result));
  }
  return result;
}

}  // namespace

StarryShaderPaintFilter::Spec::Spec() = default;
StarryShaderPaintFilter::Spec::Spec(const Spec&) = default;
StarryShaderPaintFilter::Spec::Spec(Spec&&) = default;
StarryShaderPaintFilter::Spec& StarryShaderPaintFilter::Spec::operator=(
    const Spec&) = default;
StarryShaderPaintFilter::Spec& StarryShaderPaintFilter::Spec::operator=(
    Spec&&) = default;
StarryShaderPaintFilter::Spec::~Spec() = default;

bool StarryShaderPaintFilter::Spec::operator==(const Spec& other) const {
  return shader == other.shader && uniforms == other.uniforms &&
         blur_sigmas == other.blur_sigmas &&
         downsample_blurs == other.downsample_blurs &&
         sample_radius == other.sample_radius && crop == other.crop &&
         output_crop == other.output_crop && scale == other.scale;
}

// static
std::vector<float> StarryShaderPaintFilter::MakeUniforms(Shader shader) {
  SkRuntimeEffect* effect = EffectFor(shader);
  return std::vector<float>(effect ? effect->uniformSize() / sizeof(float) : 0,
                            0.0f);
}

// static
bool StarryShaderPaintFilter::SetUniform(Shader shader,
                                         std::vector<float>& uniforms,
                                         std::string_view name,
                                         base::span<const float> values) {
  SkRuntimeEffect* effect = EffectFor(shader);
  if (!effect) {
    return false;
  }
  const SkRuntimeEffect::Uniform* uniform = effect->findUniform(name);
  if (!uniform || uniform->sizeInBytes() != values.size_bytes() ||
      uniform->offset % sizeof(float) != 0) {
    LOG(ERROR) << "[starry] no uniform " << std::string(name) << " of "
               << values.size() << " floats";
    return false;
  }
  size_t index = uniform->offset / sizeof(float);
  if (index + values.size() > uniforms.size()) {
    return false;
  }
  for (size_t i = 0; i < values.size(); ++i) {
    uniforms[index + i] = values[i];
  }
  return true;
}

StarryShaderPaintFilter::StarryShaderPaintFilter(Spec spec)
    : PaintFilter(kType, nullptr, /*has_discardable_images=*/false),
      spec_(std::move(spec)) {
  cached_sk_filter_ = Build(spec_, spec_.scale, spec_.scale);
}

StarryShaderPaintFilter::~StarryShaderPaintFilter() = default;

sk_sp<SkImageFilter> StarryShaderPaintFilter::MakeWithLocalMatrix(
    const SkMatrix& matrix) const {
  // Viz supplies only a positive scale and a translation here.
  const float scale_x = matrix.getScaleX();
  const float scale_y = matrix.getScaleY();
  if (!matrix.isScaleTranslate() || !std::isfinite(scale_x) ||
      !std::isfinite(scale_y) || scale_x <= 0 || scale_y <= 0 ||
      scale_x > kMaxScale || scale_y > kMaxScale) {
    return nullptr;
  }
  sk_sp<SkImageFilter> filter = scale_x == spec_.scale && scale_y == spec_.scale
                                    ? cached_sk_filter_
                                    : Build(spec_, scale_x, scale_y);
  return filter ? filter->makeWithLocalMatrix(SkMatrix::Translate(
                      matrix.getTranslateX(), matrix.getTranslateY()))
                : nullptr;
}

namespace {

// 背景 pass 是 1/2 分辨率：相当于金字塔的第 1 级。
constexpr int kMaterialBaseLevel = 1;
// 更粗的各级画在「图集」里，一张图集放这么多级、一个渲染通道画完：每一级都直接从这张
// 图集的来源（背景 pass，或者上一张图集的最后一级）缩出来，一个像素是来源里
// (2·taps)² 个像素的平均，taps = 1、2、4、8、16。
// 不一级一级地缩是因为渲染通道本身有固定开销（Apple 的 GPU 上八个几十像素的通道比
// 玻璃的着色器还贵）。
constexpr int kLevelsPerAtlas = 5;

// 同 Build() 里的算法：sigma 对应的金字塔层级和相邻两级的混合比例（按方差）。
void MipFor(float sigma_pixels, int* level, float* blend) {
  const float width = std::max(1.0f, 2 * sigma_pixels);
  const float lod = std::clamp(std::log2(width), 0.0f, 16.0f);
  *level = static_cast<int>(std::floor(lod));
  const float ratio = width / std::ldexp(1.0f, *level);
  *blend = std::clamp((ratio * ratio - 1) / 3, 0.0f, 1.0f);
}

// 缩小 2·taps 倍。每次双线性取样落在四个像素正中间，正好是它们的平均；来源内容的
// 边上不够一整块时重复最后一行、一列。只在合成线程上用。
SkRuntimeEffect* DownsampleEffect(int index) {
  static base::NoDestructor<std::array<sk_sp<SkRuntimeEffect>, kLevelsPerAtlas>>
      effects;
  sk_sp<SkRuntimeEffect>& effect = (*effects)[index];
  if (!effect) {
    const std::string sksl =
        "const int TAPS = " + std::to_string(1 << index) + ";\n" + R"SKSL(
uniform shader src;
uniform float4 srcRect;     // the source's content, in its texels: left, top, right, bottom
uniform float2 dstOrigin;   // where this level starts in the atlas
half4 main(float2 p) {
    float2 corner = srcRect.xy + floor(p - dstOrigin) * float(2 * TAPS);
    float4 sum = float4(0.0);
    for (int j = 0; j < TAPS; ++j) {
        for (int i = 0; i < TAPS; ++i) {
            float2 at = corner + float2(float(2 * i + 1), float(2 * j + 1));
            sum += float4(src.eval(clamp(at, srcRect.xy + 0.5, srcRect.zw - 0.5)));
        }
    }
    return half4(sum / float(TAPS * TAPS));
}
)SKSL";
    effect = Compile("glass_downsample_atlas", sksl.c_str());
  }
  return effect.get();
}

// 玻璃的着色器读某一级：图集里的一格铺到背景的范围上，范围外镜像（同背景滤镜在边上的
// 做法），取样不越过这一格。
SkRuntimeEffect* LevelEffect() {
  static base::NoDestructor<sk_sp<SkRuntimeEffect>> effect(
      Compile("glass_level", R"SKSL(
uniform shader atlas;
uniform float4 area;   // the backdrop in layer pixels: left, top, 1 / width, 1 / height
uniform float4 cell;   // this level in the atlas, texels: left, top, and the size the area maps to
uniform float2 last;   // the centre of the cell's last texel
half4 main(float2 p) {
    float2 t = (p - area.xy) * area.zw;
    t = 1.0 - abs(1.0 - mod(t, 2.0));
    return atlas.eval(clamp(cell.xy + t * cell.zw, cell.xy + 0.5, last));
}
)SKSL"));
  return effect->get();
}

sk_sp<SkShader> TexelShader(const SkImage* image) {
  return image->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                           SkSamplingOptions(SkFilterMode::kLinear));
}

}  // namespace

// static
bool StarryShaderPaintFilter::CanDrawMaterial(const Spec& spec, float scale) {
  if (!IsValid(spec) || !spec.downsample_blurs || spec.output_crop.isEmpty() ||
      spec.blur_sigmas.empty()) {
    return false;
  }
  for (float sigma : spec.blur_sigmas) {
    if (sigma == 0) {
      continue;  // 着色器用不到的输入
    }
    int level = 0;
    float blend = 0;
    MipFor(sigma * scale, &level, &blend);
    if (level < kMaterialBaseLevel) {
      return false;
    }
  }
  return true;
}

SkRect StarryShaderPaintFilter::MaterialBounds(const SkMatrix& matrix) const {
  const float scale_x = matrix.getScaleX();
  const float scale_y = matrix.getScaleY();
  return SkRect::MakeLTRB(spec_.output_crop.left() * scale_x,
                          spec_.output_crop.top() * scale_y,
                          spec_.output_crop.right() * scale_x,
                          spec_.output_crop.bottom() * scale_y)
      .makeOffset(matrix.getTranslateX(), matrix.getTranslateY());
}

bool StarryShaderPaintFilter::DrawMaterial(SkCanvas* canvas,
                                           const SkImage* backdrop,
                                           const SkMatrix& matrix,
                                           float opacity) const {
  SkRuntimeEffect* effect = EffectFor(spec_.shader);
  SkRuntimeEffect* level_effect = LevelEffect();
  const float scale_x = matrix.getScaleX();
  const float scale_y = matrix.getScaleY();
  if (!effect || !level_effect || !backdrop || !draws_material() ||
      !matrix.isScaleTranslate() || !(scale_x > 0) || !(scale_y > 0) ||
      backdrop->width() <= 0 || backdrop->height() <= 0) {
    return false;
  }

  // 每个模糊输入要哪两级。
  struct Input {
    bool used = false;
    int level = kMaterialBaseLevel;
    float blend = 0;
  };
  std::array<Input, 5> inputs;
  int finest = 16;
  int deepest = kMaterialBaseLevel;
  for (size_t i = 0; i < std::min(spec_.blur_sigmas.size(), inputs.size());
       ++i) {
    if (spec_.blur_sigmas[i] == 0) {
      continue;
    }
    Input& input = inputs[i];
    input.used = true;
    MipFor(spec_.blur_sigmas[i] * scale_x, &input.level, &input.blend);
    input.level = std::max(input.level, kMaterialBaseLevel);
    finest = std::min(finest, input.level);
    deepest = std::max(deepest, input.level + (input.blend > 0 ? 1 : 0));
  }
  finest = std::min(finest, deepest);

  // 着色器的坐标是图层的物理像素（原点在图层左上角），背景盖住其中的 `area`。
  const SkRect area = SkRect::MakeLTRB(material_backdrop_.left() * scale_x,
                                       material_backdrop_.top() * scale_y,
                                       material_backdrop_.right() * scale_x,
                                       material_backdrop_.bottom() * scale_y);
  // 背景 pass
  // 的贴图可能比内容大（合成器把尺寸往上取整好复用），内容在左上角，是 `area`
  // 的一半大。
  const SkISize content =
      SkISize::Make(std::clamp(static_cast<int>(std::lround(area.width() / 2)),
                               1, backdrop->width()),
                    std::clamp(static_cast<int>(std::lround(area.height() / 2)),
                               1, backdrop->height()));

  // 各级在哪张图里、哪一格。第 1 级就是背景 pass 自己。
  struct Level {
    sk_sp<const SkImage> image;
    SkRect cell = SkRect::MakeEmpty();
  };
  std::vector<Level> levels(deepest + 1);
  levels[kMaterialBaseLevel] = {
      sk_ref_sp(backdrop), SkRect::MakeIWH(content.width(), content.height())};
  for (int first = kMaterialBaseLevel + 1; first <= deepest;
       first += kLevelsPerAtlas) {
    const int last = std::min(deepest, first + kLevelsPerAtlas - 1);
    const Level& source = levels[first - 1];
    // 这张图集里各级从左到右排，中间空一个像素。
    int width = 0;
    int height = 0;
    for (int level = first; level <= last; ++level) {
      const int divisor = 1 << (level - kMaterialBaseLevel);
      const int w = (content.width() + divisor - 1) / divisor;
      const int h = (content.height() + divisor - 1) / divisor;
      levels[level].cell = SkRect::MakeXYWH(width, 0, w, h);
      width += w + 1;
      height = std::max(height, h);
    }
    sk_sp<SkSurface> surface =
        canvas->makeSurface(backdrop->imageInfo()
                                .makeWH(width, height)
                                .makeAlphaType(kPremul_SkAlphaType));
    if (!surface) {
      // 画不了时调用的一方会把背景原样盖上去（没有玻璃的样子）。这个后端不能直接
      // 画的话用 --enable-features=StarryGlassBackdropPass:direct/false。
      static bool logged = false;
      if (!logged) {
        logged = true;
        LOG(ERROR)
            << "[starry] glass: no offscreen surface for the backdrop levels";
      }
      return false;
    }
    sk_sp<SkShader> texels = TexelShader(source.image.get());
    for (int level = first; level <= last; ++level) {
      SkRuntimeEffect* downsample = DownsampleEffect(level - first);
      if (!downsample) {
        return false;
      }
      SkRuntimeEffectBuilder builder(sk_ref_sp(downsample));
      builder.child("src") = texels;
      builder.uniform("srcRect") =
          SkV4{source.cell.left(), source.cell.top(), source.cell.right(),
               source.cell.bottom()};
      builder.uniform("dstOrigin") =
          SkV2{levels[level].cell.left(), levels[level].cell.top()};
      SkPaint paint;
      paint.setBlendMode(SkBlendMode::kSrc);
      paint.setShader(builder.makeShader());
      surface->getCanvas()->drawRect(levels[level].cell, paint);
    }
    sk_sp<SkImage> atlas = surface->makeImageSnapshot();
    if (!atlas) {
      return false;
    }
    for (int level = first; level <= last; ++level) {
      levels[level].image = atlas;
    }
  }

  std::vector<sk_sp<SkShader>> shaders(levels.size());
  auto shader_for = [&](int level) -> sk_sp<SkShader> {
    level = std::clamp(level, finest, deepest);
    if (!shaders[level]) {
      const Level& source = levels[level];
      SkRuntimeEffectBuilder builder(sk_ref_sp(level_effect));
      builder.child("atlas") = TexelShader(source.image.get());
      builder.uniform("area") =
          SkV4{area.left(), area.top(), 1 / area.width(), 1 / area.height()};
      // 内容的宽高不一定是这一级的整数倍：最后一个像素只有一部分对应内容，按实际的
      // 比例铺，不拉伸。
      const float divisor = std::ldexp(1.0f, level - kMaterialBaseLevel);
      builder.uniform("cell") =
          SkV4{source.cell.left(), source.cell.top(), content.width() / divisor,
               content.height() / divisor};
      builder.uniform("last") =
          SkV2{source.cell.right() - 0.5f, source.cell.bottom() - 0.5f};
      shaders[level] = builder.makeShader();
    }
    return shaders[level];
  };

  // 玻璃的着色器：接好各级背景。`stage` 见着色器里的 materialStage；`atlas` 是半分辨率
  // 的材质（只有合成那一步读它，别的时候接一个用不到的）。
  auto fill = [&](SkRuntimeEffectBuilder& builder, float stage,
                  sk_sp<SkShader> atlas) {
    builder.uniform("pixelScale") = SkV2{scale_x, scale_y};
    for (size_t i = 0; i < inputs.size(); ++i) {
      builder.uniform("blurMix" + std::to_string(i)) = inputs[i].blend;
    }
    builder.uniform("materialStage") = stage;
    // 面：离边这么深（pt）以内原分辨率算。高光、外折射、SDR 的压暗都在最外面 2 pt 里
    // 变化，再留出合成时双线性取样够得到的 3 个像素。
    builder.uniform("materialEdge") = 2 + 3 / scale_x;
    // 阴影：玻璃外面，加上抗锯齿的边和双线性取样够得到的几个像素。
    builder.uniform("materialReach") = 4 / scale_x;
    for (const SkRuntimeEffect::Child& child : effect->children()) {
      int level = finest;
      if (child.name == kMaterialChild) {
        builder.child(child.name) = atlas ? atlas : shader_for(finest);
        continue;
      }
      if (child.name != kSourceChild) {
        const bool upper = child.name.starts_with("blurHi");
        const size_t index = static_cast<size_t>(child.name.back() - '0');
        if (index >= inputs.size()) {
          return false;
        }
        if (inputs[index].used) {
          level =
              inputs[index].level + (upper && inputs[index].blend > 0 ? 1 : 0);
        }
      }
      sk_sp<SkShader> shader = shader_for(level);
      if (!shader) {
        return false;
      }
      builder.child(child.name) = std::move(shader);
    }
    return true;
  };
  auto uniforms = [&] {
    return SkData::MakeWithCopy(spec_.uniforms.data(), effect->uniformSize());
  };

  // 材质先在 1/2 分辨率算：面和阴影各占图集的一格（分开放，放大时才不会互相渗进对方
  // 的边），一个渲染通道画完。面内最清晰的成分本来就是 1/4 分辨率的背景，阴影是 24 pt
  // 的渐变，缩一半看不出来；只有边上几个像素宽的一圈留在原分辨率算。
  sk_sp<SkShader> material;
  // 图层像素里的输出范围，凑成整数像素。
  const SkIRect grid =
      MaterialBounds(SkMatrix::Scale(scale_x, scale_y)).roundOut();
  const int cell_width = (grid.width() + 1) / 2;
  const int cell_height = (grid.height() + 1) / 2;
  const SkRect face_cell = SkRect::MakeIWH(cell_width, cell_height);
  const SkRect shadow_cell =
      SkRect::MakeXYWH(cell_width + 1, 0, cell_width, cell_height);
  if (half_material_ && cell_width > 0 && cell_height > 0) {
    sk_sp<SkSurface> surface =
        canvas->makeSurface(backdrop->imageInfo()
                                .makeWH(2 * cell_width + 1, cell_height)
                                .makeAlphaType(kPremul_SkAlphaType));
    if (surface) {
      surface->getCanvas()->clear(SK_ColorTRANSPARENT);
      for (int stage = 1; stage <= 2; ++stage) {
        const SkRect& cell = stage == 1 ? face_cell : shadow_cell;
        SkRuntimeEffectBuilder builder(sk_ref_sp(effect), uniforms());
        if (!fill(builder, stage, nullptr)) {
          return false;
        }
        // 着色器的坐标（图层像素）→ 这一格：缩一半。
        SkMatrix to_cell = SkMatrix::Translate(cell.left(), cell.top());
        to_cell.preScale(0.5f, 0.5f);
        to_cell.preTranslate(-grid.left(), -grid.top());
        SkPaint paint;
        paint.setBlendMode(SkBlendMode::kSrc);
        paint.setShader(builder.makeShader(&to_cell));
        surface->getCanvas()->drawRect(cell, paint);
      }
      if (sk_sp<SkImage> atlas = surface->makeImageSnapshot()) {
        material = TexelShader(atlas.get());
      }
    }
  }

  SkRuntimeEffectBuilder builder(sk_ref_sp(effect), uniforms());
  if (!fill(builder, material ? 3 : 0, material)) {
    return false;
  }
  if (material) {
    // 图集的像素 = 图层像素 × 0.5 + 偏移；取样不越过各自的一格。
    auto place = [&](const SkRect& cell) {
      return SkV4{0.5f, 0.5f, cell.left() - grid.left() * 0.5f,
                  cell.top() - grid.top() * 0.5f};
    };
    auto centres = [](const SkRect& cell) {
      return SkV4{cell.left() + 0.5f, cell.top() + 0.5f, cell.right() - 0.5f,
                  cell.bottom() - 0.5f};
    };
    builder.uniform("materialFace") = place(face_cell);
    builder.uniform("materialFaceCell") = centres(face_cell);
    builder.uniform("materialShadow") = place(shadow_cell);
    builder.uniform("materialShadowCell") = centres(shadow_cell);
  }
  const SkMatrix origin =
      SkMatrix::Translate(matrix.getTranslateX(), matrix.getTranslateY());
  SkPaint paint;
  paint.setShader(builder.makeShader(&origin));
  if (!paint.getShader()) {
    return false;
  }
  paint.setAlphaf(opacity);
  canvas->drawRect(MaterialBounds(matrix), paint);
  return true;
}

gfx::ContentColorUsage StarryShaderPaintFilter::GetContentColorUsage() const {
  return gfx::ContentColorUsage::kSRGB;
}

size_t StarryShaderPaintFilter::SerializedSize() const {
  base::CheckedNumeric<size_t> total_size = BaseSerializedSize();
  total_size += PaintOpWriter::SerializedSize<uint32_t>();
  total_size += PaintOpWriter::SerializedSize(spec_.sample_radius);
  total_size += PaintOpWriter::SerializedSize(spec_.crop);
  total_size += PaintOpWriter::SerializedSize(spec_.output_crop);
  total_size += PaintOpWriter::SerializedSize(spec_.scale);
  total_size += PaintOpWriter::SerializedSize(spec_.downsample_blurs);
  total_size += PaintOpWriter::SerializedSize(spec_.blur_sigmas.size());
  total_size += base::CheckMul(spec_.blur_sigmas.size(),
                               PaintOpWriter::SerializedSize<float>());
  total_size += PaintOpWriter::SerializedSize(spec_.uniforms.size());
  total_size += base::CheckMul(spec_.uniforms.size(),
                               PaintOpWriter::SerializedSize<float>());
  return total_size.ValueOrDefault(0u);
}

bool StarryShaderPaintFilter::EqualsForTesting(
    const StarryShaderPaintFilter& other) const {
  return spec_ == other.spec_;
}

void StarryShaderPaintFilter::Serialize(PaintOpWriter& writer) const {
  writer.Write(static_cast<uint32_t>(spec_.shader));
  writer.Write(spec_.sample_radius);
  writer.Write(spec_.crop);
  writer.Write(spec_.output_crop);
  writer.Write(spec_.scale);
  writer.Write(spec_.downsample_blurs);
  writer.WriteSize(spec_.blur_sigmas.size());
  for (float sigma : spec_.blur_sigmas) {
    writer.Write(sigma);
  }
  writer.WriteSize(spec_.uniforms.size());
  for (float value : spec_.uniforms) {
    writer.Write(value);
  }
}

// static
sk_sp<PaintFilter> StarryShaderPaintFilter::Deserialize(PaintOpReader& reader) {
  Spec spec;
  uint32_t shader = 0;
  size_t blur_count = 0;
  size_t uniform_count = 0;

  reader.Read(&shader);
  reader.Read(&spec.sample_radius);
  reader.Read(&spec.crop);
  reader.Read(&spec.output_crop);
  reader.Read(&spec.scale);
  reader.Read(&spec.downsample_blurs);
  reader.ReadSize(&blur_count);
  if (!reader.valid() || shader > static_cast<uint32_t>(Shader::kMaxValue) ||
      blur_count > kMaxBlurs) {
    return nullptr;
  }
  spec.shader = static_cast<Shader>(shader);
  spec.blur_sigmas.resize(blur_count);
  for (float& sigma : spec.blur_sigmas) {
    reader.Read(&sigma);
  }
  reader.ReadSize(&uniform_count);
  if (!reader.valid() || uniform_count > kMaxUniformFloats) {
    return nullptr;
  }
  spec.uniforms.resize(uniform_count);
  for (float& value : spec.uniforms) {
    reader.Read(&value);
  }
  if (!reader.valid() || !IsValid(spec)) {
    return nullptr;
  }
  return sk_make_sp<StarryShaderPaintFilter>(std::move(spec));
}

sk_sp<PaintFilter> StarryShaderPaintFilter::SnapshotWithImagesInternal(
    ImageProvider* image_provider) const {
  return sk_make_sp<StarryShaderPaintFilter>(spec_);
}

}  // namespace cc
