#pragma once

#include "core/StringUtil.hpp"
#include "core/Types.hpp"

#include <d2d1.h>

#include <algorithm>
#include <cmath>
#include <string>

// Small helpers shared by the MainWindow*.cpp files.
namespace cadence::ui {

[[nodiscard]] inline bool PtInRectF(const D2D1_RECT_F& r, float x, float y) noexcept {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

[[nodiscard]] inline float MoveToward(float cur, float target, float step) noexcept {
    if (std::abs(target - cur) <= step) return target;
    return cur + (target > cur ? step : -step);
}

inline constexpr double kMaxRate = 500.0;

// Piecewise slider curve: 0..50% maps 0.5..50.0 (fine control), 50..100% maps 50.0..500.0
[[nodiscard]] inline float RateToSliderT(double rate) noexcept {
    rate = std::clamp(rate, 0.5, kMaxRate);
    if (rate <= 50.0) {
        return static_cast<float>((rate - 0.5) / 49.5 * 0.5);
    }
    return static_cast<float>(0.5 + (rate - 50.0) / (kMaxRate - 50.0) * 0.5);
}

[[nodiscard]] inline double SliderTToRate(float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    if (t <= 0.5f) {
        const double v = 0.5 + (static_cast<double>(t) / 0.5) * 49.5;
        return std::round(v * 2.0) / 2.0; // 0.5 step
    }
    const double v = 50.0 + ((static_cast<double>(t) - 0.5) / 0.5) * (kMaxRate - 50.0);
    return std::round(v); // 1.0 step above 50
}

[[nodiscard]] inline std::wstring PatternLabel(ClickPattern p) {
    switch (p) {
        case ClickPattern::Single:      return L"Single";
        case ClickPattern::Double:      return L"Double";
        case ClickPattern::TripleBurst: return L"Triple burst";
        case ClickPattern::CustomBurst: return L"Custom burst";
    }
    return L"Single";
}

[[nodiscard]] inline std::wstring LimitLabel(RepeatLimitMode m) {
    switch (m) {
        case RepeatLimitMode::Unlimited: return L"Unlimited";
        case RepeatLimitMode::Count:     return L"Count";
        case RepeatLimitMode::Duration:  return L"Duration";
    }
    return L"Unlimited";
}

} // namespace cadence::ui
