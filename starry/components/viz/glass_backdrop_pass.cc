// Copyright 2026 The Starry Browser Authors

#include "starry/components/viz/glass_backdrop_pass.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/feature_list.h"
#include "base/metrics/field_trial_params.h"
#include "base/no_destructor.h"
#include "cc/paint/filter_operation.h"
#include "cc/paint/filter_operations.h"
#include "components/viz/common/quads/aggregated_render_pass_draw_quad.h"
#include "components/viz/common/quads/draw_quad.h"
#include "components/viz/common/quads/shared_quad_state.h"
#include "components/viz/common/quads/solid_color_draw_quad.h"
#include "starry/cc/paint/starry_shader_paint_filter.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/geometry/transform.h"

namespace starry {

namespace {

using Filter = cc::StarryShaderPaintFilter;

// --disable-features=StarryGlassBackdropPass 回到背景滤镜的画法（对照用）。
// --enable-features=StarryGlassBackdropPass:direct/0 只改写、不直接画：背景 pass 是原
// 分辨率，玻璃还是走滤镜（输入层、输出层各一遍）。
BASE_FEATURE(kGlassBackdropPass,
             "StarryGlassBackdropPass",
             base::FEATURE_ENABLED_BY_DEFAULT);
const base::FeatureParam<bool> kDirect{&kGlassBackdropPass, "direct", true};
// :half_material/false 直接画时材质也在原分辨率算（对照用）。
const base::FeatureParam<bool> kHalfMaterial{&kGlassBackdropPass,
                                             "half_material", true};

// 背景 pass 的编号：玻璃自己那张 pass 的编号加一个高位。SurfaceAggregator 的编号从 1
// 往上数，碰不到；同一块玻璃每一帧的编号不变，缓冲可以沿用。
constexpr uint64_t kBackdropPassIdBit = uint64_t{1} << 62;

// 直接画时背景 pass 的分辨率：一半。玻璃用到的最细的背景是 1/4（系统玻璃的背景密度），
// 一半还留着一级余量；下面的画面用双线性缩一半正好是 2×2 的平均，不会漏掉细线。
constexpr int kBackdropDivisor = 2;

const Filter* GlassBackdropFilter(
    const viz::AggregatedRenderPassDrawQuad* quad) {
  if (quad->backdrop_filters.size() != 1) {
    return nullptr;
  }
  const cc::FilterOperation& operation = quad->backdrop_filters.at(0);
  if (operation.type() != cc::FilterOperation::REFERENCE ||
      !operation.image_filter() ||
      operation.image_filter()->type() !=
          cc::PaintFilter::Type::kStarryShader) {
    return nullptr;
  }
  return static_cast<const Filter*>(operation.image_filter().get());
}

// 背景滤镜 → 盖在画面上的玻璃：玻璃外面只留阴影（带透明度），不再输出原来的背景。
// `valid` 是背景 pass 里真有画面的范围（图层坐标）：玻璃伸到窗口外面的部分没有东西可
// 画，输入裁到这个范围、范围外镜像，同背景滤镜在窗口边上的做法。`direct` 时着色器直接
// 画到目标上，背景 pass 是 1/2 分辨率。
// 每一帧都会来问，按原滤镜缓存（原滤镜在界面不变时是同一个对象）。
sk_sp<cc::PaintFilter> PassFilterFor(const Filter* backdrop,
                                     const SkRect& valid,
                                     bool direct) {
  struct Entry {
    sk_sp<const cc::PaintFilter> source;
    SkRect valid;
    bool direct;
    sk_sp<cc::PaintFilter> converted;
  };
  static base::NoDestructor<std::vector<Entry>> cache;
  for (const Entry& entry : *cache) {
    if (entry.source.get() == backdrop && entry.valid == valid &&
        entry.direct == direct) {
      return entry.converted;
    }
  }
  Filter::Spec spec = backdrop->spec();
  const float painted = 0;
  if (!Filter::SetUniform(spec.shader, spec.uniforms, "backdropMode",
                          base::span_from_ref(painted))) {
    return nullptr;
  }
  if (spec.crop.isEmpty()) {
    return nullptr;
  }
  // 着色器的输出本来没有边界（靠背景滤镜的范围裁），盖上去画要自己限定。
  if (spec.output_crop.isEmpty()) {
    spec.output_crop = spec.crop;
  }
  if (!spec.crop.intersect(valid)) {
    return nullptr;
  }
  auto converted = sk_make_sp<Filter>(std::move(spec));
  if (direct) {
    converted->set_material_backdrop(valid, kHalfMaterial.Get());
  }
  constexpr size_t kMaxEntries = 16;
  if (cache->size() >= kMaxEntries) {
    cache->erase(cache->begin());
  }
  cache->push_back({sk_ref_sp(backdrop), valid, direct, converted});
  return converted;
}

bool CanCopy(const viz::DrawQuad* quad) {
  switch (quad->material) {
    case viz::DrawQuad::Material::kAggregatedRenderPass:
    case viz::DrawQuad::Material::kDebugBorder:
    case viz::DrawQuad::Material::kPictureContent:
    case viz::DrawQuad::Material::kTextureContent:
    case viz::DrawQuad::Material::kSolidColor:
    case viz::DrawQuad::Material::kTiledContent:
      return true;
    default:
      return false;
  }
}

viz::AggregatedRenderPass* FindPass(const viz::AggregatedRenderPassList& passes,
                                    viz::AggregatedRenderPassId id) {
  for (const auto& pass : passes) {
    if (pass->id == id) {
      return pass.get();
    }
  }
  return nullptr;
}

// 在 pass 的最下面垫一个看不见的 quad。
void AppendFiller(viz::AggregatedRenderPass* pass) {
  viz::SharedQuadState* state = pass->CreateAndAppendSharedQuadState();
  state->SetAll(gfx::Transform(), pass->output_rect, pass->output_rect,
                gfx::MaskFilterInfo(), std::nullopt, /*contents_opaque=*/false,
                /*opacity_f=*/1, SkBlendMode::kSrcOver, /*sorting_context=*/0,
                /*layer_id=*/0, /*fast_rounded_corner=*/false);
  auto* filler = pass->CreateAndAppendDrawQuad<viz::SolidColorDrawQuad>();
  const gfx::Rect rect(pass->output_rect.origin(), gfx::Size(1, 1));
  filler->SetNew(state, rect, rect, SkColors::kTransparent,
                 /*anti_aliasing_off=*/true);
}

// 把 pass 里第 index 个 quad（一块玻璃）改写掉；背景 pass 放进 *backdrop。
bool RewriteQuad(const viz::AggregatedRenderPassList& passes,
                 viz::AggregatedRenderPass* pass,
                 size_t index,
                 std::unique_ptr<viz::AggregatedRenderPass>* backdrop) {
  auto at = pass->quad_list.begin();
  for (size_t i = 0; i < index; ++i) {
    ++at;
  }
  auto* glass = static_cast<viz::AggregatedRenderPassDrawQuad*>(*at);
  const Filter* backdrop_filter = GlassBackdropFilter(glass);
  if (!backdrop_filter) {
    return false;
  }
  const viz::SharedQuadState* glass_state = glass->shared_quad_state;
  // 背景 pass 用玻璃图层自己的坐标（原点在图层左上角），只支持缩放加平移。
  const gfx::Transform& to_target = glass_state->quad_to_target_transform;
  gfx::Transform from_target;
  if (!to_target.IsScaleOrTranslation() ||
      !to_target.GetInverse(&from_target) || glass->rect.IsEmpty() ||
      glass->mask_resource_id() != viz::kInvalidResourceId ||
      glass->filters_scale.x() <= 0 || glass->filters_scale.y() <= 0) {
    return false;
  }

  // 背景 pass 画得到的范围（图层的像素坐标）：玻璃图层和这张 pass 的交集。
  gfx::Rect area =
      gfx::ToEnclosingRect(from_target.MapRect(gfx::RectF(pass->output_rect)));
  area.Intersect(glass->rect);
  if (area.IsEmpty()) {
    return false;
  }
  const bool direct =
      kDirect.Get() && Filter::CanDrawMaterial(backdrop_filter->spec(),
                                               glass->filters_scale.x());
  if (direct) {
    // 缩一半：宽高凑成偶数，背景 pass 的一个像素正好是原来的 2×2，双线性取样落在
    // 四个像素正中间。
    area.set_width(area.width() + area.width() % kBackdropDivisor);
    area.set_height(area.height() + area.height() % kBackdropDivisor);
  }
  // 换到滤镜的单位（图层坐标，除掉倍率）。
  gfx::RectF valid(area);
  valid.Scale(1 / glass->filters_scale.x(), 1 / glass->filters_scale.y());
  valid.Offset(-glass->filters_origin.x(), -glass->filters_origin.y());
  sk_sp<cc::PaintFilter> pass_filter =
      PassFilterFor(backdrop_filter, gfx::RectFToSkRect(valid), direct);
  if (!pass_filter) {
    return false;
  }

  // 背景 pass 自己的坐标：`area` 平移到原点，直接画时再缩一半。
  gfx::Transform to_backdrop = from_target;
  to_backdrop.PostTranslate(-area.x(), -area.y());
  gfx::Rect output(area.size());
  if (direct) {
    to_backdrop.PostScale(1.0f / kBackdropDivisor, 1.0f / kBackdropDivisor);
    output.set_size(gfx::Size(area.width() / kBackdropDivisor,
                              area.height() / kBackdropDivisor));
  }
  gfx::Transform to_root;
  if (!to_backdrop.GetInverse(&to_root)) {
    return false;
  }
  to_root.PostConcat(pass->transform_to_root_target);

  // 玻璃后面没变就不用重画：这张 pass 的损伤没碰到背景的范围，上一帧的缓冲还在
  // （DirectRenderer::CanSkipRenderPass 会核对），滤镜参数变了由 RPDQ 的比较发现。
  const gfx::Rect area_in_target =
      gfx::ToEnclosingRect(to_target.MapRect(gfx::RectF(area)));
  const bool damaged = pass->damage_rect.Intersects(area_in_target);

  auto pass_id = viz::AggregatedRenderPassId::FromUnsafeValue(
      glass->render_pass_id.GetUnsafeValue() | kBackdropPassIdBit);
  auto copy = std::make_unique<viz::AggregatedRenderPass>(16, 64);
  // 损伤范围总是整张：要画就画全（缓冲不在了、跳不过的时候也不会被裁成空的），跳不跳
  // 只看 has_damage_from_contributing_content。
  copy->SetAll(pass_id, output, output, to_root, pass->content_color_usage,
               /*has_transparent_background=*/true,
               /*cache_render_pass=*/true,
               /*has_damage_from_contributing_content=*/damaged,
               /*generate_mipmap=*/false);

  // 玻璃下面的 quad（列表是从上到下的顺序）：落在背景范围里的再画一份。
  const viz::SharedQuadState* last_state = nullptr;
  bool skip_state = false;
  auto below = at;
  ++below;
  for (; below != pass->quad_list.end(); ++below) {
    const viz::DrawQuad* quad = *below;
    const viz::SharedQuadState* state = quad->shared_quad_state;
    if (state != last_state) {
      last_state = state;
      gfx::Transform transform = state->quad_to_target_transform;
      transform.PostConcat(to_backdrop);
      gfx::RectF bounds =
          transform.MapRect(gfx::RectF(state->visible_quad_layer_rect));
      std::optional<gfx::Rect> clip;
      if (state->clip_rect) {
        gfx::RectF mapped = to_backdrop.MapRect(gfx::RectF(*state->clip_rect));
        bounds.Intersect(mapped);
        clip = gfx::ToEnclosingRect(mapped);
      }
      skip_state = !bounds.Intersects(gfx::RectF(output));
      if (skip_state) {
        continue;
      }
      viz::SharedQuadState* copied = copy->CreateAndAppendSharedQuadState();
      *copied = *state;
      copied->quad_to_target_transform = transform;
      copied->clip_rect = clip;
      copied->overlay_damage_index.reset();
      if (!copied->mask_filter_info.IsEmpty()) {
        copied->mask_filter_info.ApplyTransform(to_backdrop);
      }
    }
    if (skip_state || !CanCopy(quad)) {
      continue;
    }
    gfx::RectF visible =
        copy->shared_quad_state_list.back()->quad_to_target_transform.MapRect(
            gfx::RectF(quad->visible_rect));
    if (!visible.Intersects(gfx::RectF(output))) {
      continue;
    }
    if (const auto* pass_quad =
            quad->DynamicCast<viz::AggregatedRenderPassDrawQuad>()) {
      auto* copied = copy->CopyFromAndAppendRenderPassDrawQuad(pass_quad);
      // 滤镜的大小跟着缩。
      copied->filters_scale.Scale(direct ? 1.0f / kBackdropDivisor : 1.0f);
    } else {
      copy->CopyFromAndAppendDrawQuad(quad);
    }
  }
  while (direct && copy->quad_list.size() < 2) {
    // 只有一个 quad（或者没有）的 pass 会被合成器省掉、直接画那个 quad，可直接画玻璃
    // 要的是这张 pass 的图。
    AppendFiller(copy.get());
  }

  // 玻璃自己那张 pass 里是玻璃上面的内容（子视图）。没有内容时玻璃的 quad 直接改成
  // 「背景 pass 过一遍玻璃」；有内容时在它后面（下面）插一个。插入会挪动整个列表，之前的
  // 指针都作废。
  cc::FilterOperations filters;
  filters.Append(cc::FilterOperation::CreateReferenceFilter(pass_filter));
  viz::AggregatedRenderPass* content = FindPass(passes, glass->render_pass_id);
  viz::AggregatedRenderPassDrawQuad* material = glass;
  if (!content || !content->quad_list.empty() ||
      !content->copy_requests.empty()) {
    auto inserted = pass->quad_list.InsertAfterAndInvalidateAllPointers<
        viz::AggregatedRenderPassDrawQuad>(at, 1);
    at = pass->quad_list.begin();
    for (size_t i = 0; i < index; ++i) {
      ++at;
    }
    glass = static_cast<viz::AggregatedRenderPassDrawQuad*>(*at);
    material = static_cast<viz::AggregatedRenderPassDrawQuad*>(*inserted);
    glass->backdrop_filters.Clear();
    glass->backdrop_filter_bounds.reset();
    // 内容只有一个 quad 时合成器不画这张 pass、每一帧把那个 quad 直接画进图层的缓冲，
    // 玻璃后面的网页一动就跟着重画。垫一个看不见的 quad：内容照普通的 pass 画一次，
    // 没变就沿用。
    if (content && content->quad_list.size() == 1 &&
        !content->shared_quad_state_list.empty()) {
      AppendFiller(content);
    }
  }
  const gfx::Rect rect = glass->rect;
  const gfx::Rect visible_rect = glass->visible_rect;
  const gfx::Vector2dF filters_scale = glass->filters_scale;
  const gfx::PointF filters_origin = glass->filters_origin;
  const bool force_anti_aliasing_off = glass->force_anti_aliasing_off;
  const bool intersects_damage_under = glass->intersects_damage_under;
  material->SetAll(glass_state, rect, visible_rect, /*needs_blending=*/true,
                   pass_id, viz::kInvalidResourceId, gfx::RectF(), gfx::Size(),
                   filters_scale, filters_origin, force_anti_aliasing_off,
                   /*backdrop_filter_quality=*/1.0f, intersects_damage_under,
                   std::move(filters), cc::FilterOperations(), std::nullopt);

  *backdrop = std::move(copy);
  return true;
}

}  // namespace

void RewriteGlassBackdrops(viz::AggregatedRenderPassList* passes) {
  if (!base::FeatureList::IsEnabled(kGlassBackdropPass)) {
    return;
  }
  for (size_t i = 0; i < passes->size(); ++i) {
    viz::AggregatedRenderPass* pass = (*passes)[i].get();
    std::vector<size_t> glass;
    size_t index = 0;
    for (const viz::DrawQuad* quad : pass->quad_list) {
      if (const auto* pass_quad =
              quad->DynamicCast<viz::AggregatedRenderPassDrawQuad>()) {
        if (GlassBackdropFilter(pass_quad)) {
          glass.push_back(index);
        }
      }
      ++index;
    }
    // 从最下面的一块开始：上面的玻璃的背景里要有下面那块已经改写好的玻璃，而且在后面
    // 插入 quad 不会动前面的序号。背景 pass 要排在用它的 pass 之前。
    for (auto it = glass.rbegin(); it != glass.rend(); ++it) {
      std::unique_ptr<viz::AggregatedRenderPass> backdrop;
      if (RewriteQuad(*passes, pass, *it, &backdrop)) {
        passes->insert(passes->begin() + i, std::move(backdrop));
        ++i;
      }
    }
  }
}

const cc::StarryShaderPaintFilter* GlassPassFilter(
    const cc::FilterOperations& filters) {
  if (filters.size() != 1) {
    return nullptr;
  }
  const cc::FilterOperation& operation = filters.at(0);
  if (operation.type() != cc::FilterOperation::REFERENCE ||
      !operation.image_filter() ||
      operation.image_filter()->type() !=
          cc::PaintFilter::Type::kStarryShader) {
    return nullptr;
  }
  return static_cast<const Filter*>(operation.image_filter().get());
}

}  // namespace starry
