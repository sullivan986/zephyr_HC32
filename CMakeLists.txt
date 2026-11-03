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

# The vendor DDL derives the reported HCLK frequency from SYSTEM_XTH, so the
# crystal frequency has to reach it as a compile definition. Default to 8 MHz
# when the PLL is disabled and the crystal is not used at all.
if(CONFIG_HC32_XTH_12MHZ)
  set(HC32_SYSTEM_XTH 12000000)
elseif(CONFIG_HC32_XTH_16MHZ)
  set(HC32_SYSTEM_XTH 16000000)
elseif(CONFIG_HC32_XTH_24MHZ)
  set(HC32_SYSTEM_XTH 24000000)
else()
  set(HC32_SYSTEM_XTH 8000000)
endif()
zephyr_compile_definitions(SYSTEM_XTH=${HC32_SYSTEM_XTH})

# Zephyr creates the drivers__serial and drivers__can libraries unconditionally
# when CONFIG_SERIAL / CONFIG_CAN is enabled, and then warns about them being
# empty if no in-tree driver is selected. Hang the sources on those libraries
# when they exist so an out-of-tree driver does not leave a dangling empty
# target; fall back to a module library otherwise (e.g. if the target was never
# created).
if(CONFIG_UART_HC32)
  if(TARGET drivers__serial)
    target_sources(drivers__serial PRIVATE drivers/serial/uart_hc32.c)
  else()
    zephyr_library_sources(drivers/serial/uart_hc32.c)
  endif()
endif()

if(CONFIG_CAN_HC32)
  if(TARGET drivers__can)
    target_sources(drivers__can PRIVATE drivers/can/can_hc32.c)
  else()
    zephyr_library_sources(drivers/can/can_hc32.c)
  endif()
endif()

if(CONFIG_DMA_HC32)
  if(TARGET drivers__dma)
    target_sources(drivers__dma PRIVATE drivers/dma/dma_hc32.c)
  else()
    zephyr_library_sources(drivers/dma/dma_hc32.c)
  endif()
endif()

if(CONFIG_PWM_HC32)
  if(TARGET drivers__pwm)
    target_sources(drivers__pwm PRIVATE drivers/pwm/pwm_hc32.c)
  else()
    zephyr_library_sources(drivers/pwm/pwm_hc32.c)
  endif()
endif()
