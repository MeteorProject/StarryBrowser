// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/design/icons.h"

#include <iterator>

#define DECLARE_VECTOR_COMMAND(x) using gfx::x;
DECLARE_VECTOR_COMMANDS
#undef DECLARE_VECTOR_COMMAND

// 笔画默认是圆头、尖角：折线拆成多段各自圆头，使拐角呈圆润效果。
#define STARRY_ICON(name, ...)                                         \
  static constexpr gfx::PathElement name##Path[] = {__VA_ARGS__};      \
  static constexpr gfx::VectorIconRep name##Reps[] = {{name##Path}};   \
  constexpr gfx::VectorIcon name = {name##Reps, std::size(name##Reps), \
                                    "starry_" #name};

namespace starry::icons {

STARRY_ICON(kChevronLeft,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.75f,
            MOVE_TO, 10.5f, 3, LINE_TO, 5.5f, 8,
            MOVE_TO, 5.5f, 8, LINE_TO, 10.5f, 13)

STARRY_ICON(kChevronRight,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.75f,
            MOVE_TO, 5.5f, 3, LINE_TO, 10.5f, 8,
            MOVE_TO, 10.5f, 8, LINE_TO, 5.5f, 13)

STARRY_ICON(kChevronDown,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.75f,
            MOVE_TO, 3, 5.75f, LINE_TO, 8, 10.75f,
            MOVE_TO, 8, 10.75f, LINE_TO, 13, 5.75f)

STARRY_ICON(kChevronUp,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.75f,
            MOVE_TO, 3, 10.25f, LINE_TO, 8, 5.25f,
            MOVE_TO, 8, 5.25f, LINE_TO, 13, 10.25f)

// 圆心 (8, 8.6)、半径 5.3 的圆从一点半的位置顺时针绕到十二点，箭头朝右。
STARRY_ICON(kArrowClockwise,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.6f,
            MOVE_TO, 11.748f, 4.852f,
            ARC_TO, 5.3f, 5.3f, 0, 1, 1, 8, 3.3f,
            MOVE_TO, 6.8f, 1.0f, LINE_TO, 9.1f, 3.3f,
            MOVE_TO, 9.1f, 3.3f, LINE_TO, 6.8f, 5.6f)

STARRY_ICON(kXmark,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.75f,
            MOVE_TO, 4, 4, LINE_TO, 12, 12,
            MOVE_TO, 12, 4, LINE_TO, 4, 12)

STARRY_ICON(kXmarkBold,
            CANVAS_DIMENSIONS, 16,
            STROKE, 2.4f,
            MOVE_TO, 4.5f, 4.5f, LINE_TO, 11.5f, 11.5f,
            MOVE_TO, 11.5f, 4.5f, LINE_TO, 4.5f, 11.5f)

STARRY_ICON(kPlus,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.75f,
            MOVE_TO, 8, 2.75f, LINE_TO, 8, 13.25f,
            MOVE_TO, 2.75f, 8, LINE_TO, 13.25f, 8)

STARRY_ICON(kMagnifyingGlass,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.7f,
            CIRCLE, 7, 7, 4.6f,
            MOVE_TO, 10.4f, 10.4f, LINE_TO, 13.6f, 13.6f)

STARRY_ICON(kGlobe,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            CIRCLE, 8, 8, 6,
            OVAL, 8, 8, 2.6f, 6,
            MOVE_TO, 2, 8, LINE_TO, 14, 8,
            MOVE_TO, 3.2f, 5, LINE_TO, 12.8f, 5,
            MOVE_TO, 3.2f, 11, LINE_TO, 12.8f, 11)

STARRY_ICON(kClock,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.5f,
            CIRCLE, 8, 8, 6,
            MOVE_TO, 8, 4.5f, LINE_TO, 8, 8,
            MOVE_TO, 8, 8, LINE_TO, 10.5f, 9.6f)

STARRY_ICON(kLockFill,
            CANVAS_DIMENSIONS, 16,
            ROUND_RECT, 3.5f, 7, 9, 7, 1.6f,
            NEW_PATH,
            STROKE, 1.6f,
            MOVE_TO, 5.3f, 7.4f, LINE_TO, 5.3f, 5.2f,
            ARC_TO, 2.7f, 2.7f, 0, 0, 1, 10.7f, 5.2f,
            LINE_TO, 10.7f, 7.4f)

STARRY_ICON(kInfoCircle,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.4f,
            CIRCLE, 8, 8, 6,
            MOVE_TO, 8, 7.4f, LINE_TO, 8, 11,
            NEW_PATH,
            CIRCLE, 8, 5, 0.95f)

STARRY_ICON(kStar,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.4f,
            MOVE_TO, 8, 2.1f,
            LINE_TO, 9.528f, 6.197f,
            LINE_TO, 13.897f, 6.384f,
            LINE_TO, 10.473f, 9.103f,
            LINE_TO, 11.644f, 13.316f,
            LINE_TO, 8, 10.9f,
            LINE_TO, 4.356f, 13.316f,
            LINE_TO, 5.527f, 9.103f,
            LINE_TO, 2.103f, 6.384f,
            LINE_TO, 6.472f, 6.197f,
            CLOSE)

STARRY_ICON(kStarFill,
            CANVAS_DIMENSIONS, 16,
            MOVE_TO, 8, 2.1f,
            LINE_TO, 9.528f, 6.197f,
            LINE_TO, 13.897f, 6.384f,
            LINE_TO, 10.473f, 9.103f,
            LINE_TO, 11.644f, 13.316f,
            LINE_TO, 8, 10.9f,
            LINE_TO, 4.356f, 13.316f,
            LINE_TO, 5.527f, 9.103f,
            LINE_TO, 2.103f, 6.384f,
            LINE_TO, 6.472f, 6.197f,
            CLOSE)

STARRY_ICON(kArrowDownCircle,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.4f,
            CIRCLE, 8, 8, 6.1f,
            MOVE_TO, 8, 4.6f, LINE_TO, 8, 11,
            MOVE_TO, 5.4f, 8.6f, LINE_TO, 8, 11.2f,
            MOVE_TO, 8, 11.2f, LINE_TO, 10.6f, 8.6f)

STARRY_ICON(kSidebarLeft,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.4f,
            ROUND_RECT, 2, 3, 12, 10, 2.2f,
            MOVE_TO, 6.2f, 3, LINE_TO, 6.2f, 13)

STARRY_ICON(kSidebarRight,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.4f,
            ROUND_RECT, 2, 3, 12, 10, 2.2f,
            MOVE_TO, 9.8f, 3, LINE_TO, 9.8f, 13)

// rectangle.topthird.inset.filled：顶部标签布局。
STARRY_ICON(kRectangleTopthirdInsetFilled,
            CANVAS_DIMENSIONS,
            16,
            STROKE,
            1.4f,
            ROUND_RECT,
            2,
            3,
            12,
            10,
            2.2f,
            NEW_PATH,
            ROUND_RECT,
            4,
            5,
            8,
            2.6f,
            0.8f)

STARRY_ICON(kSpeakerWave,
            CANVAS_DIMENSIONS, 16,
            MOVE_TO, 2.5f, 6,
            LINE_TO, 5, 6,
            LINE_TO, 8.4f, 3.2f,
            LINE_TO, 8.4f, 12.8f,
            LINE_TO, 5, 10,
            LINE_TO, 2.5f, 10,
            CLOSE,
            NEW_PATH,
            STROKE, 1.3f,
            MOVE_TO, 10.4f, 6,
            ARC_TO, 2.6f, 2.6f, 0, 0, 1, 10.4f, 10,
            MOVE_TO, 12.2f, 4.1f,
            ARC_TO, 5, 5, 0, 0, 1, 12.2f, 11.9f)

STARRY_ICON(kSpeakerSlash,
            CANVAS_DIMENSIONS, 16,
            MOVE_TO, 2.5f, 6,
            LINE_TO, 5, 6,
            LINE_TO, 8.4f, 3.2f,
            LINE_TO, 8.4f, 12.8f,
            LINE_TO, 5, 10,
            LINE_TO, 2.5f, 10,
            CLOSE,
            NEW_PATH,
            STROKE, 1.4f,
            MOVE_TO, 10.4f, 5.9f, LINE_TO, 14, 10.1f,
            MOVE_TO, 14, 5.9f, LINE_TO, 10.4f, 10.1f)

// 月牙：大圆（圆心 (7.5, 8.5)、半径 5.5）被小圆（圆心 (11, 5)、半径 4.2）切掉一块。
STARRY_ICON(kMoonFill,
            CANVAS_DIMENSIONS, 16,
            MOVE_TO, 12.997f, 8.695f,
            ARC_TO, 5.5f, 5.5f, 0, 1, 1, 7.305f, 3.003f,
            ARC_TO, 4.2f, 4.2f, 0, 0, 0, 12.997f, 8.695f,
            CLOSE)

STARRY_ICON(kSparkle,
            CANVAS_DIMENSIONS, 16,
            MOVE_TO, 8, 1.5f,
            CUBIC_TO, 8.5f, 6.2f, 9.8f, 7.5f, 14.5f, 8,
            CUBIC_TO, 9.8f, 8.5f, 8.5f, 9.8f, 8, 14.5f,
            CUBIC_TO, 7.5f, 9.8f, 6.2f, 8.5f, 1.5f, 8,
            CUBIC_TO, 6.2f, 7.5f, 7.5f, 6.2f, 8, 1.5f,
            CLOSE)

STARRY_ICON(kCheckmark,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.8f,
            MOVE_TO, 3.5f, 8.5f, LINE_TO, 6.6f, 11.6f,
            MOVE_TO, 6.6f, 11.6f, LINE_TO, 12.5f, 4.5f)

STARRY_ICON(kArrowUpRight,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.6f,
            MOVE_TO, 5, 11, LINE_TO, 11, 5,
            MOVE_TO, 6, 5, LINE_TO, 11, 5,
            MOVE_TO, 11, 5, LINE_TO, 11, 10)

STARRY_ICON(kArrowDown,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.6f,
            MOVE_TO, 8, 2.6f, LINE_TO, 8, 13.2f,
            MOVE_TO, 3.8f, 9, LINE_TO, 8, 13.2f,
            MOVE_TO, 8, 13.2f, LINE_TO, 12.2f, 9)

// 月牙的轮廓（大圆圆心 (6.5, 9.5)、半径 4.8，小圆圆心 (9.6, 6.4)、半径 3.7）和右上角的 z。
STARRY_ICON(kMoonZzz,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            MOVE_TO, 11.296f, 9.688f,
            ARC_TO, 4.8f, 4.8f, 0, 1, 1, 6.312f, 4.704f,
            ARC_TO, 3.7f, 3.7f, 0, 0, 0, 11.296f, 9.688f,
            CLOSE,
            NEW_PATH,
            STROKE, 1.1f,
            MOVE_TO, 10.8f, 1.9f, LINE_TO, 14, 1.9f,
            LINE_TO, 10.8f, 5.5f, LINE_TO, 14, 5.5f)

STARRY_ICON(kSunMax,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.4f,
            CIRCLE, 8, 8, 2.8f,
            MOVE_TO, 8, 1.4f, LINE_TO, 8, 3.2f,
            MOVE_TO, 8, 12.8f, LINE_TO, 8, 14.6f,
            MOVE_TO, 1.4f, 8, LINE_TO, 3.2f, 8,
            MOVE_TO, 12.8f, 8, LINE_TO, 14.6f, 8,
            MOVE_TO, 11.394f, 4.606f, LINE_TO, 12.667f, 3.333f,
            MOVE_TO, 4.606f, 4.606f, LINE_TO, 3.333f, 3.333f,
            MOVE_TO, 11.394f, 11.394f, LINE_TO, 12.667f, 12.667f,
            MOVE_TO, 4.606f, 11.394f, LINE_TO, 3.333f, 12.667f)

// 两个斜着的胶囊形链环（半径 2.3），在中间搭在一起。
STARRY_ICON(kLink,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.5f,
            MOVE_TO, 1.974f, 10.774f,
            LINE_TO, 5.774f, 6.974f,
            ARC_TO, 2.3f, 2.3f, 0, 0, 1, 9.026f, 10.226f,
            LINE_TO, 5.226f, 14.026f,
            ARC_TO, 2.3f, 2.3f, 0, 0, 1, 1.974f, 10.774f,
            CLOSE,
            MOVE_TO, 6.974f, 5.774f,
            LINE_TO, 10.774f, 1.974f,
            ARC_TO, 2.3f, 2.3f, 0, 0, 1, 14.026f, 5.226f,
            LINE_TO, 10.226f, 9.026f,
            ARC_TO, 2.3f, 2.3f, 0, 0, 1, 6.974f, 5.774f,
            CLOSE)

// 前面一个带加号的圆角方块，右上方露出后面那个的两条边。
STARRY_ICON(kPlusSquareOnSquare,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            ROUND_RECT, 2, 5, 9, 9, 2,
            MOVE_TO, 5, 4.4f,
            LINE_TO, 5, 4,
            ARC_TO, 2, 2, 0, 0, 1, 7, 2,
            LINE_TO, 12, 2,
            ARC_TO, 2, 2, 0, 0, 1, 14, 4,
            LINE_TO, 14, 9,
            ARC_TO, 2, 2, 0, 0, 1, 12, 11,
            LINE_TO, 11.6f, 11,
            MOVE_TO, 6.5f, 7.4f, LINE_TO, 6.5f, 11.6f,
            MOVE_TO, 4.4f, 9.5f, LINE_TO, 8.6f, 9.5f)

// 图钉：顶上一条横杠，往下收窄再展开成底座，下面是针。
STARRY_ICON(kPin,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.35f,
            MOVE_TO, 5.6f, 2.4f, LINE_TO, 10.4f, 2.4f,
            MOVE_TO, 6.6f, 2.4f,
            LINE_TO, 6.2f, 6.6f,
            LINE_TO, 4.4f, 9.4f,
            LINE_TO, 11.6f, 9.4f,
            LINE_TO, 9.8f, 6.6f,
            LINE_TO, 9.4f, 2.4f,
            MOVE_TO, 8, 9.4f, LINE_TO, 8, 14)

STARRY_ICON(kPinSlash,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.35f,
            MOVE_TO, 5.6f, 2.4f, LINE_TO, 10.4f, 2.4f,
            MOVE_TO, 6.6f, 2.4f,
            LINE_TO, 6.2f, 6.6f,
            LINE_TO, 4.4f, 9.4f,
            LINE_TO, 11.6f, 9.4f,
            LINE_TO, 9.8f, 6.6f,
            LINE_TO, 9.4f, 2.4f,
            MOVE_TO, 8, 9.4f, LINE_TO, 8, 14,
            MOVE_TO, 2.6f, 2.6f, LINE_TO, 13.4f, 13.4f)

STARRY_ICON(kXmarkSquare,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.35f,
            ROUND_RECT, 2, 2, 12, 12, 2.6f,
            MOVE_TO, 5.6f, 5.6f, LINE_TO, 10.4f, 10.4f,
            MOVE_TO, 10.4f, 5.6f, LINE_TO, 5.6f, 10.4f)

// 三条横线各带一个实心的旋钮。
STARRY_ICON(kSliderHorizontal3,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            MOVE_TO, 2, 4, LINE_TO, 14, 4,
            MOVE_TO, 2, 8, LINE_TO, 14, 8,
            MOVE_TO, 2, 12, LINE_TO, 14, 12,
            NEW_PATH,
            CIRCLE, 10, 4, 1.9f,
            NEW_PATH,
            CIRCLE, 5.5f, 8, 1.9f,
            NEW_PATH,
            CIRCLE, 9, 12, 1.9f)

STARRY_ICON(kEyeSlash,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            MOVE_TO, 1.6f, 8,
            ARC_TO, 7.6f, 7.6f, 0, 0, 1, 14.4f, 8,
            ARC_TO, 7.6f, 7.6f, 0, 0, 1, 1.6f, 8,
            CLOSE,
            CIRCLE, 8, 8, 2.2f,
            MOVE_TO, 2.8f, 2.6f, LINE_TO, 13.2f, 13.4f)

STARRY_ICON(kMacWindow,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            ROUND_RECT, 1.75f, 2.75f, 12.5f, 10.5f, 2.2f,
            MOVE_TO, 1.75f, 5.7f, LINE_TO, 14.25f, 5.7f)

// 两个向对角张开的箭头（小窗口的「移到浏览器」）：角上是直角的箭头，斜线伸向中间。
STARRY_ICON(kArrowUpLeftAndArrowDownRight,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.5f,
            MOVE_TO, 2.75f, 7.25f, LINE_TO, 2.75f, 2.75f,
            MOVE_TO, 2.75f, 2.75f, LINE_TO, 7.25f, 2.75f,
            MOVE_TO, 2.75f, 2.75f, LINE_TO, 6.75f, 6.75f,
            MOVE_TO, 13.25f, 8.75f, LINE_TO, 13.25f, 13.25f,
            MOVE_TO, 13.25f, 13.25f, LINE_TO, 8.75f, 13.25f,
            MOVE_TO, 13.25f, 13.25f, LINE_TO, 9.25f, 9.25f)

STARRY_ICON(kStarSlash,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            MOVE_TO, 8, 2.1f,
            LINE_TO, 9.528f, 6.197f,
            LINE_TO, 13.897f, 6.384f,
            LINE_TO, 10.473f, 9.103f,
            LINE_TO, 11.644f, 13.316f,
            LINE_TO, 8, 10.9f,
            LINE_TO, 4.356f, 13.316f,
            LINE_TO, 5.527f, 9.103f,
            LINE_TO, 2.103f, 6.384f,
            LINE_TO, 6.472f, 6.197f,
            CLOSE,
            MOVE_TO, 2.4f, 2.4f, LINE_TO, 13.6f, 13.6f)

STARRY_ICON(kMinusCircle,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.4f,
            CIRCLE, 8, 8, 6,
            MOVE_TO, 5.2f, 8, LINE_TO, 10.8f, 8)

// 垃圾桶：盖子一条线、提手、上宽下窄的桶身和两条竖纹。
STARRY_ICON(kTrash,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            MOVE_TO, 2.6f, 4, LINE_TO, 13.4f, 4,
            MOVE_TO, 6.2f, 4, LINE_TO, 6.2f, 2.5f, LINE_TO, 9.8f, 2.5f,
            LINE_TO, 9.8f, 4,
            MOVE_TO, 3.9f, 4,
            LINE_TO, 4.6f, 12.6f,
            ARC_TO, 1.2f, 1.2f, 0, 0, 0, 5.8f, 13.7f,
            LINE_TO, 10.2f, 13.7f,
            ARC_TO, 1.2f, 1.2f, 0, 0, 0, 11.4f, 12.6f,
            LINE_TO, 12.1f, 4,
            MOVE_TO, 6.7f, 6.4f, LINE_TO, 6.9f, 11.2f,
            MOVE_TO, 9.3f, 6.4f, LINE_TO, 9.1f, 11.2f)

// 归档的标签页（标签管理）：盖子、箱子、拉手。
STARRY_ICON(kArchivebox,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.25f,
            MOVE_TO, 2.9f, 2.6f, LINE_TO, 13.1f, 2.6f,
            ARC_TO, 1, 1, 0, 0, 1, 14.1f, 3.6f,
            LINE_TO, 14.1f, 5, ARC_TO, 1, 1, 0, 0, 1, 13.1f, 6,
            LINE_TO, 2.9f, 6, ARC_TO, 1, 1, 0, 0, 1, 1.9f, 5,
            LINE_TO, 1.9f, 3.6f, ARC_TO, 1, 1, 0, 0, 1, 2.9f, 2.6f,
            CLOSE,
            MOVE_TO, 3, 6, LINE_TO, 3, 12.2f,
            ARC_TO, 1.3f, 1.3f, 0, 0, 0, 4.3f, 13.5f,
            LINE_TO, 11.7f, 13.5f,
            ARC_TO, 1.3f, 1.3f, 0, 0, 0, 13, 12.2f,
            LINE_TO, 13, 6,
            MOVE_TO, 6.6f, 8.8f, LINE_TO, 9.4f, 8.8f)

// 撤销、恢复归档的标签：往左的箭头，尾巴往右弯下来。
STARRY_ICON(kArrowUturnBackward,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.5f,
            MOVE_TO, 5.8f, 3.2f, LINE_TO, 2.6f, 6.4f, LINE_TO, 5.8f, 9.6f,
            MOVE_TO, 2.9f, 6.4f, LINE_TO, 9.6f, 6.4f,
            ARC_TO, 3.5f, 3.5f, 0, 0, 1, 9.6f, 13.4f,
            LINE_TO, 7.4f, 13.4f)

STARRY_ICON(kTrashFill,
            CANVAS_DIMENSIONS, 16,
            MOVE_TO, 3.7f, 4.6f,
            LINE_TO, 4.5f, 12.7f,
            ARC_TO, 1.3f, 1.3f, 0, 0, 0, 5.8f, 13.9f,
            LINE_TO, 10.2f, 13.9f,
            ARC_TO, 1.3f, 1.3f, 0, 0, 0, 11.5f, 12.7f,
            LINE_TO, 12.3f, 4.6f,
            CLOSE,
            NEW_PATH,
            STROKE, 1.3f,
            MOVE_TO, 2.4f, 3.6f, LINE_TO, 13.6f, 3.6f,
            MOVE_TO, 6.2f, 3.6f, LINE_TO, 6.2f, 2.2f, LINE_TO, 9.8f, 2.2f,
            LINE_TO, 9.8f, 3.6f)

STARRY_ICON(kPause,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.6f,
            MOVE_TO, 5.5f, 3.2f, LINE_TO, 5.5f, 12.8f,
            MOVE_TO, 10.5f, 3.2f, LINE_TO, 10.5f, 12.8f)

STARRY_ICON(kPlay,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.4f,
            MOVE_TO, 4.6f, 3.2f,
            LINE_TO, 12.6f, 8,
            LINE_TO, 4.6f, 12.8f,
            CLOSE)

STARRY_ICON(kFolder,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            MOVE_TO, 2, 4.6f,
            ARC_TO, 1.6f, 1.6f, 0, 0, 1, 3.6f, 3,
            LINE_TO, 6.2f, 3,
            LINE_TO, 7.7f, 4.6f,
            LINE_TO, 12.4f, 4.6f,
            ARC_TO, 1.6f, 1.6f, 0, 0, 1, 14, 6.2f,
            LINE_TO, 14, 11.8f,
            ARC_TO, 1.6f, 1.6f, 0, 0, 1, 12.4f, 13.4f,
            LINE_TO, 3.6f, 13.4f,
            ARC_TO, 1.6f, 1.6f, 0, 0, 1, 2, 11.8f,
            CLOSE)

// 打开的文件夹：后面一片只画露出来的上半边，前面一片往右斜。
STARRY_ICON(kFolderOpen,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            MOVE_TO, 2, 10.4f,
            LINE_TO, 2, 4.6f,
            ARC_TO, 1.6f, 1.6f, 0, 0, 1, 3.6f, 3,
            LINE_TO, 6.2f, 3,
            LINE_TO, 7.7f, 4.6f,
            LINE_TO, 12, 4.6f,
            ARC_TO, 1.6f, 1.6f, 0, 0, 1, 13.6f, 6.2f,
            LINE_TO, 13.6f, 7.1f,
            MOVE_TO, 4.95f, 7.1f,
            LINE_TO, 13.9f, 7.1f,
            ARC_TO, 1.1f, 1.1f, 0, 0, 1, 14.95f, 8.55f,
            LINE_TO, 13.75f, 12.2f,
            ARC_TO, 1.7f, 1.7f, 0, 0, 1, 12.15f, 13.4f,
            LINE_TO, 3.3f, 13.4f,
            ARC_TO, 1.25f, 1.25f, 0, 0, 1, 2.1f, 11.8f,
            LINE_TO, 3.35f, 8.25f,
            ARC_TO, 1.65f, 1.65f, 0, 0, 1, 4.95f, 7.1f,
            CLOSE)

// 新建文件夹：文件夹里一个加号。
STARRY_ICON(kFolderBadgePlus,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            MOVE_TO, 2, 4.6f,
            ARC_TO, 1.6f, 1.6f, 0, 0, 1, 3.6f, 3,
            LINE_TO, 6.2f, 3,
            LINE_TO, 7.7f, 4.6f,
            LINE_TO, 12.4f, 4.6f,
            ARC_TO, 1.6f, 1.6f, 0, 0, 1, 14, 6.2f,
            LINE_TO, 14, 11.8f,
            ARC_TO, 1.6f, 1.6f, 0, 0, 1, 12.4f, 13.4f,
            LINE_TO, 3.6f, 13.4f,
            ARC_TO, 1.6f, 1.6f, 0, 0, 1, 2, 11.8f,
            CLOSE,
            MOVE_TO, 8, 6.9f, LINE_TO, 8, 11.1f,
            MOVE_TO, 5.9f, 9, LINE_TO, 10.1f, 9)

STARRY_ICON(kCheckmarkCircle,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.4f,
            CIRCLE, 8, 8, 6.1f,
            MOVE_TO, 5.2f, 8.2f, LINE_TO, 7.1f, 10.1f,
            MOVE_TO, 7.1f, 10.1f, LINE_TO, 10.9f, 6)

// 斜放的铅笔：笔杆是一个平行四边形，尖在左下，靠右上一条线是笔箍。
STARRY_ICON(kPencil,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            MOVE_TO, 10.4f, 3.1f,
            LINE_TO, 12.9f, 5.6f,
            LINE_TO, 5.4f, 13.1f,
            LINE_TO, 2.5f, 13.5f,
            LINE_TO, 2.9f, 10.6f,
            CLOSE,
            MOVE_TO, 9, 4.5f, LINE_TO, 11.5f, 7)

// square.and.pencil（新对话）：右上缺一角的方框，铅笔从缺口斜着伸进来（同页面里的图标）。
STARRY_ICON(kSquareAndPencil,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            MOVE_TO, 13, 8.6f,
            V_LINE_TO, 12,
            R_ARC_TO, 1.5f, 1.5f, 0, 0, 1, -1.5f, 1.5f,
            R_H_LINE_TO, -7,
            ARC_TO, 1.5f, 1.5f, 0, 0, 1, 3, 12,
            V_LINE_TO, 5,
            R_ARC_TO, 1.5f, 1.5f, 0, 0, 1, 1.5f, -1.5f,
            H_LINE_TO, 8,
            MOVE_TO, 11.9f, 2.5f,
            LINE_TO, 13.5f, 4.1f,
            LINE_TO, 7.9f, 9.7f,
            R_LINE_TO, -2.1f, 0.5f,
            R_LINE_TO, 0.5f, -2.1f,
            CLOSE)

// 网站权限的图标（实心）：摄像头、麦克风、位置、通知。
STARRY_ICON(kVideoFill,
            CANVAS_DIMENSIONS, 16,
            ROUND_RECT, 1.5f, 4, 9, 8, 1.8f,
            NEW_PATH,
            MOVE_TO, 11.2f, 7.1f,
            LINE_TO, 14.5f, 5,
            LINE_TO, 14.5f, 11,
            LINE_TO, 11.2f, 8.9f,
            CLOSE)

STARRY_ICON(kMicFill,
            CANVAS_DIMENSIONS, 16,
            ROUND_RECT, 5.8f, 1.5f, 4.4f, 8, 2.2f,
            NEW_PATH,
            STROKE, 1.4f,
            MOVE_TO, 3.6f, 7.6f,
            ARC_TO, 4.4f, 4.4f, 0, 0, 0, 12.4f, 7.6f,
            MOVE_TO, 8, 12, LINE_TO, 8, 14.4f)

STARRY_ICON(kLocationFill,
            CANVAS_DIMENSIONS, 16,
            MOVE_TO, 13.8f, 2.2f,
            LINE_TO, 2.2f, 7.3f,
            LINE_TO, 7.4f, 8.6f,
            LINE_TO, 8.7f, 13.8f,
            CLOSE)

STARRY_ICON(kBellFill,
            CANVAS_DIMENSIONS, 16,
            MOVE_TO, 8, 1.8f,
            ARC_TO, 4.2f, 4.2f, 0, 0, 0, 3.8f, 6,
            LINE_TO, 3.8f, 9.2f,
            LINE_TO, 2.4f, 11.4f,
            LINE_TO, 13.6f, 11.4f,
            LINE_TO, 12.2f, 9.2f,
            LINE_TO, 12.2f, 6,
            ARC_TO, 4.2f, 4.2f, 0, 0, 0, 8, 1.8f,
            CLOSE,
            NEW_PATH,
            CIRCLE, 8, 13.2f, 1.4f)

STARRY_ICON(kMinus,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.75f,
            MOVE_TO, 2.75f, 8, LINE_TO, 13.25f, 8)

// 放大镜里一个加号（缩放提示）。
STARRY_ICON(kMagnifyingGlassPlus,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.6f,
            CIRCLE, 7, 7, 4.8f,
            MOVE_TO, 10.6f, 10.6f, LINE_TO, 13.8f, 13.8f,
            MOVE_TO, 7, 4.9f, LINE_TO, 7, 9.1f,
            MOVE_TO, 4.9f, 7, LINE_TO, 9.1f, 7)

// 上下两个箭头（弹出式菜单按钮右边的那个，同页面里的 chevron.up.chevron.down）。
STARRY_ICON(kChevronUpChevronDown,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.9f,
            MOVE_TO, 4.6f, 6, LINE_TO, 8, 2.6f,
            MOVE_TO, 8, 2.6f, LINE_TO, 11.4f, 6,
            MOVE_TO, 4.6f, 10, LINE_TO, 8, 13.4f,
            MOVE_TO, 8, 13.4f, LINE_TO, 11.4f, 10)

// 两个箭头从上下收向中间一条横线（树状标签页的「折叠其他组」）。
STARRY_ICON(kChevronsToLine,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.5f,
            MOVE_TO, 5, 2.5f, LINE_TO, 8, 5.5f,
            MOVE_TO, 8, 5.5f, LINE_TO, 11, 2.5f,
            MOVE_TO, 2.5f, 8, LINE_TO, 13.5f, 8,
            MOVE_TO, 5, 13.5f, LINE_TO, 8, 10.5f,
            MOVE_TO, 8, 10.5f, LINE_TO, 11, 13.5f)

// 增加、减少缩进：上下两条长线、中间两条短线，左侧配朝右或朝左的小箭头。
STARRY_ICON(kIncreaseIndent,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            MOVE_TO, 2, 3, LINE_TO, 14, 3,
            MOVE_TO, 7.5f, 6.6f, LINE_TO, 14, 6.6f,
            MOVE_TO, 7.5f, 9.4f, LINE_TO, 14, 9.4f,
            MOVE_TO, 2, 13, LINE_TO, 14, 13,
            MOVE_TO, 2.2f, 5.8f, LINE_TO, 4.6f, 8,
            MOVE_TO, 4.6f, 8, LINE_TO, 2.2f, 10.2f)

STARRY_ICON(kDecreaseIndent,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            MOVE_TO, 2, 3, LINE_TO, 14, 3,
            MOVE_TO, 7.5f, 6.6f, LINE_TO, 14, 6.6f,
            MOVE_TO, 7.5f, 9.4f, LINE_TO, 14, 9.4f,
            MOVE_TO, 2, 13, LINE_TO, 14, 13,
            MOVE_TO, 4.6f, 5.8f, LINE_TO, 2.2f, 8,
            MOVE_TO, 2.2f, 8, LINE_TO, 4.6f, 10.2f)

// 拼图块：一个方块，上边和右边凸出半圆，下边和左边凹进半圆（同页面里的
// puzzlepiece）。
STARRY_ICON(kPuzzlepiece,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.25f,
            MOVE_TO, 3, 5,
            LINE_TO, 5.4f, 5,
            ARC_TO, 1.6f, 1.6f, 0, 1, 1, 8.6f, 5,
            LINE_TO, 11, 5,
            LINE_TO, 11, 7.4f,
            ARC_TO, 1.6f, 1.6f, 0, 1, 1, 11, 10.6f,
            LINE_TO, 11, 13,
            LINE_TO, 8.6f, 13,
            ARC_TO, 1.6f, 1.6f, 0, 1, 0, 5.4f, 13,
            LINE_TO, 3, 13,
            LINE_TO, 3, 10.6f,
            ARC_TO, 1.6f, 1.6f, 0, 1, 0, 3, 7.4f,
            CLOSE)

// 分屏（rectangle.split.2x1）：一个横的圆角框，中间一条竖线。
STARRY_ICON(kSplit2x1,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            ROUND_RECT, 1.75f, 3, 12.5f, 10, 2.2f,
            MOVE_TO, 8, 3, LINE_TO, 8, 13)

// 三格（rectangle.split.3x1）：分屏满了。
STARRY_ICON(kSplit3x1,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            ROUND_RECT, 1.5f, 3, 13, 10, 2.2f,
            MOVE_TO, 5.83f, 3, LINE_TO, 5.83f, 13,
            MOVE_TO, 10.17f, 3, LINE_TO, 10.17f, 13)

// 移出分屏（rectangle.portrait.and.arrow.right）：竖的框，箭头从里面指向右边。
STARRY_ICON(kSplitRemove,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            MOVE_TO, 9, 4.6f,
            LINE_TO, 9, 3.6f,
            ARC_TO, 1.6f, 1.6f, 0, 0, 0, 7.4f, 2,
            LINE_TO, 3.6f, 2,
            ARC_TO, 1.6f, 1.6f, 0, 0, 0, 2, 3.6f,
            LINE_TO, 2, 12.4f,
            ARC_TO, 1.6f, 1.6f, 0, 0, 0, 3.6f, 14,
            LINE_TO, 7.4f, 14,
            ARC_TO, 1.6f, 1.6f, 0, 0, 0, 9, 12.4f,
            LINE_TO, 9, 11.4f,
            MOVE_TO, 6, 8, LINE_TO, 14.2f, 8,
            MOVE_TO, 14.2f, 8, LINE_TO, 11.8f, 5.6f,
            MOVE_TO, 14.2f, 8, LINE_TO, 11.8f, 10.4f)

// 一个框（rectangle）：只显示这个标签页。
STARRY_ICON(kRectangle,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            ROUND_RECT, 1.75f, 3, 12.5f, 10, 2.2f)

// 均分（equal.square）。
STARRY_ICON(kEqualSquare,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            ROUND_RECT, 2, 2, 12, 12, 2.6f,
            MOVE_TO, 5.3f, 6.5f, LINE_TO, 10.7f, 6.5f,
            MOVE_TO, 5.3f, 9.5f, LINE_TO, 10.7f, 9.5f)

// 三个点（ellipsis）。
STARRY_ICON(kEllipsis,
            CANVAS_DIMENSIONS, 16,
            CIRCLE, 3.2f, 8, 1.35f,
            NEW_PATH,
            CIRCLE, 8, 8, 1.35f,
            NEW_PATH,
            CIRCLE, 12.8f, 8, 1.35f)

// 框里填了一半（rectangle.lefthalf.inset.filled 这一组）：分屏放在哪一边。
STARRY_ICON(kRectLeftHalf,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            ROUND_RECT, 1.75f, 3, 12.5f, 10, 2.2f,
            NEW_PATH,
            ROUND_RECT, 3.6f, 4.85f, 4, 6.3f, 1)

STARRY_ICON(kRectRightHalf,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            ROUND_RECT, 1.75f, 3, 12.5f, 10, 2.2f,
            NEW_PATH,
            ROUND_RECT, 8.4f, 4.85f, 4, 6.3f, 1)

STARRY_ICON(kRectTopHalf,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            ROUND_RECT, 1.75f, 3, 12.5f, 10, 2.2f,
            NEW_PATH,
            ROUND_RECT, 3.6f, 4.85f, 8.8f, 2.65f, 1)

STARRY_ICON(kRectBottomHalf,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            ROUND_RECT, 1.75f, 3, 12.5f, 10, 2.2f,
            NEW_PATH,
            ROUND_RECT, 3.6f, 8.5f, 8.8f, 2.65f, 1)

// 内存条（memorychip）：一块板，上下各三根针脚。
STARRY_ICON(kMemoryChip,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.25f,
            ROUND_RECT, 2, 4.5f, 12, 7, 1.6f,
            MOVE_TO, 5, 2.2f, LINE_TO, 5, 4.5f,
            MOVE_TO, 8, 2.2f, LINE_TO, 8, 4.5f,
            MOVE_TO, 11, 2.2f, LINE_TO, 11, 4.5f,
            MOVE_TO, 5, 11.5f, LINE_TO, 5, 13.8f,
            MOVE_TO, 8, 11.5f, LINE_TO, 8, 13.8f,
            MOVE_TO, 11, 11.5f, LINE_TO, 11, 13.8f)

// 处理器（cpu）：方块，里面一个小方块，四边各两根针脚。
STARRY_ICON(kCpu,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.25f,
            ROUND_RECT, 3.5f, 3.5f, 9, 9, 1.8f,
            ROUND_RECT, 6.2f, 6.2f, 3.6f, 3.6f, 0.6f,
            MOVE_TO, 6.5f, 1.4f, LINE_TO, 6.5f, 3.5f,
            MOVE_TO, 9.5f, 1.4f, LINE_TO, 9.5f, 3.5f,
            MOVE_TO, 6.5f, 12.5f, LINE_TO, 6.5f, 14.6f,
            MOVE_TO, 9.5f, 12.5f, LINE_TO, 9.5f, 14.6f,
            MOVE_TO, 1.4f, 6.5f, LINE_TO, 3.5f, 6.5f,
            MOVE_TO, 1.4f, 9.5f, LINE_TO, 3.5f, 9.5f,
            MOVE_TO, 12.5f, 6.5f, LINE_TO, 14.6f, 6.5f,
            MOVE_TO, 12.5f, 9.5f, LINE_TO, 14.6f, 9.5f)

// 实心的警告三角（exclamationmark.triangle.fill）：同一条路径里的感叹号按
// 奇偶规则挖空。
STARRY_ICON(kExclamationTriangleFill,
            CANVAS_DIMENSIONS, 16,
            MOVE_TO, 6.7f, 2.3f,
            CUBIC_TO, 7.28f, 1.3f, 8.72f, 1.3f, 9.3f, 2.3f,
            LINE_TO, 14.8f, 11.9f,
            CUBIC_TO, 15.37f, 12.9f, 14.65f, 14.1f, 13.5f, 14.1f,
            LINE_TO, 2.5f, 14.1f,
            CUBIC_TO, 1.35f, 14.1f, 0.63f, 12.9f, 1.2f, 11.9f,
            CLOSE,
            ROUND_RECT, 7.25f, 5, 1.5f, 5, 0.75f,
            CIRCLE, 8, 11.8f, 0.9f)

// 盾牌（广告与跟踪拦截）：shield.lefthalf.filled 是轮廓加上实心的左半，
// shield.slash 是轮廓加一道斜线（这个网站上关掉了）。
STARRY_ICON(kShieldLefthalfFilled,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.35f,
            MOVE_TO, 8, 1.9f,
            LINE_TO, 13, 3.6f,
            LINE_TO, 13, 7.6f,
            CUBIC_TO, 13, 10.7f, 11, 12.9f, 8, 14.2f,
            CUBIC_TO, 5, 12.9f, 3, 10.7f, 3, 7.6f,
            LINE_TO, 3, 3.6f,
            CLOSE,
            NEW_PATH,
            MOVE_TO, 8, 2.9f,
            LINE_TO, 8, 13.1f,
            CUBIC_TO, 5.7f, 12, 3.9f, 10.1f, 3.9f, 7.6f,
            LINE_TO, 3.9f, 4.2f,
            CLOSE)

STARRY_ICON(kShieldSlash,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.35f,
            MOVE_TO, 8, 1.9f,
            LINE_TO, 13, 3.6f,
            LINE_TO, 13, 7.6f,
            CUBIC_TO, 13, 10.7f, 11, 12.9f, 8, 14.2f,
            CUBIC_TO, 5, 12.9f, 3, 10.7f, 3, 7.6f,
            LINE_TO, 3, 3.6f,
            CLOSE,
            NEW_PATH,
            STROKE, 1.35f,
            MOVE_TO, 2.2f, 2.2f, LINE_TO, 13.8f, 13.8f)

// 齿轮（gearshape）：八个齿的外圈和中间的圆。
STARRY_ICON(kGearshape,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            CIRCLE, 8, 8, 2.1f,
            NEW_PATH,
            STROKE, 1.3f,
            CIRCLE, 8, 8, 4.6f,
            MOVE_TO, 8, 1.6f, LINE_TO, 8, 3.4f,
            MOVE_TO, 8, 12.6f, LINE_TO, 8, 14.4f,
            MOVE_TO, 1.6f, 8, LINE_TO, 3.4f, 8,
            MOVE_TO, 12.6f, 8, LINE_TO, 14.4f, 8,
            MOVE_TO, 3.5f, 3.5f, LINE_TO, 4.75f, 4.75f,
            MOVE_TO, 11.25f, 11.25f, LINE_TO, 12.5f, 12.5f,
            MOVE_TO, 3.5f, 12.5f, LINE_TO, 4.75f, 11.25f,
            MOVE_TO, 11.25f, 4.75f, LINE_TO, 12.5f, 3.5f)

// 网络（network，任务管理器的网络进程）：三个节点连成三角形。
STARRY_ICON(kNetwork,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            CIRCLE, 8, 3.6f, 1.9f,
            CIRCLE, 3.6f, 12.2f, 1.9f,
            CIRCLE, 12.4f, 12.2f, 1.9f,
            MOVE_TO, 7.14f, 5.29f, LINE_TO, 4.46f, 10.51f,
            MOVE_TO, 8.86f, 5.29f, LINE_TO, 11.54f, 10.51f,
            MOVE_TO, 5.5f, 12.2f, LINE_TO, 10.5f, 12.2f)

// 井号（number，拷贝进程 ID）。
STARRY_ICON(kNumber,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.4f,
            MOVE_TO, 6.4f, 2.5f, LINE_TO, 5.2f, 13.5f,
            MOVE_TO, 10.8f, 2.5f, LINE_TO, 9.6f, 13.5f,
            MOVE_TO, 3, 5.8f, LINE_TO, 13.2f, 5.8f,
            MOVE_TO, 2.6f, 10.2f, LINE_TO, 12.8f, 10.2f)

// 八边形里一个叉（xmark.octagon，结束进程）。
STARRY_ICON(kXmarkOctagon,
            CANVAS_DIMENSIONS, 16,
            STROKE, 1.3f,
            MOVE_TO, 5.59f, 2.18f,
            LINE_TO, 10.41f, 2.18f,
            LINE_TO, 13.82f, 5.59f,
            LINE_TO, 13.82f, 10.41f,
            LINE_TO, 10.41f, 13.82f,
            LINE_TO, 5.59f, 13.82f,
            LINE_TO, 2.18f, 10.41f,
            LINE_TO, 2.18f, 5.59f,
            CLOSE,
            MOVE_TO, 5.9f, 5.9f, LINE_TO, 10.1f, 10.1f,
            MOVE_TO, 10.1f, 5.9f, LINE_TO, 5.9f, 10.1f)

// 实心的八边形（xmark.octagon.fill，结束进程的确认框）：叉在同一条路径里按奇偶规则
// 挖空。
STARRY_ICON(kXmarkOctagonFill,
            CANVAS_DIMENSIONS, 16,
            MOVE_TO, 5.4f, 1.72f,
            LINE_TO, 10.6f, 1.72f,
            LINE_TO, 14.28f, 5.4f,
            LINE_TO, 14.28f, 10.6f,
            LINE_TO, 10.6f, 14.28f,
            LINE_TO, 5.4f, 14.28f,
            LINE_TO, 1.72f, 10.6f,
            LINE_TO, 1.72f, 5.4f,
            CLOSE,
            MOVE_TO, 10.69f, 6.44f,
            LINE_TO, 9.13f, 8,
            LINE_TO, 10.69f, 9.56f,
            LINE_TO, 9.56f, 10.69f,
            LINE_TO, 8, 9.13f,
            LINE_TO, 6.44f, 10.69f,
            LINE_TO, 5.31f, 9.56f,
            LINE_TO, 6.87f, 8,
            LINE_TO, 5.31f, 6.44f,
            LINE_TO, 6.44f, 5.31f,
            LINE_TO, 8, 6.87f,
            LINE_TO, 9.56f, 5.31f,
            CLOSE)

}  // namespace starry::icons
