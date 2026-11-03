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

zephyr_library_sources_ifdef(CONFIG_UART_HC32 drivers/serial/uart_hc32.c)
