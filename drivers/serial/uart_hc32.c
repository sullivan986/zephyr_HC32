/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * XHSC HC32A052 USART driver.
 *
 * Polling covers the console. When CONFIG_UART_ASYNC_API is enabled, the DMAC
 * driver is part of the build and the node has "dmas" entries, transmission
 * and reception run through the DMAC:
 *
 *   TX - the buffer is pushed into TDR by DMA. The DMA completion interrupt
 *        then arms the USART transmit-complete interrupt, so UART_TX_DONE is
 *        only reported once the last stop bit has left the shift register.
 *
 *   RX - DMA writes into the caller's buffer while the USART frame-gap timer
 *        watches for line idle. Every timer expiry reports the bytes received
 *        so far as one UART_RX_RDY event, which turns a byte stream into
 *        frames without a receive interrupt per byte. When the buffer is
 *        full the driver reports the last chunk, releases the buffer and
 *        stops, so the application re-arms reception with a fresh buffer.
 *
 * A transfer-complete interrupt is also the only way to tell a full RX buffer
 * apart from an idle line, so the RX channel gets a DMA callback.
 *
 * The DMAC cannot gather/scatter, so a second RX buffer cannot be chained in
 * hardware; uart_rx_buf_rsp() returns -ENOTSUP instead of pretending.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
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
#define UART_HC32_HAS_DMA_TX(n) DT_INST_DMAS_HAS_NAME(n, tx)
#define UART_HC32_HAS_DMA_RX(n) DT_INST_DMAS_HAS_NAME(n, rx)

/*
 * Without the DMAC driver there is no DMA device to reference, and
 * DEVICE_DT_GET() on the node would fail to link. Compile the plumbing out in
 * that case; the devicetree still carries the specifiers for when it is on.
 */
#if defined(CONFIG_DMA_HC32) && defined(CONFIG_UART_ASYNC_API)

#define UART_HC32_DMA_TX_CFG(n)                                                \
  .has_dma_tx = UART_HC32_HAS_DMA_TX(n),                                       \
  .dma_tx_channel =                                                            \
      COND_CODE_1(UART_HC32_HAS_DMA_TX(n),                                     \
                  (DT_INST_DMAS_CELL_BY_NAME(n, tx, channel)), (0U)),          \
  .dma_tx_slot = COND_CODE_1(UART_HC32_HAS_DMA_TX(n),                          \
                             (DT_INST_DMAS_CELL_BY_NAME(n, tx, slot)), (0U))

#define UART_HC32_DMA_RX_CFG(n)                                                \
  .has_dma_rx = UART_HC32_HAS_DMA_RX(n),                                       \
  .dma_rx_channel =                                                            \
      COND_CODE_1(UART_HC32_HAS_DMA_RX(n),                                     \
                  (DT_INST_DMAS_CELL_BY_NAME(n, rx, channel)), (0U)),          \
  .dma_rx_slot = COND_CODE_1(UART_HC32_HAS_DMA_RX(n),                          \
                             (DT_INST_DMAS_CELL_BY_NAME(n, rx, slot)), (0U))

#define UART_HC32_DMA_DEV(n)                                                   \
  .dma_dev = COND_CODE_1(                                                      \
      UART_HC32_HAS_DMA_TX(n),                                                 \
      (DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(n, tx))),                       \
      (COND_CODE_1(UART_HC32_HAS_DMA_RX(n),                                    \
                   (DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(n, rx))),          \
                   (NULL))))

#else

#define UART_HC32_DMA_TX_CFG(n)                                                \
  .has_dma_tx = false, .dma_tx_channel = 0U, .dma_tx_slot = 0U
#define UART_HC32_DMA_RX_CFG(n)                                                \
  .has_dma_rx = false, .dma_rx_channel = 0U, .dma_rx_slot = 0U
#define UART_HC32_DMA_DEV(n) .dma_dev = NULL

#endif

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
  void (*irq_config)(void);
  bool has_dma_tx;
  bool has_dma_rx;
  const struct device *dma_dev;
  uint32_t dma_tx_channel;
  uint32_t dma_tx_slot;
  uint32_t dma_rx_channel;
  uint32_t dma_rx_slot;
};

struct uart_hc32_data {
  uart_callback_t callback;
  void *user_data;

  const uint8_t *tx_buf;
  size_t tx_len;
  bool tx_active;

  uint8_t *rx_buf;
  size_t rx_len;
  size_t rx_offset;
  bool rx_active;

  struct k_spinlock lock;
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

  /*
   * The DMA request lines are enabled once here rather than per transfer; the
   * CR2 bits only gate the request path, they do not start anything.
   */
  if (cfg->has_dma_tx && cfg->has_dma_rx) {
    init.stcAdvFeature.u32DMATxRxTransfer = USART_DMA_TX_RX_ENABLE;
  } else if (cfg->has_dma_tx) {
    init.stcAdvFeature.u32DMATxRxTransfer = USART_DMA_TX_ENABLE;
  } else if (cfg->has_dma_rx) {
    init.stcAdvFeature.u32DMATxRxTransfer = USART_DMA_RX_ENABLE;
  }

  baud = USART_UART_Init(cfg->regs, &init);
  if (baud == 0U) {
    return -EIO;
  }

  cfg->irq_config();

  return 0;
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

#if defined(CONFIG_UART_ASYNC_API)

static void uart_hc32_event(const struct device *dev, struct uart_event *evt) {
  struct uart_hc32_data *data = dev->data;

  if (data->callback != NULL) {
    data->callback(dev, evt, data->user_data);
  }
}

static int uart_hc32_callback_set(const struct device *dev,
                                  uart_callback_t callback, void *user_data) {
  struct uart_hc32_data *data = dev->data;

  data->callback = callback;
  data->user_data = user_data;

  return 0;
}

/* Bytes the RX channel has not written to the buffer yet. */
static size_t uart_hc32_rx_pending(const struct device *dev) {
  const struct uart_hc32_config *cfg = dev->config;
  struct dma_status status;

  if ((!cfg->has_dma_rx) || (cfg->dma_dev == NULL)) {
    return 0U;
  }
  if (dma_get_status(cfg->dma_dev, cfg->dma_rx_channel, &status) != 0) {
    return 0U;
  }

  return status.pending_length;
}

static void uart_hc32_rx_stop(const struct device *dev) {
  const struct uart_hc32_config *cfg = dev->config;
  struct uart_hc32_data *data = dev->data;
  k_spinlock_key_t key;
  uint8_t *buf;

  key = k_spin_lock(&data->lock);

  if (!data->rx_active) {
    k_spin_unlock(&data->lock, key);
    return;
  }

  data->rx_active = false;
  buf = data->rx_buf;
  if (cfg->has_dma_rx) {
    (void)dma_stop(cfg->dma_dev, cfg->dma_rx_channel);
  }
  USART_DisableIrq(cfg->regs, USART_FLAG_TO);
  USART_ClearFlag(cfg->regs, USART_FLAG_TO);
  k_spin_unlock(&data->lock, key);

  struct uart_event released = {
      .type = UART_RX_BUF_RELEASED,
      .data.rx_buf = {.buf = buf},
  };
  uart_hc32_event(dev, &released);

  struct uart_event disabled = {.type = UART_RX_DISABLED};
  uart_hc32_event(dev, &disabled);
}

/*
 * Report whatever the DMA has moved so far. With stop set, or once the whole
 * buffer is accounted for, reception is torn down and the application is told
 * that it may supply a new buffer.
 */
static void uart_hc32_rx_settle(const struct device *dev, bool stop) {
  const struct uart_hc32_config *cfg = dev->config;
  struct uart_hc32_data *data = dev->data;
  size_t pending;
  size_t received;
  size_t offset;
  uint8_t *buf;
  bool done;
  k_spinlock_key_t key;

  key = k_spin_lock(&data->lock);
  if (!data->rx_active) {
    k_spin_unlock(&data->lock, key);
    return;
  }

  pending = stop ? 0U : uart_hc32_rx_pending(dev);
  pending = MIN(pending, data->rx_len - data->rx_offset);
  received = data->rx_len - data->rx_offset - pending;
  offset = data->rx_offset;
  buf = data->rx_buf;
  data->rx_offset += received;
  done = stop || (data->rx_offset >= data->rx_len);
  k_spin_unlock(&data->lock, key);

  if (received > 0U) {
    struct uart_event rdy = {
        .type = UART_RX_RDY,
        .data.rx = {.buf = buf, .offset = offset, .len = received},
    };
    uart_hc32_event(dev, &rdy);
  }

  if (done) {
    uart_hc32_rx_stop(dev);
    return;
  }

  /*
   * Restart the frame-gap timer. The counter is re-armed on each received
   * frame but has to be kicked once after it expires, otherwise the next
   * frame would be reported only when the buffer fills.
   */
  REG_MODIFY(cfg->regs->CR1, USART_CR1_TOE, USART_TOE_Data_Frame);
}

/* RX buffer filled up: the last chunk is due to the application. */
static void uart_hc32_rx_dma_done(const struct device *dma_dev, void *user_data,
                                  uint32_t channel, int status) {
  const struct device *dev = (const struct device *)user_data;

  ARG_UNUSED(dma_dev);
  ARG_UNUSED(channel);
  ARG_UNUSED(status);

  uart_hc32_rx_settle(dev, true);
}

static int uart_hc32_rx_start(const struct device *dev) {
  const struct uart_hc32_config *cfg = dev->config;
  struct uart_hc32_data *data = dev->data;
  struct dma_block_config block = {0};
  struct dma_config dma_cfg = {0};

  block.source_address = (uint32_t)(uintptr_t)&cfg->regs->RDR;
  block.dest_address = (uint32_t)(uintptr_t)data->rx_buf;
  block.block_size = (uint32_t)data->rx_len;
  block.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
  block.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;

  dma_cfg.dma_slot = cfg->dma_rx_slot;
  dma_cfg.channel_direction = PERIPHERAL_TO_MEMORY;
  dma_cfg.source_data_size = 1U;
  dma_cfg.dest_data_size = 1U;
  dma_cfg.block_count = 1U;
  dma_cfg.head_block = &block;
  dma_cfg.user_data = (void *)dev;
  dma_cfg.dma_callback = uart_hc32_rx_dma_done;

  if (dma_config(cfg->dma_dev, cfg->dma_rx_channel, &dma_cfg) != 0) {
    return -EIO;
  }
  if (dma_start(cfg->dma_dev, cfg->dma_rx_channel) != 0) {
    return -EIO;
  }

  return 0;
}

/* Frame-gap timer: one event per idle gap instead of one interrupt per byte. */
static void uart_hc32_rx_timeout_apply(const struct device *dev,
                                       int32_t timeout_us) {
  const struct uart_hc32_config *cfg = dev->config;
  uint32_t pclk = Sysctrl_GetPClkFreq();
  uint32_t psc = cfg->regs->BRR_f.PSC;
  uint32_t divider = 1U << (psc * 2U);
  uint32_t ticks;

  ticks = (uint32_t)(((uint64_t)pclk * (uint64_t)timeout_us) / 1000000U /
                     (uint64_t)divider);
  ticks = CLAMP(ticks, 1U, 0x100000U);

  REG_CLEARBITS(cfg->regs->CR1, USART_CR1_TOE);
  cfg->regs->TOR_f.TOR = ticks - 1U;
  REG_MODIFY(cfg->regs->CR1, USART_CR1_TOE, USART_TOE_Data_Frame);
}

static int uart_hc32_rx_enable(const struct device *dev, uint8_t *buf,
                               size_t len, int32_t timeout) {
  const struct uart_hc32_config *cfg = dev->config;
  struct uart_hc32_data *data = dev->data;
  k_spinlock_key_t key;

  if (!cfg->has_dma_rx) {
    return -ENOTSUP;
  }
  if ((buf == NULL) || (len == 0U)) {
    return -EINVAL;
  }

  key = k_spin_lock(&data->lock);
  if (data->rx_active) {
    k_spin_unlock(&data->lock, key);
    return -EBUSY;
  }

  data->rx_buf = buf;
  data->rx_len = len;
  data->rx_offset = 0U;
  data->rx_active = true;
  k_spin_unlock(&data->lock, key);

  USART_ClearFlag(cfg->regs, USART_FLAG_ALL);

  if (uart_hc32_rx_start(dev) != 0) {
    key = k_spin_lock(&data->lock);
    data->rx_active = false;
    k_spin_unlock(&data->lock, key);
    return -EIO;
  }

  if (timeout != SYS_FOREVER_US) {
    uart_hc32_rx_timeout_apply(dev, timeout);
    USART_EnableIrq(cfg->regs, USART_FLAG_TO);
  }

  return 0;
}

static int uart_hc32_rx_buf_rsp(const struct device *dev, uint8_t *buf,
                                size_t len) {
  ARG_UNUSED(dev);
  ARG_UNUSED(buf);
  ARG_UNUSED(len);

  /* Linear DMAC: the next buffer can only be armed after the current one has
   * been released, which uart_rx_enable() does. */
  return -ENOTSUP;
}

static int uart_hc32_rx_disable(const struct device *dev) {
  struct uart_hc32_data *data = dev->data;

  if (!data->rx_active) {
    return 0;
  }

  uart_hc32_rx_settle(dev, true);

  return 0;
}

/* Last byte is in the shift register; ask to be told when the frame is out. */
static void uart_hc32_tx_dma_done(const struct device *dma_dev, void *user_data,
                                  uint32_t channel, int status) {
  const struct device *dev = (const struct device *)user_data;
  const struct uart_hc32_config *cfg = dev->config;

  ARG_UNUSED(dma_dev);
  ARG_UNUSED(channel);
  ARG_UNUSED(status);

  USART_EnableIrq(cfg->regs, USART_FLAG_TC);
}

static int uart_hc32_tx(const struct device *dev, const uint8_t *buf,
                        size_t len, int32_t timeout) {
  const struct uart_hc32_config *cfg = dev->config;
  struct uart_hc32_data *data = dev->data;
  struct dma_block_config block = {0};
  struct dma_config dma_cfg = {0};
  k_spinlock_key_t key;

  ARG_UNUSED(timeout);

  if (!cfg->has_dma_tx) {
    return -ENOTSUP;
  }
  if ((buf == NULL) || (len == 0U)) {
    return -EINVAL;
  }

  key = k_spin_lock(&data->lock);
  if (data->tx_active) {
    k_spin_unlock(&data->lock, key);
    return -EBUSY;
  }
  data->tx_buf = buf;
  data->tx_len = len;
  data->tx_active = true;
  k_spin_unlock(&data->lock, key);

  block.source_address = (uint32_t)(uintptr_t)buf;
  block.dest_address = (uint32_t)(uintptr_t)&cfg->regs->TDR;
  block.block_size = (uint32_t)len;
  block.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
  block.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;

  dma_cfg.dma_slot = cfg->dma_tx_slot;
  dma_cfg.channel_direction = MEMORY_TO_PERIPHERAL;
  dma_cfg.source_data_size = 1U;
  dma_cfg.dest_data_size = 1U;
  dma_cfg.block_count = 1U;
  dma_cfg.head_block = &block;
  dma_cfg.user_data = (void *)dev;
  dma_cfg.dma_callback = uart_hc32_tx_dma_done;

  /* A stale TC would fire before the frame is out of the shift register. */
  USART_ClearFlag(cfg->regs, USART_FLAG_TC);
  USART_DisableIrq(cfg->regs, USART_FLAG_TC);

  if ((dma_config(cfg->dma_dev, cfg->dma_tx_channel, &dma_cfg) != 0) ||
      (dma_start(cfg->dma_dev, cfg->dma_tx_channel) != 0)) {
    key = k_spin_lock(&data->lock);
    data->tx_active = false;
    k_spin_unlock(&data->lock, key);
    return -EIO;
  }

  return 0;
}

static int uart_hc32_tx_abort(const struct device *dev) {
  const struct uart_hc32_config *cfg = dev->config;
  struct uart_hc32_data *data = dev->data;
  k_spinlock_key_t key;
  const uint8_t *buf;
  size_t len;

  if (!cfg->has_dma_tx) {
    return -ENOTSUP;
  }

  key = k_spin_lock(&data->lock);
  if (!data->tx_active) {
    k_spin_unlock(&data->lock, key);
    return -EINVAL;
  }
  data->tx_active = false;
  buf = data->tx_buf;
  len = data->tx_len;
  k_spin_unlock(&data->lock, key);

  (void)dma_stop(cfg->dma_dev, cfg->dma_tx_channel);
  USART_DisableIrq(cfg->regs, USART_FLAG_TC);
  USART_ClearFlag(cfg->regs, USART_FLAG_TC);

  struct uart_event evt = {
      .type = UART_TX_ABORTED,
      .data.tx = {.buf = buf, .len = len},
  };
  uart_hc32_event(dev, &evt);

  return 0;
}

static void uart_hc32_isr(const struct device *dev) {
  const struct uart_hc32_config *cfg = dev->config;
  struct uart_hc32_data *data = dev->data;
  uint32_t sr = cfg->regs->SR;

  if ((sr & USART_FLAG_TO) != 0U) {
    USART_ClearFlag(cfg->regs, USART_FLAG_TO);
    uart_hc32_rx_settle(dev, false);
  }

  if ((sr & USART_FLAG_TC) != 0U) {
    USART_ClearFlag(cfg->regs, USART_FLAG_TC);
    if (USART_GetIrqEn(cfg->regs, USART_FLAG_TC) != 0U) {
      k_spinlock_key_t key;
      const uint8_t *buf;
      size_t len;
      bool was_active;

      USART_DisableIrq(cfg->regs, USART_FLAG_TC);

      key = k_spin_lock(&data->lock);
      buf = data->tx_buf;
      len = data->tx_len;
      was_active = data->tx_active;
      data->tx_active = false;
      k_spin_unlock(&data->lock, key);

      if (was_active) {
        struct uart_event evt = {
            .type = UART_TX_DONE,
            .data.tx = {.buf = buf, .len = len},
        };
        uart_hc32_event(dev, &evt);
      }
    }
  }

  if ((sr & (USART_FLAG_OR | USART_FLAG_PE | USART_FLAG_FE)) != 0U) {
    /* Clear and keep going: a line glitch is visible through err_check(). */
    USART_ClearFlag(cfg->regs, USART_FLAG_OR | USART_FLAG_PE | USART_FLAG_FE);
  }
}

#endif /* CONFIG_UART_ASYNC_API */

static const struct uart_driver_api uart_hc32_api = {
    .poll_in = uart_hc32_poll_in,
    .poll_out = uart_hc32_poll_out,
    .err_check = uart_hc32_err_check,
#if defined(CONFIG_UART_ASYNC_API)
    .callback_set = uart_hc32_callback_set,
    .tx = uart_hc32_tx,
    .tx_abort = uart_hc32_tx_abort,
    .rx_enable = uart_hc32_rx_enable,
    .rx_buf_rsp = uart_hc32_rx_buf_rsp,
    .rx_disable = uart_hc32_rx_disable,
#endif
};

#if defined(CONFIG_UART_ASYNC_API)
#define UART_HC32_IRQ_CONFIG(n)                                                \
  static void uart_hc32_irq_config_##n(void) {                                 \
    IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), uart_hc32_isr,      \
                DEVICE_DT_INST_GET(n), 0);                                     \
    irq_enable(DT_INST_IRQN(n));                                               \
  }
#else
#define UART_HC32_IRQ_CONFIG(n)                                                \
  static void uart_hc32_irq_config_##n(void) {}
#endif

#define UART_HC32_INST(n)                                                      \
  UART_HC32_IRQ_CONFIG(n)                                                      \
                                                                               \
  static struct uart_hc32_data uart_hc32_data_##n;                             \
                                                                               \
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
      .irq_config = uart_hc32_irq_config_##n,                                  \
      UART_HC32_DMA_TX_CFG(n),                                                 \
      UART_HC32_DMA_RX_CFG(n),                                                 \
      UART_HC32_DMA_DEV(n),                                                    \
  };                                                                           \
                                                                               \
  DEVICE_DT_INST_DEFINE(n, uart_hc32_init, NULL, &uart_hc32_data_##n,          \
                        &uart_hc32_config_##n, PRE_KERNEL_1,                   \
                        CONFIG_SERIAL_INIT_PRIORITY, &uart_hc32_api);

DT_INST_FOREACH_STATUS_OKAY(UART_HC32_INST)
