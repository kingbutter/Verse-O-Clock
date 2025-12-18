#pragma once

#ifndef DEVICE_ID
  #define DEVICE_ID "xiao_esp32c3_7p5"
#endif

#ifndef FW_VERSION
  #define FW_VERSION "dev"
  #define FW_VERSION_IS_DEV 1
#endif

#if defined(RELEASE_BUILD)
  #if defined(FW_VERSION_IS_DEV)
    #error "FW_VERSION is 'dev'. Release builds must define FW_VERSION explicitly."
  #endif
#endif