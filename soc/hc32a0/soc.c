/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * XHSC HC32A052 early clock and CMSIS glue.
 *
 * The vendor ships system_hc32a052.c, but its SystemCoreClockUpdate() also
 * reprograms SysTick, which fights the Zephyr timer driver. The vendor file is
 * therefore not compiled; this file provides the CMSIS symbols instead.
 */

#include <stdbool.h>

#include <zephyr/platform/hooks.h>

#include <soc.h>

/* Reset default is RCH / 3 = 4 MHz. Sysctrl_* helpers use this value for
 * their busy-wait loops before the clock tree has been programmed.
 */
uint32_t SystemCoreClock = 4000000U;
uint32_t SystemHCLK = 4000000U;
uint32_t SystemPCLK = 4000000U;

/* Set once the crystal reported stable; readable by the application. */
uint8_t hc32_xth_ok;

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

#if defined(CONFIG_HC32_CLOCK_SRC_XTH)

/*
 * Per-crystal PLL settings. The vendor driver rejects any frequency whose
 * VCO (fin * mul_n) falls outside 384..512 MHz, so each crystal needs its own
 * combination to land on exactly 64 MHz:
 *
 *    8 MHz / 1 * 64 / 8 = 64 MHz
 *   12 MHz / 2 * 64 / 6 = 64 MHz
 *   16 MHz / 2 * 64 / 8 = 64 MHz
 *   24 MHz / 2 * 32 / 6 = 64 MHz
 */
#if defined(CONFIG_HC32_XTH_12MHZ)
#define HC32_XTH_HZ 12000000U
#define HC32_XTH_RANGE SysctrlXthFreq8_12MHz
#define HC32_PLL_IN_DIV 2U
#define HC32_PLL_MUL_N 64U
#define HC32_PLL_OUT_DIV 6U
#elif defined(CONFIG_HC32_XTH_16MHZ)
#define HC32_XTH_HZ 16000000U
#define HC32_XTH_RANGE SysctrlXthFreq8_16MHz
#define HC32_PLL_IN_DIV 2U
#define HC32_PLL_MUL_N 64U
#define HC32_PLL_OUT_DIV 8U
#elif defined(CONFIG_HC32_XTH_24MHZ)
#define HC32_XTH_HZ 24000000U
#define HC32_XTH_RANGE SysctrlXthFreq20_24MHz
#define HC32_PLL_IN_DIV 2U
#define HC32_PLL_MUL_N 32U
#define HC32_PLL_OUT_DIV 6U
#else /* HC32_XTH_8MHZ */
#define HC32_XTH_HZ 8000000U
#define HC32_XTH_RANGE SysctrlXthFreq8_12MHz
#define HC32_PLL_IN_DIV 1U
#define HC32_PLL_MUL_N 64U
#define HC32_PLL_OUT_DIV 8U
#endif

#endif /* CONFIG_HC32_CLOCK_SRC_XTH */

/* How many 100 us steps to wait for an oscillator to report stable. */
#define HC32_OSC_WAIT_STEPS 2000U /* 200 ms */

#if defined(CONFIG_HC32_CLOCK_PLL)

static void hc32_regs_unlock(void) {
  HC_SYSCTRL->CR3 = 0x5A5AU;
  HC_SYSCTRL->CR3 = 0xA5A5U;
}

static bool hc32_pll_start(void) {
  /* The bandgap has to be up before the PLL is useful. */
  HC_SYSCTRL->PERICLKEN0_f.ADC = TRUE;
  HC_ADC->BGR_f.BGREN = TRUE;
  delay100us(2U);

  hc32_regs_unlock();
  HC_SYSCTRL->CR2_f.PLL = TRUE;

  for (uint32_t i = 0U; i < HC32_OSC_WAIT_STEPS; i++) {
    if (HC_SYSCTRL->STATE_f.PLL == 1U) {
      return true;
    }
    delay100us(1U);
  }

  return false;
}

/* Program the PLL dividers/multipliers; does not enable it. */
static bool hc32_pll_configure(en_sysctrl_pll_clksource_t src, uint8_t in_div,
                               uint8_t mul_n, uint8_t out_div) {
  stc_sysctrl_pll_cfg_t pll = {0};

  pll.enPllSource = src;
  pll.u8SourceDiv = in_div;
  pll.enStableWaitCycle = SysctrlPllStableCycle16384;
  pll.u8MullN = mul_n;
  pll.u8MullF = 0U;
  pll.u8OutputDiv = out_div;

  return Sysctrl_SetPLLFreq(&pll) == Ok;
}

#endif /* CONFIG_HC32_CLOCK_PLL */

#if defined(CONFIG_HC32_CLOCK_PLL) && defined(CONFIG_HC32_CLOCK_SRC_XTH)

/*
 * Start the external crystal.
 *
 * Sysctrl_ClkSourceEnable(SysctrlClkXTH, ...) polls the state bit with no
 * timeout, so a board without a crystal (or with a dead one) hangs the boot
 * there. Poll with a bound instead and let the caller fall back to RCH.
 */
static bool hc32_xth_start(void) {
  (void)Sysctrl_SetXTHFreq(HC32_XTH_RANGE);
  (void)Sysctrl_XTHDriverCfg(SysctrlXtalDriver2);
  (void)Sysctrl_SetXTHStableTime(SysctrlXthStableCycle16384);

  /* PD0/PD1 are the crystal pins; the vendor helper puts them in analog mode.
   */
  HC_SYSCTRL->PERICLKEN1_f.GPIOD = 1U;
  HC_GPIOD->ADS |= 3U;

  hc32_regs_unlock();
  HC_SYSCTRL->CR2_f.XTH = TRUE;

  for (uint32_t i = 0U; i < HC32_OSC_WAIT_STEPS; i++) {
    if (HC_SYSCTRL->STATE_f.XTH == 1U) {
      return true;
    }
    delay100us(1U);
  }

  return false;
}

#endif /* CONFIG_HC32_CLOCK_PLL && CONFIG_HC32_CLOCK_SRC_XTH */

#if defined(CONFIG_SOC_EARLY_INIT_HOOK)
/*
 * Runs from z_cstart() after .data/.bss are set up and before PRE_KERNEL_1
 * drivers, so the CMSIS clock variables are already readable here.
 *
 * Default: XTH (8 MHz on the STK) / 1 * 64 / 8 = 64 MHz HCLK = PCLK.
 * Set CONFIG_HC32_CLOCK_SRC_RCH to use the internal 12 MHz RC instead, or
 * CONFIG_HC32_CLOCK_PLL=n to stay on 12 MHz RCH without any PLL.
 *
 * Any failure falls back to RCH rather than hanging, and records the result in
 * hc32_xth_ok so the application can tell the crystal is missing.
 */
void soc_early_init_hook(void) {
  bool pll_ok = false;

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

#if defined(CONFIG_HC32_CLOCK_SRC_XTH)
  hc32_xth_ok = hc32_xth_start() ? 1U : 0U;

  if (hc32_xth_ok != 0U) {
    pll_ok = hc32_pll_configure(SysctrlPllXthXtal, HC32_PLL_IN_DIV,
                                HC32_PLL_MUL_N, HC32_PLL_OUT_DIV) &&
             hc32_pll_start() && (Sysctrl_SysClkSwitch(SysctrlClkPLL) == Ok);
  }

  if (!pll_ok) {
    /* No crystal: run the PLL off the internal RC instead of giving up. */
    hc32_xth_ok = 0U;
  }
#endif /* CONFIG_HC32_CLOCK_SRC_XTH */

  if (!pll_ok) {
    pll_ok = hc32_pll_configure(SysctrlPllRch, 2U, 64U, 6U) &&
             hc32_pll_start() && (Sysctrl_SysClkSwitch(SysctrlClkPLL) == Ok);
  }

  if (!pll_ok) {
    /* Stay on the 12 MHz RCH that is already running. */
    (void)Flash_WaitCycle(FlashWaitCycle0);
  }
#else
  (void)Flash_WaitCycle(FlashWaitCycle0);
#endif /* CONFIG_HC32_CLOCK_PLL */

  SystemCoreClockUpdate();
}
#endif /* CONFIG_SOC_EARLY_INIT_HOOK */
