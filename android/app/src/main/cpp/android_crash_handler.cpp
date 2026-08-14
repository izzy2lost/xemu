#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>

namespace {
constexpr const char* kCrashTag = "xemu-android";
constexpr size_t kPathMax = 512;

static char g_inline_aio_flag_path[kPathMax];

/*
 * Session marker.
 *
 * The emulator runs in its own :xemu process and is large, so Android's
 * lowmemorykiller reclaims it readily -- especially once backgrounded. When
 * that happens there is *no* record we can read: the kill is reported to
 * logcat -b system, which needs READ_LOGS (signature|privileged), so a normal
 * app can never see it. The user-visible result is "the app crashed" and a
 * debug export containing nothing at all.
 *
 * So the process reports on itself. It writes a marker on startup, keeps the
 * lifecycle state current, and deletes it on clean exit. If the marker is
 * still present at the next launch, the previous session died without
 * unwinding -- and whether a signal was recorded tells us if it was our crash
 * or the OS reclaiming us.
 */
static char g_session_marker_path[kPathMax];
static char g_session_state[32] = "starting";
static long long g_session_start_ms;
static int g_session_signal;

static int GetTid() {
  return static_cast<int>(syscall(SYS_gettid));
}

struct BacktraceState {
  void** addrs;
  int count;
  int max;
};

static _Unwind_Reason_Code UnwindCallback(struct _Unwind_Context* ctx, void* arg) {
  BacktraceState* state = static_cast<BacktraceState*>(arg);
  if (state->count >= state->max) {
    return _URC_END_OF_STACK;
  }
  uintptr_t pc = _Unwind_GetIP(ctx);
  if (pc != 0) {
    state->addrs[state->count++] = reinterpret_cast<void*>(pc);
  }
  return _URC_NO_REASON;
}

static void LogBacktrace() {
  void* addrs[64];
  BacktraceState state{addrs, 0, static_cast<int>(sizeof(addrs) / sizeof(addrs[0]))};
  _Unwind_Backtrace(UnwindCallback, &state);
  for (int i = 0; i < state.count; ++i) {
    Dl_info info;
    if (dladdr(addrs[i], &info) && info.dli_fname) {
      uintptr_t base = reinterpret_cast<uintptr_t>(info.dli_fbase);
      uintptr_t pc = reinterpret_cast<uintptr_t>(addrs[i]);
      uintptr_t rel = (base != 0 && pc >= base) ? (pc - base) : 0;
      const char* sym = info.dli_sname ? info.dli_sname : "?";
      __android_log_print(ANDROID_LOG_ERROR, kCrashTag,
                          "  #%02d pc %p %s (%s+0x%zx)",
                          i, addrs[i], info.dli_fname, sym,
                          static_cast<size_t>(rel));
    } else {
      __android_log_print(ANDROID_LOG_ERROR, kCrashTag, "  #%02d pc %p", i, addrs[i]);
    }
  }
}

static void LogAddress(const char* label, uintptr_t address) {
  void* ptr = reinterpret_cast<void*>(address);
  Dl_info info;
  memset(&info, 0, sizeof(info));
  if (address != 0 && dladdr(ptr, &info) && info.dli_fname) {
    uintptr_t base = reinterpret_cast<uintptr_t>(info.dli_fbase);
    uintptr_t relative = (base != 0 && address >= base) ? address - base : 0;
    const char* symbol = info.dli_sname ? info.dli_sname : "?";
    __android_log_print(ANDROID_LOG_ERROR, kCrashTag,
                        "  %s pc=%p module=%s symbol=%s relative=0x%zx",
                        label, ptr, info.dli_fname, symbol,
                        static_cast<size_t>(relative));
  } else {
    __android_log_print(ANDROID_LOG_ERROR, kCrashTag,
                        "  %s pc=%p (module unresolved)", label, ptr);
  }
}

static void LogInterruptedContext(void* raw_context) {
#if defined(__aarch64__)
  if (!raw_context) {
    __android_log_print(ANDROID_LOG_ERROR, kCrashTag,
                        "  interrupted context unavailable");
    return;
  }

  const ucontext_t* context = static_cast<const ucontext_t*>(raw_context);
  const mcontext_t& machine = context->uc_mcontext;
  const uintptr_t pc = static_cast<uintptr_t>(machine.pc);
  const uintptr_t sp = static_cast<uintptr_t>(machine.sp);
  const uintptr_t fp = static_cast<uintptr_t>(machine.regs[29]);
  const uintptr_t lr = static_cast<uintptr_t>(machine.regs[30]);

  LogAddress("fault", pc);
  LogAddress("link ", lr);
  __android_log_print(ANDROID_LOG_ERROR, kCrashTag,
                      "  context sp=%p fp=%p pstate=0x%llx",
                      reinterpret_cast<void*>(sp),
                      reinterpret_cast<void*>(fp),
                      static_cast<unsigned long long>(machine.pstate));
  for (int i = 0; i < 31; ++i) {
    __android_log_print(
        ANDROID_LOG_ERROR, kCrashTag, "  register x%02d=%p", i,
        reinterpret_cast<void*>(static_cast<uintptr_t>(machine.regs[i])));
  }
#else
  (void)raw_context;
  __android_log_print(ANDROID_LOG_ERROR, kCrashTag,
                      "  interrupted register logging unsupported on this ABI");
#endif
}

static long long NowMs() {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    return 0;
  }
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * Async-signal-safe enough to call from the crash handler: no malloc, no
 * stdio, single write() of a preformatted buffer.
 */
static void WriteSessionMarker() {
  if (g_session_marker_path[0] == '\0') {
    return;
  }

  char buf[512];
  int n = snprintf(buf, sizeof(buf),
                   "version=1\n"
                   "pid=%d\n"
                   "start_ms=%lld\n"
                   "update_ms=%lld\n"
                   "state=%s\n"
                   "signal=%d\n",
                   (int)getpid(), g_session_start_ms, NowMs(),
                   g_session_state, g_session_signal);
  if (n <= 0) {
    return;
  }

  int fd = open(g_session_marker_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return;
  }
  ssize_t ignored = write(fd, buf, (size_t)n);
  (void)ignored;
  close(fd);
}

static void MarkInlineAioRequired() {
  if (g_inline_aio_flag_path[0] == '\0') {
    return;
  }

  int fd = open(g_inline_aio_flag_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return;
  }

  static const char kValue[] = "1\n";
  ssize_t ignored = write(fd, kValue, sizeof(kValue) - 1);
  (void)ignored;
  close(fd);
}

static void CrashHandler(int sig, siginfo_t* info, void* ucontext) {
  if (sig == SIGILL) {
    MarkInlineAioRequired();
  }
  /* Distinguishes "we crashed" from "the OS reclaimed us" on next launch. */
  g_session_signal = sig;
  WriteSessionMarker();
  __android_log_print(ANDROID_LOG_ERROR, kCrashTag,
                      "Caught signal %d code=%d fault_addr=%p in tid %d",
                      sig, info ? info->si_code : 0,
                      info ? info->si_addr : nullptr, GetTid());
  LogInterruptedContext(ucontext);
  LogBacktrace();
  signal(sig, SIG_DFL);
  raise(sig);
}

static void InstallCrashHandlers() {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = CrashHandler;
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
  sigaction(SIGABRT, &sa, nullptr);
  sigaction(SIGILL, &sa, nullptr);
  sigaction(SIGSEGV, &sa, nullptr);
}
}  // namespace

extern "C" void xemu_android_set_inline_aio_crash_flag_path(const char* path) {
  if (!path) {
    g_inline_aio_flag_path[0] = '\0';
    return;
  }

  size_t len = strnlen(path, sizeof(g_inline_aio_flag_path) - 1);
  memcpy(g_inline_aio_flag_path, path, len);
  g_inline_aio_flag_path[len] = '\0';
}

extern "C" void xemu_android_session_begin(const char* path) {
  if (!path) {
    g_session_marker_path[0] = '\0';
    return;
  }
  size_t len = strnlen(path, sizeof(g_session_marker_path) - 1);
  memcpy(g_session_marker_path, path, len);
  g_session_marker_path[len] = '\0';

  g_session_start_ms = NowMs();
  g_session_signal = 0;
  snprintf(g_session_state, sizeof(g_session_state), "%s", "foreground");
  WriteSessionMarker();
}

extern "C" void xemu_android_session_set_state(const char* state) {
  if (!state || g_session_marker_path[0] == '\0') {
    return;
  }
  snprintf(g_session_state, sizeof(g_session_state), "%s", state);
  WriteSessionMarker();
}

extern "C" void xemu_android_session_end(void) {
  if (g_session_marker_path[0] == '\0') {
    return;
  }
  unlink(g_session_marker_path);
  g_session_marker_path[0] = '\0';
}

__attribute__((constructor)) static void InstallCrashHandlersOnLoad() {
  InstallCrashHandlers();
}
