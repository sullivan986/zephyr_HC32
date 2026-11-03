/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * XHSC HC32A052 CAN driver.
 *
 * The HC32 CAN controller (see RM chapter 21) has one primary transmit buffer
 * (PTB), a four-deep secondary one, one receive FIFO and eight acceptance
 * filter groups.
 *
 * Scope of this driver:
 *   - classic CAN only, 8 data bytes, standard and extended identifiers
 *   - transmit through PTB, completion reported from the interrupt
 *   - receive through the RB FIFO, drained in the interrupt
 *   - filters evaluated in software over a single hardware filter group in
 *     accept-all mode. Mapping the eight hardware groups one-to-one to Zephyr
 *     filters is not possible: each Zephyr filter is an independent
 *     (id, mask) pair, while the hardware groups only ever form a union, so a
 *     "match anything else" filter also matches what an earlier group already
 *     accepted. Software dispatch is the honest way to give the API the
 *     semantics it promises. Revisit if a use case needs hardware filtering
 *     for load reasons.
 */

#define DT_DRV_COMPAT xhsc_hc32a052_can

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <can.h>
#include <soc.h>

/* Number of Zephyr filters the driver can track in software. */
#define CAN_HC32_MAX_FILTERS 8

/* Bound for a controller state transition, in 1 us steps. */
#define CAN_HC32_TIMEOUT_US 10000U

/*
 * Zephyr timing fields are 1-based counts of time quanta; the HC32 registers
 * store "value - 1" (see RM 21.3.4: tSEG_1 = (SEG_1 + 2) TQ, tSEG_2 = (SEG_2 +
 * 1) TQ). Total bit time matches: 1 + prop_seg + phase_seg1 + phase_seg2 in
 * Zephyr equals SEG_1 + SEG_2 + 3 in the HC32 encoding.
 */
#define CAN_HC32_SEG1(t) ((t)->prop_seg + (t)->phase_seg1 - 1U)
#define CAN_HC32_SEG2(t) ((t)->phase_seg2 - 1U)

struct can_hc32_filter {
  can_rx_callback_t callback;
  void *user_data;
  struct can_filter filter;
  bool used;
};
struct can_hc32_config {
  /*
   * Must be the first member: the CAN subsystem upcasts the driver config
   * to this type to read min_bitrate / max_bitrate and the initial timing.
   */
  struct can_driver_config common;
  CAN_TypeDef *regs;
  void (*irq_config_func)(const struct device *dev);
  uint8_t tx_port;
  uint8_t tx_pin;
  uint8_t tx_af;
  uint8_t rx_port;
  uint8_t rx_pin;
  uint8_t rx_af;
  bool has_stby;
  uint8_t stby_port;
  uint8_t stby_pin;
};

struct can_hc32_data {
  /* Must be the first member: the subsystem reads mode/started from here. */
  struct can_driver_data common;
  struct can_hc32_filter filters[CAN_HC32_MAX_FILTERS];
  struct k_spinlock lock;
  struct can_timing timing;
  /* One free primary transmit buffer, given back on completion or abort. */
  struct k_sem tx_free;
  /* Pending transmission, guarded by lock. */
  can_tx_callback_t tx_callback;
  void *tx_user_data;
  enum can_state state;
  struct can_bus_err_cnt err_cnt;
};

static GPIO_TypeDef *can_hc32_port_regs(uint8_t port) {
  switch (port) {
  case 0:
    return HC_GPIOA;
  case 1:
    return HC_GPIOB;
  case 2:
    return HC_GPIOC;
  default:
    return HC_GPIOD;
  }
}

static void can_hc32_pin_config(uint8_t port, uint8_t pin, uint8_t af,
                                bool output) {
  GPIO_TypeDef *regs = can_hc32_port_regs(port);
  stc_gpio_cfg_t cfg = {0};

  cfg.enDir = output ? GpioDirOut : GpioDirIn;
  /* CAN_TX at 1 Mbit/s needs the strong driver; RX is an input anyway. */
  cfg.enDrv = GpioDrvH;
  cfg.enPu = GpioPuDisable;
  cfg.enPd = GpioPdDisable;
  cfg.enOD = GpioOdDisable;

  (void)Sysctrl_SetPeripheralGate(
      (en_sysctrl_peripheral_gate_t)(SysctrlPeripheralGpioA + port), TRUE);
  (void)Gpio_Init(regs, (en_gpio_pin_t)pin, &cfg);
  (void)Gpio_SetAfMode(regs, (en_gpio_pin_t)pin, (en_gpio_af_t)af);
}

/*
 * A single hardware group in accept-all mode: AIDEE = 0 accepts both frame
 * formats, and a zero mask lets every identifier through.
 */
static void can_hc32_filter_accept_all(void) {
  stc_can_filter_t filter = {
      .u32CODE = 0U,
      .u32MASK = 0U,
      .enFilterSel = CanFilterSel1,
      .enAcfFormat = CanAllFrames,
  };

  CAN_FilterConfig(&filter, TRUE);
}

static int can_hc32_set_timing(const struct device *dev,
                               const struct can_timing *timing) {
  struct can_hc32_data *data = dev->data;

  if (timing->prescaler < 1U || timing->prescaler > 256U) {
    return -EINVAL;
  }
  if (CAN_HC32_SEG1(timing) > 63U || CAN_HC32_SEG2(timing) > 7U) {
    return -EINVAL;
  }
  if (CAN_HC32_SEG2(timing) < (uint16_t)(timing->sjw - 1U)) {
    return -EINVAL;
  }

  data->timing = *timing;

  return 0;
}

static int can_hc32_set_mode(const struct device *dev, can_mode_t mode) {
  struct can_hc32_data *data = dev->data;

  if ((mode & ~(CAN_MODE_LOOPBACK | CAN_MODE_LISTENONLY)) != 0U) {
    /* CAN FD, one-shot and three-sample are not implemented. */
    return -ENOTSUP;
  }

  if (data->common.started) {
    return -EBUSY;
  }

  /* Applied by can_hc32_start(), which needs the controller in reset. */
  data->common.mode = mode;

  return 0;
}

static int can_hc32_start(const struct device *dev) {
  const struct can_hc32_config *cfg = dev->config;
  struct can_hc32_data *data = dev->data;
  const struct can_timing *timing;
  stc_can_init_config_t init = {0};

  timing = &data->timing;

  init.stcCanBt.PRESC = (uint8_t)(timing->prescaler - 1U);
  init.stcCanBt.SEG_1 = (uint8_t)CAN_HC32_SEG1(timing);
  init.stcCanBt.SEG_2 = (uint8_t)CAN_HC32_SEG2(timing);
  init.stcCanBt.SJW = (uint8_t)(timing->sjw - 1U);
  init.enCanRxBufAll = CanRxNormal;
  init.enCanRxBufMode = CanRxBufNotStored;
  init.enCanSTBMode = CanSTBFifoMode;
  init.stcWarningLimit.CanWarningLimitVal = 0xFFU;
  init.stcWarningLimit.CanErrorWarningLimitVal = 0x0AU;

  CAN_Init(&init);

  if ((data->common.mode & CAN_MODE_LOOPBACK) != 0U) {
    CAN_ModeConfig(CanInternalLoopBackMode, CanSelfAckDisable, TRUE);
  }
  if ((data->common.mode & CAN_MODE_LISTENONLY) != 0U) {
    CAN_ModeConfig(CanListenOnlyMode, CanSelfAckDisable, TRUE);
  }

  can_hc32_filter_accept_all();

  CAN_IrqFlgClr(CanRxIrqFlg);
  CAN_IrqFlgClr(CanRxOverIrqFlg);
  CAN_IrqFlgClr(CanTxPrimaryIrqFlg);
  CAN_IrqFlgClr(CanBusErrorIrqFlg);
  CAN_IrqCmd(CanRxIrqEn, TRUE);
  CAN_IrqCmd(CanBusErrorIrqEn, TRUE);

  cfg->irq_config_func(dev);

  data->common.started = true;

  return 0;
}

static int can_hc32_stop(const struct device *dev) {
  struct can_hc32_data *data = dev->data;
  k_spinlock_key_t key;
  bool pending;

  CAN_IrqCmd(CanRxIrqEn, FALSE);
  CAN_IrqCmd(CanTxPrimaryIrqEn, FALSE);
  CAN_IrqCmd(CanBusErrorIrqEn, FALSE);
  CAN_DeInit();

  key = k_spin_lock(&data->lock);
  pending = (data->tx_callback != NULL);
  data->tx_callback = NULL;
  data->tx_user_data = NULL;
  k_spin_unlock(&data->lock, key);

  if (pending) {
    /* The pending frame is dropped, hand the buffer slot back. */
    k_sem_give(&data->tx_free);
  }

  data->common.started = false;

  return 0;
}

/* Release a transmission the controller will never finish. */
static void can_hc32_abort_tx(const struct device *dev, int err) {
  struct can_hc32_data *data = dev->data;
  can_tx_callback_t callback;
  void *user_data;
  k_spinlock_key_t key;

  key = k_spin_lock(&data->lock);
  callback = data->tx_callback;
  user_data = data->tx_user_data;
  data->tx_callback = NULL;
  data->tx_user_data = NULL;
  k_spin_unlock(&data->lock, key);

  if (callback == NULL) {
    return;
  }

  CAN_IrqCmd(CanTxPrimaryIrqEn, FALSE);
  CAN_TransmitCmd(CanPTBTxAbortCmd);
  CAN_IrqFlgClr(CanTxPrimaryIrqFlg);
  k_sem_give(&data->tx_free);

  callback(dev, err, user_data);
}

static int can_hc32_send(const struct device *dev,
                         const struct can_frame *frame, k_timeout_t timeout,
                         can_tx_callback_t callback, void *user_data) {
  struct can_hc32_data *data = dev->data;
  stc_can_txframe_t tx = {0};

  if ((frame->flags & ~(CAN_FRAME_IDE | CAN_FRAME_RTR)) != 0U) {
    /* No FD, BRS, ESI or error frames. */
    return -ENOTSUP;
  }
  if (frame->dlc > CAN_MAX_DLC) {
    return -EINVAL;
  }
  if (!data->common.started) {
    return -ENETDOWN;
  }

  /*
   * Wait for the primary transmit buffer. Completion is reported from the
   * interrupt, so this blocks in the caller's context instead of polling;
   * a frame that never completes (no ACK on the bus) only costs the caller
   * its timeout, the buffer itself is recovered by can_hc32_abort_tx().
   */
  if (k_sem_take(&data->tx_free, timeout) != 0) {
    if (CAN_StatusGet(CanBusoff) == TRUE) {
      can_hc32_abort_tx(dev, -EIO);
      return -EIO;
    }
    return -EAGAIN;
  }

  if (CAN_StatusGet(CanBusoff) == TRUE) {
    k_sem_give(&data->tx_free);
    return -EIO;
  }

  {
    k_spinlock_key_t key = k_spin_lock(&data->lock);

    data->tx_callback = callback;
    data->tx_user_data = user_data;
    k_spin_unlock(&data->lock, key);
  }

  tx.StdID = frame->id;
  tx.Control_f.DLC = frame->dlc;
  tx.Control_f.RTR = (frame->flags & CAN_FRAME_RTR) != 0U ? 1U : 0U;
  tx.Control_f.IDE = (frame->flags & CAN_FRAME_IDE) != 0U ? 1U : 0U;
  memcpy(tx.Data, frame->data, frame->dlc);
  tx.enBufferSel = CanPTBSel;

  CAN_SetFrame(&tx);
  /* Drop a stale completion flag before arming the interrupt, otherwise
   * it would report this frame as sent before it even started. The flag is
   * gated by its interrupt enable on this IP, so both are needed even though
   * the completion is also picked up from the interrupt.
   */
  CAN_IrqFlgClr(CanTxPrimaryIrqFlg);
  CAN_IrqCmd(CanTxPrimaryIrqEn, TRUE);
  CAN_TransmitCmd(CanPTBTxCmd);

  return 0;
}

static int can_hc32_add_rx_filter(const struct device *dev,
                                  can_rx_callback_t callback, void *user_data,
                                  const struct can_filter *filter) {
  struct can_hc32_data *data = dev->data;
  k_spinlock_key_t key;
  int slot = -1;

  for (int i = 0; i < CAN_HC32_MAX_FILTERS; i++) {
    key = k_spin_lock(&data->lock);
    if (!data->filters[i].used) {
      data->filters[i].used = true;
      data->filters[i].callback = callback;
      data->filters[i].user_data = user_data;
      data->filters[i].filter = *filter;
      k_spin_unlock(&data->lock, key);
      slot = i;
      break;
    }
    k_spin_unlock(&data->lock, key);
  }

  return slot;
}

static void can_hc32_remove_rx_filter(const struct device *dev, int filter_id) {
  struct can_hc32_data *data = dev->data;
  k_spinlock_key_t key;

  if (filter_id < 0 || filter_id >= CAN_HC32_MAX_FILTERS) {
    return;
  }

  key = k_spin_lock(&data->lock);
  data->filters[filter_id].used = false;
  data->filters[filter_id].callback = NULL;
  k_spin_unlock(&data->lock, key);
}

static int can_hc32_get_state(const struct device *dev, enum can_state *state,
                              struct can_bus_err_cnt *err_cnt) {
  struct can_hc32_data *data = dev->data;

  err_cnt->tx_err_cnt = CAN_TxErrorCntGet();
  err_cnt->rx_err_cnt = CAN_RxErrorCntGet();

  if (CAN_StatusGet(CanBusoff) == TRUE) {
    *state = CAN_STATE_BUS_OFF;
  } else if (CAN_IrqFlgGet(CanErrorPassivenodeIrqFlg) == TRUE) {
    *state = CAN_STATE_ERROR_PASSIVE;
  } else if (err_cnt->tx_err_cnt >= 96U || err_cnt->rx_err_cnt >= 96U) {
    *state = CAN_STATE_ERROR_WARNING;
  } else {
    *state = CAN_STATE_ERROR_ACTIVE;
  }

  data->state = *state;
  data->err_cnt = *err_cnt;

  return 0;
}

static int can_hc32_get_core_clock(const struct device *dev, uint32_t *rate) {
  ARG_UNUSED(dev);

  /* The CAN controller is clocked from PCLK, no separate divider exists. */
  *rate = Sysctrl_GetPClkFreq();

  return 0;
}

static int can_hc32_get_max_filters(const struct device *dev, bool ide) {
  ARG_UNUSED(dev);
  ARG_UNUSED(ide);

  /* Filters are matched in software, so the limit is the same for both. */
  return CAN_HC32_MAX_FILTERS;
}

static int can_hc32_get_capabilities(const struct device *dev,
                                     can_mode_t *cap) {
  ARG_UNUSED(dev);

  *cap = CAN_MODE_LOOPBACK | CAN_MODE_LISTENONLY;

  return 0;
}

#ifdef CONFIG_CAN_MANUAL_RECOVERY_MODE
static int can_hc32_recover(const struct device *dev, k_timeout_t timeout) {
  ARG_UNUSED(timeout);

  /* Leaving reset and re-enabling the transceiver restarts the protocol
   * state machine and clears the error counters.
   */
  return can_hc32_start(dev);
}
#endif /* CONFIG_CAN_MANUAL_RECOVERY_MODE */

static void can_hc32_rx(struct can_hc32_data *data, const struct device *dev) {
  stc_can_rxframe_t rx;

  while (CAN_IrqFlgGet(CanRxIrqFlg) == TRUE) {
    CAN_IrqFlgClr(CanRxIrqFlg);
    CAN_Receive(&rx);

    struct can_frame frame = {0};

    frame.dlc = rx.Cst.Control_f.DLC;
    if (rx.Cst.Control_f.IDE != 0U) {
      frame.id = rx.ExtID & CAN_EXT_ID_MASK;
      frame.flags |= CAN_FRAME_IDE;
    } else {
      frame.id = rx.StdID & CAN_STD_ID_MASK;
    }
    if (rx.Cst.Control_f.RTR != 0U) {
      frame.flags |= CAN_FRAME_RTR;
    }
    memcpy(frame.data, rx.Data, MIN(frame.dlc, CAN_MAX_DLC));

    for (int i = 0; i < CAN_HC32_MAX_FILTERS; i++) {
      k_spinlock_key_t key = k_spin_lock(&data->lock);
      struct can_hc32_filter entry = data->filters[i];
      k_spin_unlock(&data->lock, key);

      if (!entry.used) {
        continue;
      }

      bool ide = (entry.filter.flags & CAN_FILTER_IDE) != 0U;
      bool frame_ide = (frame.flags & CAN_FRAME_IDE) != 0U;

      if (ide != frame_ide) {
        continue;
      }
      if ((frame.id & entry.filter.mask) !=
          (entry.filter.id & entry.filter.mask)) {
        continue;
      }

      entry.callback(dev, &frame, entry.user_data);
    }
  }
}

static void can_hc32_isr(const struct device *dev) {
  struct can_hc32_data *data = dev->data;

  can_hc32_rx(data, dev);

  if (CAN_IrqFlgGet(CanTxPrimaryIrqFlg) == TRUE) {
    can_tx_callback_t callback;
    void *user_data;

    CAN_IrqFlgClr(CanTxPrimaryIrqFlg);
    CAN_IrqCmd(CanTxPrimaryIrqEn, FALSE);

    k_spinlock_key_t key = k_spin_lock(&data->lock);

    callback = data->tx_callback;
    user_data = data->tx_user_data;
    data->tx_callback = NULL;
    data->tx_user_data = NULL;

    k_spin_unlock(&data->lock, key);

    k_sem_give(&data->tx_free);

    if (callback != NULL) {
      callback(dev, 0, user_data);
    }
  }

  /* Both flags are write-one-to-clear; clear anything left over. */
  if (CAN_IrqFlgGet(CanBusErrorIrqFlg) == TRUE) {
    CAN_IrqFlgClr(CanBusErrorIrqFlg);
  }
}

static int can_hc32_init(const struct device *dev) {
  const struct can_hc32_config *cfg = dev->config;
  struct can_hc32_data *data = dev->data;

  sys_slist_init(&data->common.state_change_callbacks);
  k_sem_init(&data->tx_free, 1, 1);

  (void)Sysctrl_SetPeripheralGate(SysctrlPeripheralCAN, TRUE);

  can_hc32_pin_config(cfg->rx_port, cfg->rx_pin, cfg->rx_af, false);
  can_hc32_pin_config(cfg->tx_port, cfg->tx_pin, cfg->tx_af, true);

  if (cfg->has_stby) {
    /* Transceivers are enabled by pulling STBY/EN low. */
    can_hc32_pin_config(cfg->stby_port, cfg->stby_pin, 0U, true);
    (void)Gpio_ClrIO(can_hc32_port_regs(cfg->stby_port),
                     (en_gpio_pin_t)cfg->stby_pin);
  }

  return 0;
}

static DEVICE_API(can, can_hc32_driver_api) = {
    .get_capabilities = can_hc32_get_capabilities,
    .start = can_hc32_start,
    .stop = can_hc32_stop,
    .set_mode = can_hc32_set_mode,
    .set_timing = can_hc32_set_timing,
    .send = can_hc32_send,
    .add_rx_filter = can_hc32_add_rx_filter,
    .remove_rx_filter = can_hc32_remove_rx_filter,
    .get_state = can_hc32_get_state,
    .get_core_clock = can_hc32_get_core_clock,
    .get_max_filters = can_hc32_get_max_filters,
#ifdef CONFIG_CAN_MANUAL_RECOVERY_MODE
    .recover = can_hc32_recover,
#endif /* CONFIG_CAN_MANUAL_RECOVERY_MODE */
    .timing_min =
        {
            .sjw = 0x1,
            .prop_seg = 0x1,
            .phase_seg1 = 0x1,
            .phase_seg2 = 0x1,
            .prescaler = 0x1,
        },
    .timing_max =
        {
            .sjw = 0x8,
            .prop_seg = 0x3F,
            .phase_seg1 = 0x3F,
            .phase_seg2 = 0x8,
            .prescaler = 0x100,
        },
};

#define CAN_HC32_INST(n)                                                       \
  static void can_hc32_irq_config_##n(const struct device *dev) {              \
    ARG_UNUSED(dev);                                                           \
    IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), can_hc32_isr,       \
                DEVICE_DT_INST_GET(n), 0);                                     \
    irq_enable(DT_INST_IRQN(n));                                               \
  }                                                                            \
                                                                               \
  static const struct can_hc32_config can_hc32_config_##n = {                  \
      .common = CAN_DT_DRIVER_CONFIG_INST_GET(n, 0, 1000000),                  \
      .regs = (CAN_TypeDef *)DT_INST_REG_ADDR(n),                              \
      .irq_config_func = can_hc32_irq_config_##n,                              \
      .tx_port = DT_INST_PROP(n, tx_port),                                     \
      .tx_pin = DT_INST_PROP(n, tx_pin),                                       \
      .tx_af = DT_INST_PROP(n, tx_af),                                         \
      .rx_port = DT_INST_PROP(n, rx_port),                                     \
      .rx_pin = DT_INST_PROP(n, rx_pin),                                       \
      .rx_af = DT_INST_PROP(n, rx_af),                                         \
      .has_stby = DT_INST_NODE_HAS_PROP(n, stby_port),                         \
      .stby_port = DT_INST_PROP_OR(n, stby_port, 0),                           \
      .stby_pin = DT_INST_PROP_OR(n, stby_pin, 0),                             \
  };                                                                           \
                                                                               \
  static struct can_hc32_data can_hc32_data_##n = {                            \
      .state = CAN_STATE_STOPPED,                                              \
  };                                                                           \
                                                                               \
  DEVICE_DT_INST_DEFINE(n, can_hc32_init, NULL, &can_hc32_data_##n,            \
                        &can_hc32_config_##n, POST_KERNEL,                     \
                        CONFIG_CAN_INIT_PRIORITY, &can_hc32_driver_api);

DT_INST_FOREACH_STATUS_OKAY(CAN_HC32_INST)
