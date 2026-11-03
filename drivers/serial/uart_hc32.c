/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * XHSC HC32A052 USART driver (polling only).
 *
 * Interrupt-driven operation is not implemented yet; the driver covers what the
 * console and simple debug output need.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/util.h>

#include <soc.h>

#define DT_DRV_COMPAT xhsc_hc32a052_usart

/* Busy-wait bound for one byte; generous at 115200 baud even on a slow clock.
 */
#define UART_HC32_TX_TIMEOUT 0x100000U

#define UART_HC32_PORT_A 0U
#define UART_HC32_PORT_B 1U
#define UART_HC32_PORT_C 2U
#define UART_HC32_PORT_D 3U

#define UART_HC32_HAS_RX(n) DT_INST_NODE_HAS_PROP(n, rx_port)

struct uart_hc32_config {
  USART_TypeDef *regs;
  en_sysctrl_peripheral_gate_t usart_gate;
  uint8_t tx_port;
  uint8_t tx_pin;
  uint8_t tx_af;
  uint8_t rx_port;
  uint8_t rx_pin;
  uint8_t rx_af;
  uint32_t speed;
};

static GPIO_TypeDef *hc32_port_regs(uint8_t port) {
  switch (port) {
  case UART_HC32_PORT_A:
    return HC_GPIOA;
  case UART_HC32_PORT_B:
    return HC_GPIOB;
  case UART_HC32_PORT_C:
    return HC_GPIOC;
  default:
    return HC_GPIOD;
  }
}

static void hc32_port_clock_enable(uint8_t port) {
  (void)Sysctrl_SetPeripheralGate(
      (en_sysctrl_peripheral_gate_t)(SysctrlPeripheralGpioA + port), TRUE);
}

static void hc32_pin_mux(uint8_t port, uint8_t pin, uint8_t af) {
  GPIO_TypeDef *regs = hc32_port_regs(port);
  stc_gpio_cfg_t cfg = {0};

  cfg.enDir = GpioDirIn;
  cfg.enDrv = GpioDrvH;
  cfg.enPu = GpioPuEnable;
  cfg.enPd = GpioPdDisable;
  cfg.enOD = GpioOdDisable;

  hc32_port_clock_enable(port);
  (void)Gpio_Init(regs, (en_gpio_pin_t)pin, &cfg);
  (void)Gpio_SetAfMode(regs, (en_gpio_pin_t)pin, (en_gpio_af_t)af);
}

static int uart_hc32_init(const struct device *dev) {
  const struct uart_hc32_config *cfg = dev->config;
  stc_usart_uart_init_t init = {0};
  uint32_t baud;

  (void)Sysctrl_SetPeripheralGate(cfg->usart_gate, TRUE);

  hc32_pin_mux(cfg->tx_port, cfg->tx_pin, cfg->tx_af);
  if (cfg->rx_port != 0xFFU) {
    hc32_pin_mux(cfg->rx_port, cfg->rx_pin, cfg->rx_af);
  }

  init.u32BaudRate = cfg->speed;
  init.u32SampleMode = USART_OVERSAMPLING_16;
  init.u32FrameLength = USART_FRAME_LEN_8BITS;
  init.u32Parity = USART_PARITY_NONE;
  init.u32StopBits = USART_STOP_1BIT;
  init.u32TransMode =
      (cfg->rx_port != 0xFFU) ? USART_MODE_TX_RX : USART_MODE_TX;

  baud = USART_UART_Init(cfg->regs, &init);

  return (baud == 0U) ? -EIO : 0;
}

static int uart_hc32_poll_in(const struct device *dev, unsigned char *c) {
  const struct uart_hc32_config *cfg = dev->config;

  if (USART_GetFlag(cfg->regs, USART_FLAG_RC) == 0U) {
    return -1;
  }

  *c = (unsigned char)REG_READ(cfg->regs->RDR);

  return 0;
}

static void uart_hc32_poll_out(const struct device *dev, unsigned char c) {
  const struct uart_hc32_config *cfg = dev->config;
  uint8_t ch = c;

  (void)USART_UART_Transmit(cfg->regs, &ch, 1U, UART_HC32_TX_TIMEOUT);
}

static int uart_hc32_err_check(const struct device *dev) {
  const struct uart_hc32_config *cfg = dev->config;
  int err = 0;

  if (USART_GetFlag(cfg->regs, USART_FLAG_PE) != 0U) {
    err |= UART_ERROR_PARITY;
  }
  if (USART_GetFlag(cfg->regs, USART_FLAG_FE) != 0U) {
    err |= UART_ERROR_FRAMING;
  }
  if (USART_GetFlag(cfg->regs, USART_FLAG_OR) != 0U) {
    err |= UART_ERROR_OVERRUN;
  }

  return err;
}

static const struct uart_driver_api uart_hc32_api = {
    .poll_in = uart_hc32_poll_in,
    .poll_out = uart_hc32_poll_out,
    .err_check = uart_hc32_err_check,
};

#define UART_HC32_INST(n)                                                      \
  static const struct uart_hc32_config uart_hc32_config_##n = {                \
      .regs = (USART_TypeDef *)DT_INST_REG_ADDR(n),                            \
      .usart_gate =                                                            \
          ((DT_INST_REG_ADDR(n) == 0x40006000U) ? SysctrlPeripheralUsart0      \
                                                : SysctrlPeripheralUsart1),    \
      .tx_port = DT_INST_PROP(n, tx_port),                                     \
      .tx_pin = DT_INST_PROP(n, tx_pin),                                       \
      .tx_af = DT_INST_PROP(n, tx_af),                                         \
      .rx_port = COND_CODE_1(UART_HC32_HAS_RX(n), (DT_INST_PROP(n, rx_port)),  \
                             (0xFFU)),                                         \
      .rx_pin =                                                                \
          COND_CODE_1(UART_HC32_HAS_RX(n), (DT_INST_PROP(n, rx_pin)), (0U)),   \
      .rx_af =                                                                 \
          COND_CODE_1(UART_HC32_HAS_RX(n), (DT_INST_PROP(n, rx_af)), (0U)),    \
      .speed = DT_INST_PROP(n, current_speed),                                 \
  };                                                                           \
                                                                               \
  DEVICE_DT_INST_DEFINE(n, uart_hc32_init, NULL, NULL, &uart_hc32_config_##n,  \
                        PRE_KERNEL_1, CONFIG_SERIAL_INIT_PRIORITY,             \
                        &uart_hc32_api);

DT_INST_FOREACH_STATUS_OKAY(UART_HC32_INST)
