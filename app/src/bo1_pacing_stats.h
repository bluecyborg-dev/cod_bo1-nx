// Counters of the game's frame pacing waits, read by the native renderer's slow-frame report (its interface comes
// from project8-sw's guest_pacing_stats.h). Nothing feeds them yet: BO1's swap wait isn't replaced, so they stay 0.
#pragma once

#include <atomic>
#include <cstdint>

namespace bo1::pacing {
// Time the render thread slept in the presentation-interval wait.
inline std::atomic<uint64_t> g_vblank_wait_ns{0};
// Late frames let through without waiting for the next vblank.
inline std::atomic<uint64_t> g_late_skips{0};
}  // namespace bo1::pacing
