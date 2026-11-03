/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * XHSC HC32A052 SoC definitions.
 */

#ifndef ZEPHYR_SOC_XHSC_HC32A0_SOC_H_
#define ZEPHYR_SOC_XHSC_HC32A0_SOC_H_

#include <ddl.h>
#include <dmac.h>
#include <flash.h>
#include <gpio.h>
#include <gtim.h>
#include <sysctrl.h>
#include <usart.h>

/*
 * Set by soc_early_init_hook(): 1 when the external crystal started and the
 * PLL is running from it, 0 when the core fell back to the internal RCH.
 */
extern uint8_t hc32_xth_ok;

#endif /* ZEPHYR_SOC_XHSC_HC32A0_SOC_H_ */
