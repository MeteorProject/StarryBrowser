// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_CC_PAINT_STARRY_SHADER_PAINT_FILTER_H_
#define STARRY_CC_PAINT_STARRY_SHADER_PAINT_FILTER_H_

#include <stddef.h>
#include <stdint.h>

#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "cc/paint/paint_export.h"
#include "cc/paint/paint_filter.h"

class SkCanvas;
class SkImage;

namespace cc {

class PaintOpReader;
class PaintOpWriter;

// 在滤镜的输入上跑一段内置的 SkSL。用作图层的背景滤镜时，输入就是图层后面已经画好的
// 内容（网页和别的界面），Liquid Glass 用它折射、模糊背后的画面。
//
// 着色器源码编在二进制里（starry_glass_sksl.cc），跨进程只传「哪一段着色器」和一组
// 数字：渲染进程也能往 GPU 进程发 PaintFilter，不能让它带着任意源码过去。
//
// 这个文件在 Starry 的仓库里，但编进 //cc/paint（patches/cc/paint/BUILD.gn.patch）：
// PaintFilter 的类型要能被 PaintOpWriter / PaintOpReader 序列化。
class CC_PAINT_EXPORT StarryShaderPaintFilter final : public PaintFilter {
 public:
  static constexpr Type kType = Type::kStarryShader;

  enum class Shader : uint32_t {
    kGlass = 0,
    kMaxValue = kGlass,
  };

  static constexpr size_t kMaxUniformFloats = 256;
  static constexpr size_t kMaxBlurs = 6;
  static constexpr float kMaxSigma = 2048;
  static constexpr float kMaxSampleRadius = 8192;
  static constexpr float kMaxScale = 16;

  struct CC_PAINT_EXPORT Spec {
    Spec();
    Spec(const Spec&);
    Spec(Spec&&);
    Spec& operator=(const Spec&);
    Spec& operator=(Spec&&);
    ~Spec();

    bool operator==(const Spec& other) const;

    Shader shader = Shader::kGlass;
    // 屏幕倍率：一个图层坐标单位是多少个物理像素。RuntimeShader 不接受缩放，
    // 所以滤镜按物理像素构建，模糊、取样范围和裁剪都乘上它。
    // 合成器用 MakeWithLocalMatrix 匹配动画中的实际倍率，再加平移；见
    // patches/components/viz/service/display/skia_renderer.cc.patch。
    // 自己画时先撤掉画布的缩放（GlassView::OnPaint）。
    float scale = 1;
    // 着色器的 uniform 块，全是 float，布局由着色器的声明决定；用 SetUniform 按名字填。
    std::vector<float> uniforms;
    // 着色器里的 `uniform shader blurN` 是输入做了这么大的高斯模糊（图层坐标，会跟着
    // 屏幕倍率放大）；0 表示不模糊，直接是输入。`src` 总是输入本身。
    std::vector<float> blur_sigmas;
    // Approximate the blur branches with a shared mip pyramid. The original
    // source and final material stay at full resolution. False uses Gaussians.
    bool downsample_blurs = true;
    // 着色器取样时离当前像素最远多少（图层坐标）。Skia 按它准备输入的范围。
    float sample_radius = 0;
    // 输入范围（图层坐标），范围外镜像；降采样各级也沿用它来复用相同的背景。
    // 空矩形表示不显式裁剪。
    SkRect crop = SkRect::MakeEmpty();
    // Optional output-only crop, independent of the input/mip boundaries above.
    // Keep the compositor layer wide enough to track damage in the input area.
    SkRect output_crop = SkRect::MakeEmpty();
  };

  // 一个填满 0、大小正好的 uniform 块；着色器编译不过时是空的。
  static std::vector<float> MakeUniforms(Shader shader);
  // 按名字设 uniform（float、float2、float4）。名字不存在或大小不对时返回 false。
  static bool SetUniform(Shader shader,
                         std::vector<float>& uniforms,
                         std::string_view name,
                         base::span<const float> values);

  explicit StarryShaderPaintFilter(Spec spec);
  ~StarryShaderPaintFilter() override;

  const Spec& spec() const { return spec_; }

  // 合成器用的「直接画」（starry/components/viz/glass_backdrop_pass.h）：玻璃后面的画面已经
  // 按 1/2 分辨率画进一张图，着色器直接画到目标上，不经过滤镜的输入层、输出层。
  // CanDrawMaterial：这份参数能不能这样画（所有用到的模糊都不比 1/2 分辨率细）。
  static bool CanDrawMaterial(const Spec& spec, float scale);
  // `backdrop` 是那张图盖住的范围（图层坐标），范围外镜像。只在合成器里用，不跨进程。
  // `half_material`：材质（面和阴影）先在 1/2 分辨率算，原分辨率只算边上的一圈。
  void set_material_backdrop(const SkRect& backdrop, bool half_material) {
    material_backdrop_ = backdrop;
    half_material_ = half_material;
  }
  bool draws_material() const { return !material_backdrop_.isEmpty(); }
  // 画布的坐标是图层的物理像素；`matrix` 同 MakeWithLocalMatrix（倍率加平移）。
  // 返回画的范围（画布坐标）。
  SkRect MaterialBounds(const SkMatrix& matrix) const;
  bool DrawMaterial(SkCanvas* canvas,
                    const SkImage* backdrop,
                    const SkMatrix& matrix,
                    float opacity) const;

  // Viz 的滤镜坐标包含屏幕倍率和动画缩放。RuntimeShader 不接受带缩放的
  // local matrix：按实际 render surface 的倍率重建取样坐标，再只套平移。
  sk_sp<SkImageFilter> MakeWithLocalMatrix(const SkMatrix& matrix) const;

  size_t SerializedSize() const override;
  bool EqualsForTesting(const StarryShaderPaintFilter& other) const;

  void Serialize(PaintOpWriter& writer) const;
  // 数据不合法时返回空。
  static sk_sp<PaintFilter> Deserialize(PaintOpReader& reader);

 protected:
  sk_sp<PaintFilter> SnapshotWithImagesInternal(
      ImageProvider* image_provider) const override;

 private:
  Spec spec_;
  SkRect material_backdrop_ = SkRect::MakeEmpty();
  bool half_material_ = true;
};

}  // namespace cc

#endif  // STARRY_CC_PAINT_STARRY_SHADER_PAINT_FILTER_H_
