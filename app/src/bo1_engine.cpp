// Call of Duty: Black Ops for Nintendo Switch: game engine functions intercepted by the port.
//
// Each translated function sub_X is a weak alias of __imp__sub_X, so defining sub_X here replaces it in every call
// while __imp__sub_X stays the original. The generated code is not touched.
//
// Addresses are for our default.xex (base version 0.0.0.3, see app/bo1_manifest.toml), found by hand:
//   Com_Error  0x82332180  (code, format, ...): the most frequent callee of error format strings
//              (refs/bo1-recomp/tools/find_string_callers.py "Couldn't", "exceeded"...: 34 calls); its prologue
//              saves r5-r10 (variadic) and keeps r3 (code) and r4 (format). bo1-recomp's TU11 build has it at
//              0x82313280.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/platform.h>

#include "native/bo1_native_system.h"
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xthread.h>

namespace {

// Minimal printf over the guest arguments (r5..r10: on PowerPC variadic arguments are passed in registers).
// Enough for engine error messages. (Adapted from bo1-recomp, MIT.)
std::string FormatGuest(PPCContext& ctx, uint8_t* base, uint32_t format) {
  const PPCRegister* args[] = {&ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  size_t next = 0;
  auto guest_string = [&](uint32_t address) -> std::string {
    if (!address) return "(null)";
    const char* s = reinterpret_cast<const char*>(base + address);
    return std::string(s, strnlen(s, 512));
  };
  std::string fmt = guest_string(format), out;
  for (size_t i = 0; i < fmt.size(); ++i) {
    char c = fmt[i];
    if (c != '%') {
      if (static_cast<unsigned char>(c) >= 0x20 || c == '\n') out += c;
      continue;
    }
    size_t j = i + 1;  // skip flags, width, precision and length
    while (j < fmt.size() && std::strchr("-+ #0123456789.lhzI", fmt[j])) ++j;
    if (j >= fmt.size()) break;
    char conversion = fmt[j];
    i = j;
    if (conversion == '%') {
      out += '%';
      continue;
    }
    if (next >= std::size(args)) {
      out += '?';
      continue;
    }
    const PPCRegister& arg = *args[next++];
    char buf[64];
    switch (conversion) {
      case 's':
        out += guest_string(arg.u32);
        break;
      case 'd':
      case 'i':
        std::snprintf(buf, sizeof(buf), "%d", int32_t(arg.u32));
        out += buf;
        break;
      case 'u':
        std::snprintf(buf, sizeof(buf), "%u", arg.u32);
        out += buf;
        break;
      case 'x':
      case 'X':
      case 'p':
        std::snprintf(buf, sizeof(buf), conversion == 'X' ? "%X" : "%x", arg.u32);
        out += buf;
        break;
      case 'c':
        out += char(arg.u32);
        break;
      default:
        out += '?';
        break;
    }
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  return out;
}

}  // namespace

// --- Idle game threads ----------------------------------------------------------------------------------------------
//
// On the Xbox 360 every game thread had a hardware thread of its own, and the engine waits for work by polling. On the
// Switch the game gets three 1 GHz cores, and on the first console run two threads spun at ~95% each ("Backend" and
// "Server", see rex_profile.log), leaving one core for everything else: the intro video stalled. Found by sampling the
// macOS build (`sample <pid>`):
//
//   Backend: sub_8238FC18(condition, help) waits until condition() != 0 and, while waiting, helps the job queue:
//              loop { if (sub_825443F0(0)) sub_82545D98(...);  if (condition()) break; }      <- no sleep at all
//            sub_825443F0(queue) returns 1 if the job queue (default 0x8274_62E0) has pending work.
//   Server:  WaitForSingleObjectEx(handle, 0, ...) (sub_824DE0B8) called again and again with a zero timeout.
//
// The same two patterns bo1-recomp found on PC (its TU11 hooks: job queue "run one" and WaitForSingleObject). Here:
// a thread that keeps finding the job queue empty in a tight loop sleeps 100 us per poll, and a zero-timeout poll that
// timed out 256 times in a row on the same handle waits up to 1 ms (returning as soon as the handle is signaled).
// Results are unchanged; only the spinning is gone.
//
// A third one, found with the console stack sampler (2026-10-16, Kino der Toten, shadows off at the 20 FPS cap: the
// three cores at 300 %, the two job queue workers at ~50-67 % each):
//
//   JQ Worker: sub_82545308 (the worker loop) asks sub_825450A8 for a job (it scans 8 priority lists and yields when
//              all are empty) and, when there is none, calls sub_825444B8. That one waits on the queue's event (1 ms
//              timeouts; the producer pulses it per job) only while the "jobs pending" count is 0; while any job is
//              still running elsewhere the count stays above 0, so it just yields and returns, and the worker spins.
//
// The console profile then showed the workers rarely find the lists empty: a job whose callback returns "not finished"
// is put back on its list and handed out again at once (see the hook on sub_82546470 below), which is the spin.

namespace {

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

constexpr uint64_t kTightPollNs = 200'000;    // polls closer than this belong to one busy loop
constexpr uint32_t kEmptyPollsBeforeSleep = 64;
constexpr auto kIdleSleep = std::chrono::microseconds(100);
constexpr uint32_t kPollTimeoutsBeforeWait = 256;
constexpr uint32_t kStatusTimeout = 0x102;

}  // namespace

REX_EXTERN(__imp__sub_825443F0);
REX_HOOK_RAW(sub_825443F0) {
  __imp__sub_825443F0(ctx, base);
  thread_local uint64_t last_empty_ns = 0;
  thread_local uint32_t empty_streak = 0;
  if (ctx.r3.u32 & 0xFF) {  // work is waiting: no sleep, reset the streak
    empty_streak = 0;
    return;
  }
  const uint64_t now = NowNs();
  empty_streak = (now - last_empty_ns < kTightPollNs) ? empty_streak + 1 : 0;
  last_empty_ns = now;
  if (empty_streak >= kEmptyPollsBeforeSleep) {
    std::this_thread::sleep_for(kIdleSleep);
  }
}

REXCVAR_DEFINE_INT32(bo1_job_worker_sleep_us, 100, "bo1",
                     "Job queue workers that keep requeuing an unfinished job in a tight loop sleep this long per pass "
                     "(0 = spin as the game does)")
    .range(0, 2000);

// sub_82546470(list, job) appends a 124-byte job to one of the job queue's lock-free lists. The worker loop calls it at
// 0x8254551C for a job whose callback returned "not finished" (non-zero): the job goes back on its list and the next
// sub_825450A8 hands it straight out again, so a job that waits for something keeps two workers spinning. The
// helpers' "run one job" (sub_82545618, called by Backend's wait loop sub_8238FC18 through sub_82545D98 and by other
// threads waiting for jobs) requeues the same way at 0x82545788: with only the workers throttled, Backend took the
// spin over (20-28 % -> 50-77 % of a core on the console). Such a
// requeue less than kTightPollNs after the previous one on the same thread belongs to that loop; from the 16th in a row
// the thread sleeps bo1_job_worker_sleep_us after it (0 = the game's own spinning). The job is already back on its list,
// so another thread can still take it; only the polling slows down. The game's main thread also runs "run one job"
// (sub_8238FB98 -> sub_82545D98) while it waits for jobs; sleeping there put 19 % of its time in these sleeps and added
// input lag on the console, so the main thread is never throttled.
constexpr uint32_t kRequeuesBeforeSleep = 16;
constexpr uint32_t kWorkerRequeueReturn = 0x8254552C;  // sub_82545308, the workers' loop
constexpr uint32_t kHelperRequeueReturn = 0x8254578C;  // sub_82545618, "run one job"

std::atomic<uint64_t> g_requeues{0}, g_requeue_sleeps{0};

REX_EXTERN(__imp__sub_82546470);
REX_HOOK_RAW(sub_82546470) {
  const bool requeue = ctx.lr == kWorkerRequeueReturn || ctx.lr == kHelperRequeueReturn;
  uint32_t callback = 0;
  if (requeue) {
    // The job's type is at +12 and its callback at type+8 (as the worker loop reads them).
    uint32_t type = 0;
    std::memcpy(&type, base + ctx.r4.u32 + 12, 4);
    type = __builtin_bswap32(type);
    if (type) {
      std::memcpy(&callback, base + type + 8, 4);
      callback = __builtin_bswap32(callback);
    }
  }
  __imp__sub_82546470(ctx, base);
  if (!requeue) return;
  g_requeues.fetch_add(1, std::memory_order_relaxed);
  thread_local uint64_t last_ns = 0;
  thread_local uint32_t streak = 0;
  const uint64_t now = NowNs();
  streak = (now - last_ns < kTightPollNs) ? streak + 1 : 0;
  last_ns = now;
  thread_local const bool main_thread = [] {
    auto* thread = rex::system::XThread::GetCurrentThread();
    return thread && thread->main_thread();
  }();
  const int32_t sleep_us = REXCVAR_GET(bo1_job_worker_sleep_us);
  if (sleep_us > 0 && streak >= kRequeuesBeforeSleep && !main_thread) {
    g_requeue_sleeps.fetch_add(1, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
  }
  // Every 10 s: how many requeues, how many sleeps, and the callback of the last job requeued by this thread.
  static std::atomic<uint64_t> report_ns{0};
  uint64_t due = report_ns.load(std::memory_order_relaxed);
  if (!due) {
    report_ns.compare_exchange_strong(due, now + 10'000'000'000ull);
  } else if (now >= due && report_ns.compare_exchange_strong(due, now + 10'000'000'000ull)) {
    REXLOG_INFO("bo1: job queue last 10 s: {} jobs requeued as not finished, {} sleeps of {} us (last callback {:08X})",
                g_requeues.exchange(0), g_requeue_sleeps.exchange(0), sleep_us, callback);
  }
}

// WaitForSingleObjectEx(handle, milliseconds, alertable) -> status (0x102 on timeout).
REX_EXTERN(__imp__sub_824DE0B8);
REX_HOOK_RAW(sub_824DE0B8) {
  thread_local uint32_t poll_handle = 0;
  thread_local uint32_t poll_timeouts = 0;
  const uint32_t handle = ctx.r3.u32;
  const bool poll = ctx.r4.u32 == 0;
  if (poll && handle == poll_handle && poll_timeouts >= kPollTimeoutsBeforeWait) {
    ctx.r4.u64 = 1;  // wait up to 1 ms instead of returning at once
  }
  __imp__sub_824DE0B8(ctx, base);
  if (!poll) {
    return;
  }
  if (ctx.r3.u32 != kStatusTimeout) {
    poll_timeouts = 0;
    return;
  }
  if (handle != poll_handle) {
    poll_handle = handle;
    poll_timeouts = 0;
  }
  ++poll_timeouts;
}

// --- Cinematic (Bink) trace -----------------------------------------------------------------------------------------
//
// Bring-up diagnostics for the intro video freezing on the Switch after ~1.4 s (audio and picture together, while the
// game keeps running). The video is decoded by a job (sub_825A02B8 -> sub_8259FAB0 -> sub_8259EF58 -> Bink in
// 0x82612E00..0x826225A8) that the "Server" thread runs. Counts calls and return values once per second, so the Mac
// (where the video plays) and the Switch can be compared step by step. Cheap: a few atomic increments per call.

namespace {

struct TraceCounter {
  const char* name;
  std::atomic<uint32_t> calls{0};
  std::atomic<uint32_t> nonzero{0};  // r3 != 0 on return
  std::atomic<uint32_t> last_r3{0};
};

TraceCounter g_trace[] = {{"job 825A02B8"}, {"job body 8259FAB0"}, {"decode 8259EF58"},
                          {"bink 82615558"}, {"bink 82615B50"}};

// Bink read-buffer state, from sub_8259EEE0(const BINKREALTIME*) = the engine's "percentage full" computation (PC
// decompilation: R_Cinematic_Advance pauses the video, "underrun", when it is below 20% and seizes the disc to refill
// it below 50%). BINKREALTIME: +0 FrameNum, +44 ReadBufferSize, +48 ReadBufferUsed (big-endian u32).
std::atomic<uint32_t> g_bink_frame{0}, g_bink_used{0}, g_bink_size{0}, g_bink_pct{0}, g_bink_pct_min{1000};
std::atomic<uint32_t> g_bink_forced{0};  // frames let through by the BinkWait valve (below)

void TraceReport() {
  static std::atomic<uint64_t> last_ns{0};
  const uint64_t now = NowNs();
  uint64_t prev = last_ns.load(std::memory_order_relaxed);
  if (prev == 0) {
    last_ns.compare_exchange_strong(prev, now);
    return;
  }
  if (now - prev < 1'000'000'000ull || !last_ns.compare_exchange_strong(prev, now)) {
    return;
  }
  std::string line = fmt::format("bink frame {} (forced {}) buffer {}/{} = {}% (min {}%) | ", g_bink_frame.load(), g_bink_forced.exchange(0),
                                 g_bink_used.load(), g_bink_size.load(), g_bink_pct.load(),
                                 g_bink_pct_min.exchange(1000));
  for (auto& t : g_trace) {
    line += fmt::format("{} {} ({} nz, last {:#x}) | ", t.name, t.calls.exchange(0), t.nonzero.exchange(0),
                        t.last_r3.load());
  }
  REXLOG_INFO("bo1: cinematic trace 1 s: {}", line);
}

template <size_t I>
void Traced(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*)) {
  original(ctx, base);
  auto& t = g_trace[I];
  t.calls.fetch_add(1, std::memory_order_relaxed);
  if (ctx.r3.u32) t.nonzero.fetch_add(1, std::memory_order_relaxed);
  t.last_r3.store(ctx.r3.u32, std::memory_order_relaxed);
  TraceReport();
}

}  // namespace

// --- Bink audio-clock deadlock valve ---------------------------------------------------------------------------------
//
// R_Cinematic_Advance (sub_8259EF58) decodes the next frame only when BinkWait(bink) (sub_82614E00) returns 0. With a
// soundtrack, Bink's clock follows the audio. On the Switch the recompiled Bink decodes slower than real time, the
// decoded audio runs out, the audio clock stops, BinkWait answers "wait" forever, nothing is decoded and no audio is
// submitted again: the intro froze with its sound after ~70 frames (reproduced on the Mac by slowing
// R_Cinematic_Advance down, see BO1_DEBUG_CINE_ADVANCE_SLEEP_MS). The Xbox 360 never fell behind, so it never hit it.
// Fallback, like the PC engine's "custom skip logic" (timer based): while Bink's own clock works it decides; when it
// is stuck and the video is behind real time, BinkWait answers "go" and BinkShouldSkip "catch up", so the video
// follows the wall clock and the decoded frames feed Bink audio again. (Function identities from the PC decompilation's R_Cinematic_Advance, matched by call order:
// BinkGetRealtime 826150F0, BinkPause 82614FB8, BinkWait 82614E00, BinkDoFrame 82615948, BinkShouldSkip 82614DA0,
// BinkNextFrame 82615950. BINK +12 = FrameNum.)
namespace {

uint32_t LoadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Wall-clock fallback for one BINK: (anchor time, anchor frame) is re-set every time Bink's own clock says "go", so
// the fallback only measures how far the video fell behind real time since Bink last moved by itself.
struct BinkClock {
  std::atomic<uint32_t> bink{0};
  std::atomic<uint32_t> anchor_frame{0};
  std::atomic<uint64_t> anchor_ns{0};
};
BinkClock g_bink_clock;

// BINK: +12 FrameNum, +20 FrameRate, +24 FrameRateDiv (checked on the Mac: 1280x720, 587 frames, 30/1).
void BinkReadState(uint8_t* base, uint32_t bink, uint32_t& frame, double& fps) {
  frame = LoadBe32(base + bink + 12);
  const uint32_t rate = LoadBe32(base + bink + 20), div = LoadBe32(base + bink + 24);
  fps = (rate && div) ? double(rate) / double(div) : 30.0;
  if (fps < 1.0 || fps > 120.0) fps = 30.0;
}

void BinkAnchor(uint32_t bink, uint32_t frame, uint64_t now) {
  g_bink_clock.bink.store(bink, std::memory_order_relaxed);
  g_bink_clock.anchor_frame.store(frame, std::memory_order_relaxed);
  g_bink_clock.anchor_ns.store(now, std::memory_order_relaxed);
}

// How many frames the video is behind real time since the last anchor (0 if not behind or unknown).
uint32_t BinkFramesBehind(uint32_t bink, uint32_t frame, double fps, uint64_t now) {
  if (g_bink_clock.bink.load(std::memory_order_relaxed) != bink) {
    BinkAnchor(bink, frame, now);
    return 0;
  }
  const double elapsed = double(now - g_bink_clock.anchor_ns.load(std::memory_order_relaxed)) * 1e-9;
  const double expected = double(g_bink_clock.anchor_frame.load(std::memory_order_relaxed)) + elapsed * fps;
  return expected > double(frame) + 1.0 ? uint32_t(expected - double(frame)) : 0;
}

}  // namespace

// BinkWait(bink): non-zero = "not time for the next frame yet".
REX_EXTERN(__imp__sub_82614E00);
REX_HOOK_RAW(sub_82614E00) {
  const uint32_t bink = ctx.r3.u32;
  __imp__sub_82614E00(ctx, base);
  uint32_t frame;
  double fps;
  BinkReadState(base, bink, frame, fps);
  const uint64_t now = NowNs();
  if (ctx.r3.u32 == 0) {  // Bink's own clock moves: follow it
    BinkAnchor(bink, frame, now);
    return;
  }
  if (BinkFramesBehind(bink, frame, fps, now) > 0) {
    ctx.r3.u64 = 0;  // behind real time while Bink's clock is stuck: decode now
    g_bink_forced.fetch_add(1, std::memory_order_relaxed);
  }
}

// BinkShouldSkip(bink): non-zero = "behind, decode the next frame too" (R_Cinematic_Advance's catch-up loop).
REX_EXTERN(__imp__sub_82614DA0);
REX_HOOK_RAW(sub_82614DA0) {
  const uint32_t bink = ctx.r3.u32;
  __imp__sub_82614DA0(ctx, base);
  if (ctx.r3.u32 != 0) {
    return;
  }
  uint32_t frame;
  double fps;
  BinkReadState(base, bink, frame, fps);
  const uint32_t frames = LoadBe32(base + bink + 8);
  // Catch up while more than one frame behind real time (never past the last frame: the loop also stops there).
  if (frame + 1 < frames && BinkFramesBehind(bink, frame, fps, NowNs()) > 1) {
    ctx.r3.u64 = 1;
    g_bink_forced.fetch_add(1, std::memory_order_relaxed);
  }
}

REX_EXTERN(__imp__sub_8259EEE0);
REX_HOOK_RAW(sub_8259EEE0) {
  const uint32_t realtime = ctx.r3.u32;
  auto be32 = [&](uint32_t address) {
    const uint8_t* p = base + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  };
  __imp__sub_8259EEE0(ctx, base);
  const uint32_t pct = ctx.r3.u32;
  g_bink_frame.store(be32(realtime + 0), std::memory_order_relaxed);
  g_bink_size.store(be32(realtime + 44), std::memory_order_relaxed);
  g_bink_used.store(be32(realtime + 48), std::memory_order_relaxed);
  g_bink_pct.store(pct, std::memory_order_relaxed);
  uint32_t lowest = g_bink_pct_min.load(std::memory_order_relaxed);
  while (pct < lowest && !g_bink_pct_min.compare_exchange_weak(lowest, pct)) {
  }
}

REX_EXTERN(__imp__sub_825A02B8);
// Debugging aid: BO1_DEBUG_EXEC="<console command>" queues that command once, BO1_DEBUG_EXEC_DELAY_S (default 20)
// seconds after the first cinematic job ran (the menu's TV screens keep this job running), e.g.
//   BO1_DEBUG_EXEC="devmap zombie_theater" out/mac/bo1 ...
// to load a map on the Mac without navigating the menus. Uses Cbuf_AddText (sub_8232EA30(localClientNum, text): it
// takes the command buffer lock, critical section 0x2F, so any thread may call it; the main loop runs it next frame).
REXCVAR_DEFINE_STRING(bo1_debug_hash_files, "", "bo1",
                      "Debug: comma-separated game files to hash per MiB at start-up (see bo1_app.h)");
REXCVAR_DEFINE_STRING(bo1_debug_exec, "", "bo1",
                      "Debug: console command queued once after bo1_debug_exec_delay_s ('+' = space), e.g. "
                      "devmap+zombie_theater");
REXCVAR_DEFINE_INT32(bo1_debug_exec_delay_s, 20, "bo1",
                     "Debug: seconds after the first cinematic job before bo1_debug_exec is queued");
// The SDK's console profiler (sdk/src/ui/switch_perf.cpp) appends a report to logs/rex/rex_profile.log on the SD card
// every 10 s: CPU per thread, libnx call counts and, with stack sampling, where each thread spends its time. These two
// settings turn sampling on without the flag file on the card and copy every report into the log, so a Wi-Fi session
// (tools/nxlink_run.sh) brings it back to the Mac. Addresses come out as image+0x...: resolve them with addr2line on
// the ELF of that build (out/nx/bo1).
REXCVAR_DEFINE_BOOL(bo1_profile_stacks, false, "bo1",
                    "Switch: sample every thread's stack at 1 kHz for the profiler report (pauses each thread "
                    "briefly); same as logs/rex/stacks_profile.flag");
REXCVAR_DEFINE_BOOL(bo1_profile_log, false, "bo1",
                    "Switch: copy every 10 s profiler report (logs/rex/rex_profile.log) into the log");

#if defined(__SWITCH__)
extern "C" bool RexSwitchPerfStacksWanted(void) { return REXCVAR_GET(bo1_profile_stacks); }

// The profiler calls this on its own thread, which runs at a high priority (0x2A) and pauses other threads to sample
// them: it must not log there (a log call takes the logger's lock and writes to the SD card and the network). The
// report only goes into a buffer; Bo1StartProfileLog's thread, at a normal priority, sends it to the log.
namespace {
std::mutex g_profile_mutex;
std::string g_profile_text;
}  // namespace

extern "C" void RexSwitchPerfReportText(const char* text, size_t len) {
  if (!REXCVAR_GET(bo1_profile_log)) return;
  std::lock_guard<std::mutex> lock(g_profile_mutex);
  if (g_profile_text.size() + len <= (512u << 10)) g_profile_text.append(text, len);
}

void Bo1StartProfileLog() {
  if (!REXCVAR_GET(bo1_profile_log)) return;
  // Never joined: it lives as long as the process (a joinable std::thread destroyed at exit would terminate).
  new std::thread([] {
    std::string text;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      {
        std::lock_guard<std::mutex> lock(g_profile_mutex);
        text.swap(g_profile_text);
      }
      std::string_view rest(text);
      while (!rest.empty()) {
        const size_t end = rest.find('\n');
        const std::string_view line = rest.substr(0, end);
        if (!line.empty()) REXLOG_INFO("[profile] {}", line);
        if (end == std::string_view::npos) break;
        rest.remove_prefix(end + 1);
      }
      text.clear();
    }
  });
}
#else
void Bo1StartProfileLog() {}
#endif
// Engine settings (dvars) for the console, as the CoD4 port does (codmw_dvars): set 10 s after the first cinematic job
// and again every 5 s, in case a map script sets one back. The default turns off what costs the most on the Switch
// for the least visible loss: dynamic shadows (sm_enable 0: the sun's cascades and the spot lights' maps were ~3,100 of
// ~4,800 draws a frame in Kino der Toten; the baked shadows of the lightmaps stay), depth of field, distortion, flame
// effects, shell casings, decals on static models and ragdolls (texture streaming: bo1_stream_off_in_game).
// "name value; name value" ('+' = space).
// Texture streaming (high-detail mips from images.pak) off during gameplay only: it made ~80 new textures a second on
// the console, the main source of stutter, and its high mips were the rainbow floor in Kino. It cannot simply be set
// with the other dvars: with r_stream 0 a map load waits forever for the streamer (Mac and console), and writing it
// while a load runs restarts the streamer each time. So it goes off once the native renderer sees gameplay (300+
// draws a frame for 5 s) and back on as soon as a loading screen shows (under 100 draws for 1.5 s).
REXCVAR_DEFINE_BOOL(bo1_stream_off_in_game, true, "bo1",
                    "Turn texture streaming (r_stream) off during gameplay and back on for loading screens");
REXCVAR_DEFINE_DOUBLE(bo1_stream_restore_after_s, 1.5, "bo1",
                      "Seconds a loading screen must last (and r_stream stay changed) before streaming is restored");
REXCVAR_DEFINE_INT32(bo1_dvars_delay_s, 10, "bo1",
                     "Seconds after the first cinematic job before bo1_dvars is first applied");
REXCVAR_DEFINE_STRING(bo1_dvars,
                      "r_dof_enable 0; r_distortion 0; r_flameFX_enable 0; cg_brass 0; fx_marks_smodels 0; "
                      "ragdoll_enable 0",
                      "bo1", "Engine settings to apply (\"dvar value; dvar value\"; empty = the game's own)");
// Test preset for the frames that stutter: on the console (2026-10-16) the frames of 70-199 ms averaged ~1,370 draws
// against ~860 for an average frame (GPU wait 23 ms): crowds and effects. These skip particle effects and elements
// that are off screen or too small, drop the big particle clouds, and keep fewer corpses. Added to bo1_dvars.
REXCVAR_DEFINE_BOOL(bo1_fx_lite, false, "bo1",
                    "Fewer effects: fx_cull_effect_spawn/elem_spawn/elem_draw 1, fx_drawClouds 0, ai_corpseCount 3");
REXCVAR_DECLARE(bool, bo1_native_skip_shadow_maps);
REXCVAR_DEFINE_STRING(bo1_shadows, "off", "bo1",
                      "Dynamic shadows, live (settings menu: L+R+Right): off (sm_enable 0: fastest, but models flicker "
                      "between lit and dark near lights), min (the shadow system on with the least it can draw, and no "
                      "shadow map drawn: no dynamic shadows and no flicker), or none to leave them to bo1_dvars")
    .allowed({"off", "min", "none"});

#if REX_PLATFORM_SWITCH
#include <rex/input/switch/switch_input_driver.h>
#endif

// Hold MINUS and press D-pad Right/Left in game: the next/previous bo1_shadows preset, applied within half a second by
// MaybeApplyDvars (as a change in the settings menu would be).
void Bo1CycleShadows(int direction) {
  static const char* const kPresets[] = {"off", "min"};
  constexpr int kCount = int(std::size(kPresets));
  const std::string current = REXCVAR_GET(bo1_shadows);
  int index = 0;
  for (int i = 0; i < kCount; ++i) {
    if (current == kPresets[i]) index = i;
  }
  index = (index + direction + kCount) % kCount;
  REXCVAR_SET(bo1_shadows, kPresets[index]);
  REXLOG_INFO("bo1: MINUS + D-pad: shadows {}", kPresets[index]);
}

void Bo1RegisterShadowCombo() {
#if REX_PLATFORM_SWITCH
  rex::input::nx::SetMinusDpadHandler(&Bo1CycleShadows);
#endif
}

REX_EXTERN(__imp__sub_8232EA30);
namespace {
// Queues a console command with Cbuf_AddText on this thread's guest stack, then puts every register back.
void QueueCommand(PPCContext& ctx, uint8_t* base, const std::string& command) {
  const PPCContext saved = ctx;
  const std::string text = command + "\n";
  ctx.r1.u32 -= 0x400;
  const uint32_t text_address = ctx.r1.u32 + 0x100;
  const size_t size = std::min<size_t>(text.size() + 1, 0x2F0);
  std::memcpy(base + text_address, text.c_str(), size);
  base[text_address + size - 1] = 0;
  ctx.r3.u64 = 0;
  ctx.r4.u64 = text_address;
  __imp__sub_8232EA30(ctx, base);
  ctx = saved;
}

// r_stream's dvar_t (registered at 0x8258E6B0 as an int 0..7, default 7, pointer stored at 0x83CF6850): +0 name,
// +0x10 type (5 = int), +0x18 current value, +0x28 latched value, +0x38 reset value. Written directly so it takes effect
// at once: "set r_stream" goes through the command buffer, which the main thread does not run while a map loads, and a
// load started with streaming off waits forever for the streamer.
constexpr uint32_t kStreamDvarPointer = 0x83CF6850;
constexpr uint32_t kStreamDvarName = 0x820D6C3C;
bool WriteStreamDvar(uint8_t* base, uint32_t value) {
  auto be32 = [&](uint32_t a) {
    uint32_t v;
    std::memcpy(&v, base + a, 4);
    return __builtin_bswap32(v);
  };
  const uint32_t dvar = be32(kStreamDvarPointer);
  if (!dvar || dvar >= 0xA0000000u || be32(dvar) != kStreamDvarName || be32(dvar + 0x10) != 5) return false;
  const uint32_t v = __builtin_bswap32(value);
  std::memcpy(base + dvar + 0x18, &v, 4);
  std::memcpy(base + dvar + 0x28, &v, 4);
  return true;
}

int32_t ReadStreamDvar(uint8_t* base) {
  auto be32 = [&](uint32_t a) {
    uint32_t v;
    std::memcpy(&v, base + a, 4);
    return __builtin_bswap32(v);
  };
  const uint32_t dvar = be32(kStreamDvarPointer);
  if (!dvar || dvar >= 0xA0000000u || be32(dvar) != kStreamDvarName || be32(dvar + 0x10) != 5) return -1;
  return int32_t(be32(dvar + 0x18));
}

// Finds a dvar_t by its name string's guest address. The dvars live in one pool; r_stream's (found through its
// pointer) is in it, so the pool is searched around it. Returns 0 when not found.
uint32_t FindDvarByName(uint8_t* base, uint32_t name_address) {
  auto be32 = [&](uint32_t a) {
    uint32_t v;
    std::memcpy(&v, base + a, 4);
    return __builtin_bswap32(v);
  };
  const uint32_t anchor = be32(kStreamDvarPointer);
  if (!anchor || anchor >= 0xA0000000u || be32(anchor) != kStreamDvarName) return 0;
  constexpr uint32_t kRange = 4u << 20;
  const uint32_t begin = anchor > kRange ? anchor - kRange : 0;
  for (uint32_t a = begin & ~3u; a < anchor + kRange; a += 4) {
    if (be32(a) == name_address && be32(a + 0x10) <= 10) return a;
  }
  return 0;
}

// Writes a dvar's current and latched value directly, for the dvars whose "set" only takes effect at the next map
// load (sm_sunShadowScale). Types seen: 0 bool (a byte), 1 float, 5 int.
bool WriteDvarDirect(uint8_t* base, uint32_t dvar, const char* value) {
  uint32_t type;
  std::memcpy(&type, base + dvar + 0x10, 4);
  type = __builtin_bswap32(type);
  uint32_t word;
  if (type == 0) {
    base[dvar + 0x18] = base[dvar + 0x28] = uint8_t(std::atoi(value) != 0);
    return true;
  } else if (type == 1) {
    const float f = float(std::atof(value));
    std::memcpy(&word, &f, 4);
  } else if (type == 5) {
    word = uint32_t(std::atoi(value));
  } else {
    return false;
  }
  word = __builtin_bswap32(word);
  std::memcpy(base + dvar + 0x18, &word, 4);
  std::memcpy(base + dvar + 0x28, &word, 4);
  return true;
}

void MaybeToggleStreaming(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  if (!REXCVAR_GET(bo1_stream_off_in_game) || !bo1::native::Enabled()) return;
  // The dvar's own value is the state, so nothing gets out of step whatever changed it.
  const int32_t value = ReadStreamDvar(base);
  if (value < 0) return;
  static uint64_t state_since = 0, value_since = 0;
  static int last_state = -1;  // 0 loading screen, 1 gameplay, 2 neither
  static int32_t last_value = -1;
  if (value != last_value) {
    value_since = NowNs();
    last_value = value;
  }
  const uint32_t draws = bo1::native::LastFrameDraws();
  const uint64_t now = NowNs();
  const int state = draws >= 300 ? 1 : (draws < 100 ? 0 : 2);
  if (state != last_state) {
    state_since = now;
    last_state = state;
  }
  if (state == 1 && value != 0 && now - state_since >= 5'000'000'000ull) {
    if (WriteStreamDvar(base, 0)) REXLOG_INFO("bo1: gameplay ({} draws a frame): texture streaming off", draws);
  } else if (state == 0 && value != 7 &&
             now - std::max(state_since, value_since) >=
                 uint64_t(std::max(REXCVAR_GET(bo1_stream_restore_after_s), 0.0) * 1e9)) {
    if (WriteStreamDvar(base, 7)) {
      REXLOG_INFO("bo1: loading screen ({} draws a frame): texture streaming back on (r_stream 7)", draws);
    }
  }
}

// The console commands of a bo1_shadows preset. "min" keeps the shadow system on (with it off, models near lights
// alternate between lit and dark every frame) and sets it to draw as little as it can: one light, no character or
// static model casters, small low-quality maps, a quarter of the sun's range; the renderer then skips every shadow map
// draw (bo1_native_skip_shadow_maps). The game's own values (sub_8258CDA8's dvar registration) are 4 lights, both
// caster kinds, quality 1, sun scale 1, small sun map off; "off" leaves the system off, where the others do not
// matter. sm_sunEnable, sm_spotEnable and r_spotLightShadows are not used: setting them changed nothing in a frame
// trace. (lite, medium and full presets, which drew the maps, were tried on 2026-10-16 and dropped:
// docs/OPTIMIZATION_LOG.md.)
std::string ShadowCommands(const std::string& preset) {
  if (preset == "off") return "set sm_enable 0";
  if (preset != "min") return {};
  return "set sm_enable 1; set sm_maxLights 1; set r_spotLightEntityShadows 0; set r_spotLightSModelShadows 0; "
         "set sm_qualitySpotShadow 0; set sm_sunShadowScale 0.25; set sm_sunShadowSmallEnable 1";
}

// The values of a preset for the dvars that "set" does not change until the next map load (sm_sunShadowScale: when
// switched live, the sun shadow stayed at its old size), written straight into the dvars as well.
void WriteLatchedShadowDvars(uint8_t* base, const std::string& preset) {
  struct Latched {
    uint32_t name_address;  // the name string in default.xex
    const char *off, *min;
  };
  static const Latched kLatched[] = {
      {0x820D6E0C, "1", "0.25"},  // sm_sunShadowScale
  };
  static uint32_t dvars[std::size(kLatched)] = {};
  for (size_t i = 0; i < std::size(kLatched); ++i) {
    const Latched& row = kLatched[i];
    const char* value = preset == "min" ? row.min : preset == "off" ? row.off : nullptr;
    if (!value) continue;
    if (!dvars[i]) {
      dvars[i] = FindDvarByName(base, row.name_address);
      if (dvars[i]) {
        uint32_t type;
        std::memcpy(&type, base + dvars[i] + 0x10, 4);
        REXLOG_INFO("bo1: dvar with name {:08X} at {:08X}, type {}", row.name_address, dvars[i],
                    __builtin_bswap32(type));
      }
    }
    if (!dvars[i] || !WriteDvarDirect(base, dvars[i], value)) {
      REXLOG_WARN("bo1: could not write the dvar at name {:08X} directly", row.name_address);
    }
  }
}

void MaybeApplyDvars(PPCContext& ctx, uint8_t* base) {
  // bo1_dvars once; afterwards every 5 s all but r_stream*: writing r_stream (even with the same value) while a map
  // loads restarts the streaming system and the load never finished (reproduced on the Mac and the console). The
  // shadow preset goes with them, and also at once when it is changed in the settings menu.
  static std::string periodic;
  static const std::string commands = [] {
    std::string list = REXCVAR_GET(bo1_dvars);
    if (REXCVAR_GET(bo1_fx_lite)) {
      list += "; fx_cull_effect_spawn 1; fx_cull_elem_spawn 1; fx_cull_elem_draw 1; fx_drawClouds 0; ai_corpseCount 3";
    }
    std::replace(list.begin(), list.end(), '+', ' ');
    std::string out;
    size_t start = 0;
    while (start < list.size()) {
      size_t end = list.find(';', start);
      if (end == std::string::npos) end = list.size();
      std::string item = list.substr(start, end - start);
      start = end + 1;
      const size_t first = item.find_first_not_of(' ');
      if (first == std::string::npos) continue;
      item = item.substr(first, item.find_last_not_of(' ') - first + 1);
      out += (out.empty() ? "set " : "; set ") + item;
      if (item.rfind("r_stream", 0) != 0) periodic += (periodic.empty() ? "set " : "; set ") + item;
    }
    return out;
  }();
  // Debugging aid: BO1_DEBUG_SHADOW_CYCLE="off,min" switches bo1_shadows to the next preset every 20 s, as the
  // settings menu would (checks live switching on the Mac, which has no controller menu in scripted runs).
  static const std::vector<std::string> cycle = [] {
    std::vector<std::string> out;
    if (const char* v = std::getenv("BO1_DEBUG_SHADOW_CYCLE")) {
      std::string list = v;
      for (size_t start = 0; start < list.size();) {
        size_t end = list.find(',', start);
        if (end == std::string::npos) end = list.size();
        out.push_back(list.substr(start, end - start));
        start = end + 1;
      }
    }
    return out;
  }();
  if (!cycle.empty()) {
    static uint64_t cycle_ns = 0;
    static size_t cycle_index = 0;
    const uint64_t t = NowNs();
    if (!cycle_ns) cycle_ns = t;
    if (t - cycle_ns >= 20'000'000'000ull) {
      cycle_ns = t;
      REXCVAR_SET(bo1_shadows, cycle[cycle_index++ % cycle.size()]);
    }
  }
  static std::atomic<uint64_t> first_ns{0}, last_ns{0}, shadows_checked_ns{0};
  // Only this hook's thread reads these two.
  static std::string shadows = REXCVAR_GET(bo1_shadows);
  static std::string shadow_commands = ShadowCommands(shadows);
  const uint64_t now = NowNs();
  uint64_t expected = 0;
  first_ns.compare_exchange_strong(expected, now);
  if (now - first_ns.load() < uint64_t(std::max(REXCVAR_GET(bo1_dvars_delay_s), 0)) * 1'000'000'000ull) return;
  uint64_t last = last_ns.load();
  // The settings menu writes the cvar on the UI thread; the preset names fit std::string's inline buffer, so a read
  // that races with it can only see a wrong value for a moment.
  if (last && now - shadows_checked_ns.load() >= 500'000'000ull) {
    shadows_checked_ns.store(now);
    const std::string current = REXCVAR_GET(bo1_shadows);
    REXCVAR_SET(bo1_native_skip_shadow_maps, current == "min");
    if (current != shadows && ShadowCommands(current) != shadow_commands) {
      shadows = current;
      shadow_commands = ShadowCommands(current);
      if (!shadow_commands.empty()) QueueCommand(ctx, base, shadow_commands);
      WriteLatchedShadowDvars(base, current);
      REXLOG_INFO("bo1: shadows set to {} live: {}", current, shadow_commands);
      return;
    }
  }
  if (last && now - last < 5'000'000'000ull) return;
  if (!last_ns.compare_exchange_strong(last, now)) return;
  std::string batch = last ? periodic : commands;
  if (!shadow_commands.empty()) batch += (batch.empty() ? "" : "; ") + shadow_commands;
  if (batch.empty()) return;
  QueueCommand(ctx, base, batch);
  if (!last) {
    REXCVAR_SET(bo1_native_skip_shadow_maps, shadows == "min");
    REXLOG_INFO("bo1: engine settings queued: {}", batch);
  }
}

void MaybeInjectDebugCommand(PPCContext& ctx, uint8_t* base) {
  // From the environment (Mac) or from the setting bo1_debug_exec (Switch: --bo1_debug_exec=devmap+zombie_theater,
  // '+' stands for a space because nxlink splits its arguments on spaces).
  static const std::string command_storage = [] {
    std::string c;
    if (const char* v = std::getenv("BO1_DEBUG_EXEC")) c = v;
    if (c.empty()) {
      c = REXCVAR_GET(bo1_debug_exec);
      std::replace(c.begin(), c.end(), '+', ' ');
    }
    return c;
  }();
  const char* command = command_storage.c_str();
  if (!*command) return;
  static const uint64_t delay_ns = [] {
    const char* v = std::getenv("BO1_DEBUG_EXEC_DELAY_S");
    return uint64_t(v ? std::atoi(v) : REXCVAR_GET(bo1_debug_exec_delay_s)) * 1'000'000'000ull;
  }();
  static std::atomic<uint64_t> first_ns{0};
  static std::atomic<bool> done{false};
  const uint64_t now = NowNs();
  uint64_t expected = 0;
  first_ns.compare_exchange_strong(expected, now);
  if (done.load() || now - first_ns.load() < delay_ns || done.exchange(true)) return;
  QueueCommand(ctx, base, command);
  REXLOG_WARN("bo1: debug command queued: {}", command);
}
}  // namespace

REX_HOOK_RAW(sub_825A02B8) {
  MaybeApplyDvars(ctx, base);
  MaybeToggleStreaming(ctx, base);
  MaybeInjectDebugCommand(ctx, base);
  // Debugging aid: BO1_DEBUG_CINE_JOB_SLEEP_MS=<ms> delays every run of the cinematic job, to reproduce on a fast PC
  // the Switch, where the game (and so this per-frame job) runs at ~8-10 FPS.
  static const int sleep_ms = [] {
    const char* v = std::getenv("BO1_DEBUG_CINE_JOB_SLEEP_MS");
    return v ? std::atoi(v) : 0;
  }();
  if (sleep_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  Traced<0>(ctx, base, __imp__sub_825A02B8);
}
REX_EXTERN(__imp__sub_8259FAB0);
REX_HOOK_RAW(sub_8259FAB0) { Traced<1>(ctx, base, __imp__sub_8259FAB0); }
REX_EXTERN(__imp__sub_8259EF58);
REX_HOOK_RAW(sub_8259EF58) {
  // Debugging aid: BO1_DEBUG_CINE_ADVANCE_SLEEP_MS=<ms> makes every R_Cinematic_Advance slower, like Bink decoding on
  // the Switch's 1 GHz cores.
  static const int sleep_ms = [] {
    const char* v = std::getenv("BO1_DEBUG_CINE_ADVANCE_SLEEP_MS");
    return v ? std::atoi(v) : 0;
  }();
  if (sleep_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  Traced<2>(ctx, base, __imp__sub_8259EF58);
}
REX_EXTERN(__imp__sub_82615558);
REX_HOOK_RAW(sub_82615558) { Traced<3>(ctx, base, __imp__sub_82615558); }
REX_EXTERN(__imp__sub_82615B50);
REX_HOOK_RAW(sub_82615B50) { Traced<4>(ctx, base, __imp__sub_82615B50); }

// Com_Error(errorParm_t code, const char* format, ...): log the message, then run the original (which may longjmp
// back to the engine's main loop, or end the game for fatal errors).
REX_EXTERN(__imp__sub_82332180);
REX_HOOK_RAW(sub_82332180) {
  static const char* kNames[] = {"fatal", "drop", "server disconnect", "disconnect", "script", "script (drop)",
                                 "localization"};
  const uint32_t code = ctx.r3.u32;
  const char* name = code < std::size(kNames) ? kNames[code] : "?";
  REXLOG_ERROR("bo1: engine error (code {} {}): {}", code, name, FormatGuest(ctx, base, ctx.r4.u32));
  __imp__sub_82332180(ctx, base);
}
