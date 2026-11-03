/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * XHSC HC32A052 GTIM PWM driver.
 *
 * GTIM0 and GTIM1 are 16-bit up counters with two compare/capture channels
 * each. They share their hardware with the BTIM blocks: selecting GTIM0 turns
 * BTIM0/1/2 off and GTIM1 costs BTIM3/4/5. The driver claims the block for
 * GTIM use, which the vendor DDL expresses through Sysctrl_SetFunc().
 *
 * The counter clock is fixed at init to the fastest prescaler that keeps the
 * tick at or below 1 MHz. A 1 MHz tick fits a full 65.5 ms period in the
 * 16-bit auto-reload register, which covers servo frames and LED dimming with
 * 1 us resolution, and keeps pwm_get_cycles_per_sec() stable across calls.
 * Faster PWM (motor drives, DC-DC converters) belongs on the ATIM blocks,
 * which have their own driver.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <soc.h>

#define DT_DRV_COMPAT xhsc_hc32a052_gtim

#define PWM_HC32_MAX_CHANNELS 2U
#define PWM_HC32_TICK_TARGET_HZ 1000000U
#define PWM_HC32_PRS_MAX 15U

#define PWM_HC32_HAS_CH(n, ch) DT_INST_NODE_HAS_PROP(n, ch##_port)

struct pwm_hc32_channel {
  uint8_t port;
  uint8_t pin;
  uint8_t af;
};

struct pwm_hc32_config {
  GTIM_TypeDef *regs;
  en_sysctrl_peripheral_gate_t gate;
  bool is_gtim0;
  uint32_t channels;
  struct pwm_hc32_channel ch[PWM_HC32_MAX_CHANNELS];
};

struct pwm_hc32_data {
  uint32_t tick_hz;
  uint32_t period[PWM_HC32_MAX_CHANNELS];
  struct k_spinlock lock;
};

static GPIO_TypeDef *pwm_hc32_port_regs(uint8_t port) {
  switch (port) {
  case 0U:
    return HC_GPIOA;
  case 1U:
    return HC_GPIOB;
  case 2U:
    return HC_GPIOC;
  default:
    return HC_GPIOD;
  }
}

static void pwm_hc32_pin_config(const struct pwm_hc32_channel *ch) {
  stc_gpio_cfg_t cfg = {0};
  GPIO_TypeDef *regs = pwm_hc32_port_regs(ch->port);

  (void)Sysctrl_SetPeripheralGate(
      (en_sysctrl_peripheral_gate_t)(SysctrlPeripheralGpioA + ch->port), TRUE);

  cfg.bOutputVal = FALSE;
  cfg.enDir = GpioDirOut;
  cfg.enDrv = GpioDrvH;
  cfg.enPu = GpioPuDisable;
  cfg.enPd = GpioPdDisable;
  cfg.enOD = GpioOdDisable;

  (void)Gpio_Init(regs, (en_gpio_pin_t)ch->pin, &cfg);
  (void)Gpio_SetAfMode(regs, (en_gpio_pin_t)ch->pin, (en_gpio_af_t)ch->af);
}

/* Fastest divider that keeps the tick at or below the target frequency. */
static uint32_t pwm_hc32_pick_prescaler(uint32_t pclk) {
  uint32_t prs;

  for (prs = 0U; prs <= PWM_HC32_PRS_MAX; ++prs) {
    if ((pclk >> prs) <= PWM_HC32_TICK_TARGET_HZ) {
      break;
    }
  }

  return MIN(prs, PWM_HC32_PRS_MAX);
}

static int pwm_hc32_set_cycles(const struct device *dev, uint32_t channel,
                               uint32_t period_cycles, uint32_t pulse_cycles,
                               pwm_flags_t flags) {
  const struct pwm_hc32_config *cfg = dev->config;
  struct pwm_hc32_data *data = dev->data;
  uint32_t ccr;
  k_spinlock_key_t key;

  if (channel >= cfg->channels) {
    return -EINVAL;
  }
  if (period_cycles > 0x10000U) {
    return -EINVAL;
  }
  if (pulse_cycles > period_cycles) {
    return -EINVAL;
  }

  key = k_spin_lock(&data->lock);

  /* period 0 stops the output: the pin is held at the idle level so a
   * disabled channel does not keep a servo powered or an LED lit.
   */
  if (period_cycles == 0U) {
    Gtim_SetCompareCaptureMode(cfg->regs, channel * 4U,
                               GTIM_COMPARE_CAPTURE_FORCE_LOW_LEVEL);
    data->period[channel] = 0U;
    k_spin_unlock(&data->lock, key);
    return 0;
  }

  if (pulse_cycles == 0U) {
    Gtim_SetCompareCaptureMode(cfg->regs, channel * 4U,
                               GTIM_COMPARE_CAPTURE_FORCE_LOW_LEVEL);
    data->period[channel] = period_cycles;
    k_spin_unlock(&data->lock, key);
    return 0;
  }

  if (pulse_cycles >= period_cycles) {
    Gtim_SetCompareCaptureMode(cfg->regs, channel * 4U,
                               GTIM_COMPARE_CAPTURE_FORCE_HIGH_LEVEL);
    data->period[channel] = period_cycles;
    k_spin_unlock(&data->lock, key);
    return 0;
  }

  Gtim_SetAutoReload(cfg->regs, (uint16_t)(period_cycles - 1U));

  /*
   * PWM_INVERTED drives the pin high while CNT < CCR, so CCR is the pulse
   * width directly. With inverted polarity the idle level is high, which is
   * the same waveform with CCR counted from the other end.
   */
  if ((flags & PWM_POLARITY_INVERTED) != 0U) {
    ccr = period_cycles - pulse_cycles;
    Gtim_SetCompareCaptureMode(cfg->regs, channel * 4U,
                               GTIM_COMPARE_CAPTURE_PWM_NORMAL);
  } else {
    ccr = pulse_cycles;
    Gtim_SetCompareCaptureMode(cfg->regs, channel * 4U,
                               GTIM_COMPARE_CAPTURE_PWM_INVERTED);
  }

  Gtim_SetCompareCaptureReg(cfg->regs, channel * 4U, ccr);
  data->period[channel] = period_cycles;
  Gtim_Enable(cfg->regs);

  k_spin_unlock(&data->lock, key);

  return 0;
}

static int pwm_hc32_get_cycles_per_sec(const struct device *dev,
                                       uint32_t channel, uint64_t *cycles) {
  const struct pwm_hc32_config *cfg = dev->config;
  struct pwm_hc32_data *data = dev->data;

  if (channel >= cfg->channels) {
    return -EINVAL;
  }

  *cycles = data->tick_hz;

  return 0;
}

static int pwm_hc32_init(const struct device *dev) {
  const struct pwm_hc32_config *cfg = dev->config;
  struct pwm_hc32_data *data = dev->data;
  stc_gtim_init_t init = {0};
  uint32_t pclk = Sysctrl_GetPClkFreq();
  uint32_t prs;
  uint32_t channel;

  /* GTIM0 shares silicon with BTIM0/1/2, GTIM1 with BTIM3/4/5. */
  if (cfg->is_gtim0) {
    (void)Sysctrl_SetFunc(SysctrlGtim0UseAsBtim012, FALSE);
  } else {
    (void)Sysctrl_SetFunc(SysctrlGtim1UseAsBtim345, FALSE);
  }
  (void)Sysctrl_SetPeripheralGate(cfg->gate, TRUE);

  for (channel = 0U; channel < cfg->channels; ++channel) {
    if (cfg->ch[channel].port != 0xFFU) {
      pwm_hc32_pin_config(&cfg->ch[channel]);
    }
  }

  prs = pwm_hc32_pick_prescaler(pclk);
  data->tick_hz = pclk >> prs;

  init.u32TaskMode = GTIM_TASK_MODE_CONTINUOUS_COUNTER;
  init.u32WorkMode = GTIM_WORK_MODE_PCLK;
  init.u32Prescaler = prs << GTIM_CR0_PRS_Pos;
  init.u32ToggleEn = GTIM_TOGGLE_DISABLE;
  init.u32TriggerSource = GTIM_TRIGGER_SOURCE_ETR;
  init.u32ExInputPolarity = GTIM_ETR_POLARITY_NORMAL;
  init.u32AutoReloadVal = 0xFFFFU;

  if (Gtim_Init(cfg->regs, &init) != Ok) {
    return -EIO;
  }

  /* Every channel idles low until pwm_set_cycles() gives it a duty. */
  for (channel = 0U; channel < cfg->channels; ++channel) {
    Gtim_SetCompareCaptureMode(cfg->regs, channel * 4U,
                               GTIM_COMPARE_CAPTURE_FORCE_LOW_LEVEL);
    Gtim_SetCompareCaptureReg(cfg->regs, channel * 4U, 0U);
  }

  Gtim_Enable(cfg->regs);

  return 0;
}

#define PWM_HC32_CH_CFG(n, ch)                                                 \
  .port = COND_CODE_1(PWM_HC32_HAS_CH(n, ch), (DT_INST_PROP(n, ch##_port)),    \
                      (0xFFU)),                                                \
  .pin =                                                                       \
      COND_CODE_1(PWM_HC32_HAS_CH(n, ch), (DT_INST_PROP(n, ch##_pin)), (0U)),  \
  .af = COND_CODE_1(PWM_HC32_HAS_CH(n, ch), (DT_INST_PROP(n, ch##_af)), (0U))

#define PWM_HC32_INST(n)                                                       \
  static const struct pwm_hc32_config pwm_hc32_config_##n = {                  \
      .regs = (GTIM_TypeDef *)DT_INST_REG_ADDR(n),                             \
      .gate = (DT_INST_REG_ADDR(n) == 0x40001800U)                             \
                  ? SysctrlPeripheralGtim0Btim012                              \
                  : SysctrlPeripheralGtim1Btim345,                             \
      .is_gtim0 = (DT_INST_REG_ADDR(n) == 0x40001800U),                        \
      .channels = 2U,                                                          \
      .ch =                                                                    \
          {                                                                    \
              {PWM_HC32_CH_CFG(n, ch0)},                                       \
              {PWM_HC32_CH_CFG(n, ch1)},                                       \
          },                                                                   \
  };                                                                           \
                                                                               \
  static struct pwm_hc32_data pwm_hc32_data_##n;                               \
                                                                               \
  static const struct pwm_driver_api pwm_hc32_api_##n = {                      \
      .set_cycles = pwm_hc32_set_cycles,                                       \
      .get_cycles_per_sec = pwm_hc32_get_cycles_per_sec,                       \
  };                                                                           \
                                                                               \
  DEVICE_DT_INST_DEFINE(n, pwm_hc32_init, NULL, &pwm_hc32_data_##n,            \
                        &pwm_hc32_config_##n, PRE_KERNEL_1,                    \
                        CONFIG_PWM_INIT_PRIORITY, &pwm_hc32_api_##n);

DT_INST_FOREACH_STATUS_OKAY(PWM_HC32_INST)
