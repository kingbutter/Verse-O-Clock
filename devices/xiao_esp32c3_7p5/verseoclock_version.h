#pragma once

// -----------------------------------------------------------------------------
// Firmware version / identity
// -----------------------------------------------------------------------------
//
// Single source of truth for:
//   - DEVICE_ID: selects the correct OTA manifest + assets
//   - FW_VERSION: compared against the latest release version
//
// Recommended:
//   - Set DEVICE_ID to match devices.json "id" for this device.
//   - Set FW_VERSION to the Git tag (ex: "v25.12.0").
//
// CI tip: override FW_VERSION during compile:
//   -DFW_VERSION="\"v25.12.0\""
//
// If not overridden, FW_VERSION defaults to "dev".

#ifndef DEVICE_ID
  #define DEVICE_ID "xiao_esp32c3_7p5"
#endif

#ifndef FW_VERSION
  #define FW_VERSION "dev"
#endif

// -----------------------------------------------------------------------------
// Release safety guard
// -----------------------------------------------------------------------------
// If OTA is enabled, do not allow a release build with FW_VERSION="dev".
// This prevents accidentally shipping a dev firmware to GitHub Releases.
//
#if defined(ENABLE_HTTP_OTA)
  #if FW_VERSION[0] == 'd'
    #error "FW_VERSION is 'dev'. Release builds must define FW_VERSION explicitly."
  #endif
#endif
