#pragma once

// Custom C++ / NEON overrides evaluated ahead of localized guest copies in BANJO_HOT_HOOK_EX.
// Keeps app/generated/hot/n_*.h untouched while intercepting both direct and virtual-table callers
// in both BANJO_HOT_IN_PLACE (Switch) and hooked (PC) builds.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

#include "cpu/banjo_enhancements.h"
#include "cpu/hot/banjo_hand_natives.h"

REXCVAR_DECLARE(bool, banjo_native_fmod_reverb);
REXCVAR_DECLARE(bool, banjo_native_fmod_reverb_verify);

REX_EXTERN(sub_82D5F7A8);

namespace banjo::hand::n_82D60CB8 {

struct ReverbBuffers {
  uint32_t self;
  uint32_t in_addr;
  uint32_t out_addr;
  uint32_t mono_addr;
  uint32_t vec_a_addr;
  uint32_t vec_b_addr;
  uint32_t early_buf_addr;
  uint32_t er_buf_addr;
  uint32_t ap0_addr;
  uint32_t ap1_addr;
  uint32_t tank_buf_addr[8];
  uint32_t ch;
  uint32_t nsamples;
  uint32_t io_bytes;
  uint32_t mono_bytes;
  uint32_t early_mask;
  uint32_t er_mask;
  uint32_t ap0_mask;
  uint32_t ap1_mask;
  uint32_t tank_mask[8];
};

[[gnu::always_inline]] inline bool ValidMask(uint32_t mask) {
  return mask <= 0x0FFFFFFFu && (mask & (mask + 1u)) == 0u;
}

[[gnu::always_inline]] inline uint32_t Load32P(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}

inline bool Validate(uint8_t* base, const PPCContext& ctx, ReverbBuffers& b) {
  b.self = ctx.r3.u32;
  b.in_addr = ctx.r4.u32;
  b.out_addr = ctx.r5.u32;
  const int32_t ch_s = ctx.r6.s32;
  b.nsamples = ctx.r7.u32;
  if (b.nsamples == 0u || ch_s <= 0 || ch_s > 8) [[unlikely]] return false;
  b.ch = uint32_t(ch_s);
  if ((b.ch == 6u || b.ch == 8u) && ((b.in_addr & 15u) != 0u || (b.nsamples & 3u) != 0u)) [[unlikely]] return false;
  if (b.nsamples > 0x0FFFFFFFu / b.ch) [[unlikely]] return false;
  b.io_bytes = b.nsamples * b.ch * 4u;
  b.mono_bytes = b.nsamples * 4u;

  if (!detail::Straight(b.self, 1452u) || !detail::Straight(b.in_addr, b.io_bytes) ||
      !detail::Straight(b.out_addr, b.io_bytes)) [[unlikely]]
    return false;
  if (b.in_addr != b.out_addr && audio_filter::Overlap(b.in_addr, b.io_bytes, b.out_addr, b.io_bytes)) [[unlikely]]
    return false;

  const uint8_t* ps = ::banjo::hot::Raw(base, b.self);
  if (int32_t(Load32P(ps + 720u)) != 8) [[unlikely]] return false;

  b.mono_addr = Load32P(ps + 4u);
  b.vec_a_addr = Load32P(ps + 500u);
  b.vec_b_addr = Load32P(ps + 504u);
  b.early_buf_addr = Load32P(ps + 1084u);
  b.early_mask = Load32P(ps + 1216u);
  b.er_buf_addr = Load32P(ps + 1296u);
  b.er_mask = Load32P(ps + 1360u);
  b.ap0_addr = Load32P(ps + 1396u);
  b.ap0_mask = Load32P(ps + 1428u);
  b.ap1_addr = Load32P(ps + 1400u);
  b.ap1_mask = Load32P(ps + 1432u);
  if ((b.vec_a_addr & 15u) != 0u || (b.vec_b_addr & 15u) != 0u) [[unlikely]] return false;
  if (!ValidMask(b.early_mask) || !ValidMask(b.er_mask) || !ValidMask(b.ap0_mask) || !ValidMask(b.ap1_mask))
      [[unlikely]]
    return false;
  if (!detail::Straight(b.mono_addr, b.mono_bytes) || !detail::Straight(b.vec_a_addr, 32u) ||
      !detail::Straight(b.vec_b_addr, 96u) || !detail::Straight(b.early_buf_addr, (b.early_mask + 1u) * 4u) ||
      !detail::Straight(b.er_buf_addr, (b.er_mask + 1u) * 4u) ||
      !detail::Straight(b.ap0_addr, (b.ap0_mask + 1u) * 4u) ||
      !detail::Straight(b.ap1_addr, (b.ap1_mask + 1u) * 4u)) [[unlikely]]
    return false;

  for (uint32_t t = 0; t < 8u; ++t) {
    b.tank_buf_addr[t] = Load32P(ps + 724u + 4u * t);
    b.tank_mask[t] = Load32P(ps + 924u + 4u * t);
    if (!ValidMask(b.tank_mask[t]) || !detail::Straight(b.tank_buf_addr[t], (b.tank_mask[t] + 1u) * 4u)) [[unlikely]]
      return false;
  }

  // Ensure scratch/delay lines do not alias self, in, out, or mono.
  const struct {
    uint32_t addr;
    uint32_t bytes;
  } fixed[] = {
      {b.self, 1452u},
      {b.mono_addr, b.mono_bytes},
      {b.vec_a_addr, 32u},
      {b.vec_b_addr, 96u},
      {b.early_buf_addr, (b.early_mask + 1u) * 4u},
      {b.er_buf_addr, (b.er_mask + 1u) * 4u},
      {b.ap0_addr, (b.ap0_mask + 1u) * 4u},
      {b.ap1_addr, (b.ap1_mask + 1u) * 4u},
  };
  for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); ++i) {
    if (audio_filter::Overlap(fixed[i].addr, fixed[i].bytes, b.in_addr, b.io_bytes) ||
        audio_filter::Overlap(fixed[i].addr, fixed[i].bytes, b.out_addr, b.io_bytes)) [[unlikely]]
      return false;
    for (size_t j = i + 1; j < sizeof(fixed) / sizeof(fixed[0]); ++j) {
      if (audio_filter::Overlap(fixed[i].addr, fixed[i].bytes, fixed[j].addr, fixed[j].bytes)) [[unlikely]]
        return false;
    }
    for (uint32_t t = 0; t < 8u; ++t) {
      if (audio_filter::Overlap(fixed[i].addr, fixed[i].bytes, b.tank_buf_addr[t], (b.tank_mask[t] + 1u) * 4u))
          [[unlikely]]
        return false;
    }
  }
  for (uint32_t t = 0; t < 8u; ++t) {
    const uint32_t tb = (b.tank_mask[t] + 1u) * 4u;
    if (audio_filter::Overlap(b.tank_buf_addr[t], tb, b.in_addr, b.io_bytes) ||
        audio_filter::Overlap(b.tank_buf_addr[t], tb, b.out_addr, b.io_bytes)) [[unlikely]]
      return false;
    for (uint32_t u = t + 1; u < 8u; ++u) {
      if (audio_filter::Overlap(b.tank_buf_addr[t], tb, b.tank_buf_addr[u], (b.tank_mask[u] + 1u) * 4u)) [[unlikely]]
        return false;
    }
  }
  return true;
}

[[gnu::always_inline]] inline void AddAntiDenormalDC(uint8_t* p_in, uint32_t ch, uint32_t nsamples) {
  constexpr uint32_t kDcBits = 0x26901D7Du;
  float dc_pos;
  std::memcpy(&dc_pos, &kDcBits, 4);
  if (ch == 6u) {
    const uint32_t quads = nsamples >> 2u;
#if defined(__aarch64__)
    const float32x4_t vp = vdupq_n_f32(dc_pos);
    const float32x4_t vn = vdupq_n_f32(-dc_pos);
    const float32x4_t vm = {dc_pos, dc_pos, -dc_pos, -dc_pos};
    for (uint32_t q = 0; q < quads; ++q, p_in += 96u) {
      audio_dsp::StoreF4BE(p_in + 0u, vaddq_f32(audio_dsp::LoadF4BE(p_in + 0u), vp));
      audio_dsp::StoreF4BE(p_in + 16u, vaddq_f32(audio_dsp::LoadF4BE(p_in + 16u), vm));
      audio_dsp::StoreF4BE(p_in + 32u, vaddq_f32(audio_dsp::LoadF4BE(p_in + 32u), vn));
      audio_dsp::StoreF4BE(p_in + 48u, vaddq_f32(audio_dsp::LoadF4BE(p_in + 48u), vp));
      audio_dsp::StoreF4BE(p_in + 64u, vaddq_f32(audio_dsp::LoadF4BE(p_in + 64u), vm));
      audio_dsp::StoreF4BE(p_in + 80u, vaddq_f32(audio_dsp::LoadF4BE(p_in + 80u), vn));
    }
#else
    for (uint32_t s = 0; s < quads * 4u; ++s, p_in += 24u) {
      const float d = (s & 1u) ? -dc_pos : dc_pos;
      for (uint32_t c = 0; c < 6u; ++c) audio_filter::StoreF(p_in + 4u * c, audio_filter::LoadF(p_in + 4u * c) + d);
    }
#endif
  } else if (ch == 8u) {
    const uint32_t quads = nsamples >> 2u;
#if defined(__aarch64__)
    const float32x4_t vp = vdupq_n_f32(dc_pos);
    const float32x4_t vn = vdupq_n_f32(-dc_pos);
    for (uint32_t q = 0; q < quads; ++q, p_in += 128u) {
      audio_dsp::StoreF4BE(p_in + 0u, vaddq_f32(audio_dsp::LoadF4BE(p_in + 0u), vp));
      audio_dsp::StoreF4BE(p_in + 16u, vaddq_f32(audio_dsp::LoadF4BE(p_in + 16u), vp));
      audio_dsp::StoreF4BE(p_in + 32u, vaddq_f32(audio_dsp::LoadF4BE(p_in + 32u), vn));
      audio_dsp::StoreF4BE(p_in + 48u, vaddq_f32(audio_dsp::LoadF4BE(p_in + 48u), vn));
      audio_dsp::StoreF4BE(p_in + 64u, vaddq_f32(audio_dsp::LoadF4BE(p_in + 64u), vp));
      audio_dsp::StoreF4BE(p_in + 80u, vaddq_f32(audio_dsp::LoadF4BE(p_in + 80u), vp));
      audio_dsp::StoreF4BE(p_in + 96u, vaddq_f32(audio_dsp::LoadF4BE(p_in + 96u), vn));
      audio_dsp::StoreF4BE(p_in + 112u, vaddq_f32(audio_dsp::LoadF4BE(p_in + 112u), vn));
    }
#else
    for (uint32_t s = 0; s < quads * 4u; ++s, p_in += 32u) {
      const float d = (s & 1u) ? -dc_pos : dc_pos;
      for (uint32_t c = 0; c < 8u; ++c) audio_filter::StoreF(p_in + 4u * c, audio_filter::LoadF(p_in + 4u * c) + d);
    }
#endif
  } else {
    for (uint32_t c = 0; c < ch; ++c) {
      for (uint32_t s = 0; s < nsamples; ++s) {
        uint8_t* pc = p_in + 4u * (s * ch + c);
        const float v = audio_filter::LoadF(pc);
        audio_filter::StoreF(pc, (s & 1u) ? (v - dc_pos) : (v + dc_pos));
      }
    }
  }
}

[[gnu::always_inline]] inline void DownmixToMono(const uint8_t* p_in, uint8_t* p_mono, uint32_t ch, uint32_t nsamples) {
  if (ch == 1u) {
    std::memcpy(p_mono, p_in, size_t(nsamples) * 4u);
    return;
  }
  if (ch == 6u) {
    const uint32_t quads = nsamples >> 2u;
#if defined(__aarch64__)
    const float32x4_t vmask_hi = {1.0f, 1.0f, 0.0f, 0.0f};
    const float32x4_t vmask_lo = {0.0f, 0.0f, 1.0f, 1.0f};
    for (uint32_t q = 0; q < quads; ++q, p_in += 96u, p_mono += 16u) {
      const float32x4_t a0 = audio_dsp::LoadF4BE(p_in + 0u);
      const float32x4_t a1 = audio_dsp::LoadF4BE(p_in + 16u);
      const float32x4_t a2 = audio_dsp::LoadF4BE(p_in + 32u);
      const float32x4_t b0 = audio_dsp::LoadF4BE(p_in + 48u);
      const float32x4_t b1 = audio_dsp::LoadF4BE(p_in + 64u);
      const float32x4_t b2 = audio_dsp::LoadF4BE(p_in + 80u);
      const float s0 = vaddvq_f32(a0) + vaddvq_f32(vmulq_f32(a1, vmask_hi));
      const float s1 = vaddvq_f32(vmulq_f32(a1, vmask_lo)) + vaddvq_f32(a2);
      const float s2 = vaddvq_f32(b0) + vaddvq_f32(vmulq_f32(b1, vmask_hi));
      const float s3 = vaddvq_f32(vmulq_f32(b1, vmask_lo)) + vaddvq_f32(b2);
      const float32x4_t m = {s0, s1, s2, s3};
      audio_dsp::StoreF4BE(p_mono, m);
    }
#else
    for (uint32_t s = 0; s < quads * 4u; ++s, p_in += 24u, p_mono += 4u) {
      float sum03 = 0.0f, sum45 = 0.0f;
      for (uint32_t c = 0; c < 4u; ++c) sum03 += audio_filter::LoadF(p_in + 4u * c);
      sum45 = audio_filter::LoadF(p_in + 16u) + audio_filter::LoadF(p_in + 20u);
      audio_filter::StoreF(p_mono, sum03 + sum45);
    }
#endif
    return;
  }
  if (ch == 8u) {
    const uint32_t quads = nsamples >> 2u;
#if defined(__aarch64__)
    for (uint32_t q = 0; q < quads; ++q, p_in += 128u, p_mono += 16u) {
      const float s0 = vaddvq_f32(audio_dsp::LoadF4BE(p_in + 0u)) + vaddvq_f32(audio_dsp::LoadF4BE(p_in + 16u));
      const float s1 = vaddvq_f32(audio_dsp::LoadF4BE(p_in + 32u)) + vaddvq_f32(audio_dsp::LoadF4BE(p_in + 48u));
      const float s2 = vaddvq_f32(audio_dsp::LoadF4BE(p_in + 64u)) + vaddvq_f32(audio_dsp::LoadF4BE(p_in + 80u));
      const float s3 = vaddvq_f32(audio_dsp::LoadF4BE(p_in + 96u)) + vaddvq_f32(audio_dsp::LoadF4BE(p_in + 112u));
      const float32x4_t m = {s0, s1, s2, s3};
      audio_dsp::StoreF4BE(p_mono, m);
    }
#else
    for (uint32_t s = 0; s < quads * 4u; ++s, p_in += 32u, p_mono += 4u) {
      float sum03 = 0.0f, sum47 = 0.0f;
      for (uint32_t c = 0; c < 4u; ++c) sum03 += audio_filter::LoadF(p_in + 4u * c);
      for (uint32_t c = 4u; c < 8u; ++c) sum47 += audio_filter::LoadF(p_in + 4u * c);
      audio_filter::StoreF(p_mono, sum03 + sum47);
    }
#endif
    return;
  }
  for (uint32_t s = 0; s < nsamples; ++s, p_in += 4u * ch, p_mono += 4u) {
    float acc = 0.0f;
    for (uint32_t c = 0; c < ch; ++c) acc += audio_filter::LoadF(p_in + 4u * c);
    audio_filter::StoreF(p_mono, acc);
  }
}

inline bool TryRun(PPCContext& __restrict ctx, uint8_t* base) {
  ReverbBuffers b{};
  if (!Validate(base, ctx, b)) [[unlikely]] return false;

  const uint16_t spk_mask = uint16_t(ctx.r10.u16);
  const float dry_gain = float(ctx.f2.f64);

  ::banjo::hot::Card(base, b.in_addr, b.io_bytes);
  ::banjo::hot::Card(base, b.out_addr, b.io_bytes);
  ::banjo::hot::Card(base, b.mono_addr, b.mono_bytes);
  ::banjo::hot::Card(base, b.self, 1452u);
  ::banjo::hot::Card(base, b.vec_a_addr, 32u);
  ::banjo::hot::Card(base, b.vec_b_addr, 96u);
  ::banjo::hot::Card(base, b.early_buf_addr, (b.early_mask + 1u) * 4u);
  ::banjo::hot::Card(base, b.er_buf_addr, (b.er_mask + 1u) * 4u);
  ::banjo::hot::Card(base, b.ap0_addr, (b.ap0_mask + 1u) * 4u);
  ::banjo::hot::Card(base, b.ap1_addr, (b.ap1_mask + 1u) * 4u);
  for (uint32_t t = 0; t < 8u; ++t) ::banjo::hot::Card(base, b.tank_buf_addr[t], (b.tank_mask[t] + 1u) * 4u);

  uint8_t* ps = ::banjo::hot::Raw(base, b.self);
  uint8_t* p_in = ::banjo::hot::Raw(base, b.in_addr);
  uint8_t* p_out = ::banjo::hot::Raw(base, b.out_addr);
  uint8_t* p_mono = ::banjo::hot::Raw(base, b.mono_addr);
  uint8_t* p_vec_a = ::banjo::hot::Raw(base, b.vec_a_addr);
  uint8_t* p_vec_b = ::banjo::hot::Raw(base, b.vec_b_addr);
  uint8_t* p_early = ::banjo::hot::Raw(base, b.early_buf_addr);
  uint8_t* p_er = ::banjo::hot::Raw(base, b.er_buf_addr);
  uint8_t* p_ap0 = ::banjo::hot::Raw(base, b.ap0_addr);
  uint8_t* p_ap1 = ::banjo::hot::Raw(base, b.ap1_addr);
  uint8_t* p_tank[8];
  for (uint32_t t = 0; t < 8u; ++t) p_tank[t] = ::banjo::hot::Raw(base, b.tank_buf_addr[t]);

  // Stage 1 & 2: Anti-denormal DC flip-flop + channel-to-mono downmix.
  AddAntiDenormalDC(p_in, b.ch, b.nsamples);
  DownmixToMono(p_in, p_mono, b.ch, b.nsamples);

  // Update smoothed parameter ramps via guest's parameter updater
  PPCContext fmod_ctx = ctx;
  fmod_ctx.r3.u32 = b.self;
  fmod_ctx.r4.u32 = b.nsamples;
  fmod_ctx.r5.u32 = b.ch;
  fmod_ctx.r6.u32 = b.in_addr;
  sub_82D5F7A8(fmod_ctx, base);

  // Stage 3a: Synchronize clean pre-loop state and flags (mirrors loc_82D60F50..loc_82D61194).
  ctx.fpscr.disableFlushMode();
  uint32_t early_w = Load32P(ps + 1220u);
  uint32_t early_r[9];
  for (uint32_t i = 0; i < 9u; ++i) {
    ps[1200u + i] = 0u;
    early_r[i] = (Load32P(ps + 1128u + 4u * i) + early_w - 1u) & b.early_mask;
    audio_filter::Store32(ps + 1224u + 4u * i, early_r[i]);
  }

  uint32_t late_w[8], late_r_a[8], late_r_b[8];
  float fb_gain[8], lp_g[8], lp_omg[8];
  for (uint32_t t = 0; t < 8u; ++t) {
    late_w[t] = Load32P(ps + 956u + 4u * t);
    late_r_a[t] = (Load32P(ps + 788u + 4u * t) + late_w[t] - 1u) & b.tank_mask[t];
    late_r_b[t] = (Load32P(ps + 852u + 4u * t) + late_w[t] - 1u) & b.tank_mask[t];
    audio_filter::Store32(ps + 988u + 4u * t, late_r_a[t]);
    audio_filter::Store32(ps + 1052u + 4u * t, late_r_b[t]);
    ps[916u + t] = 0u;
    ps[108u + t] = 0u;
    fb_gain[t] = audio_filter::LoadF(ps + 508u + 4u * t);
    audio_filter::StoreF(ps + 540u + 4u * t, fb_gain[t]);
    audio_filter::StoreF(ps + 604u + 4u * t, fb_gain[t]);
    lp_g[t] = audio_filter::LoadF(ps + 76u + 4u * t);
    lp_omg[t] = 1.0f - lp_g[t];
    audio_filter::StoreF(ps + 116u + 4u * t, lp_g[t]);
    audio_filter::StoreF(ps + 148u + 4u * t, lp_omg[t]);
  }

  ps[664u] = 0u;
  const float hf_gain = audio_filter::LoadF(ps + 660u);
  audio_filter::StoreF(ps + 668u, hf_gain);

  ps[680u] = 0u;
  const float late_gain = audio_filter::LoadF(ps + 656u);
  audio_filter::StoreF(ps + 684u, late_gain);

  ps[20u] = 0u;
  const float in_lpf_g = audio_filter::LoadF(ps + 16u);
  const float in_lpf_omg = 1.0f - in_lpf_g;
  audio_filter::StoreF(ps + 24u, in_lpf_g);

  ps[640u] = 0u;
  const float early_gain = audio_filter::LoadF(ps + 636u);
  audio_filter::StoreF(ps + 644u, early_gain);

  ps[700u] = 0u;
  const float diff_gain = audio_filter::LoadF(ps + 696u);
  audio_filter::StoreF(ps + 704u, diff_gain);

  float lp1 = audio_filter::LoadF(ps + 36u);
  float lp2 = audio_filter::LoadF(ps + 40u);
  const float bq_b0 = audio_filter::LoadF(ps + 44u);
  const float bq_b1 = audio_filter::LoadF(ps + 48u);
  const float bq_b2 = audio_filter::LoadF(ps + 52u);
  const float bq_a1 = audio_filter::LoadF(ps + 56u);
  const float bq_a2 = audio_filter::LoadF(ps + 60u);
  const bool use_bq = (audio_filter::LoadF(ps + 64u) != 0.0f);
  float bq_s0 = audio_filter::LoadF(ps + 68u);
  float bq_s1 = audio_filter::LoadF(ps + 72u);

  const float ap_k = audio_filter::LoadF(ps + 716u);
  uint32_t ap0_r = Load32P(ps + 1444u);
  uint32_t ap0_w = Load32P(ps + 1436u);
  uint32_t ap1_r = Load32P(ps + 1448u);
  uint32_t ap1_w = Load32P(ps + 1440u);

  uint32_t er_w = Load32P(ps + 1364u);
  uint32_t er_r[7];
  for (uint32_t i = 0; i < 7u; ++i) er_r[i] = Load32P(ps + 1368u + 4u * i);

#if defined(__aarch64__)
  float32x4_t va0 = audio_dsp::LoadF4BE(p_vec_a + 0u);
  float32x4_t va1 = audio_dsp::LoadF4BE(p_vec_a + 16u);
  const float32x4_t vlp_g0 = {lp_g[0], lp_g[1], lp_g[2], lp_g[3]};
  const float32x4_t vlp_g1 = {lp_g[4], lp_g[5], lp_g[6], lp_g[7]};
  const float32x4_t vlp_o0 = {lp_omg[0], lp_omg[1], lp_omg[2], lp_omg[3]};
  const float32x4_t vlp_o1 = {lp_omg[4], lp_omg[5], lp_omg[6], lp_omg[7]};
  const float32x4_t vfb0 = {fb_gain[0], fb_gain[1], fb_gain[2], fb_gain[3]};
  const float32x4_t vfb1 = {fb_gain[4], fb_gain[5], fb_gain[6], fb_gain[7]};
  const float32x4_t vhf = vdupq_n_f32(hf_gain);
  const float32x4_t vdiff = vdupq_n_f32(diff_gain);
  const float32x4_t vgp = {early_gain, -early_gain, early_gain, -early_gain};
  const float32x4_t vgq = {early_gain, early_gain, -early_gain, -early_gain};
  const float32x4_t veg = vdupq_n_f32(early_gain);
  const float32x4_t vneg_eg = vdupq_n_f32(-early_gain);
  float32x4_t last_u0p{}, last_u1p{}, last_u0pp{}, last_u1pp{}, last_w0{}, last_w1{};
#else
  float va[8];
  for (uint32_t t = 0; t < 8u; ++t) va[t] = audio_filter::LoadF(p_vec_a + 4u * t);
  float last_u0p[4]{}, last_u1p[4]{}, last_u0pp[4]{}, last_u1pp[4]{}, last_w0[4]{}, last_w1[4]{};
#endif

  const uint32_t per_ch_taps = 8u / b.ch;
  const uint32_t stride_bytes = b.ch * 4u;

  for (uint32_t s = 0; s < b.nsamples; ++s) {
    // 1) Input 2-stage LPF + optional biquad.
    const float xm = audio_filter::LoadF(p_mono + 4u * s);
    lp1 = std::fmaf(lp1, in_lpf_omg, in_lpf_g * xm);
    lp2 = std::fmaf(lp2, in_lpf_omg, in_lpf_g * lp1);
    float sig = lp2;
    if (use_bq) {
      const float prev_s1 = bq_s1;
      const float y = std::fmaf(bq_b0, lp2, bq_s0);
      const float t1 = std::fmaf(bq_b1, lp2, bq_a1 * y);
      bq_s1 = std::fmaf(bq_b2, lp2, bq_a2 * y);
      bq_s0 = t1 + prev_s1;
      sig = y;
    }

    // 2) Push into early reflection buffer; read ER feed tap.
    early_w = (early_w - 1u) & b.early_mask;
    audio_filter::StoreF(p_early + 4u * early_w, sig);
    early_r[0] = (early_r[0] - 1u) & b.early_mask;
    const float er_in = audio_filter::LoadF(p_early + 4u * early_r[0]) * late_gain;

    // 3) 2 cascaded allpasses -> ER tapped delay line.
    ap0_r = (ap0_r - 1u) & b.ap0_mask;
    ap0_w = (ap0_w - 1u) & b.ap0_mask;
    const float ap0_d = audio_filter::LoadF(p_ap0 + 4u * ap0_r);
    const float ap0_y = -std::fmaf(ap_k, er_in, -ap0_d);
    audio_filter::StoreF(p_ap0 + 4u * ap0_w, std::fmaf(ap_k, ap0_y, er_in));

    ap1_r = (ap1_r - 1u) & b.ap1_mask;
    ap1_w = (ap1_w - 1u) & b.ap1_mask;
    const float ap1_d = audio_filter::LoadF(p_ap1 + 4u * ap1_r);
    const float ap1_y = -std::fmaf(ap_k, ap0_y, -ap1_d);
    audio_filter::StoreF(p_ap1 + 4u * ap1_w, std::fmaf(ap_k, ap1_y, ap0_y));

    er_w = (er_w - 1u) & b.er_mask;
    audio_filter::StoreF(p_er + 4u * er_w, ap1_y);
    float er_taps[8];
    er_taps[0] = ap1_y;
    for (uint32_t i = 0; i < 7u; ++i) {
      er_r[i] = (er_r[i] - 1u) & b.er_mask;
      er_taps[i + 1u] = audio_filter::LoadF(p_er + 4u * er_r[i]);
    }

    // 4) 8 reverb tanks + dual 4x4 Hadamard diffusion matrix in NEON registers.
    float etaps[8], ttaps[8];
    for (uint32_t t = 0; t < 8u; ++t) {
      early_r[t + 1u] = (early_r[t + 1u] - 1u) & b.early_mask;
      etaps[t] = audio_filter::LoadF(p_early + 4u * early_r[t + 1u]);
      late_r_a[t] = (late_r_a[t] - 1u) & b.tank_mask[t];
      ttaps[t] = audio_filter::LoadF(p_tank[t] + 4u * late_r_a[t]);
      late_w[t] = (late_w[t] - 1u) & b.tank_mask[t];
    }

#if defined(__aarch64__)
    const float32x4_t ve0 = {etaps[0], etaps[1], etaps[2], etaps[3]};
    const float32x4_t ve1 = {etaps[4], etaps[5], etaps[6], etaps[7]};
    const float32x4_t vt0 = {ttaps[0], ttaps[1], ttaps[2], ttaps[3]};
    const float32x4_t vt1 = {ttaps[4], ttaps[5], ttaps[6], ttaps[7]};
    const float32x4_t vv0 = vmulq_f32(vdiff, vfmaq_f32(vmulq_f32(ve0, vhf), vt0, vfb0));
    const float32x4_t vv1 = vmulq_f32(vdiff, vfmaq_f32(vmulq_f32(ve1, vhf), vt1, vfb1));
    va0 = vfmaq_f32(vmulq_f32(vlp_o0, va0), vlp_g0, vv0);
    va1 = vfmaq_f32(vmulq_f32(vlp_o1, va1), vlp_g1, vv1);

    const float32x4_t u0p = vfmaq_f32(va0, vgp, vrev64q_f32(va0));
    const float32x4_t u0pp = vfmaq_f32(u0p, vgq, vextq_f32(u0p, u0p, 2));
    const float32x4_t u1p = vfmaq_f32(va1, vgp, vrev64q_f32(va1));
    const float32x4_t u1pp = vfmaq_f32(u1p, vgq, vextq_f32(u1p, u1p, 2));
    const float32x4_t w0 = vfmaq_f32(u0pp, veg, u1pp);
    const float32x4_t w1 = vfmaq_f32(u1pp, vneg_eg, u0pp);
    last_u0p = u0p;
    last_u1p = u1p;
    last_u0pp = u0pp;
    last_u1pp = u1pp;
    last_w0 = w0;
    last_w1 = w1;

    // Cyclic writeback shift: w0[1..3], w1[0..3], w0[0] -> tanks 0..7.
    float tw[8];
    vst1q_f32(tw, vextq_f32(w0, w1, 1));
    vst1q_f32(tw + 4, vextq_f32(w1, w0, 1));
    for (uint32_t t = 0; t < 8u; ++t) audio_filter::StoreF(p_tank[t] + 4u * late_w[t], tw[t]);
#else
    for (uint32_t t = 0; t < 8u; ++t) {
      const float v = diff_gain * std::fmaf(ttaps[t], fb_gain[t], etaps[t] * hf_gain);
      va[t] = std::fmaf(lp_g[t], v, lp_omg[t] * va[t]);
    }
    const float u0p[4] = {
        std::fmaf(early_gain, va[1], va[0]),
        std::fmaf(-early_gain, va[0], va[1]),
        std::fmaf(early_gain, va[3], va[2]),
        std::fmaf(-early_gain, va[2], va[3]),
    };
    const float u0pp[4] = {
        std::fmaf(early_gain, u0p[2], u0p[0]),
        std::fmaf(early_gain, u0p[3], u0p[1]),
        std::fmaf(-early_gain, u0p[0], u0p[2]),
        std::fmaf(-early_gain, u0p[1], u0p[3]),
    };
    const float u1p[4] = {
        std::fmaf(early_gain, va[5], va[4]),
        std::fmaf(-early_gain, va[4], va[5]),
        std::fmaf(early_gain, va[7], va[6]),
        std::fmaf(-early_gain, va[6], va[7]),
    };
    const float u1pp[4] = {
        std::fmaf(early_gain, u1p[2], u1p[0]),
        std::fmaf(early_gain, u1p[3], u1p[1]),
        std::fmaf(-early_gain, u1p[0], u1p[2]),
        std::fmaf(-early_gain, u1p[1], u1p[3]),
    };
    float w0[4], w1[4];
    for (uint32_t k = 0; k < 4u; ++k) {
      w0[k] = std::fmaf(early_gain, u1pp[k], u0pp[k]);
      w1[k] = std::fmaf(-early_gain, u0pp[k], u1pp[k]);
      last_u0p[k] = u0p[k];
      last_u1p[k] = u1p[k];
      last_u0pp[k] = u0pp[k];
      last_u1pp[k] = u1pp[k];
      last_w0[k] = w0[k];
      last_w1[k] = w1[k];
    }
    for (uint32_t t = 0; t < 3u; ++t) audio_filter::StoreF(p_tank[t] + 4u * late_w[t], w0[t + 1u]);
    for (uint32_t t = 3u; t < 7u; ++t) audio_filter::StoreF(p_tank[t] + 4u * late_w[t], w1[t - 3u]);
    audio_filter::StoreF(p_tank[7] + 4u * late_w[7], w0[0]);
#endif

    // 5) Channel output mix & dry/wet combine (mirrors loc_82D619C0..loc_82D61B2C).
    const uint8_t* p_in_s = p_in + s * stride_bytes;
    uint8_t* p_out_s = p_out + s * stride_bytes;
    for (uint32_t c = 0; c < b.ch; ++c) {
      const float in_c = audio_filter::LoadF(p_in_s + 4u * c);
      if ((spk_mask & (1u << c)) == 0u) {
        audio_filter::StoreF(p_out_s + 4u * c, in_c);
        continue;
      }
      float er_even = 0.0f, er_odd = 0.0f, tk_even = 0.0f, tk_odd = 0.0f, er_tail = 0.0f, tk_tail = 0.0f;
      uint32_t idx = c;
      uint32_t k = 0u;
      for (; k + 1u < per_ch_taps; k += 2u) {
        er_even += er_taps[idx];
        er_odd += er_taps[idx + b.ch];
        late_r_b[idx] = (late_r_b[idx] - 1u) & b.tank_mask[idx];
        tk_even += audio_filter::LoadF(p_tank[idx] + 4u * late_r_b[idx]);
        const uint32_t idx2 = idx + b.ch;
        late_r_b[idx2] = (late_r_b[idx2] - 1u) & b.tank_mask[idx2];
        tk_odd += audio_filter::LoadF(p_tank[idx2] + 4u * late_r_b[idx2]);
        idx += 2u * b.ch;
      }
      if (k < per_ch_taps) {
        er_tail = er_taps[idx];
        late_r_b[idx] = (late_r_b[idx] - 1u) & b.tank_mask[idx];
        tk_tail = audio_filter::LoadF(p_tank[idx] + 4u * late_r_b[idx]);
      }
      const float er_sum = (er_tail + er_odd) + er_even;
      const float late_sum = (tk_tail + tk_odd) + tk_even;
      const float mixed = (dry_gain == 0.0f) ? (er_sum + late_sum) : (std::fmaf(in_c, dry_gain, er_sum) + late_sum);
      audio_filter::StoreF(p_out_s + 4u * c, mixed);
    }
  }

  // Write back modified scalar state, indices, and final tank vectors.
  audio_filter::StoreF(ps + 36u, lp1);
  audio_filter::StoreF(ps + 40u, lp2);
  if (use_bq) {
    audio_filter::StoreF(ps + 68u, bq_s0);
    audio_filter::StoreF(ps + 72u, bq_s1);
  }
  audio_filter::Store32(ps + 1220u, early_w);
  for (uint32_t i = 0; i < 9u; ++i) audio_filter::Store32(ps + 1224u + 4u * i, early_r[i]);
  audio_filter::Store32(ps + 1444u, ap0_r);
  audio_filter::Store32(ps + 1436u, ap0_w);
  audio_filter::Store32(ps + 1448u, ap1_r);
  audio_filter::Store32(ps + 1440u, ap1_w);
  audio_filter::Store32(ps + 1364u, er_w);
  for (uint32_t i = 0; i < 7u; ++i) audio_filter::Store32(ps + 1368u + 4u * i, er_r[i]);
  for (uint32_t t = 0; t < 8u; ++t) {
    audio_filter::Store32(ps + 956u + 4u * t, late_w[t]);
    audio_filter::Store32(ps + 988u + 4u * t, late_r_a[t]);
    audio_filter::Store32(ps + 1052u + 4u * t, late_r_b[t]);
  }
#if defined(__aarch64__)
  audio_dsp::StoreF4BE(p_vec_a + 0u, va0);
  audio_dsp::StoreF4BE(p_vec_a + 16u, va1);
  audio_dsp::StoreF4BE(p_vec_b + 0u, last_u0p);
  audio_dsp::StoreF4BE(p_vec_b + 16u, last_u1p);
  audio_dsp::StoreF4BE(p_vec_b + 32u, last_u0pp);
  audio_dsp::StoreF4BE(p_vec_b + 48u, last_u1pp);
  audio_dsp::StoreF4BE(p_vec_b + 64u, last_w0);
  audio_dsp::StoreF4BE(p_vec_b + 80u, last_w1);
#else
  for (uint32_t t = 0; t < 8u; ++t) audio_filter::StoreF(p_vec_a + 4u * t, va[t]);
  for (uint32_t k = 0; k < 4u; ++k) {
    audio_filter::StoreF(p_vec_b + 0u + 4u * k, last_u0p[k]);
    audio_filter::StoreF(p_vec_b + 16u + 4u * k, last_u1p[k]);
    audio_filter::StoreF(p_vec_b + 32u + 4u * k, last_u0pp[k]);
    audio_filter::StoreF(p_vec_b + 48u + 4u * k, last_u1pp[k]);
    audio_filter::StoreF(p_vec_b + 64u + 4u * k, last_w0[k]);
    audio_filter::StoreF(p_vec_b + 80u + 4u * k, last_w1[k]);
  }
#endif
  ctx.r3.u64 = 0;
  return true;
}

inline bool VerifyAndRun(PPCContext& __restrict ctx, uint8_t* base, void (*ref_fn)(PPCContext&, uint8_t*)) {
  ReverbBuffers b{};
  if (!Validate(base, ctx, b)) return false;

  struct Snap {
    uint32_t addr;
    std::vector<uint8_t> data;
  };
  std::vector<Snap> snaps;
  auto push = [&](uint32_t addr, uint32_t len) {
    const uint8_t* p = ::banjo::hot::Raw(base, addr);
    snaps.push_back({addr, std::vector<uint8_t>(p, p + len)});
  };
  push(b.self, 1452u);
  push(b.in_addr, b.io_bytes);
  if (b.out_addr != b.in_addr) push(b.out_addr, b.io_bytes);
  push(b.mono_addr, b.mono_bytes);
  push(b.vec_a_addr, 32u);
  push(b.vec_b_addr, 96u);
  push(b.early_buf_addr, (b.early_mask + 1u) * 4u);
  push(b.er_buf_addr, (b.er_mask + 1u) * 4u);
  push(b.ap0_addr, (b.ap0_mask + 1u) * 4u);
  push(b.ap1_addr, (b.ap1_mask + 1u) * 4u);
  for (uint32_t t = 0; t < 8u; ++t) push(b.tank_buf_addr[t], (b.tank_mask[t] + 1u) * 4u);

  const PPCContext saved_ctx = ctx;
  if (!TryRun(ctx, base)) {
    for (const auto& sn : snaps) std::memcpy(::banjo::hot::Raw(base, sn.addr), sn.data.data(), sn.data.size());
    ctx = saved_ctx;
    return false;
  }

  std::vector<uint8_t> native_out(b.io_bytes);
  std::memcpy(native_out.data(), ::banjo::hot::Raw(base, b.out_addr), b.io_bytes);

  for (const auto& sn : snaps) std::memcpy(::banjo::hot::Raw(base, sn.addr), sn.data.data(), sn.data.size());
  ctx = saved_ctx;
  ref_fn(ctx, base);

  float max_diff = 0.0f;
  uint32_t bad_idx = 0;
  const uint8_t* ref_out = ::banjo::hot::Raw(base, b.out_addr);
  for (uint32_t i = 0; i < b.nsamples * b.ch; ++i) {
    const float fn = audio_filter::LoadF(native_out.data() + 4u * i);
    const float fr = audio_filter::LoadF(ref_out + 4u * i);
    const float d = std::fabs(fn - fr);
    if (d > max_diff || !(d <= 1e-5f)) {
      if (max_diff == 0.0f || d > max_diff) {
        max_diff = d;
        bad_idx = i;
      }
    }
  }
  if (!(max_diff <= 1e-5f)) {
    REXLOG_WARN("[hot] FMOD reverb verify mismatch ch={} n={} idx={} max_diff={}", b.ch, b.nsamples, bad_idx,
                max_diff);
  }
  return true;
}

inline void LogOnce(bool active) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true, std::memory_order_relaxed)) {
    REXLOG_INFO("[hot] native FMOD SFX reverb (banjo_native_fmod_reverb): {}", active ? "ON" : "off");
  }
}

}  // namespace banjo::hand::n_82D60CB8

namespace banjo::hot {

template <>
[[gnu::always_inline]] inline bool TryCustomOverride<&n_82D60CB8::Native>(PPCContext& ctx, uint8_t* base) {
  const bool on = banjo::EnhancementActive(banjo::EnhancementGroup::kCpu, bool(REXCVAR_GET(banjo_native_fmod_reverb)));
  ::banjo::hand::n_82D60CB8::LogOnce(on);
  if (!on) [[unlikely]] return false;
  if (bool(REXCVAR_GET(banjo_native_fmod_reverb_verify))) [[unlikely]] {
    return ::banjo::hand::n_82D60CB8::VerifyAndRun(ctx, base, &n_82D60CB8::Native);
  }
  return ::banjo::hand::n_82D60CB8::TryRun(ctx, base);
}

}  // namespace banjo::hot
