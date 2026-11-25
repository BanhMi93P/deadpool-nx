#pragma once

// Custom C++ / NEON overrides evaluated ahead of localized guest copies in
// BANJO_HOT_HOOK_EX. Keeps app/generated/hot/n_*.h untouched while intercepting
// both direct and virtual-table callers in both BANJO_HOT_IN_PLACE (Switch) and
// hooked (PC) builds.

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

[[gnu::always_inline]] inline uint32_t Load32P(const uint8_t *p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}

[[gnu::always_inline]] inline void LogDeclineOnce(const char *reason,
                                                  uint32_t ch,
                                                  uint32_t nsamples,
                                                  int32_t tanks) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true, std::memory_order_relaxed)) {
    REXLOG_WARN("[hot] n_82D60CB8 declined ({}): ch={} n={} tanks={}", reason,
                ch, nsamples, tanks);
  }
}

inline bool Validate(uint8_t *base, const PPCContext &ctx, ReverbBuffers &b) {
  b.self = ctx.r3.u32;
  b.in_addr = ctx.r4.u32;
  b.out_addr = ctx.r5.u32;
  const int32_t ch_s = ctx.r6.s32;
  b.nsamples = ctx.r7.u32;
  if (b.nsamples == 0u || ch_s <= 0 || ch_s > 8) [[unlikely]] {
    LogDeclineOnce("bad args", uint32_t(ch_s), b.nsamples, 0);
    return false;
  }
  b.ch = uint32_t(ch_s);
  if (b.nsamples > 0x0FFFFFFFu / b.ch) [[unlikely]]
    return false;
  b.io_bytes = b.nsamples * b.ch * 4u;
  b.mono_bytes = b.nsamples * 4u;

  if (!detail::Straight(b.self, 1452u) ||
      !detail::Straight(b.in_addr, b.io_bytes) ||
      !detail::Straight(b.out_addr, b.io_bytes)) [[unlikely]] {
    LogDeclineOnce("non-straight io", b.ch, b.nsamples, 0);
    return false;
  }
  if (b.in_addr != b.out_addr &&
      audio_filter::Overlap(b.in_addr, b.io_bytes, b.out_addr, b.io_bytes))
      [[unlikely]] {
    LogDeclineOnce("io overlap", b.ch, b.nsamples, 0);
    return false;
  }

  const uint8_t *ps = ::banjo::hot::Raw(base, b.self);
  const int32_t num_tanks = int32_t(Load32P(ps + 720u));
  if (num_tanks != 8) [[unlikely]] {
    LogDeclineOnce("tanks != 8", b.ch, b.nsamples, num_tanks);
    return false;
  }

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
  if (!ValidMask(b.early_mask) || !ValidMask(b.er_mask) ||
      !ValidMask(b.ap0_mask) || !ValidMask(b.ap1_mask)) [[unlikely]] {
    LogDeclineOnce("invalid delay mask", b.ch, b.nsamples, num_tanks);
    return false;
  }
  if (!detail::Straight(b.mono_addr, b.mono_bytes) ||
      !detail::Straight(b.vec_a_addr, 32u) ||
      !detail::Straight(b.vec_b_addr, 96u) ||
      !detail::Straight(b.early_buf_addr, (b.early_mask + 1u) * 4u) ||
      !detail::Straight(b.er_buf_addr, (b.er_mask + 1u) * 4u) ||
      !detail::Straight(b.ap0_addr, (b.ap0_mask + 1u) * 4u) ||
      !detail::Straight(b.ap1_addr, (b.ap1_mask + 1u) * 4u)) [[unlikely]] {
    LogDeclineOnce("non-straight scratch", b.ch, b.nsamples, num_tanks);
    return false;
  }

  for (uint32_t t = 0; t < 8u; ++t) {
    b.tank_buf_addr[t] = Load32P(ps + 724u + 4u * t);
    b.tank_mask[t] = Load32P(ps + 924u + 4u * t);
    if (!ValidMask(b.tank_mask[t]) ||
        !detail::Straight(b.tank_buf_addr[t], (b.tank_mask[t] + 1u) * 4u))
        [[unlikely]] {
      LogDeclineOnce("invalid tank", b.ch, b.nsamples, num_tanks);
      return false;
    }
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
    if (audio_filter::Overlap(fixed[i].addr, fixed[i].bytes, b.in_addr,
                              b.io_bytes) ||
        audio_filter::Overlap(fixed[i].addr, fixed[i].bytes, b.out_addr,
                              b.io_bytes)) [[unlikely]] {
      LogDeclineOnce("fixed/io overlap", b.ch, b.nsamples, int32_t(i));
      return false;
    }
    for (size_t j = i + 1; j < sizeof(fixed) / sizeof(fixed[0]); ++j) {
      // vec_a (j==2) and vec_b (j==3) live inside the DSP state struct (i==0).
      if (i == 0 && (j == 2u || j == 3u))
        continue;
      if (audio_filter::Overlap(fixed[i].addr, fixed[i].bytes, fixed[j].addr,
                                fixed[j].bytes)) [[unlikely]] {
        LogDeclineOnce("fixed/fixed overlap", b.ch, b.nsamples,
                       int32_t(i * 10u + j));
        return false;
      }
    }
    for (uint32_t t = 0; t < 8u; ++t) {
      if (audio_filter::Overlap(fixed[i].addr, fixed[i].bytes,
                                b.tank_buf_addr[t], (b.tank_mask[t] + 1u) * 4u))
          [[unlikely]] {
        LogDeclineOnce("fixed/tank overlap", b.ch, b.nsamples,
                       int32_t(i * 10u + t));
        return false;
      }
    }
  }
  for (uint32_t t = 0; t < 8u; ++t) {
    const uint32_t tb = (b.tank_mask[t] + 1u) * 4u;
    if (audio_filter::Overlap(b.tank_buf_addr[t], tb, b.in_addr, b.io_bytes) ||
        audio_filter::Overlap(b.tank_buf_addr[t], tb, b.out_addr, b.io_bytes))
        [[unlikely]] {
      LogDeclineOnce("tank/io overlap", b.ch, b.nsamples, int32_t(t));
      return false;
    }
    for (uint32_t u = t + 1; u < 8u; ++u) {
      if (audio_filter::Overlap(b.tank_buf_addr[t], tb, b.tank_buf_addr[u],
                                (b.tank_mask[u] + 1u) * 4u)) [[unlikely]] {
        LogDeclineOnce("tank/tank overlap", b.ch, b.nsamples,
                       int32_t(t * 10u + u));
        return false;
      }
    }
  }
  return true;
}

[[gnu::always_inline]] inline void AddAntiDenormalDC(uint8_t *p_in, uint32_t ch,
                                                     uint32_t nsamples) {
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
      audio_dsp::StoreF4BE(p_in + 0u,
                           vaddq_f32(audio_dsp::LoadF4BE(p_in + 0u), vp));
      audio_dsp::StoreF4BE(p_in + 16u,
                           vaddq_f32(audio_dsp::LoadF4BE(p_in + 16u), vm));
      audio_dsp::StoreF4BE(p_in + 32u,
                           vaddq_f32(audio_dsp::LoadF4BE(p_in + 32u), vn));
      audio_dsp::StoreF4BE(p_in + 48u,
                           vaddq_f32(audio_dsp::LoadF4BE(p_in + 48u), vp));
      audio_dsp::StoreF4BE(p_in + 64u,
                           vaddq_f32(audio_dsp::LoadF4BE(p_in + 64u), vm));
      audio_dsp::StoreF4BE(p_in + 80u,
                           vaddq_f32(audio_dsp::LoadF4BE(p_in + 80u), vn));
    }
    for (uint32_t s = quads * 4u; s < nsamples; ++s, p_in += 24u) {
      const float d = (s & 1u) ? -dc_pos : dc_pos;
      for (uint32_t c = 0; c < 6u; ++c)
        audio_filter::StoreF(p_in + 4u * c,
                             audio_filter::LoadF(p_in + 4u * c) + d);
    }
#else
    for (uint32_t s = 0; s < nsamples; ++s, p_in += 24u) {
      const float d = (s & 1u) ? -dc_pos : dc_pos;
      for (uint32_t c = 0; c < 6u; ++c)
        audio_filter::StoreF(p_in + 4u * c,
                             audio_filter::LoadF(p_in + 4u * c) + d);
    }
#endif
  } else if (ch == 8u) {
    const uint32_t quads = nsamples >> 2u;
#if defined(__aarch64__)
    const float32x4_t vp = vdupq_n_f32(dc_pos);
    const float32x4_t vn = vdupq_n_f32(-dc_pos);
    for (uint32_t q = 0; q < quads; ++q, p_in += 128u) {
      audio_dsp::StoreF4BE(p_in + 0u,
                           vaddq_f32(audio_dsp::LoadF4BE(p_in + 0u), vp));
      audio_dsp::StoreF4BE(p_in + 16u,
                           vaddq_f32(audio_dsp::LoadF4BE(p_in + 16u), vp));
      audio_dsp::StoreF4BE(p_in + 32u,
                           vaddq_f32(audio_dsp::LoadF4BE(p_in + 32u), vn));
      audio_dsp::StoreF4BE(p_in + 48u,
                           vaddq_f32(audio_dsp::LoadF4BE(p_in + 48u), vn));
      audio_dsp::StoreF4BE(p_in + 64u,
                           vaddq_f32(audio_dsp::LoadF4BE(p_in + 64u), vp));
      audio_dsp::StoreF4BE(p_in + 80u,
                           vaddq_f32(audio_dsp::LoadF4BE(p_in + 80u), vp));
      audio_dsp::StoreF4BE(p_in + 96u,
                           vaddq_f32(audio_dsp::LoadF4BE(p_in + 96u), vn));
      audio_dsp::StoreF4BE(p_in + 112u,
                           vaddq_f32(audio_dsp::LoadF4BE(p_in + 112u), vn));
    }
    for (uint32_t s = quads * 4u; s < nsamples; ++s, p_in += 32u) {
      const float d = (s & 1u) ? -dc_pos : dc_pos;
      for (uint32_t c = 0; c < 8u; ++c)
        audio_filter::StoreF(p_in + 4u * c,
                             audio_filter::LoadF(p_in + 4u * c) + d);
    }
#else
    for (uint32_t s = 0; s < nsamples; ++s, p_in += 32u) {
      const float d = (s & 1u) ? -dc_pos : dc_pos;
      for (uint32_t c = 0; c < 8u; ++c)
        audio_filter::StoreF(p_in + 4u * c,
                             audio_filter::LoadF(p_in + 4u * c) + d);
    }
#endif
  } else {
    for (uint32_t c = 0; c < ch; ++c) {
      for (uint32_t s = 0; s < nsamples; ++s) {
        uint8_t *pc = p_in + 4u * (s * ch + c);
        const float v = audio_filter::LoadF(pc);
        audio_filter::StoreF(pc, (s & 1u) ? (v - dc_pos) : (v + dc_pos));
      }
    }
  }
}

[[gnu::always_inline]] inline void DownmixToMono(const uint8_t *p_in,
                                                 uint8_t *p_mono, uint32_t ch,
                                                 uint32_t nsamples) {
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
    for (uint32_t s = quads * 4u; s < nsamples;
         ++s, p_in += 24u, p_mono += 4u) {
      float sum03 = 0.0f, sum45 = 0.0f;
      for (uint32_t c = 0; c < 4u; ++c)
        sum03 += audio_filter::LoadF(p_in + 4u * c);
      sum45 = audio_filter::LoadF(p_in + 16u) + audio_filter::LoadF(p_in + 20u);
      audio_filter::StoreF(p_mono, sum03 + sum45);
    }
#else
    for (uint32_t s = 0; s < nsamples; ++s, p_in += 24u, p_mono += 4u) {
      float sum03 = 0.0f, sum45 = 0.0f;
      for (uint32_t c = 0; c < 4u; ++c)
        sum03 += audio_filter::LoadF(p_in + 4u * c);
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
      const float s0 = vaddvq_f32(audio_dsp::LoadF4BE(p_in + 0u)) +
                       vaddvq_f32(audio_dsp::LoadF4BE(p_in + 16u));
      const float s1 = vaddvq_f32(audio_dsp::LoadF4BE(p_in + 32u)) +
                       vaddvq_f32(audio_dsp::LoadF4BE(p_in + 48u));
      const float s2 = vaddvq_f32(audio_dsp::LoadF4BE(p_in + 64u)) +
                       vaddvq_f32(audio_dsp::LoadF4BE(p_in + 80u));
      const float s3 = vaddvq_f32(audio_dsp::LoadF4BE(p_in + 96u)) +
                       vaddvq_f32(audio_dsp::LoadF4BE(p_in + 112u));
      const float32x4_t m = {s0, s1, s2, s3};
      audio_dsp::StoreF4BE(p_mono, m);
    }
    for (uint32_t s = quads * 4u; s < nsamples;
         ++s, p_in += 32u, p_mono += 4u) {
      float sum03 = 0.0f, sum47 = 0.0f;
      for (uint32_t c = 0; c < 4u; ++c)
        sum03 += audio_filter::LoadF(p_in + 4u * c);
      for (uint32_t c = 4u; c < 8u; ++c)
        sum47 += audio_filter::LoadF(p_in + 4u * c);
      audio_filter::StoreF(p_mono, sum03 + sum47);
    }
#else
    for (uint32_t s = 0; s < nsamples; ++s, p_in += 32u, p_mono += 4u) {
      float sum03 = 0.0f, sum47 = 0.0f;
      for (uint32_t c = 0; c < 4u; ++c)
        sum03 += audio_filter::LoadF(p_in + 4u * c);
      for (uint32_t c = 4u; c < 8u; ++c)
        sum47 += audio_filter::LoadF(p_in + 4u * c);
      audio_filter::StoreF(p_mono, sum03 + sum47);
    }
#endif
    return;
  }
  for (uint32_t s = 0; s < nsamples; ++s, p_in += 4u * ch, p_mono += 4u) {
    float acc = 0.0f;
    for (uint32_t c = 0; c < ch; ++c)
      acc += audio_filter::LoadF(p_in + 4u * c);
    audio_filter::StoreF(p_mono, acc);
  }
}

struct ReverbParams {
  uint32_t early_w;
  uint32_t early_r[9];
  uint32_t early_r_old[9];
  bool early_ramp[9];

  uint32_t late_w[8];
  uint32_t late_r_a[8];
  uint32_t late_r_a_old[8];
  uint32_t late_r_b[8];
  float fb_gain[8];
  float fb_old[8];
  bool tank_ramp[8];

  float lp_g[8];
  float lp_omg[8];
  float lp_old[8];
  bool lp_ramp[8];

  float hf_gain, hf_old;
  bool hf_ramp;
  float late_gain, late_old;
  bool late_ramp;
  float in_lpf_g, in_lpf_omg, in_lpf_old;
  bool in_lpf_ramp;
  float early_gain, early_old;
  bool early_gain_ramp;
  float diff_gain, diff_old;
  bool diff_ramp;

  float lp1, lp2;
  float bq_b0, bq_b1, bq_b2, bq_a1, bq_a2;
  bool use_bq;
  float bq_s0, bq_s1;
  float ap_k;
  uint32_t ap0_r, ap0_w, ap1_r, ap1_w;
  uint32_t er_w;
  uint32_t er_r[7];
};

template <bool Ramp>
[[gnu::always_inline]] inline void
RunSampleLoop(const ReverbBuffers &b, const uint8_t *p_in, uint8_t *p_out,
              const uint8_t *p_mono, const uint8_t *p_vec_a,
              uint8_t *p_vec_a_out, uint8_t *p_vec_b_out, uint8_t *p_early,
              uint8_t *p_er, uint8_t *p_ap0, uint8_t *p_ap1,
              uint8_t *const p_tank[8], ReverbParams &p, uint16_t spk_mask,
              float dry_gain) {
#if defined(__aarch64__)
  float32x4_t va0 = audio_dsp::LoadF4BE(p_vec_a + 0u);
  float32x4_t va1 = audio_dsp::LoadF4BE(p_vec_a + 16u);
  const float32x4_t vlp_g0 = {p.lp_g[0], p.lp_g[1], p.lp_g[2], p.lp_g[3]};
  const float32x4_t vlp_g1 = {p.lp_g[4], p.lp_g[5], p.lp_g[6], p.lp_g[7]};
  const float32x4_t vlp_o0 = {p.lp_omg[0], p.lp_omg[1], p.lp_omg[2],
                              p.lp_omg[3]};
  const float32x4_t vlp_o1 = {p.lp_omg[4], p.lp_omg[5], p.lp_omg[6],
                              p.lp_omg[7]};
  const float32x4_t vfb0 = {p.fb_gain[0], p.fb_gain[1], p.fb_gain[2],
                            p.fb_gain[3]};
  const float32x4_t vfb1 = {p.fb_gain[4], p.fb_gain[5], p.fb_gain[6],
                            p.fb_gain[7]};
  const float32x4_t vhf = vdupq_n_f32(p.hf_gain);
  const float32x4_t vdiff = vdupq_n_f32(p.diff_gain);
  const float32x4_t vgp = {p.early_gain, -p.early_gain, p.early_gain,
                           -p.early_gain};
  const float32x4_t vgq = {p.early_gain, p.early_gain, -p.early_gain,
                           -p.early_gain};
  const float32x4_t veg = vdupq_n_f32(p.early_gain);
  const float32x4_t vneg_eg = vdupq_n_f32(-p.early_gain);
  float32x4_t last_u0p{}, last_u1p{}, last_u0pp{}, last_u1pp{}, last_w0{},
      last_w1{};
#else
  float va[8];
  for (uint32_t t = 0; t < 8u; ++t)
    va[t] = audio_filter::LoadF(p_vec_a + 4u * t);
  float last_u0p[4]{}, last_u1p[4]{}, last_u0pp[4]{}, last_u1pp[4]{},
      last_w0[4]{}, last_w1[4]{};
#endif

  const float inv_n = 1.0f / float(b.nsamples);
  const uint32_t per_ch_taps = 8u / b.ch;
  const uint32_t stride_bytes = b.ch * 4u;

  for (uint32_t s = 0; s < b.nsamples; ++s) {
    float in_lpf_g_s = p.in_lpf_g;
    float in_lpf_omg_s = p.in_lpf_omg;
    float early_gain_s = p.early_gain;
    float diff_gain_s = p.diff_gain;
    float late_gain_s = p.late_gain;
    float hf_gain_s = p.hf_gain;
    float t_new = 0.0f, t_old = 0.0f;
    if constexpr (Ramp) {
      t_new = float(s);
      t_old = float(b.nsamples - s);
      if (p.in_lpf_ramp) {
        in_lpf_g_s = std::fmaf(p.in_lpf_old, t_old, p.in_lpf_g * t_new) * inv_n;
        in_lpf_omg_s = 1.0f - in_lpf_g_s;
      }
      if (p.early_gain_ramp)
        early_gain_s =
            std::fmaf(p.early_old, t_old, p.early_gain * t_new) * inv_n;
      if (p.diff_ramp)
        diff_gain_s = std::fmaf(p.diff_old, t_old, p.diff_gain * t_new) * inv_n;
      if (p.late_ramp)
        late_gain_s = std::fmaf(p.late_old, t_old, p.late_gain * t_new) * inv_n;
      if (p.hf_ramp)
        hf_gain_s = std::fmaf(p.hf_old, t_old, p.hf_gain * t_new) * inv_n;
    }

    // 1) Input 2-stage LPF + optional biquad.
    const float xm = audio_filter::LoadF(p_mono + 4u * s);
    p.lp1 = std::fmaf(p.lp1, in_lpf_omg_s, in_lpf_g_s * xm);
    p.lp2 = std::fmaf(p.lp2, in_lpf_omg_s, in_lpf_g_s * p.lp1);
    float sig = p.lp2;
    if (p.use_bq) {
      const float prev_s1 = p.bq_s1;
      const float y = std::fmaf(p.bq_b0, p.lp2, p.bq_s0);
      const float t1 = std::fmaf(p.bq_b1, p.lp2, p.bq_a1 * y);
      p.bq_s1 = std::fmaf(p.bq_b2, p.lp2, p.bq_a2 * y);
      p.bq_s0 = t1 + prev_s1;
      sig = y;
    }

    // 2) Push into early reflection buffer; read ER feed tap.
    p.early_w = (p.early_w - 1u) & b.early_mask;
    audio_filter::StoreF(p_early + 4u * p.early_w, sig);
    p.early_r[0] = (p.early_r[0] - 1u) & b.early_mask;
    float er_in =
        audio_filter::LoadF(p_early + 4u * p.early_r[0]) * late_gain_s;
    if constexpr (Ramp) {
      if (p.early_ramp[0]) {
        p.early_r_old[0] = (p.early_r_old[0] - 1u) & b.early_mask;
        const float er_old =
            audio_filter::LoadF(p_early + 4u * p.early_r_old[0]) * late_gain_s;
        er_in = std::fmaf(er_old, t_old, er_in * t_new) * inv_n;
      }
    }

    // 3) 2 cascaded allpasses -> ER tapped delay line.
    p.ap0_r = (p.ap0_r - 1u) & b.ap0_mask;
    p.ap0_w = (p.ap0_w - 1u) & b.ap0_mask;
    const float ap0_d = audio_filter::LoadF(p_ap0 + 4u * p.ap0_r);
    const float ap0_y = -std::fmaf(p.ap_k, er_in, -ap0_d);
    audio_filter::StoreF(p_ap0 + 4u * p.ap0_w, std::fmaf(p.ap_k, ap0_y, er_in));

    p.ap1_r = (p.ap1_r - 1u) & b.ap1_mask;
    p.ap1_w = (p.ap1_w - 1u) & b.ap1_mask;
    const float ap1_d = audio_filter::LoadF(p_ap1 + 4u * p.ap1_r);
    const float ap1_y = -std::fmaf(p.ap_k, ap0_y, -ap1_d);
    audio_filter::StoreF(p_ap1 + 4u * p.ap1_w, std::fmaf(p.ap_k, ap1_y, ap0_y));

    p.er_w = (p.er_w - 1u) & b.er_mask;
    audio_filter::StoreF(p_er + 4u * p.er_w, ap1_y);
    float er_taps[8];
    er_taps[0] = ap1_y;
    for (uint32_t i = 0; i < 7u; ++i) {
      p.er_r[i] = (p.er_r[i] - 1u) & b.er_mask;
      er_taps[i + 1u] = audio_filter::LoadF(p_er + 4u * p.er_r[i]);
    }

    // 4) 8 reverb tanks + dual 4x4 Hadamard diffusion matrix in NEON registers.
    float etaps[8], ttaps[8];
    for (uint32_t t = 0; t < 8u; ++t) {
      p.early_r[t + 1u] = (p.early_r[t + 1u] - 1u) & b.early_mask;
      etaps[t] = audio_filter::LoadF(p_early + 4u * p.early_r[t + 1u]);
      p.late_r_a[t] = (p.late_r_a[t] - 1u) & b.tank_mask[t];
      ttaps[t] = audio_filter::LoadF(p_tank[t] + 4u * p.late_r_a[t]);
      p.late_w[t] = (p.late_w[t] - 1u) & b.tank_mask[t];
      if constexpr (Ramp) {
        if (p.early_ramp[t + 1u]) {
          p.early_r_old[t + 1u] = (p.early_r_old[t + 1u] - 1u) & b.early_mask;
          const float old_e =
              audio_filter::LoadF(p_early + 4u * p.early_r_old[t + 1u]);
          etaps[t] = std::fmaf(old_e, t_old, etaps[t] * t_new) * inv_n;
        }
        const float t_cur = ttaps[t] * p.fb_gain[t];
        if (p.tank_ramp[t]) {
          p.late_r_a_old[t] = (p.late_r_a_old[t] - 1u) & b.tank_mask[t];
          const float t_old_s =
              audio_filter::LoadF(p_tank[t] + 4u * p.late_r_a_old[t]) *
              p.fb_old[t];
          ttaps[t] = std::fmaf(t_old_s, t_old, t_cur * t_new) * inv_n;
        } else {
          ttaps[t] = t_cur;
        }
      }
    }

#if defined(__aarch64__)
    const float32x4_t ve0 = {etaps[0], etaps[1], etaps[2], etaps[3]};
    const float32x4_t ve1 = {etaps[4], etaps[5], etaps[6], etaps[7]};
    const float32x4_t vt0 = {ttaps[0], ttaps[1], ttaps[2], ttaps[3]};
    const float32x4_t vt1 = {ttaps[4], ttaps[5], ttaps[6], ttaps[7]};
    float32x4_t u0p, u0pp, u1p, u1pp, w0, w1;
    if constexpr (!Ramp) {
      const float32x4_t vv0 =
          vmulq_f32(vdiff, vfmaq_f32(vmulq_f32(ve0, vhf), vt0, vfb0));
      const float32x4_t vv1 =
          vmulq_f32(vdiff, vfmaq_f32(vmulq_f32(ve1, vhf), vt1, vfb1));
      va0 = vfmaq_f32(vmulq_f32(vlp_o0, va0), vlp_g0, vv0);
      va1 = vfmaq_f32(vmulq_f32(vlp_o1, va1), vlp_g1, vv1);
      u0p = vfmaq_f32(va0, vgp, vrev64q_f32(va0));
      u0pp = vfmaq_f32(u0p, vgq, vextq_f32(u0p, u0p, 2));
      u1p = vfmaq_f32(va1, vgp, vrev64q_f32(va1));
      u1pp = vfmaq_f32(u1p, vgq, vextq_f32(u1p, u1p, 2));
      w0 = vfmaq_f32(u0pp, veg, u1pp);
      w1 = vfmaq_f32(u1pp, vneg_eg, u0pp);
    } else {
      const float32x4_t vhf_s = vdupq_n_f32(hf_gain_s);
      const float32x4_t vdiff_s = vdupq_n_f32(diff_gain_s);
      const float32x4_t vv0 = vmulq_f32(vdiff_s, vfmaq_f32(vt0, ve0, vhf_s));
      const float32x4_t vv1 = vmulq_f32(vdiff_s, vfmaq_f32(vt1, ve1, vhf_s));
      float lpg_s[8], lpo_s[8];
      for (uint32_t t = 0; t < 8u; ++t) {
        lpg_s[t] =
            p.lp_ramp[t]
                ? std::fmaf(p.lp_old[t], t_old, p.lp_g[t] * t_new) * inv_n
                : p.lp_g[t];
        lpo_s[t] = 1.0f - lpg_s[t];
      }
      const float32x4_t lg0 = {lpg_s[0], lpg_s[1], lpg_s[2], lpg_s[3]};
      const float32x4_t lg1 = {lpg_s[4], lpg_s[5], lpg_s[6], lpg_s[7]};
      const float32x4_t lo0 = {lpo_s[0], lpo_s[1], lpo_s[2], lpo_s[3]};
      const float32x4_t lo1 = {lpo_s[4], lpo_s[5], lpo_s[6], lpo_s[7]};
      va0 = vfmaq_f32(vmulq_f32(lo0, va0), lg0, vv0);
      va1 = vfmaq_f32(vmulq_f32(lo1, va1), lg1, vv1);
      const float32x4_t vgp_s = {early_gain_s, -early_gain_s, early_gain_s,
                                 -early_gain_s};
      const float32x4_t vgq_s = {early_gain_s, early_gain_s, -early_gain_s,
                                 -early_gain_s};
      const float32x4_t veg_s = vdupq_n_f32(early_gain_s);
      const float32x4_t vneg_s = vdupq_n_f32(-early_gain_s);
      u0p = vfmaq_f32(va0, vgp_s, vrev64q_f32(va0));
      u0pp = vfmaq_f32(u0p, vgq_s, vextq_f32(u0p, u0p, 2));
      u1p = vfmaq_f32(va1, vgp_s, vrev64q_f32(va1));
      u1pp = vfmaq_f32(u1p, vgq_s, vextq_f32(u1p, u1p, 2));
      w0 = vfmaq_f32(u0pp, veg_s, u1pp);
      w1 = vfmaq_f32(u1pp, vneg_s, u0pp);
    }
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
    for (uint32_t t = 0; t < 8u; ++t)
      audio_filter::StoreF(p_tank[t] + 4u * p.late_w[t], tw[t]);
#else
    for (uint32_t t = 0; t < 8u; ++t) {
      const float fb_v = Ramp ? ttaps[t] : (ttaps[t] * p.fb_gain[t]);
      const float v = diff_gain_s * std::fmaf(etaps[t], hf_gain_s, fb_v);
      const float lpg =
          (Ramp && p.lp_ramp[t])
              ? std::fmaf(p.lp_old[t], t_old, p.lp_g[t] * t_new) * inv_n
              : p.lp_g[t];
      const float lpo = 1.0f - lpg;
      va[t] = std::fmaf(lpg, v, lpo * va[t]);
    }
    const float u0p[4] = {
        std::fmaf(early_gain_s, va[1], va[0]),
        std::fmaf(-early_gain_s, va[0], va[1]),
        std::fmaf(early_gain_s, va[3], va[2]),
        std::fmaf(-early_gain_s, va[2], va[3]),
    };
    const float u0pp[4] = {
        std::fmaf(early_gain_s, u0p[2], u0p[0]),
        std::fmaf(early_gain_s, u0p[3], u0p[1]),
        std::fmaf(-early_gain_s, u0p[0], u0p[2]),
        std::fmaf(-early_gain_s, u0p[1], u0p[3]),
    };
    const float u1p[4] = {
        std::fmaf(early_gain_s, va[5], va[4]),
        std::fmaf(-early_gain_s, va[4], va[5]),
        std::fmaf(early_gain_s, va[7], va[6]),
        std::fmaf(-early_gain_s, va[6], va[7]),
    };
    const float u1pp[4] = {
        std::fmaf(early_gain_s, u1p[2], u1p[0]),
        std::fmaf(early_gain_s, u1p[3], u1p[1]),
        std::fmaf(-early_gain_s, u1p[0], u1p[2]),
        std::fmaf(-early_gain_s, u1p[1], u1p[3]),
    };
    float w0[4], w1[4];
    for (uint32_t k = 0; k < 4u; ++k) {
      w0[k] = std::fmaf(early_gain_s, u1pp[k], u0pp[k]);
      w1[k] = std::fmaf(-early_gain_s, u0pp[k], u1pp[k]);
      last_u0p[k] = u0p[k];
      last_u1p[k] = u1p[k];
      last_u0pp[k] = u0pp[k];
      last_u1pp[k] = u1pp[k];
      last_w0[k] = w0[k];
      last_w1[k] = w1[k];
    }
    for (uint32_t t = 0; t < 3u; ++t)
      audio_filter::StoreF(p_tank[t] + 4u * p.late_w[t], w0[t + 1u]);
    for (uint32_t t = 3u; t < 7u; ++t)
      audio_filter::StoreF(p_tank[t] + 4u * p.late_w[t], w1[t - 3u]);
    audio_filter::StoreF(p_tank[7] + 4u * p.late_w[7], w0[0]);
#endif

    // 5) Channel output mix & dry/wet combine (mirrors
    // loc_82D619C0..loc_82D61B2C).
    const uint8_t *p_in_s = p_in + s * stride_bytes;
    uint8_t *p_out_s = p_out + s * stride_bytes;
    for (uint32_t c = 0; c < b.ch; ++c) {
      const float in_c = audio_filter::LoadF(p_in_s + 4u * c);
      if ((spk_mask & (1u << c)) == 0u) {
        audio_filter::StoreF(p_out_s + 4u * c, in_c);
        continue;
      }
      float er_even = 0.0f, er_odd = 0.0f, tk_even = 0.0f, tk_odd = 0.0f,
            er_tail = 0.0f, tk_tail = 0.0f;
      uint32_t idx = c;
      uint32_t k = 0u;
      for (; k + 1u < per_ch_taps; k += 2u) {
        er_even += er_taps[idx];
        er_odd += er_taps[idx + b.ch];
        p.late_r_b[idx] = (p.late_r_b[idx] - 1u) & b.tank_mask[idx];
        tk_even += audio_filter::LoadF(p_tank[idx] + 4u * p.late_r_b[idx]);
        const uint32_t idx2 = idx + b.ch;
        p.late_r_b[idx2] = (p.late_r_b[idx2] - 1u) & b.tank_mask[idx2];
        tk_odd += audio_filter::LoadF(p_tank[idx2] + 4u * p.late_r_b[idx2]);
        idx += 2u * b.ch;
      }
      if (k < per_ch_taps) {
        er_tail = er_taps[idx];
        p.late_r_b[idx] = (p.late_r_b[idx] - 1u) & b.tank_mask[idx];
        tk_tail = audio_filter::LoadF(p_tank[idx] + 4u * p.late_r_b[idx]);
      }
      const float er_sum = (er_tail + er_odd) + er_even;
      const float late_sum = (tk_tail + tk_odd) + tk_even;
      const float mixed = (dry_gain == 0.0f)
                              ? (er_sum + late_sum)
                              : (std::fmaf(in_c, dry_gain, er_sum) + late_sum);
      audio_filter::StoreF(p_out_s + 4u * c, mixed);
    }
  }

#if defined(__aarch64__)
  audio_dsp::StoreF4BE(p_vec_a_out + 0u, va0);
  audio_dsp::StoreF4BE(p_vec_a_out + 16u, va1);
  audio_dsp::StoreF4BE(p_vec_b_out + 0u, last_u0p);
  audio_dsp::StoreF4BE(p_vec_b_out + 16u, last_u1p);
  audio_dsp::StoreF4BE(p_vec_b_out + 32u, last_u0pp);
  audio_dsp::StoreF4BE(p_vec_b_out + 48u, last_u1pp);
  audio_dsp::StoreF4BE(p_vec_b_out + 64u, last_w0);
  audio_dsp::StoreF4BE(p_vec_b_out + 80u, last_w1);
#else
  for (uint32_t t = 0; t < 8u; ++t)
    audio_filter::StoreF(p_vec_a_out + 4u * t, va[t]);
  for (uint32_t k = 0; k < 4u; ++k) {
    audio_filter::StoreF(p_vec_b_out + 0u + 4u * k, last_u0p[k]);
    audio_filter::StoreF(p_vec_b_out + 16u + 4u * k, last_u1p[k]);
    audio_filter::StoreF(p_vec_b_out + 32u + 4u * k, last_u0pp[k]);
    audio_filter::StoreF(p_vec_b_out + 48u + 4u * k, last_u1pp[k]);
    audio_filter::StoreF(p_vec_b_out + 64u + 4u * k, last_w0[k]);
    audio_filter::StoreF(p_vec_b_out + 80u + 4u * k, last_w1[k]);
  }
#endif
}

inline bool TryRun(PPCContext &__restrict ctx, uint8_t *base) {
  ReverbBuffers b{};
  if (!Validate(base, ctx, b)) [[unlikely]]
    return false;

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
  for (uint32_t t = 0; t < 8u; ++t)
    ::banjo::hot::Card(base, b.tank_buf_addr[t], (b.tank_mask[t] + 1u) * 4u);

  uint8_t *ps = ::banjo::hot::Raw(base, b.self);
  uint8_t *p_in = ::banjo::hot::Raw(base, b.in_addr);
  uint8_t *p_out = ::banjo::hot::Raw(base, b.out_addr);
  uint8_t *p_mono = ::banjo::hot::Raw(base, b.mono_addr);
  uint8_t *p_vec_a = ::banjo::hot::Raw(base, b.vec_a_addr);
  uint8_t *p_vec_b = ::banjo::hot::Raw(base, b.vec_b_addr);
  uint8_t *p_early = ::banjo::hot::Raw(base, b.early_buf_addr);
  uint8_t *p_er = ::banjo::hot::Raw(base, b.er_buf_addr);
  uint8_t *p_ap0 = ::banjo::hot::Raw(base, b.ap0_addr);
  uint8_t *p_ap1 = ::banjo::hot::Raw(base, b.ap1_addr);
  uint8_t *p_tank[8];
  for (uint32_t t = 0; t < 8u; ++t)
    p_tank[t] = ::banjo::hot::Raw(base, b.tank_buf_addr[t]);

  // Stage 1 & 2: Anti-denormal DC flip-flop + channel-to-mono downmix.
  AddAntiDenormalDC(p_in, b.ch, b.nsamples);
  DownmixToMono(p_in, p_mono, b.ch, b.nsamples);

  // Stage 3a: Synchronize pre-loop state and ramp flags (mirrors
  // loc_82D60F50..loc_82D61194).
  ctx.fpscr.disableFlushMode();
  ReverbParams p{};
  bool any_ramp = false;
  p.early_w = Load32P(ps + 1220u);
  for (uint32_t i = 0; i < 9u; ++i) {
    const uint32_t target = Load32P(ps + 1128u + 4u * i);
    const uint32_t prev = Load32P(ps + 1164u + 4u * i);
    p.early_r[i] = (target + p.early_w - 1u) & b.early_mask;
    audio_filter::Store32(ps + 1224u + 4u * i, p.early_r[i]);
    if (target == prev) {
      ps[1200u + i] = 0u;
    } else {
      ps[1200u + i] = 1u;
      audio_filter::Store32(ps + 1164u + 4u * i, target);
      p.early_r_old[i] = (prev + p.early_w - 1u) & b.early_mask;
      audio_filter::Store32(ps + 1260u + 4u * i, p.early_r_old[i]);
      p.early_ramp[i] = true;
      any_ramp = true;
    }
  }

  for (uint32_t t = 0; t < 8u; ++t) {
    p.late_w[t] = Load32P(ps + 956u + 4u * t);
    const uint32_t tdel_a = Load32P(ps + 788u + 4u * t);
    const uint32_t pdel_a = Load32P(ps + 820u + 4u * t);
    const uint32_t tdel_b = Load32P(ps + 852u + 4u * t);
    p.late_r_a[t] = (tdel_a + p.late_w[t] - 1u) & b.tank_mask[t];
    p.late_r_b[t] = (tdel_b + p.late_w[t] - 1u) & b.tank_mask[t];
    audio_filter::Store32(ps + 988u + 4u * t, p.late_r_a[t]);
    audio_filter::Store32(ps + 1052u + 4u * t, p.late_r_b[t]);
    bool t_ramp = (tdel_a != pdel_a);
    if (t_ramp) {
      ps[916u + t] = 1u;
      audio_filter::Store32(ps + 820u + 4u * t, tdel_a);
      p.late_r_a_old[t] = (pdel_a + p.late_w[t] - 1u) & b.tank_mask[t];
      audio_filter::Store32(ps + 1020u + 4u * t, p.late_r_a_old[t]);
    } else {
      ps[916u + t] = 0u;
    }

    p.fb_gain[t] = audio_filter::LoadF(ps + 508u + 4u * t);
    const float fb_prev = audio_filter::LoadF(ps + 572u + 4u * t);
    audio_filter::StoreF(ps + 540u + 4u * t, p.fb_gain[t]);
    if (p.fb_gain[t] == fb_prev) {
      p.fb_old[t] = p.fb_gain[t];
      audio_filter::StoreF(ps + 604u + 4u * t, p.fb_gain[t]);
    } else {
      if (!t_ramp) {
        t_ramp = true;
        ps[916u + t] = 1u;
        p.late_r_a_old[t] = (pdel_a + p.late_w[t] - 1u) & b.tank_mask[t];
        audio_filter::Store32(ps + 1020u + 4u * t, p.late_r_a_old[t]);
      }
      p.fb_old[t] = fb_prev;
      audio_filter::StoreF(ps + 604u + 4u * t, fb_prev);
      audio_filter::StoreF(ps + 572u + 4u * t, p.fb_gain[t]);
    }
    p.tank_ramp[t] = t_ramp;
    if (t_ramp)
      any_ramp = true;

    p.lp_g[t] = audio_filter::LoadF(ps + 76u + 4u * t);
    const float lp_prev = audio_filter::LoadF(ps + 212u + 4u * t);
    p.lp_omg[t] = 1.0f - p.lp_g[t];
    audio_filter::StoreF(ps + 116u + 4u * t, p.lp_g[t]);
    audio_filter::StoreF(ps + 148u + 4u * t, p.lp_omg[t]);
    if (p.lp_g[t] == lp_prev) {
      ps[108u + t] = 0u;
      p.lp_old[t] = p.lp_g[t];
      p.lp_ramp[t] = false;
    } else {
      ps[108u + t] = 1u;
      p.lp_old[t] = lp_prev;
      p.lp_ramp[t] = true;
      audio_filter::StoreF(ps + 180u + 4u * t, lp_prev);
      audio_filter::StoreF(ps + 212u + 4u * t, p.lp_g[t]);
      any_ramp = true;
    }
  }

  auto init_scalar_ramp = [&](uint32_t off_t, uint32_t off_p, uint32_t off_flag,
                              uint32_t off_cur, uint32_t off_old,
                              float &cur_out, float &old_out, bool &ramp_out) {
    cur_out = audio_filter::LoadF(ps + off_t);
    const float prev = audio_filter::LoadF(ps + off_p);
    audio_filter::StoreF(ps + off_cur, cur_out);
    if (cur_out == prev) {
      ps[off_flag] = 0u;
      old_out = cur_out;
      ramp_out = false;
    } else {
      ps[off_flag] = 1u;
      audio_filter::StoreF(ps + off_old, prev);
      audio_filter::StoreF(ps + off_p, cur_out);
      old_out = prev;
      ramp_out = true;
      any_ramp = true;
    }
  };

  init_scalar_ramp(660u, 676u, 664u, 668u, 672u, p.hf_gain, p.hf_old,
                   p.hf_ramp);
  init_scalar_ramp(656u, 692u, 680u, 684u, 688u, p.late_gain, p.late_old,
                   p.late_ramp);
  init_scalar_ramp(16u, 32u, 20u, 24u, 28u, p.in_lpf_g, p.in_lpf_old,
                   p.in_lpf_ramp);
  p.in_lpf_omg = 1.0f - p.in_lpf_g;
  init_scalar_ramp(636u, 652u, 640u, 644u, 648u, p.early_gain, p.early_old,
                   p.early_gain_ramp);
  init_scalar_ramp(696u, 712u, 700u, 704u, 708u, p.diff_gain, p.diff_old,
                   p.diff_ramp);

  p.lp1 = audio_filter::LoadF(ps + 36u);
  p.lp2 = audio_filter::LoadF(ps + 40u);
  p.bq_b0 = audio_filter::LoadF(ps + 44u);
  p.bq_b1 = audio_filter::LoadF(ps + 48u);
  p.bq_b2 = audio_filter::LoadF(ps + 52u);
  p.bq_a1 = audio_filter::LoadF(ps + 56u);
  p.bq_a2 = audio_filter::LoadF(ps + 60u);
  p.use_bq = (audio_filter::LoadF(ps + 64u) != 0.0f);
  p.bq_s0 = audio_filter::LoadF(ps + 68u);
  p.bq_s1 = audio_filter::LoadF(ps + 72u);

  p.ap_k = audio_filter::LoadF(ps + 716u);
  p.ap0_r = Load32P(ps + 1444u);
  p.ap0_w = Load32P(ps + 1436u);
  p.ap1_r = Load32P(ps + 1448u);
  p.ap1_w = Load32P(ps + 1440u);

  p.er_w = Load32P(ps + 1364u);
  for (uint32_t i = 0; i < 7u; ++i)
    p.er_r[i] = Load32P(ps + 1368u + 4u * i);

  if (any_ramp) {
    RunSampleLoop<true>(b, p_in, p_out, p_mono, p_vec_a, p_vec_a, p_vec_b,
                        p_early, p_er, p_ap0, p_ap1, p_tank, p, spk_mask,
                        dry_gain);
  } else {
    RunSampleLoop<false>(b, p_in, p_out, p_mono, p_vec_a, p_vec_a, p_vec_b,
                         p_early, p_er, p_ap0, p_ap1, p_tank, p, spk_mask,
                         dry_gain);
  }

  // Write back modified scalar state and ring indices.
  audio_filter::StoreF(ps + 36u, p.lp1);
  audio_filter::StoreF(ps + 40u, p.lp2);
  if (p.use_bq) {
    audio_filter::StoreF(ps + 68u, p.bq_s0);
    audio_filter::StoreF(ps + 72u, p.bq_s1);
  }
  audio_filter::Store32(ps + 1220u, p.early_w);
  for (uint32_t i = 0; i < 9u; ++i) {
    audio_filter::Store32(ps + 1224u + 4u * i, p.early_r[i]);
    if (p.early_ramp[i])
      audio_filter::Store32(ps + 1260u + 4u * i, p.early_r_old[i]);
  }
  audio_filter::Store32(ps + 1444u, p.ap0_r);
  audio_filter::Store32(ps + 1436u, p.ap0_w);
  audio_filter::Store32(ps + 1448u, p.ap1_r);
  audio_filter::Store32(ps + 1440u, p.ap1_w);
  audio_filter::Store32(ps + 1364u, p.er_w);
  for (uint32_t i = 0; i < 7u; ++i)
    audio_filter::Store32(ps + 1368u + 4u * i, p.er_r[i]);
  for (uint32_t t = 0; t < 8u; ++t) {
    audio_filter::Store32(ps + 956u + 4u * t, p.late_w[t]);
    audio_filter::Store32(ps + 988u + 4u * t, p.late_r_a[t]);
    if (p.tank_ramp[t])
      audio_filter::Store32(ps + 1020u + 4u * t, p.late_r_a_old[t]);
    audio_filter::Store32(ps + 1052u + 4u * t, p.late_r_b[t]);
  }
  return true;
}

inline bool VerifyAndRun(PPCContext &__restrict ctx, uint8_t *base,
                         void (*ref_fn)(PPCContext &, uint8_t *)) {
  ReverbBuffers b{};
  if (!Validate(base, ctx, b))
    return false;

  struct Snap {
    uint32_t addr;
    std::vector<uint8_t> data;
  };
  std::vector<Snap> snaps;
  auto push = [&](uint32_t addr, uint32_t len) {
    const uint8_t *p = ::banjo::hot::Raw(base, addr);
    snaps.push_back({addr, std::vector<uint8_t>(p, p + len)});
  };
  push(b.self, 1452u);
  push(b.in_addr, b.io_bytes);
  if (b.out_addr != b.in_addr)
    push(b.out_addr, b.io_bytes);
  push(b.mono_addr, b.mono_bytes);
  push(b.vec_a_addr, 32u);
  push(b.vec_b_addr, 96u);
  push(b.early_buf_addr, (b.early_mask + 1u) * 4u);
  push(b.er_buf_addr, (b.er_mask + 1u) * 4u);
  push(b.ap0_addr, (b.ap0_mask + 1u) * 4u);
  push(b.ap1_addr, (b.ap1_mask + 1u) * 4u);
  for (uint32_t t = 0; t < 8u; ++t)
    push(b.tank_buf_addr[t], (b.tank_mask[t] + 1u) * 4u);

  const PPCContext saved_ctx = ctx;
  if (!TryRun(ctx, base)) {
    for (const auto &sn : snaps)
      std::memcpy(::banjo::hot::Raw(base, sn.addr), sn.data.data(),
                  sn.data.size());
    ctx = saved_ctx;
    return false;
  }

  std::vector<uint8_t> native_out(b.io_bytes);
  std::memcpy(native_out.data(), ::banjo::hot::Raw(base, b.out_addr),
              b.io_bytes);

  for (const auto &sn : snaps)
    std::memcpy(::banjo::hot::Raw(base, sn.addr), sn.data.data(),
                sn.data.size());
  ctx = saved_ctx;
  ref_fn(ctx, base);

  float max_diff = 0.0f;
  uint32_t bad_idx = 0;
  const uint8_t *ref_out = ::banjo::hot::Raw(base, b.out_addr);
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
    REXLOG_WARN(
        "[hot] FMOD reverb verify mismatch ch={} n={} idx={} max_diff={}", b.ch,
        b.nsamples, bad_idx, max_diff);
  }
  return true;
}

inline void LogOnce(bool active) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true, std::memory_order_relaxed)) {
    REXLOG_INFO("[hot] native FMOD SFX reverb (banjo_native_fmod_reverb): {}",
                active ? "ON" : "off");
  }
}

} // namespace banjo::hand::n_82D60CB8

namespace banjo::hot::n_82D60CB8 {
void Native(PPCContext &ctx, uint8_t *base);
}

REX_EXTERN(__imp__sub_829CF3F0);
REX_EXTERN(sub_82D71908);

REXCVAR_DEFINE_BOOL(
    deadpool_acquire_async_read, true, "Banjo/Enhancements",
    "Acquire semantics on FArchiveAsync::Serialize (sub_823E55A0) "
    "precache-pending counters and non-positive chunk "
    "guard; prevents reading stale decompressed streaming buffers on AArch64")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace banjo::hand::n_823E55A0 {

[[gnu::always_inline]] inline uint32_t LoadAcquireBE32(uint8_t *base,
                                                       uint32_t addr) noexcept {
  const auto *ptr = reinterpret_cast<const uint32_t *>(
      base + uint64_t(addr) + REX_PHYS_HOST_OFFSET(addr));
  return __builtin_bswap32(__atomic_load_n(ptr, __ATOMIC_ACQUIRE));
}

[[gnu::always_inline]] inline void
WaitAsyncCounter(PPCContext &ctx, uint8_t *base, uint32_t counter_addr) {
  constexpr uint32_t kMaxSpins =
      5000u; // ~5s bound so async worker failures never hard-hang the console
  for (uint32_t spins = 0; LoadAcquireBE32(base, counter_addr) != 0u; ++spins) {
    if (spins >= kMaxSpins) [[unlikely]] {
      auto *ptr = reinterpret_cast<uint32_t *>(
          base + uint64_t(counter_addr) + REX_PHYS_HOST_OFFSET(counter_addr));
      const uint32_t stuck =
          __builtin_bswap32(__atomic_load_n(ptr, __ATOMIC_ACQUIRE));
      REXLOG_WARN("[hot] WaitAsyncCounter timeout after {}ms at {:#x} "
                  "(val={}); clearing counter",
                  kMaxSpins, counter_addr, stuck);
      __atomic_store_n(ptr, 0u, __ATOMIC_RELEASE);
      ::banjo::hot::Card(base, counter_addr, 4u);
      break;
    }
    ctx.f1.f64 = rex::ppc::SingleToDoubleAnyMode(0x3A83126Fu); // 0.001f seconds
    __imp__sub_829CF3F0(ctx, base);
  }
}

inline bool TryRun(PPCContext &__restrict ctx, uint8_t *base) {
  uint32_t self = ctx.r3.u32;
  uint32_t dst = ctx.r4.u32;
  int32_t rem = ctx.r5.s32;
  if (rem <= 0) {
    return true;
  }

  // Reserve the guest 144-byte stack frame before calling Sleep / Precache /
  // XMemCpy.
  const uint32_t sp_in = ctx.r1.u32;
  const uint32_t sp_frame = sp_in - 144u;
  REX_STORE_U32_C(sp_frame, sp_in);
  ctx.r1.u32 = sp_frame;

  while (rem > 0) {
    const int32_t cur_pos = static_cast<int32_t>(REX_LOAD_D_U32(self, 172));
    const int32_t buf_start = static_cast<int32_t>(REX_LOAD_D_U32(self, 184));
    const int32_t buf_end = static_cast<int32_t>(REX_LOAD_D_U32(self, 192));
    if (cur_pos < buf_start || cur_pos >= buf_end) {
      const int32_t precached_start =
          static_cast<int32_t>(REX_LOAD_D_U32(self, 176));
      int32_t req_size = rem;
      if (cur_pos >= precached_start) {
        const int32_t precached_len =
            static_cast<int32_t>(REX_LOAD_D_U32(self, 180));
        if (int32_t(uint32_t(cur_pos) + uint32_t(rem)) <=
            int32_t(uint32_t(precached_len) + uint32_t(precached_start))) {
          req_size = int32_t((uint32_t(precached_len) - uint32_t(cur_pos)) +
                             uint32_t(precached_start));
        }
      }
      WaitAsyncCounter(ctx, base, self + 208u);
      WaitAsyncCounter(ctx, base, self + 212u);

      const uint32_t vtable = REX_LOAD_D_U32(self, 0);
      const uint32_t precache_fn = REX_LOAD_D_U32(vtable, 76);
      ctx.r3.u64 = self;
      ctx.r4.u64 = REX_LOAD_D_U32(self, 172);
      ctx.r5.u64 = uint32_t(req_size);
      REX_CALL_INDIRECT_FUNC(precache_fn);
    }

    // Acquire counter == 0 so async decompression stores to BufferPtr /
    // BufferStartPos / BufferEndPos are visible.
    WaitAsyncCounter(ctx, base, self + 208u);

    const uint32_t cur_pos_u = REX_LOAD_D_U32(self, 172);
    const int32_t cur_pos_s = static_cast<int32_t>(cur_pos_u);
    const int32_t buf_end_after =
        static_cast<int32_t>(REX_LOAD_D_U32(self, 192));
    int32_t chunk = rem;
    if (int32_t(cur_pos_u + uint32_t(rem)) > buf_end_after) {
      chunk = buf_end_after - cur_pos_s;
    }
    if (chunk <= 0) [[unlikely]] {
      break;
    }

    const uint32_t buf_ptr = REX_LOAD_D_U32(self, 200);
    const uint32_t buf_start_after = REX_LOAD_D_U32(self, 184);
    const uint32_t src = (buf_ptr - buf_start_after) + cur_pos_u;
    ctx.r3.u64 = dst;
    ctx.r4.u64 = src;
    ctx.r5.u64 = uint32_t(chunk);
    sub_82D71908(ctx, base);

    const uint32_t cur_pos_post = REX_LOAD_D_U32(self, 172);
    rem -= chunk;
    dst += uint32_t(chunk);
    REX_STORE_D_U32_C(self, 172, cur_pos_post + uint32_t(chunk));
  }

  ctx.r1.u32 = sp_in;
  return true;
}

} // namespace banjo::hand::n_823E55A0

REXCVAR_DEFINE_BOOL(deadpool_native_physx_solver, true, "Banjo/Enhancements",
                    "Native ARM64 NEON implementations of PhysX PGS "
                    "contact/angular/friction solvers "
                    "(sub_8332BF18, sub_8332CDF8, sub_8332D9E8)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace banjo::hand::physx {

[[gnu::always_inline]] inline uint16_t Load16(const uint8_t *base,
                                              uint32_t ea) noexcept {
  uint16_t v;
  std::memcpy(&v, ::banjo::hot::Raw(const_cast<uint8_t *>(base), ea), 2);
  return __builtin_bswap16(v);
}

[[gnu::always_inline]] inline void Store16(uint8_t *base, uint32_t ea,
                                           uint16_t v) noexcept {
  const uint16_t raw = __builtin_bswap16(v);
  std::memcpy(::banjo::hot::Raw(base, ea), &raw, 2);
}

[[gnu::always_inline]] inline float LoadFAt(const uint8_t *base,
                                            uint32_t ea) noexcept {
  return audio_filter::LoadF(
      ::banjo::hot::Raw(const_cast<uint8_t *>(base), ea));
}

[[gnu::always_inline]] inline void StoreFAt(uint8_t *base, uint32_t ea,
                                            float v) noexcept {
  audio_filter::StoreF(::banjo::hot::Raw(base, ea), v);
}

#if defined(__aarch64__)
[[gnu::always_inline]] inline float32x4_t
LoadVec4Aligned(const uint8_t *base, uint32_t ea) noexcept {
  return vreinterpretq_f32_u8(vrev32q_u8(
      vld1q_u8(::banjo::hot::Raw(const_cast<uint8_t *>(base), ea & ~0xFu))));
}

[[gnu::always_inline]] inline void StoreVec4Aligned(uint8_t *base, uint32_t ea,
                                                    float32x4_t v) noexcept {
  vst1q_u8(::banjo::hot::Raw(base, ea & ~0xFu),
           vrev32q_u8(vreinterpretq_u8_f32(v)));
}

[[gnu::always_inline]] inline float32x4_t
LoadVec3Unaligned(const uint8_t *base, uint32_t ea) noexcept {
  const float tmp[4] = {
      LoadFAt(base, ea),
      LoadFAt(base, ea + 4u),
      LoadFAt(base, ea + 8u),
      0.0f,
  };
  return vld1q_f32(tmp);
}

[[gnu::always_inline]] inline float VnmsubS(float a, float b,
                                            float c) noexcept {
  return -std::fmaf(a, b, -c);
}

[[gnu::always_inline]] inline float32x4_t VnmsubV(float32x4_t a, float32x4_t b,
                                                  float32x4_t c) noexcept {
  return vnegq_f32(vfmaq_f32(vnegq_f32(c), a, b));
}

[[gnu::always_inline]] inline float ClampFmaxmin(float min_v, float max_v,
                                                 float x) noexcept {
  const float32x2_t mx = vmax_f32(vdup_n_f32(min_v), vdup_n_f32(x));
  return vget_lane_f32(vmin_f32(vdup_n_f32(max_v), mx), 0);
}

[[gnu::always_inline]] inline float Dot3(float32x4_t a,
                                         float32x4_t b) noexcept {
  const float32x4_t prod = vmulq_f32(a, b);
  return (0.0f + vgetq_lane_f32(prod, 2)) +
         (vgetq_lane_f32(prod, 1) + vgetq_lane_f32(prod, 0));
}
#endif // defined(__aarch64__)

// 1. sub_8332BF18 - PhysX PGS linear + angular contact/joint solver (128-byte
// rows).
inline bool TryRun_8332BF18(PPCContext &__restrict ctx, uint8_t *base) {
#if defined(__aarch64__)
  if (!bool(REXCVAR_GET(deadpool_native_physx_solver)) || !base) [[unlikely]]
    return false;
  const uint32_t hdr = ctx.r3.u32;
  if (hdr < 0x10000u || (hdr & 0xFu) != 0u || !detail::Straight(hdr, 32u))
    return false;
  const uint8_t *hdr_p = ::banjo::hot::Raw(base, hdr);
  const uint32_t count = uint32_t(hdr_p[1]) + 1u;
  const uint64_t total_bytes = 32ull + uint64_t(count) * 128ull;
  if (total_bytes > 0x1000000ull ||
      !detail::Straight(hdr, static_cast<uint32_t>(total_bytes)))
    return false;
  const uint32_t body0_va = audio_filter::Load32(base, hdr + 4u);
  const uint32_t body1_va = audio_filter::Load32(base, hdr + 8u);
  if (body0_va < 0x10000u || (body0_va & 0xFu) != 0u ||
      !detail::Straight(body0_va, 48u) || body1_va < 0x10000u ||
      (body1_va & 0xFu) != 0u || !detail::Straight(body1_va, 48u)) {
    return false;
  }

  const uint32_t flags = uint32_t(hdr_p[2]);
  const bool has_body0 = (flags & 0x10u) != 0u;
  const bool has_body1 = (flags & 0x20u) != 0u;

  ctx.fpscr.enableFlushMode();

  const float32x4_t v_invM0 = vdupq_n_f32(LoadFAt(base, body0_va + 44u));
  const float32x4_t v_invM1 = vdupq_n_f32(LoadFAt(base, body1_va + 44u));

  float32x4_t linVel0 = LoadVec4Aligned(base, body0_va);
  float32x4_t angVel0 = LoadVec4Aligned(base, body0_va + 16u);
  float32x4_t linVel1 = LoadVec4Aligned(base, body1_va);
  float32x4_t angVel1 = LoadVec4Aligned(base, body1_va + 16u);

  uint32_t row = hdr + 32u;
  for (uint32_t i = 0; i < count; ++i, row += 128u) {
    const float32x4_t v61 = LoadVec4Aligned(base, row - 16u);
    const float32x4_t v60 = LoadVec4Aligned(base, row);
    const float32x4_t v11 = LoadVec4Aligned(base, row + 16u);
    const float32x4_t v59 = LoadVec4Aligned(base, row + 32u);
    const float32x4_t v26 = LoadVec4Aligned(base, row + 48u);
    const float32x4_t v56 = LoadVec4Aligned(base, row + 64u);
    const float32x4_t v0 = LoadVec4Aligned(base, row + 80u);

    float32x4_t t = vmulq_f32(v61, linVel0);
    t = vfmaq_f32(t, v11, angVel0);
    t = VnmsubV(v60, linVel1, t);
    t = VnmsubV(v59, angVel1, t);

    const float normalVel = (0.0f + vgetq_lane_f32(t, 2)) +
                            (vgetq_lane_f32(t, 1) + vgetq_lane_f32(t, 0));

    const float applied = vgetq_lane_f32(v0, 0);
    const float maxImp = vgetq_lane_f32(v0, 1);
    const float minImp = vgetq_lane_f32(v0, 2);
    const float targetV = vgetq_lane_f32(v0, 3);

    const float k1 = vgetq_lane_f32(v61, 3);
    const float k57 = vgetq_lane_f32(v60, 3);
    const float k31 = vgetq_lane_f32(v11, 3);
    const float k55 = vgetq_lane_f32(v59, 3);
    const float k25 = vgetq_lane_f32(v26, 3);
    const float w56 = vgetq_lane_f32(v56, 3);

    const float s50 = targetV - normalVel;
    const float s5 = k57 * s50;
    const float s4 = VnmsubS(k1, k31, s5);
    const float s5b = VnmsubS(w56, k55, s5);
    const float s7 = VnmsubS(applied, k55, s4);

    const float new_w56 = std::fmaf(s5b, k25, w56);
    StoreFAt(base, (row + 76u) & ~3u, new_w56);

    const float cand = std::fmaf(s7, k25, applied);
    const float clamped = ClampFmaxmin(minImp, maxImp, cand);
    StoreFAt(base, (row + 80u) & ~3u, clamped);

    const float delta = clamped - applied;
    if (has_body0) {
      const float32x4_t vd = vdupq_n_f32(delta);
      linVel0 = vfmaq_f32(linVel0, v_invM0, vmulq_f32(vd, v61));
      angVel0 = vfmaq_f32(angVel0, v26, vd);
    }
    if (has_body1) {
      const float32x4_t vd = vdupq_n_f32(delta);
      linVel1 = VnmsubV(vmulq_f32(vd, v60), v_invM1, linVel1);
      angVel1 = VnmsubV(v56, vd, angVel1);
    }
  }

  ::banjo::hot::Card(base, hdr + 32u, static_cast<uint32_t>(count * 128u));
  StoreVec4Aligned(base, body0_va, linVel0);
  StoreVec4Aligned(base, body0_va + 16u, angVel0);
  ::banjo::hot::Card(base, body0_va, 32u);
  StoreVec4Aligned(base, body1_va, linVel1);
  StoreVec4Aligned(base, body1_va + 16u, angVel1);
  ::banjo::hot::Card(base, body1_va, 32u);
  ctx.r3.u64 = uint32_t(count << 7);
  return true;
#else
  (void)ctx;
  (void)base;
  return false;
#endif
}

// 2. sub_8332CDF8 - PhysX PGS angular-only contact/joint solver (128-byte rows,
// 3x3 inertia).
inline bool TryRun_8332CDF8(PPCContext &__restrict ctx, uint8_t *base) {
#if defined(__aarch64__)
  if (!bool(REXCVAR_GET(deadpool_native_physx_solver)) || !base) [[unlikely]]
    return false;
  const uint32_t hdr = ctx.r3.u32;
  if (hdr < 0x10000u || (hdr & 0xFu) != 0u || !detail::Straight(hdr, 32u))
    return false;
  const uint8_t *hdr_p = ::banjo::hot::Raw(base, hdr);
  const uint32_t count = uint32_t(hdr_p[1]) + 1u;
  const uint64_t total_bytes = 32ull + uint64_t(count) * 128ull;
  if (total_bytes > 0x1000000ull ||
      !detail::Straight(hdr, static_cast<uint32_t>(total_bytes)))
    return false;
  const uint32_t body0_va = audio_filter::Load32(base, hdr + 4u);
  const uint32_t body1_va = audio_filter::Load32(base, hdr + 8u);
  if (body0_va < 0x10000u || (body0_va & 3u) != 0u ||
      !detail::Straight(body0_va, 96u) || body1_va < 0x10000u ||
      (body1_va & 3u) != 0u || !detail::Straight(body1_va, 96u)) {
    return false;
  }

  const uint32_t flags = uint32_t(hdr_p[2]);
  const bool has_body0 = (flags & 0x10u) != 0u;
  const bool has_body1 = (flags & 0x20u) != 0u;

  ctx.fpscr.enableFlushMode();

  float32x4_t angVel0 = LoadVec3Unaligned(base, body0_va + 16u);
  float32x4_t angVel1 = LoadVec3Unaligned(base, body1_va + 16u);

  float32x4_t I0_c0 = vdupq_n_f32(0.0f);
  float32x4_t I0_c1 = vdupq_n_f32(0.0f);
  float32x4_t I0_c2 = vdupq_n_f32(0.0f);
  if (has_body0) {
    I0_c0 = LoadVec3Unaligned(base, body0_va + 60u);
    I0_c1 = LoadVec3Unaligned(base, body0_va + 72u);
    I0_c2 = LoadVec3Unaligned(base, body0_va + 84u);
  }

  float32x4_t I1_c0 = vdupq_n_f32(0.0f);
  float32x4_t I1_c1 = vdupq_n_f32(0.0f);
  float32x4_t I1_c2 = vdupq_n_f32(0.0f);
  if (has_body1) {
    I1_c0 = LoadVec3Unaligned(base, body1_va + 60u);
    I1_c1 = LoadVec3Unaligned(base, body1_va + 72u);
    I1_c2 = LoadVec3Unaligned(base, body1_va + 84u);
  }

  uint32_t row = hdr + 32u;
  for (uint32_t i = 0; i < count; ++i, row += 128u) {
    const float32x4_t v47 = LoadVec4Aligned(base, row - 16u);
    const float32x4_t v46 = LoadVec4Aligned(base, row);

    const float32x4_t p0 = vmulq_f32(v47, angVel0);
    const float32x4_t p1 = vmulq_f32(v46, angVel1);
    const float dot0 = (0.0f + vgetq_lane_f32(p0, 2)) +
                       (vgetq_lane_f32(p0, 1) + vgetq_lane_f32(p0, 0));
    const float dot1 = (0.0f + vgetq_lane_f32(p1, 2)) +
                       (vgetq_lane_f32(p1, 1) + vgetq_lane_f32(p1, 0));

    const float k7 = vgetq_lane_f32(v47, 3);
    const float k43 = vgetq_lane_f32(v46, 3);

    const float s12 = LoadFAt(base, row + 44u);
    const float s5 = LoadFAt(base, row + 60u);
    const float s6 = LoadFAt(base, row + 76u);
    const float32x4_t v0 = LoadVec4Aligned(base, row + 80u);

    const float applied = vgetq_lane_f32(v0, 0);
    const float maxImp = vgetq_lane_f32(v0, 1);
    const float minImp = vgetq_lane_f32(v0, 2);
    const float targetV = vgetq_lane_f32(v0, 3);

    const float s39 = dot0 - dot1;
    const float s38 = targetV - s39;
    const float s4 = k43 * s38;
    const float s3 = VnmsubS(s6, s12, s4);
    const float new_s6 = std::fmaf(s3, s5, s6);
    StoreFAt(base, (row + 76u) & ~3u, new_s6);

    const float s9 = LoadFAt(base, row + 28u);
    const float s8 = VnmsubS(k7, s9, s4);
    const float s9b = VnmsubS(applied, s12, s8);
    const float cand = std::fmaf(s9b, s5, applied);
    const float clamped = ClampFmaxmin(minImp, maxImp, cand);
    StoreFAt(base, (row + 80u) & ~3u, clamped);

    const float delta = clamped - applied;
    if (has_body0) {
      const float32x4_t d0 = vmulq_f32(v47, vdupq_n_f32(delta));
      float32x4_t acc0 = vmulq_laneq_f32(I0_c0, d0, 0);
      acc0 = vfmaq_laneq_f32(acc0, I0_c1, d0, 1);
      acc0 = vfmaq_laneq_f32(acc0, I0_c2, d0, 2);
      angVel0 = vaddq_f32(angVel0, acc0);
    }
    if (has_body1) {
      const float32x4_t d1 = vmulq_f32(v46, vdupq_n_f32(delta));
      float32x4_t acc1 = vmulq_laneq_f32(I1_c0, d1, 0);
      acc1 = vfmaq_laneq_f32(acc1, I1_c1, d1, 1);
      acc1 = vfmaq_laneq_f32(acc1, I1_c2, d1, 2);
      angVel1 = vsubq_f32(angVel1, acc1);
    }
  }

  ::banjo::hot::Card(base, hdr + 32u, static_cast<uint32_t>(count * 128u));
  const uint32_t b0 = body0_va + 16u;
  const uint32_t b1 = body1_va + 16u;
  StoreFAt(base, (b0 + 0u) & ~3u, vgetq_lane_f32(angVel0, 0));
  StoreFAt(base, (b0 + 4u) & ~3u, vgetq_lane_f32(angVel0, 1));
  StoreFAt(base, (b0 + 8u) & ~3u, vgetq_lane_f32(angVel0, 2));
  ::banjo::hot::Card(base, b0, 12u);
  StoreFAt(base, (b1 + 0u) & ~3u, vgetq_lane_f32(angVel1, 0));
  StoreFAt(base, (b1 + 4u) & ~3u, vgetq_lane_f32(angVel1, 1));
  StoreFAt(base, (b1 + 8u) & ~3u, vgetq_lane_f32(angVel1, 2));
  ::banjo::hot::Card(base, b1, 12u);
  ctx.r3.u64 = uint32_t(count << 7);
  return true;
#else
  (void)ctx;
  (void)base;
  return false;
#endif
}

// 3. sub_8332D9E8 - PhysX PGS contact normal + Coulomb friction solver (96B
// contact, 80B friction rows).
inline bool TryRun_8332D9E8(PPCContext &__restrict ctx, uint8_t *base) {
#if defined(__aarch64__)
  if (!bool(REXCVAR_GET(deadpool_native_physx_solver)) || !base) [[unlikely]]
    return false;
  const uint32_t hdr = ctx.r3.u32;
  const uint32_t fric_va = ctx.r4.u32;
  if (hdr < 0x10000u || (hdr & 0xFu) != 0u || fric_va < 0x10000u ||
      !detail::Straight(hdr, 32u) || !detail::Straight(fric_va, 24u)) {
    return false;
  }

  const uint8_t *hdr_p = ::banjo::hot::Raw(base, hdr);
  const uint32_t numContacts = uint32_t(hdr_p[2]);
  const uint32_t numFriction = uint32_t(hdr_p[3]);
  const uint64_t total_bytes =
      32ull + uint64_t(numContacts) * 96ull + uint64_t(numFriction) * 80ull;
  if (total_bytes > 0x1000000ull ||
      !detail::Straight(hdr, static_cast<uint32_t>(total_bytes)))
    return false;

  const uint32_t contactEnd = hdr + 32u + numContacts * 96u;
  const uint32_t fricEnd = contactEnd + numFriction * 80u;
  const uint32_t body0_va = audio_filter::Load32(base, hdr + 4u);
  const uint32_t body1_va = audio_filter::Load32(base, hdr + 8u);
  if (body0_va < 0x10000u || (body0_va & 0xFu) != 0u ||
      !detail::Straight(body0_va, 48u) || body1_va < 0x10000u ||
      (body1_va & 0xFu) != 0u || !detail::Straight(body1_va, 48u)) {
    return false;
  }

  uint8_t *fric_p = ::banjo::hot::Raw(base, fric_va);
  const bool hasFricFlag = (fric_p[20] != 0u);
  if (hasFricFlag && numFriction > 0u) {
    for (uint32_t i = 0; i < numFriction; ++i) {
      const uint32_t q = contactEnd + i * 80u;
      if (Load16(base, q + 60u) == 0u) {
        return false; // uninitialized tangent frame: let guest copy handle
                      // sub_8332D040 lazy init
      }
    }
  }

  const uint8_t flags = hdr_p[1];
  const bool has_body0 = (flags & 0x10u) != 0u;
  const bool has_body1 = (flags & 0x20u) != 0u;

  ctx.fpscr.enableFlushMode();

  const float32x4_t v_invM0 = vdupq_n_f32(LoadFAt(base, body0_va + 44u));
  const float32x4_t v_invM1 = vdupq_n_f32(LoadFAt(base, body1_va + 44u));

  float32x4_t linVel0 = LoadVec4Aligned(base, body0_va);
  float32x4_t angVel0 = LoadVec4Aligned(base, body0_va + 16u);
  float32x4_t linVel1 = LoadVec4Aligned(base, body1_va);
  float32x4_t angVel1 = LoadVec4Aligned(base, body1_va + 16u);

  const uint32_t hdr_tag = audio_filter::Load32(base, hdr + 12u);
  float normalSum = 0.0f;
  if (hdr_tag != audio_filter::Load32(base, fric_va)) {
    const uint32_t tag_be = __builtin_bswap32(hdr_tag);
    std::memcpy(fric_p, &tag_be, 4);
    normalSum = 0.0f;
  } else {
    normalSum = LoadFAt(base, fric_va + 4u);
  }

  uint32_t p = hdr + 32u;
  for (uint32_t i = 0; i < numContacts; ++i, p += 96u) {
    const float32x4_t n = LoadVec4Aligned(base, p);
    const float32x4_t raXn = LoadVec4Aligned(base, p + 16u);
    const float32x4_t rbXn = LoadVec4Aligned(base, p + 32u);
    const float32x4_t delAng0 = LoadVec4Aligned(base, p + 48u);
    const float32x4_t delAng1 = LoadVec4Aligned(base, p + 64u);

    const float vn0 = Dot3(n, linVel0) + Dot3(raXn, angVel0);
    const float vn1 = Dot3(n, linVel1) + Dot3(rbXn, angVel1);
    const float vn = vn0 - vn1;

    const float targetVel = vgetq_lane_f32(delAng0, 3);
    const float velMul = vgetq_lane_f32(delAng1, 3);
    const float appliedF2 = vgetq_lane_f32(rbXn, 3);
    const float deltaF = (targetVel - vn) * velMul;
    StoreFAt(base, p + 44u, appliedF2 + deltaF);

    const float bias = vgetq_lane_f32(n, 3);
    const float appliedF = vgetq_lane_f32(raXn, 3);
    const float baseDelta = deltaF - bias;
    const float cand = appliedF + baseDelta;
    const bool neg = (0.0f > cand);
    const float clamped = neg ? 0.0f : cand;
    const float actualDelta = neg ? -appliedF : baseDelta;
    normalSum += clamped;
    StoreFAt(base, p + 28u, clamped);

    if (has_body0) {
      const float32x4_t vd = vdupq_n_f32(actualDelta);
      angVel0 = vfmaq_f32(angVel0, delAng0, vd);
      linVel0 = vfmaq_f32(linVel0, n, vmulq_f32(vd, v_invM0));
    }
    if (has_body1) {
      const float32x4_t vd = vdupq_n_f32(actualDelta);
      angVel1 = VnmsubV(delAng1, vd, angVel1);
      linVel1 = VnmsubV(n, vmulq_f32(vd, v_invM1), linVel1);
    }
  }

  StoreFAt(base, fric_va + 4u, normalSum);

  if (hasFricFlag && normalSum != 0.0f) {
    std::memset(fric_p, 0, 4);
    const float maxStatic = normalSum * LoadFAt(base, hdr + 16u);
    const float maxDynamic = normalSum * LoadFAt(base, hdr + 20u);
    uint32_t q = contactEnd;
    for (uint32_t i = 0; i < numFriction; ++i, q += 80u) {
      const float32x4_t t = LoadVec4Aligned(base, q);
      const float32x4_t raXt = LoadVec4Aligned(base, q + 16u);
      const float32x4_t rbXt = LoadVec4Aligned(base, q + 32u);
      const float32x4_t fDelAng0 = LoadVec4Aligned(base, q + 48u);
      const float32x4_t fDelAng1 = LoadVec4Aligned(base, q + 64u);

      const float vf = (Dot3(t, linVel0) + Dot3(raXt, angVel0)) -
                       (Dot3(t, linVel1) + Dot3(rbXt, angVel1));

      const float targetF = vgetq_lane_f32(t, 3);
      const float appliedF = vgetq_lane_f32(raXt, 3);
      const float velMulF = vgetq_lane_f32(fDelAng1, 3);
      float deltaF = -((targetF + vf) * velMulF);
      float newF = deltaF + appliedF;
      const float absNewF = std::fabs(newF);
      if (absNewF > maxStatic) {
        Store16(base, q + 62u, 1u);
        const float rcp = 1.0f / absNewF;
        newF = newF * (maxDynamic * rcp);
        deltaF = newF - appliedF;
      }
      StoreFAt(base, q + 28u, newF);

      if (has_body0) {
        const float32x4_t vd = vdupq_n_f32(deltaF);
        angVel0 = vfmaq_f32(angVel0, fDelAng0, vd);
        linVel0 = vfmaq_f32(linVel0, t, vmulq_f32(vd, v_invM0));
      }
      if (has_body1) {
        const float32x4_t vd = vdupq_n_f32(deltaF);
        angVel1 = VnmsubV(fDelAng1, vd, angVel1);
        linVel1 = VnmsubV(t, vmulq_f32(vd, v_invM1), linVel1);
      }
    }
  }

  ::banjo::hot::Card(base, fric_va, 24u);
  if (total_bytes > 32ull) {
    ::banjo::hot::Card(base, hdr + 32u,
                       static_cast<uint32_t>(total_bytes - 32ull));
  }
  StoreVec4Aligned(base, body0_va, linVel0);
  StoreVec4Aligned(base, body0_va + 16u, angVel0);
  ::banjo::hot::Card(base, body0_va, 32u);
  StoreVec4Aligned(base, body1_va, linVel1);
  StoreVec4Aligned(base, body1_va + 16u, angVel1);
  ::banjo::hot::Card(base, body1_va, 32u);
  ctx.r3.u64 = fricEnd - hdr;
  return true;
#else
  (void)ctx;
  (void)base;
  return false;
#endif
}

} // namespace banjo::hand::physx

namespace banjo::hot::n_823E55A0 {
void Native(PPCContext &ctx, uint8_t *base);
}
namespace banjo::hot::n_8332BF18 {
void Native(PPCContext &ctx, uint8_t *base);
}
namespace banjo::hot::n_8332CDF8 {
void Native(PPCContext &ctx, uint8_t *base);
}
namespace banjo::hot::n_8332D9E8 {
void Native(PPCContext &ctx, uint8_t *base);
}

namespace banjo::hot {

template <>
[[gnu::always_inline]] inline bool
TryCustomOverride<&n_82D60CB8::Native>(PPCContext &ctx, uint8_t *base) {
  const bool on =
      banjo::EnhancementActive(banjo::EnhancementGroup::kCpu,
                               bool(REXCVAR_GET(banjo_native_fmod_reverb)));
  ::banjo::hand::n_82D60CB8::LogOnce(on);
  if (!on) [[unlikely]]
    return false;
  if (bool(REXCVAR_GET(banjo_native_fmod_reverb_verify))) [[unlikely]] {
    return ::banjo::hand::n_82D60CB8::VerifyAndRun(ctx, base,
                                                   &n_82D60CB8::Native);
  }
  return ::banjo::hand::n_82D60CB8::TryRun(ctx, base);
}

template <>
[[gnu::always_inline]] inline bool
TryCustomOverride<&n_823E55A0::Native>(PPCContext &ctx, uint8_t *base) {
  if (!bool(REXCVAR_GET(deadpool_acquire_async_read))) [[unlikely]]
    return false;
  return ::banjo::hand::n_823E55A0::TryRun(ctx, base);
}

template <>
[[gnu::always_inline]] inline bool
TryCustomOverride<&n_8332BF18::Native>(PPCContext &ctx, uint8_t *base) {
  return ::banjo::hand::physx::TryRun_8332BF18(ctx, base);
}

template <>
[[gnu::always_inline]] inline bool
TryCustomOverride<&n_8332CDF8::Native>(PPCContext &ctx, uint8_t *base) {
  return ::banjo::hand::physx::TryRun_8332CDF8(ctx, base);
}

template <>
[[gnu::always_inline]] inline bool
TryCustomOverride<&n_8332D9E8::Native>(PPCContext &ctx, uint8_t *base) {
  return ::banjo::hand::physx::TryRun_8332D9E8(ctx, base);
}

} // namespace banjo::hot
