// blur-nx (2026-11-17, #220 of docs/cpu-ideas-196-225-1116.md; the 24-hour run): a per-thread small-block front for the
// host's C++ allocations (Switch only). The console's sampled stacks (15 ms build) put the host allocator on the frame
// chain: operator new -> _memalign_r -> _malloc_r on Game Master (Renut's D3D hooks: BeginVertices, RunCommandBuffer),
// on the job workers (BeginCommandBuffer, NewBuffer), on the render thread (the replay closures) and on the runtime's
// timer thread, every call through newlib's one recursive lock (rmutexLock: 237 sampled stacks).
//
// The global operator new / delete (the standard's replaceable forms; the aligned ones stay the library's) take
// requests up to 512 bytes from ten size classes carved out of one arena, each thread keeping its own free lists: no
// lock on the way in or out except when a list is refilled or handed back (32 blocks at a time). Everything else, and
// everything allocated before the arena exists, is the system allocator's: delete tells the two apart by the arena's
// address range, which is exact. The arena (blur_small_alloc_mb: 32 MB in the base, 0: off) is taken once, after setup
// (RenutApp::OnPostSetup: blur::small_alloc::Enable); when it is used up the system allocator answers again.
//
// Switch only: the NRO is one linked image, so every new and delete in it are these. The PC build's runtime is a DLL
// with its own operator delete: a block allocated here and released there would reach the CRT's free().
//
// What it changes: only where small C++ objects live and how fast they come and go. A block of the arena handed to
// free() instead of delete would be a bug in the caller (C++ forbids it), which the system allocator tolerated.

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <new>

// renut-ultimate (2026-11-17, port 151; OFF by default for Deadpool UE3 custom FMalloc allocator).
REXCVAR_DEFINE_INT32(blur_small_alloc_mb, 0, "Blur/Enhancements",
                     "#220 (Switch): an arena of this many MB for the host's small C++ allocations (up to 512 bytes), "
                     "free lists per thread instead of the system allocator's lock (0: off, the system allocator for all)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(__SWITCH__)

// Port 270 (sasrt-nx 1.0.1 / blur-nx): wrap _sbrk_r under malloc's recursive lock so sbrk(0) queries
// from render/memory threads cannot race malloc_extend_top and corrupt newlib's break.
#include <malloc.h>
#include <sys/reent.h>

#include <cerrno>

extern "C" {
extern char *fake_heap_start, *fake_heap_end;
}

namespace {
char* g_break = nullptr;
}  // namespace

extern "C" __attribute__((weak)) uint64_t plume_region_give_back_for_sbrk(void);

extern "C" __attribute__((used)) void* _sbrk_r(struct _reent* r, ptrdiff_t incr) {
  __malloc_lock(r);
  if (!g_break) g_break = fake_heap_start;
  char* const prev = g_break;
  char* end = fake_heap_end ? fake_heap_end : static_cast<char*>(__builtin_frame_address(0));
  if (prev && incr > 0 && prev + incr > end && fake_heap_end && plume_region_give_back_for_sbrk &&
      plume_region_give_back_for_sbrk() != 0)
    end = fake_heap_end;
  void* result;
  if (!prev || prev + incr > end) {
    r->_errno = ENOMEM;
    result = reinterpret_cast<void*>(-1);
  } else {
    g_break = prev + incr;
    result = prev;
  }
  __malloc_unlock(r);
  return result;
}

namespace {

constexpr uint32_t kClasses = 10;
constexpr uint32_t kSize[kClasses] = {16, 32, 48, 64, 96, 128, 192, 256, 384, 512};
constexpr size_t kMaxSmall = 512;
constexpr uint32_t kBatch = 32;       // blocks moved between a thread and the global lists at a time
constexpr uint32_t kKeep = 128;       // above this many free blocks of a class a thread hands half back
constexpr size_t kPiece = 64 * 1024;  // the arena is carved in pieces of this size, one class each
constexpr size_t kMaxArena = size_t(1024) << 20;

inline uint32_t ClassOf(size_t n) {  // n in 1..512
  if (n <= 64) return uint32_t((n - 1) >> 4);  // 16, 32, 48, 64
  if (n <= 128) return n <= 96 ? 4u : 5u;
  if (n <= 256) return n <= 192 ? 6u : 7u;
  return n <= 384 ? 8u : 9u;
}

struct Node {
  Node* next;
};

// The arena [g_begin, g_end): set once by Enable (g_end first, then g_begin with release).
std::atomic<uint8_t*> g_begin{nullptr};
std::atomic<uint8_t*> g_end{nullptr};
// Each piece's class, written under the global lock when the piece is carved, before any of its blocks is handed out
// (a thread that deletes a block got it through the program's own synchronisation with the one that allocated it).
uint8_t g_piece_class[kMaxArena / kPiece];

struct Global {
  std::mutex mutex;
  Node* free[kClasses] = {};
  uint8_t* carve = nullptr;          // the next uncarved piece
  uint8_t* cursor[kClasses] = {};    // the current piece of each class: its next block and its end
  uint8_t* limit[kClasses] = {};
  uint64_t exhausted = 0;
};
Global& G() {
  static Global g;  // a function-local static: ready at the first new (static constructors included), never destroyed
  return g;
}

struct Cache {
  Node* free[kClasses] = {};
  uint32_t count[kClasses] = {};
  ~Cache();
};
thread_local Cache t_cache;

// Up to kBatch blocks of class c into the thread's list; the caller holds the global lock.
void Refill(Global& g, Cache& tc, uint32_t c) {
  uint32_t got = 0;
  while (got < kBatch && g.free[c]) {
    Node* n = g.free[c];
    g.free[c] = n->next;
    n->next = tc.free[c];
    tc.free[c] = n;
    ++got;
  }
  const uint32_t size = kSize[c];
  uint8_t* const begin = g_begin.load(std::memory_order_relaxed);
  uint8_t* const end = g_end.load(std::memory_order_relaxed);
  while (got < kBatch) {
    if (!g.cursor[c] || g.cursor[c] + size > g.limit[c]) {
      if (!g.carve || g.carve + kPiece > end) {
        ++g.exhausted;
        break;
      }
      g_piece_class[size_t(g.carve - begin) / kPiece] = uint8_t(c);
      g.cursor[c] = g.carve;
      g.limit[c] = g.carve + kPiece;
      g.carve += kPiece;
    }
    Node* n = reinterpret_cast<Node*>(g.cursor[c]);
    g.cursor[c] += size;
    n->next = tc.free[c];
    tc.free[c] = n;
    ++got;
  }
  tc.count[c] += got;
}

// The thread's blocks of class c down to 'keep', back to the global list.
void Release(Cache& tc, uint32_t c, uint32_t keep) {
  Global& g = G();
  std::lock_guard lock(g.mutex);
  while (tc.count[c] > keep) {
    Node* n = tc.free[c];
    tc.free[c] = n->next;
    --tc.count[c];
    n->next = g.free[c];
    g.free[c] = n;
  }
}

Cache::~Cache() {
  for (uint32_t c = 0; c < kClasses; ++c)
    if (count[c]) Release(*this, c, 0);
}

[[gnu::always_inline]] inline void* SmallNew(size_t n) {
  if (n - 1 < kMaxSmall && g_begin.load(std::memory_order_acquire)) [[likely]] {
    const uint32_t c = ClassOf(n);
    Cache& tc = t_cache;
    if (!tc.free[c]) [[unlikely]] {
      Global& g = G();
      std::lock_guard lock(g.mutex);
      Refill(g, tc, c);
    }
    if (Node* blk = tc.free[c]) [[likely]] {
      tc.free[c] = blk->next;
      --tc.count[c];
      return blk;
    }
  }
  return nullptr;
}

[[gnu::always_inline]] inline bool SmallDelete(void* p) {
  uint8_t* const b = g_begin.load(std::memory_order_relaxed);
  uint8_t* const q = static_cast<uint8_t*>(p);
  if (!b || q < b || q >= g_end.load(std::memory_order_relaxed)) return false;
  const uint32_t c = g_piece_class[size_t(q - b) / kPiece];
  Cache& tc = t_cache;
  Node* n = static_cast<Node*>(p);
  n->next = tc.free[c];
  tc.free[c] = n;
  if (++tc.count[c] > kKeep) [[unlikely]] Release(tc, c, kKeep / 2);
  return true;
}

void* SystemNew(size_t n) {
  for (;;) {
    if (void* p = std::malloc(n ? n : 1)) return p;
    std::new_handler h = std::get_new_handler();
    if (!h) throw std::bad_alloc();
    h();
  }
}

}  // namespace

namespace blur::small_alloc {

void Enable() {
  const int mb = REXCVAR_GET(blur_small_alloc_mb);
  if (mb <= 0 || g_begin.load(std::memory_order_acquire)) return;
  const size_t bytes = std::min(size_t(mb) << 20, kMaxArena);
  void* arena = nullptr;
  if (posix_memalign(&arena, kPiece, bytes) != 0) {
    arena = nullptr;
  }
  if (!arena) {
    REXLOG_WARN("[alloc] blur_small_alloc_mb: {} MB not available; the system allocator stays", mb);
    return;
  }
  {
    Global& g = G();
    std::lock_guard lock(g.mutex);
    g.carve = static_cast<uint8_t*>(arena);
  }
  g_end.store(static_cast<uint8_t*>(arena) + bytes, std::memory_order_release);
  g_begin.store(static_cast<uint8_t*>(arena), std::memory_order_release);
  REXLOG_INFO("[alloc] #220: small C++ allocations (up to {} bytes) from a {} MB arena, free lists per thread",
              kMaxSmall, bytes >> 20);
}

uint64_t Exhausted() { return G().exhausted; }

}  // namespace blur::small_alloc

void* operator new(std::size_t n) {
  if (void* p = SmallNew(n)) return p;
  return SystemNew(n);
}
void* operator new[](std::size_t n) {
  if (void* p = SmallNew(n)) return p;
  return SystemNew(n);
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  if (void* p = SmallNew(n)) return p;
  return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  if (void* p = SmallNew(n)) return p;
  return std::malloc(n ? n : 1);
}
void operator delete(void* p) noexcept {
  if (p && !SmallDelete(p)) std::free(p);
}
void operator delete[](void* p) noexcept {
  if (p && !SmallDelete(p)) std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
  if (p && !SmallDelete(p)) std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
  if (p && !SmallDelete(p)) std::free(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
  if (p && !SmallDelete(p)) std::free(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
  if (p && !SmallDelete(p)) std::free(p);
}

#else

namespace blur::small_alloc {
void Enable() {}
uint64_t Exhausted() { return 0; }
}  // namespace blur::small_alloc

#endif
