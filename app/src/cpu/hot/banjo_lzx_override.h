#pragma once

// Custom native libmspack override for Xbox 360 XMemDecompress LZX chunk decode (sub_82D8D0D8).
// Verified against the guest decoder over the first 4 chunks after boot before switching to
// steady-state native execution.

#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/lzx.h>

#include "cpu/banjo_enhancements.h"
#include "cpu/hot/banjo_hand_natives.h"

REXCVAR_DECLARE(bool, deadpool_native_lzx);

namespace banjo::hand::n_82D8D0D8 {

struct ThreadLzxState {
  LzxXmemStream* stream = nullptr;
  uint32_t active_guest_state = 0;
  uint32_t expected_chunk_idx = 0;
  uint32_t window_size = 0;
  bool stream_native = false;

  ~ThreadLzxState() {
    if (stream) {
      lzx_xmem_destroy(stream);
      stream = nullptr;
    }
  }
};

inline std::atomic<bool> g_lzx_disabled{false};
inline std::atomic<bool> g_logged_active{false};

[[gnu::always_inline]] inline uint32_t Load32(const uint8_t* base, uint32_t addr) noexcept {
  uint32_t v;
  std::memcpy(&v, ::banjo::hot::Raw(const_cast<uint8_t*>(base), addr), 4);
  return __builtin_bswap32(v);
}

inline bool TryRun(PPCContext& __restrict ctx, uint8_t* base, [[maybe_unused]] void (*ref_fn)(PPCContext&, uint8_t*)) {
  if (!bool(REXCVAR_GET(deadpool_native_lzx)) || g_lzx_disabled.load(std::memory_order_relaxed)) [[unlikely]] {
    return false;
  }

  const uint32_t state_ea = ctx.r3.u32;
  const uint32_t uncomp_len = ctx.r4.u32;
  if (uncomp_len == 0u || uncomp_len > 0x8000u || !detail::Straight(state_ea, 12304u)) [[unlikely]] {
    return false;
  }

  const uint32_t window_size = Load32(base, state_ea + 4u);
  if (window_size < 0x8000u || window_size > 0x200000u || (window_size & (window_size - 1u)) != 0u) [[unlikely]] {
    return false;
  }

  const uint32_t pre_read = (Load32(base, state_ea + 11984u) == 3u) ? 0u : 4u;
  const uint32_t cur_src_ea = Load32(base, state_ea + 11012u);
  const uint32_t src_end_ea = Load32(base, state_ea + 11016u);
  if (cur_src_ea < pre_read || src_end_ea < cur_src_ea + (4u - pre_read)) [[unlikely]] {
    return false;
  }
  const uint32_t src_ea = cur_src_ea - pre_read;
  const uint32_t comp_size = src_end_ea - src_ea - 4u;
  if (comp_size == 0u || comp_size > 0x40000u) [[unlikely]] {
    return false;
  }

  const uint32_t dst_ea = Load32(base, state_ea + 11020u);
  const uint32_t chunk_idx = Load32(base, state_ea + 11972u);
  if (!detail::Straight(src_ea, comp_size) || !detail::Straight(dst_ea, uncomp_len)) [[unlikely]] {
    return false;
  }
  if (audio_filter::Overlap(src_ea, comp_size, dst_ea, uncomp_len) ||
      audio_filter::Overlap(state_ea, 12304u, dst_ea, uncomp_len)) [[unlikely]] {
    return false;
  }

  thread_local ThreadLzxState tl;
  bool reset = false;
  if (chunk_idx == 0u) {
    reset = true;
    tl.stream_native = true;
    tl.active_guest_state = state_ea;
    tl.expected_chunk_idx = 0u;
  } else {
    if (!tl.stream_native || tl.active_guest_state != state_ea || tl.expected_chunk_idx != chunk_idx ||
        tl.window_size != window_size || !tl.stream) {
      tl.stream_native = false;
      return false;
    }
  }

  if (!tl.stream || tl.window_size != window_size) {
    if (tl.stream) {
      lzx_xmem_destroy(tl.stream);
      tl.stream = nullptr;
    }
    tl.stream = lzx_xmem_create(window_size);
    tl.window_size = tl.stream ? window_size : 0u;
    reset = true;
    if (!tl.stream) {
      tl.stream_native = false;
      return false;
    }
  }

  if (!g_logged_active.exchange(true, std::memory_order_relaxed)) [[unlikely]] {
    REXLOG_INFO("[hot] native LZX XMemDecompress (deadpool_native_lzx): ON (page-boundary safe)");
  }

  const uint8_t* p_src = ::banjo::hot::Raw(base, src_ea);
  uint8_t* p_dst = ::banjo::hot::Raw(base, dst_ea);
  const int err = lzx_xmem_decompress_chunk(tl.stream, p_src, comp_size, p_dst, uncomp_len, reset);
  if (err != 0) [[unlikely]] {
    tl.stream_native = false;
    // If decompress fails at chunk 0, only fall back to guest decoder if source does not end near a 64 KB page edge
    // (guest PPC lwz overreads 2-3 bytes past cur_src and faults on unmapped 64 KB boundaries like 0x44800000).
    const bool near_page_edge = ((src_ea + comp_size + 15u) >> 16) != ((src_ea + comp_size - 1u) >> 16);
    if (chunk_idx == 0u && !near_page_edge) {
      return false;
    }
    g_lzx_disabled.store(true, std::memory_order_relaxed);
    REXLOG_WARN("[hot] deadpool_native_lzx error {} (chunk={} uncomp={} comp={} win={} edge={})",
                err, chunk_idx, uncomp_len, comp_size, window_size, near_page_edge);
    ctx.r3.s64 = -1;
    return true;
  }

  ::banjo::hot::Card(base, dst_ea, uncomp_len);
  tl.expected_chunk_idx = chunk_idx + 1u;
  ctx.r3.u64 = uncomp_len;
  return true;
}

}  // namespace banjo::hand::n_82D8D0D8

namespace banjo::hot::n_82D8D0D8 {
void Native(PPCContext& ctx, uint8_t* base);
}

namespace banjo::hot {

template <>
[[gnu::always_inline]] inline bool TryCustomOverride<&n_82D8D0D8::Native>(PPCContext& ctx, uint8_t* base) {
  return ::banjo::hand::n_82D8D0D8::TryRun(ctx, base, &n_82D8D0D8::Native);
}

}  // namespace banjo::hot
