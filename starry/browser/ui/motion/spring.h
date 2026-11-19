// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_MOTION_SPRING_H_
#define STARRY_BROWSER_UI_MOTION_SPRING_H_

namespace starry {

// 参数化阻尼振子：质量 1、刚度 (2π / response)²、阻尼 4π · dampingFraction / response。
//
// 采用解析解而非数值积分，运动轨迹与帧率无关。中途变更目标时从当前位移和速度延续
// 新的曲线，确保打断动画连贯过渡。
struct Spring {
  double response = 0.5;
  double damping_fraction = 0.825;

  bool operator==(const Spring&) const = default;
};

// 相对终点的位移和速度。
struct SpringState {
  double displacement = 0;
  double velocity = 0;
};

// 从 displacement（相对终点）以 velocity 出发，t 秒后的位移和速度。
SpringState SolveSpring(const Spring& spring,
                        double displacement,
                        double velocity,
                        double t);

// 当位移小于 epsilon 且速度小于 10 倍 epsilon 时视为收敛。
// 返回从出发到停下的秒数，最多 max_seconds。
double SpringSettleTime(const Spring& spring,
                        double displacement,
                        double velocity,
                        double epsilon,
                        double max_seconds = 4);

}  // namespace starry

#endif  // STARRY_BROWSER_UI_MOTION_SPRING_H_
