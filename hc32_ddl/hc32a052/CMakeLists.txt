# SPDX-License-Identifier: Apache-2.0

zephyr_include_directories(
  soc
  drivers/inc
)

# Only the blocks needed to boot, print on USART and toggle GPIO are built.
# Add more driver sources here (or make them conditional) as drivers land.
zephyr_sources(
  drivers/src/ddl.c
  drivers/src/sysctrl.c
  drivers/src/gpio.c
  drivers/src/flash.c
  drivers/src/usart.c
)
