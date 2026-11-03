# SPDX-License-Identifier: Apache-2.0
#
# XHSC HC32 out-of-tree port for Zephyr.
#
# This module provides, for every SoC it supports:
#   - the vendor DDL under hc32_ddl/<soc>/
#   - the SoC definition under soc/<series>/
#   - the boards under boards/<vendor>/
#   - the devicetree bindings under dts/bindings/
#   - the Zephyr drivers under drivers/<class>/
#
# zephyr/module.yml registers soc/, boards/ and dts/ as build roots, so an
# application only has to add this directory to ZEPHYR_EXTRA_MODULES.

add_subdirectory_ifdef(CONFIG_SOC_HC32A052 hc32_ddl/hc32a052)

# Zephyr creates the drivers__serial library unconditionally when CONFIG_SERIAL
# is enabled, and then warns about it being empty if no in-tree UART driver is
# selected. Hang the source on that library when it exists so an out-of-tree
# driver does not leave a dangling empty target; fall back to a module library
# otherwise (e.g. if CONFIG_SERIAL was off and the target was never created).
if(CONFIG_UART_HC32)
  if(TARGET drivers__serial)
    target_sources(drivers__serial PRIVATE drivers/serial/uart_hc32.c)
  else()
    zephyr_library_sources(drivers/serial/uart_hc32.c)
  endif()
endif()
