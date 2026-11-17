/**
 * @file        boot_sec.cpp
 * @brief       Hardware device integrity and boot verification for Nintendo Switch
 *
 * @license     Proprietary - Anti-tamper runtime gate
 */

#include "boot_sec.h"

#if defined(__SWITCH__) || defined(REX_PLATFORM_SWITCH)

#include <switch.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>
#include <malloc.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#include <stb/stb_image.h>

extern "C" {
extern const uint8_t g_boot_sec_blob[];
extern const uint64_t g_boot_sec_blob_size;
}

namespace {

// Magic tag: "NXBOEVID" (Switch Boot Video)
constexpr uint64_t kMagic = 0x564944454F424E58ull;

constexpr uint64_t kKeyU64[4] = {
    0x3C559A10FE32894Bull,
    0x2301EFCDAB12887Eull,
    0xDCFEABCD89674545ull,
    0x55AA1032547698BAull
};

constexpr uint64_t kMult = 0x9E3779B97F4A7C15ull;
constexpr uint64_t kAdd = 0x85EBCA6B147B5413ull;

#pragma pack(push, 1)
struct BootSecHeader {
  uint64_t magic;
  uint32_t raw_payload_size;
  uint32_t enc_payload_size;
  uint32_t frame_count;
  uint16_t width;
  uint16_t height;
  uint16_t fps;
  uint16_t audio_channels;
  uint32_t audio_bytes;
  uint8_t sha256[32];
};
#pragma pack(pop)
static_assert(sizeof(BootSecHeader) == 64, "Header must be exactly 64 bytes");

// Self-contained SHA-256 implementation
namespace sha256_internal {

inline uint32_t ror(uint32_t x, uint32_t n) {
  return (x >> n) | (x << (32 - n));
}

inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
  return (x & y) ^ (~x & z);
}

inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
  return (x & y) ^ (x & z) ^ (y & z);
}

inline uint32_t sig0(uint32_t x) {
  return ror(x, 2) ^ ror(x, 13) ^ ror(x, 22);
}

inline uint32_t sig1(uint32_t x) {
  return ror(x, 6) ^ ror(x, 11) ^ ror(x, 25);
}

inline uint32_t gam0(uint32_t x) {
  return ror(x, 7) ^ ror(x, 18) ^ (x >> 3);
}

inline uint32_t gam1(uint32_t x) {
  return ror(x, 17) ^ ror(x, 19) ^ (x >> 10);
}

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

void Compute(const uint8_t* data, size_t len, uint8_t out[32]) {
  uint32_t h[8] = {
      0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
  };

  uint64_t bit_len = (uint64_t)len * 8;
  size_t padded_len = ((len + 8) / 64 + 1) * 64;
  uint8_t* padded = (uint8_t*)std::malloc(padded_len);
  if (!padded) return;
  std::memcpy(padded, data, len);
  padded[len] = 0x80;
  std::memset(padded + len + 1, 0, padded_len - len - 1 - 8);
  for (int i = 0; i < 8; ++i) {
    padded[padded_len - 1 - i] = (uint8_t)(bit_len >> (i * 8));
  }

  for (size_t chunk = 0; chunk < padded_len; chunk += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = ((uint32_t)padded[chunk + i * 4] << 24) |
             ((uint32_t)padded[chunk + i * 4 + 1] << 16) |
             ((uint32_t)padded[chunk + i * 4 + 2] << 8) |
             ((uint32_t)padded[chunk + i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      w[i] = gam1(w[i - 2]) + w[i - 7] + gam0(w[i - 15]) + w[i - 16];
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], h_val = h[7];
    for (int i = 0; i < 64; ++i) {
      uint32_t t1 = h_val + sig1(e) + ch(e, f, g) + K[i] + w[i];
      uint32_t t2 = sig0(a) + maj(a, b, c);
      h_val = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += h_val;
  }
  std::free(padded);

  for (int i = 0; i < 8; ++i) {
    out[i * 4]     = (uint8_t)(h[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
    out[i * 4 + 3] = (uint8_t)(h[i]);
  }
}

}  // namespace sha256_internal

void DecryptData(uint8_t* out, const uint8_t* in, size_t n) {
  size_t words = n / 8;
  size_t rem = n % 8;
  for (size_t i = 0; i < words; ++i) {
    uint64_t val;
    std::memcpy(&val, in + i * 8, 8);
    uint64_t k = kKeyU64[i & 3] ^ (i * kMult + kAdd);
    uint64_t res = val ^ k;
    std::memcpy(out + i * 8, &res, 8);
  }
  if (rem) {
    size_t offset = words * 8;
    uint64_t k = kKeyU64[words & 3] ^ (words * kMult + kAdd);
    uint8_t k_bytes[8];
    std::memcpy(k_bytes, &k, 8);
    for (size_t b = 0; b < rem; ++b) {
      out[offset + b] = in[offset + b] ^ k_bytes[b];
    }
  }
}

}  // namespace

bool VerifyDeviceIntegrityAndPlayBootSplash() {
  if (g_boot_sec_blob_size < sizeof(BootSecHeader)) {
    return false;
  }

  const BootSecHeader* header = reinterpret_cast<const BootSecHeader*>(g_boot_sec_blob);
  if (header->magic != kMagic) {
    return false;
  }
  if (header->enc_payload_size != (g_boot_sec_blob_size - sizeof(BootSecHeader))) {
    return false;
  }

  uint8_t* decrypted = static_cast<uint8_t*>(std::malloc(header->raw_payload_size));
  if (!decrypted) {
    return false;
  }

  DecryptData(decrypted, g_boot_sec_blob + sizeof(BootSecHeader), header->enc_payload_size);

  uint8_t computed_hash[32];
  sha256_internal::Compute(decrypted, header->raw_payload_size, computed_hash);
  if (std::memcmp(computed_hash, header->sha256, 32) != 0) {
    std::memset(decrypted, 0, header->raw_payload_size);
    std::free(decrypted);
    return false;
  }

  // Audio stream setup (48 kHz Stereo 16-bit PCM)
  bool audio_started = false;
  AudioOutBuffer audio_out_buf{};
  void* audio_mem = nullptr;
  if (header->audio_bytes > 0 && R_SUCCEEDED(audoutInitialize())) {
    if (audoutGetSampleRate() == 48000 && audoutGetChannelCount() == 2 &&
        audoutGetPcmFormat() == PcmFormat_Int16) {
      const uint32_t alloc_bytes = (header->audio_bytes + 0xFFF) & ~0xFFFu;
      audio_mem = memalign(0x1000, alloc_bytes);
      if (audio_mem) {
        std::memcpy(audio_mem, decrypted, header->audio_bytes);
        audio_out_buf.next = nullptr;
        audio_out_buf.buffer = audio_mem;
        audio_out_buf.buffer_size = alloc_bytes;
        audio_out_buf.data_size = header->audio_bytes;
        audio_out_buf.data_offset = 0;
        if (R_SUCCEEDED(audoutStartAudioOut()) &&
            R_SUCCEEDED(audoutAppendAudioOutBuffer(&audio_out_buf))) {
          audio_started = true;
        }
      }
    }
  }

  // Parse video frame table
  const uint8_t* frame_lens_ptr = decrypted + header->audio_bytes;
  const uint32_t* frame_lens = reinterpret_cast<const uint32_t*>(frame_lens_ptr);
  const uint8_t* frame_data = frame_lens_ptr + header->frame_count * sizeof(uint32_t);

  std::vector<const uint8_t*> frame_ptrs(header->frame_count);
  const uint8_t* curr = frame_data;
  for (uint32_t i = 0; i < header->frame_count; ++i) {
    frame_ptrs[i] = curr;
    curr += frame_lens[i];
  }

  // Display mandatory 6.0-second video via native libnx framebuffer
  NWindow* win = nwindowGetDefault();
  if (win && nwindowIsValid(win)) {
    Framebuffer fb;
    Result rc = framebufferCreate(&fb, win, 1280, 720, PIXEL_FORMAT_RGBA_8888, 2);
    if (R_SUCCEEDED(rc)) {
      framebufferMakeLinear(&fb);

      const u64 start_tick = armGetSystemTick();
      const u64 duration_ticks = armNsToTicks(6'000'000'000ull);  // 6.0s mandatory
      const u64 fps = header->fps > 0 ? header->fps : 25;
      const u64 frame_ticks = armNsToTicks(1'000'000'000ull / fps);
      u64 next_tick = start_tick;

      for (uint32_t i = 0; i < header->frame_count; ++i) {
        int w = 0, h = 0, comp = 0;
        stbi_uc* rgba = stbi_load_from_memory(frame_ptrs[i], static_cast<int>(frame_lens[i]), &w, &h, &comp, 4);
        if (rgba) {
          u32 stride = 0;
          void* dst = framebufferBegin(&fb, &stride);
          if (dst) {
            if (stride == 1280 * 4) {
              std::memcpy(dst, rgba, 1280 * 720 * 4);
            } else {
              for (int y = 0; y < 720; ++y) {
                std::memcpy(static_cast<uint8_t*>(dst) + y * stride,
                            rgba + y * 1280 * 4,
                            1280 * 4);
              }
            }
            framebufferEnd(&fb);
          }
          stbi_image_free(rgba);
        }

        next_tick += frame_ticks;
        const u64 now = armGetSystemTick();
        if (next_tick > now) {
          svcSleepThread(armTicksToNs(next_tick - now));
        }
      }

      // Ensure full 6.0s duration has elapsed
      while (armGetSystemTick() - start_tick < duration_ticks) {
        svcSleepThread(10'000'000);
      }

      framebufferClose(&fb);
    }
  }

  // Teardown audio
  if (audio_started) {
    audoutStopAudioOut();
    audoutExit();
  }
  if (audio_mem) {
    std::free(audio_mem);
  }

  // Securely wipe all decrypted buffers from RAM immediately
  std::memset(decrypted, 0, header->raw_payload_size);
  std::free(decrypted);

  return true;
}

#endif  // REX_PLATFORM_SWITCH
