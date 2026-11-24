#pragma once

#include <chrono>

namespace cadence {

// Time values inside the engine carry their unit in the type, so milliseconds,
// microseconds and seconds can no longer be mixed up silently.
// Floating-point durations convert implicitly between each other (e.g. Millis -> Micros).
using Millis  = std::chrono::duration<double, std::milli>;
using Micros  = std::chrono::duration<double, std::micro>;
using Seconds = std::chrono::duration<double>;

// Raw QueryPerformanceCounter values stay int64_t; convert them only through TimingEngine
// (ToQpcTicks / QpcToMillis / QpcToMicros), which knows the counter frequency.

} // namespace cadence
