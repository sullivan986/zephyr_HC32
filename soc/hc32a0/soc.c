/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * XHSC HC32A052 early clock and CMSIS glue.
 *
 * The vendor ships system_hc32a052.c, but its SystemCoreClockUpdate() also
 * reprograms SysTick, which fights the Zephyr timer driver. The vendor file is
 * therefore not compiled; this file provides the CMSIS symbols instead.
 */

#include <zephyr/platform/hooks.h>

#include <soc.h>

/* Reset default is RCH / 3 = 4 MHz. Sysctrl_* helpers use this value for
 * their busy-wait loops before the clock tree has been programmed.
 */
uint32_t SystemCoreClock = 4000000U;
uint32_t SystemHCLK = 4000000U;
uint32_t SystemPCLK = 4000000U;

void SystemCoreClockUpdate(void) {
  SystemCoreClock = Sysctrl_GetHClkFreq();
  SystemHCLK = SystemCoreClock;
  SystemPCLK = Sysctrl_GetPClkFreq();
}

/*
 * The DDL's delay1ms()/delay100us(), which Sysctrl_ClkSourceEnable() calls
 * before enabling the PLL, busy-wait for SysTick->VAL to change. SysTick is
 * stopped out of reset, so without this the very first clock-tree call spins
 * forever and nothing after it ever runs.
 *
 * SysTick is started as a free-running counter with the interrupt disabled;
 * the Zephyr SysTick driver reprograms it at PRE_KERNEL_2.
 */
static void hc32_systick_start(void) {
  SysTick->CTRL = 0U;
  SysTick->LOAD = 0xFFFFFFU;
  SysTick->VAL = 0U;
  SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;
}

#if defined(CONFIG_SOC_EARLY_INIT_HOOK)
/*
 * Runs from z_cstart() after .data/.bss are set up and before PRE_KERNEL_1
 * drivers, so the CMSIS clock variables are already readable here.
 *
 * 12 MHz RCH / 2 * 64 / 6 = 64 MHz HCLK = PCLK. The internal RC oscillator is
 * used on purpose so the boot does not depend on an external crystal being
 * fitted; switch to XTH below once the board is known to have one.
 */
void soc_early_init_hook(void) {
  stc_sysctrl_pll_cfg_t pll = {0};

  /* Must come first: every DDL delay below needs a running SysTick. */
  hc32_systick_start();

  (void)Sysctrl_SetRCHTrim(SysctrlRchFreq12MHz);
  (void)Sysctrl_SetRCLTrim(SysctrlRclFreq32768);
  (void)Sysctrl_SetRCLStableTime(SysctrlRclStableCycle256);
  (void)Sysctrl_ClkSourceEnable(SysctrlClkRCH, TRUE);

  (void)Sysctrl_SetHCLKDiv(SysctrlHclkDiv1);
  (void)Sysctrl_SetPCLKDiv(SysctrlPclkDiv1);

#if defined(CONFIG_HC32_CLOCK_PLL)
  /* Must be programmed before the core exceeds 48 MHz. */
  (void)Flash_WaitCycle(FlashWaitCycle2);

  pll.enPllSource = SysctrlPllRch;
  pll.u8SourceDiv = 2U;
  pll.enStableWaitCycle = SysctrlPllStableCycle16384;
  pll.u8MullN = 64U;
  pll.u8MullF = 0U;
  pll.u8OutputDiv = 6U;

  /* Stay on RCH if the vendor rejects the parameters: a slower but working
   * clock beats hanging on a PLL that will never lock. */
  if (Sysctrl_SetPLLFreq(&pll) == Ok) {
    (void)Sysctrl_ClkSourceEnable(SysctrlClkPLL, TRUE);
    (void)Sysctrl_SysClkSwitch(SysctrlClkPLL);
  }
#else
  (void)Flash_WaitCycle(FlashWaitCycle0);
#endif

  SystemCoreClockUpdate();
}
#endif /* CONFIG_SOC_EARLY_INIT_HOOK */
