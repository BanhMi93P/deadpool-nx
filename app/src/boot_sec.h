/**
 * @file        boot_sec.h
 * @brief       Hardware device integrity and boot verification for Nintendo Switch
 *
 * @license     Proprietary - Anti-tamper runtime gate
 */
#pragma once

#include <cstdint>

#if defined(__SWITCH__) || defined(REX_PLATFORM_SWITCH)
/**
 * @brief Verifies device integrity, checks anti-tamper security signatures,
 *        and presents the mandatory 4.0-second boot splash screen via native framebuffer.
 * @return true if integrity check passed and splash completed; false if tampered or corrupt.
 */
bool VerifyDeviceIntegrityAndPlayBootSplash();
#else
inline bool VerifyDeviceIntegrityAndPlayBootSplash() { return true; }
#endif

