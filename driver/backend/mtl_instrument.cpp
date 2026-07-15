/*
 * mtl_instrument.cpp — per-function CPU profiler backing the PROFILE_INSTRUMENT=1
 * build mode (scripts/build-driver.sh). The backend hot-path TUs are compiled
 * with -finstrument-functions; GCC then calls the two __cyg_profile hooks below
 * on every function enter/exit. This TU is compiled WITHOUT the flag and every
 * function is additionally marked no_instrument_function (belt and suspenders).
 *
 * Design (wine PE / Rosetta constraints — see the profiling notes):
 *  - Gated at runtime on getenv("D9MT_PROFILE_INSTRUMENT"): unset -> the hooks
 *    are a load+compare+ret. The instrumented BUILD can run "normally".
 *  - Per-thread state (thread_local pointer, lazily calloc'd): a shadow call
 *    stack {fn, enter_tsc, children_ticks} plus an open-addressing aggregate
 *    table fn -> {calls, self_ticks, total_ticks}. The CS thread is the hot
 *    thread; thread-local state means zero locks/atomics on the hot path.
 *  - Clock: rdtsc (cheap and monotonic under Rosetta), calibrated against
 *    QueryPerformanceCounter at dump time to convert ticks -> ns.
 *  - Symbolication: mingw PE has no dladdr. Each dump line logs the function's
 *    OFFSET from the DLL load base (&__ImageBase), symbolicated OFFLINE against
 *    llvm-nm/objdump output of build/driver/d3d9.dll.
 *  - Dump: whenever a thread's shadow stack returns to depth 0 and >=10s have
 *    elapsed since its last dump, it prints its CUMULATIVE table to stderr
 *    (ends up in the wine launch log). Lines are `D9MTINST F <off> <calls>
 *    <self_ns> <total_ns>` between BEGIN/END markers; ranking is done offline.
 *  - Recursion: total_ns of a directly/indirectly recursive function counts
 *    each nested activation (inclusive time is per-activation). self_ns is
 *    unaffected. The backend hot path has no meaningful recursion.
 */

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define NOINSTR __attribute__((no_instrument_function))

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

  constexpr uint32_t kMaxDepth   = 512u;
  constexpr uint32_t kTableSize  = 1u << 15;         /* 32768 slots, pow2   */
  constexpr double   kDumpPeriodSec = 10.0;

  struct StackFrame {
    void*    fn;
    uint64_t t0;          /* rdtsc at enter                       */
    uint64_t childTicks;  /* inclusive ticks of completed callees */
  };

  struct Slot {
    void*    fn;
    uint64_t calls;
    uint64_t selfTicks;
    uint64_t totalTicks;
  };

  struct ThreadState {
    StackFrame stack[kMaxDepth];
    Slot       table[kTableSize];
    uint32_t   depth;
    uint32_t   overflow;     /* enters skipped past kMaxDepth        */
    uint64_t   droppedSlots; /* exits dropped because table was full */
    uint32_t   zeroEvents;   /* depth-0 returns; QPC checked every 8192 */
    LARGE_INTEGER lastDumpQpc;
    DWORD      tid;
  };

  /* -1 = not probed yet, 0 = off, 1 = on. Racy init is benign (idempotent). */
  volatile int g_enabledState = -1;

  /* Global tsc<->QPC calibration anchor, set once by the first thread. */
  volatile LONG g_calibInit = 0;
  uint64_t      g_tsc0  = 0;
  LARGE_INTEGER g_qpc0  = {};
  LARGE_INTEGER g_qpcHz = {};

  thread_local ThreadState* t_state = nullptr;

  NOINSTR static inline uint64_t rdtscNow() {
    return __builtin_ia32_rdtsc();
  }

  NOINSTR static bool profEnabled() {
    int e = g_enabledState;
    if (e >= 0)
      return e != 0;
    const char* v = std::getenv("D9MT_PROFILE_INSTRUMENT");
    e = (v && v[0] && v[0] != '0') ? 1 : 0;
    g_enabledState = e;
    return e != 0;
  }

  NOINSTR static ThreadState* initThread() {
    ThreadState* ts = static_cast<ThreadState*>(std::calloc(1u, sizeof(ThreadState)));
    if (!ts)
      return nullptr;
    ts->tid = GetCurrentThreadId();
    if (InterlockedCompareExchange(&g_calibInit, 1, 0) == 0) {
      QueryPerformanceFrequency(&g_qpcHz);
      QueryPerformanceCounter(&g_qpc0);
      g_tsc0 = rdtscNow();
    }
    QueryPerformanceCounter(&ts->lastDumpQpc);
    t_state = ts;
    return ts;
  }

  NOINSTR static Slot* lookup(ThreadState* ts, void* fn) {
    uint32_t h = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fn) >> 2)
               * 2654435761u;
    uint32_t i = h & (kTableSize - 1u);
    for (uint32_t probe = 0u; probe < kTableSize; probe++) {
      Slot& s = ts->table[i];
      if (s.fn == fn)
        return &s;
      if (!s.fn) {         /* claim empty slot (single-thread table: no race) */
        s.fn = fn;
        return &s;
      }
      i = (i + 1u) & (kTableSize - 1u);
    }
    return nullptr;        /* table full — drop */
  }

  /* ticks -> ns using the QPC-calibrated tsc rate since process anchor. */
  NOINSTR static double nsPerTick() {
    LARGE_INTEGER qpcNow;
    QueryPerformanceCounter(&qpcNow);
    uint64_t tscNow = rdtscNow();
    double elapsedNs = double(qpcNow.QuadPart - g_qpc0.QuadPart) * 1e9
                     / double(g_qpcHz.QuadPart ? g_qpcHz.QuadPart : 1);
    double elapsedTicks = double(tscNow - g_tsc0);
    return elapsedTicks > 0.0 ? elapsedNs / elapsedTicks : 0.0;
  }

  NOINSTR static void dump(ThreadState* ts) {
    double npt = nsPerTick();
    if (npt <= 0.0)
      return;
    uintptr_t base = reinterpret_cast<uintptr_t>(&__ImageBase);
    std::fprintf(stderr,
      "D9MTINST BEGIN tid=%lu base=0x%08lx ns_per_tick=%.6f dropped=%llu overflow=%lu\n",
      static_cast<unsigned long>(ts->tid),
      static_cast<unsigned long>(base),
      npt,
      static_cast<unsigned long long>(ts->droppedSlots),
      static_cast<unsigned long>(ts->overflow));
    for (uint32_t i = 0u; i < kTableSize; i++) {
      const Slot& s = ts->table[i];
      if (!s.fn)
        continue;
      std::fprintf(stderr, "D9MTINST F 0x%08lx %llu %llu %llu\n",
        static_cast<unsigned long>(reinterpret_cast<uintptr_t>(s.fn) - base),
        static_cast<unsigned long long>(s.calls),
        static_cast<unsigned long long>(uint64_t(double(s.selfTicks)  * npt)),
        static_cast<unsigned long long>(uint64_t(double(s.totalTicks) * npt)));
    }
    std::fprintf(stderr, "D9MTINST END tid=%lu\n",
      static_cast<unsigned long>(ts->tid));
    std::fflush(stderr);
    QueryPerformanceCounter(&ts->lastDumpQpc);
  }

}

extern "C" NOINSTR void __cyg_profile_func_enter(void* this_fn, void* call_site) {
  (void)call_site;
  if (!profEnabled())
    return;
  ThreadState* ts = t_state;
  if (!ts && !(ts = initThread()))
    return;
  if (ts->depth >= kMaxDepth) {
    ts->overflow++;
    return;
  }
  StackFrame& f = ts->stack[ts->depth++];
  f.fn         = this_fn;
  f.childTicks = 0u;
  f.t0         = rdtscNow();       /* last: exclude hook prologue */
}

extern "C" NOINSTR void __cyg_profile_func_exit(void* this_fn, void* call_site) {
  (void)call_site;
  if (!profEnabled())
    return;
  ThreadState* ts = t_state;
  if (!ts)
    return;
  if (ts->overflow) {              /* unwinding past-depth enters */
    ts->overflow--;
    return;
  }
  if (!ts->depth)
    return;

  uint64_t now = rdtscNow();       /* first: exclude hook body */

  /* Normally the top frame matches. Exceptions/longjmp can skip exit hooks;
   * resynchronize by searching down for this_fn and discarding frames above. */
  uint32_t top = ts->depth - 1u;
  if (ts->stack[top].fn != this_fn) {
    uint32_t j = top;
    while (j > 0u && ts->stack[j].fn != this_fn)
      j--;
    if (ts->stack[j].fn != this_fn)
      return;                      /* not on our stack — ignore */
    ts->depth = j + 1u;
    top = j;
  }

  StackFrame f = ts->stack[top];
  ts->depth = top;

  uint64_t incl = now - f.t0;
  uint64_t self = incl > f.childTicks ? incl - f.childTicks : 0u;

  if (Slot* s = lookup(ts, f.fn)) {
    s->calls++;
    s->totalTicks += incl;
    s->selfTicks  += self;
  } else {
    ts->droppedSlots++;
  }

  if (ts->depth) {
    ts->stack[ts->depth - 1u].childTicks += incl;
  } else if ((++ts->zeroEvents & 0x1FFFu) == 0u) {
    /* Depth-0 returns fire per CS dispatch (thousands/frame); only touch the
     * (syscall-backed) QPC clock every 8192th one, then dump every ~10s. */
    LARGE_INTEGER qpcNow;
    QueryPerformanceCounter(&qpcNow);
    if (double(qpcNow.QuadPart - ts->lastDumpQpc.QuadPart)
        > kDumpPeriodSec * double(g_qpcHz.QuadPart))
      dump(ts);
  }
}
