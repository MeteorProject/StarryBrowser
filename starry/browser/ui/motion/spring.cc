// Copyright 2026 The Starry Browser Authors

#include "starry/browser/ui/motion/spring.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace starry {

namespace {

// 阻尼比接近 1 时欠阻尼和过阻尼公式存在数值精度损失（ωd → 0、r1 → r2），
// 走临界阻尼分支。
constexpr double kCriticalBand = 1e-6;
constexpr double kMinimumResponse = 1e-3;
// 找停下的时间时的步长：比 120 Hz 的一帧还短。
constexpr double kSettleStep = 1.0 / 240;

}  // namespace

SpringState SolveSpring(const Spring& spring,
                        double x0,
                        double v0,
                        double t) {
  t = std::max(t, 0.0);
  const double omega =
      2 * std::numbers::pi / std::max(spring.response, kMinimumResponse);
  const double zeta = std::max(spring.damping_fraction, 0.0);

  if (zeta < 1 - kCriticalBand) {
    // x(t) = e^(−ζω t) (x0 cos ωd t + (v0 + ζω x0) / ωd · sin ωd t)
    const double decay = zeta * omega;
    const double omega_d = omega * std::sqrt(1 - zeta * zeta);
    const double b = (v0 + decay * x0) / omega_d;
    const double envelope = std::exp(-decay * t);
    const double c = std::cos(omega_d * t);
    const double s = std::sin(omega_d * t);
    return {envelope * (x0 * c + b * s),
            envelope * (v0 * c - (decay * b + omega_d * x0) * s)};
  }
  if (zeta > 1 + kCriticalBand) {
    // x(t) = c1 e^(r1 t) + c2 e^(r2 t)，r = −ω (ζ ∓ √(ζ² − 1))
    const double root = std::sqrt(zeta * zeta - 1);
    const double r1 = -omega * (zeta - root);
    const double r2 = -omega * (zeta + root);
    const double c1 = (v0 - r2 * x0) / (r1 - r2);
    const double c2 = x0 - c1;
    const double e1 = std::exp(r1 * t);
    const double e2 = std::exp(r2 * t);
    return {c1 * e1 + c2 * e2, c1 * r1 * e1 + c2 * r2 * e2};
  }
  // x(t) = e^(−ω t) (x0 + (v0 + ω x0) t)
  const double b = v0 + omega * x0;
  const double envelope = std::exp(-omega * t);
  return {envelope * (x0 + b * t), envelope * (v0 - omega * b * t)};
}

double SpringSettleTime(const Spring& spring,
                        double x0,
                        double v0,
                        double epsilon,
                        double max_seconds) {
  epsilon = std::max(epsilon, 1e-9);
  // 最后一次还在动的时刻之后一步就是停下。
  double last_moving = -kSettleStep;
  for (double t = 0; t <= max_seconds; t += kSettleStep) {
    const SpringState state = SolveSpring(spring, x0, v0, t);
    if (std::abs(state.displacement) >= epsilon ||
        std::abs(state.velocity) >= epsilon * 10) {
      last_moving = t;
    }
  }
  return std::min(last_moving + kSettleStep, max_seconds);
}

}  // namespace starry
