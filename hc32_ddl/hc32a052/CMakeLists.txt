# SPDX-License-Identifier: Apache-2.0

# Global: soc.h and the DDL headers are included from the SoC and driver code
# that lives outside this directory.
zephyr_include_directories(
  soc
  drivers/inc
)

# Vendor code gets its own library so diagnostics can be relaxed for it only.
zephyr_library()

# Only the blocks needed to boot, print on USART and toggle GPIO are built.
# Add more driver sources here (or make them conditional) as drivers land.
zephyr_library_sources(
  drivers/src/ddl.c
  drivers/src/sysctrl.c
  drivers/src/gpio.c
  drivers/src/flash.c
  drivers/src/usart.c
)

# USART_SC_Init() (smart card mode, unused here) leaves a local variable
# uninitialised on one path. The vendor sources are kept verbatim, so silence
# the diagnostic instead of patching them.
target_compile_options(${ZEPHYR_CURRENT_LIBRARY} PRIVATE -Wno-maybe-uninitialized)
