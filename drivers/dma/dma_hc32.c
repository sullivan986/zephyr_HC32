/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * XHSC HC32A052 DMAC driver.
 *
 * The controller has five identical channels sharing one interrupt line. Each
 * channel moves a contiguous block between a fixed-width source and
 * destination, triggered either by software or by a peripheral request (the
 * "slot" in devicetree). Channel 0 has the highest fixed priority.
 *
 * The vendor DDL models a transfer as block_size * transfer_count elementary
 * transfers. Zephyr instead describes one contiguous block, so the mapping is
 * block_size = 1 and transfer_count = number of elements; that also keeps the
 * 4-bit block size register out of the way.
 *
 * Cyclic operation is not implemented. The DDL can reload the counters, but
 * the completion interrupt then stops being a per-iteration event, and nothing
 * in this port needs it yet; asking for it returns -ENOTSUP instead of
 * silently transferring once.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <soc.h>

#define DT_DRV_COMPAT xhsc_hc32a052_dma

#define DMA_HC32_MAX_CHANNELS 5U

/* Per channel register spacing in the CONFA0/SRCADR0/DSTADR0 arrays. */
#define DMA_HC32_CH_STRIDE 0x10U

struct dma_hc32_config {
  DMAC_TypeDef *regs;
  void (*irq_connect)(void);
  uint32_t channels;
};

struct dma_hc32_chan {
  dma_callback_t callback;
  void *user_data;
  uint32_t dir;
  uint32_t slot;
  uint32_t src_width;
  uint32_t dst_width;
  bool src_inc;
  bool dst_inc;
  uint32_t src_addr;
  uint32_t dst_addr;
  uint32_t size;
  bool busy;
};

struct dma_hc32_data {
  struct dma_hc32_chan chan[DMA_HC32_MAX_CHANNELS];
  struct k_spinlock lock;
};

static inline uint32_t dma_hc32_width_bytes(uint32_t width) {
  return (width == 0U) ? 1U : width;
}

static inline en_dma_transfer_width_t dma_hc32_width(uint32_t bytes) {
  switch (bytes) {
  case 1U:
    return DmaMsk8Bit;
  case 2U:
    return DmaMsk16Bit;
  case 4U:
    return DmaMsk32Bit;
  default:
    return (en_dma_transfer_width_t)0xFFU;
  }
}

/* There is no DDL getter for the enable bit, so read CONFA:ENS directly. */
static inline bool dma_hc32_channel_enabled(uint32_t channel) {
  volatile uint32_t *confa =
      (uint32_t *)((uintptr_t)&HC_DMAC->CONFA0 + channel * DMA_HC32_CH_STRIDE);

  return ((*confa & DMAC_CONFA0_ENS_Msk) != 0U);
}

static inline uint32_t dma_hc32_remaining(uint32_t channel) {
  volatile uint32_t *confa =
      (uint32_t *)((uintptr_t)&HC_DMAC->CONFA0 + channel * DMA_HC32_CH_STRIDE);

  return (*confa & DMAC_CONFA0_TC_Msk);
}

/*
 * Load one channel. Dma_InitChannel() zeroes CONFB, which also clears the
 * completion interrupt enable and the status field, so both have to be applied
 * afterwards.
 */
static int dma_hc32_program(uint32_t channel, struct dma_hc32_chan *chan) {
  stc_dma_cfg_t cfg = {0};
  en_dma_transfer_width_t width;

  if (chan->src_width != chan->dst_width) {
    return -EINVAL;
  }

  width = dma_hc32_width(chan->src_width);
  if ((uint32_t)width == 0xFFU) {
    return -EINVAL;
  }
  if ((chan->size == 0U) || (chan->size > 0xFFFFU)) {
    return -EINVAL;
  }

  cfg.enMode = DmaMskBlock;
  cfg.u16BlockSize = 1U;
  cfg.u16TransferCnt = (uint16_t)chan->size;
  cfg.enTransferWidth = width;
  cfg.enSrcAddrMode = chan->src_inc ? DmaMskSrcAddrInc : DmaMskSrcAddrFix;
  cfg.enDstAddrMode = chan->dst_inc ? DmaMskDstAddrInc : DmaMskDstAddrFix;
  cfg.enSrcAddrReloadCtl = DmaMskSrcAddrReloadDisable;
  cfg.enDestAddrReloadCtl = DmaMskDstAddrReloadDisable;
  cfg.enSrcBcTcReloadCtl = DmaMskBcTcReloadDisable;
  cfg.u32SrcAddress = chan->src_addr;
  cfg.u32DstAddress = chan->dst_addr;
  cfg.enTransferMode = DmaMskOneTransfer;
  cfg.enPriority = DmaMskPriorityFix;
  cfg.enRequestNum = (en_dma_trig_sel_t)chan->slot;

  if (Dma_InitChannel((en_dma_channel_t)(channel << 4), &cfg) != Ok) {
    return -EIO;
  }

  Dma_ClrStat((en_dma_channel_t)(channel << 4));
  if (chan->callback != NULL) {
    Dma_EnableChannelIrq((en_dma_channel_t)(channel << 4));
  } else {
    Dma_DisableChannelIrq((en_dma_channel_t)(channel << 4));
  }

  return 0;
}

static int dma_hc32_config(const struct device *dev, uint32_t channel,
                           struct dma_config *config) {
  struct dma_hc32_data *data = dev->data;
  const struct dma_hc32_config *cfg = dev->config;
  struct dma_hc32_chan *chan;
  struct dma_block_config *blk;
  k_spinlock_key_t key;
  int ret;

  if (channel >= cfg->channels) {
    return -EINVAL;
  }
  if (config->head_block == NULL) {
    return -EINVAL;
  }
  if (config->block_count > 1U) {
    return -ENOTSUP;
  }
  if (config->cyclic != 0U) {
    return -ENOTSUP;
  }
  if (config->dma_slot > 0x7FU) {
    return -EINVAL;
  }

  /* The controller only decrements or holds addresses, never gathers. */
  blk = config->head_block;
  if ((blk->source_gather_en != 0U) || (blk->dest_scatter_en != 0U) ||
      (blk->source_addr_adj == DMA_ADDR_ADJ_DECREMENT) ||
      (blk->dest_addr_adj == DMA_ADDR_ADJ_DECREMENT)) {
    return -ENOTSUP;
  }

  key = k_spin_lock(&data->lock);
  chan = &data->chan[channel];

  chan->callback = config->dma_callback;
  chan->user_data = config->user_data;
  chan->dir = config->channel_direction;
  chan->slot = config->dma_slot;
  chan->src_width = dma_hc32_width_bytes(config->source_data_size);
  chan->dst_width = dma_hc32_width_bytes(config->dest_data_size);
  chan->src_inc = (blk->source_addr_adj == DMA_ADDR_ADJ_INCREMENT);
  chan->dst_inc = (blk->dest_addr_adj == DMA_ADDR_ADJ_INCREMENT);
  chan->src_addr = blk->source_address;
  chan->dst_addr = blk->dest_address;
  chan->size = blk->block_size;
  chan->busy = false;

  ret = dma_hc32_program(channel, chan);

  k_spin_unlock(&data->lock, key);

  return ret;
}

static int dma_hc32_start(const struct device *dev, uint32_t channel) {
  struct dma_hc32_data *data = dev->data;
  const struct dma_hc32_config *cfg = dev->config;
  struct dma_hc32_chan *chan;
  k_spinlock_key_t key;
  int ret;

  if (channel >= cfg->channels) {
    return -EINVAL;
  }

  key = k_spin_lock(&data->lock);
  chan = &data->chan[channel];

  if (chan->size == 0U) {
    k_spin_unlock(&data->lock, key);
    return -EINVAL;
  }

  ret = dma_hc32_program(channel, chan);
  if (ret != 0) {
    k_spin_unlock(&data->lock, key);
    return ret;
  }

  Dma_Enable();
  Dma_EnableChannel((en_dma_channel_t)(channel << 4));
  chan->busy = true;

  k_spin_unlock(&data->lock, key);

  return 0;
}

static int dma_hc32_stop(const struct device *dev, uint32_t channel) {
  struct dma_hc32_data *data = dev->data;
  const struct dma_hc32_config *cfg = dev->config;
  k_spinlock_key_t key;

  if (channel >= cfg->channels) {
    return -EINVAL;
  }

  key = k_spin_lock(&data->lock);
  Dma_DisableChannel((en_dma_channel_t)(channel << 4));
  Dma_DisableChannelIrq((en_dma_channel_t)(channel << 4));
  Dma_ClrStat((en_dma_channel_t)(channel << 4));
  data->chan[channel].busy = false;
  k_spin_unlock(&data->lock, key);

  return 0;
}

static int dma_hc32_reload(const struct device *dev, uint32_t channel,
                           uint32_t src, uint32_t dst, size_t size) {
  struct dma_hc32_data *data = dev->data;
  const struct dma_hc32_config *cfg = dev->config;
  struct dma_hc32_chan *chan;
  k_spinlock_key_t key;
  int ret;

  if (channel >= cfg->channels) {
    return -EINVAL;
  }
  if ((size == 0U) || (size > 0xFFFFU)) {
    return -EINVAL;
  }

  key = k_spin_lock(&data->lock);
  chan = &data->chan[channel];

  Dma_DisableChannel((en_dma_channel_t)(channel << 4));

  chan->src_addr = src;
  chan->dst_addr = dst;
  chan->size = (uint32_t)size;

  ret = dma_hc32_program(channel, chan);
  k_spin_unlock(&data->lock, key);

  return ret;
}

static int dma_hc32_get_status(const struct device *dev, uint32_t channel,
                               struct dma_status *status) {
  const struct dma_hc32_config *cfg = dev->config;
  struct dma_hc32_data *data = dev->data;

  if (channel >= cfg->channels) {
    return -EINVAL;
  }

  status->dir = (enum dma_channel_direction)data->chan[channel].dir;
  status->busy = dma_hc32_channel_enabled(channel);
  status->pending_length = dma_hc32_remaining(channel) *
                           dma_hc32_width_bytes(data->chan[channel].src_width);
  status->free = 0U;
  status->write_position = 0U;
  status->read_position = 0U;
  status->total_copied = 0U;

  return 0;
}

static void dma_hc32_isr(const struct device *dev) {
  struct dma_hc32_data *data = dev->data;
  const struct dma_hc32_config *cfg = dev->config;
  uint32_t channel;

  for (channel = 0U; channel < cfg->channels; ++channel) {
    struct dma_hc32_chan *chan = &data->chan[channel];
    en_dma_stat_t stat = Dma_GetStat((en_dma_channel_t)(channel << 4));
    dma_callback_t callback;
    void *user_data;

    if ((stat == DmaDefault) || (stat == DmaTransferPause)) {
      continue;
    }

    Dma_DisableChannelIrq((en_dma_channel_t)(channel << 4));
    Dma_ClrStat((en_dma_channel_t)(channel << 4));
    chan->busy = false;

    callback = chan->callback;
    user_data = chan->user_data;

    if (callback != NULL) {
      callback(dev, user_data, channel,
               (stat == DmaTransferComplete) ? DMA_STATUS_COMPLETE : -EIO);
    }
  }
}

static int dma_hc32_init(const struct device *dev) {
  const struct dma_hc32_config *cfg = dev->config;

  (void)Sysctrl_SetPeripheralGate(SysctrlPeripheralDma, TRUE);
  Dma_Disable();

  cfg->irq_connect();

  return 0;
}

#define DMA_HC32_INST(n)                                                       \
  static void dma_hc32_irq_connect_##n(void) {                                 \
    IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), dma_hc32_isr,       \
                DEVICE_DT_INST_GET(n), 0);                                     \
    irq_enable(DT_INST_IRQN(n));                                               \
  }                                                                            \
                                                                               \
  static const struct dma_hc32_config dma_hc32_config_##n = {                  \
      .regs = (DMAC_TypeDef *)DT_INST_REG_ADDR(n),                             \
      .irq_connect = dma_hc32_irq_connect_##n,                                 \
      .channels = DT_INST_PROP(n, dma_channels),                               \
  };                                                                           \
                                                                               \
  static struct dma_hc32_data dma_hc32_data_##n;                               \
                                                                               \
  static const struct dma_driver_api dma_hc32_api_##n = {                      \
      .config = dma_hc32_config,                                               \
      .reload = dma_hc32_reload,                                               \
      .start = dma_hc32_start,                                                 \
      .stop = dma_hc32_stop,                                                   \
      .get_status = dma_hc32_get_status,                                       \
  };                                                                           \
                                                                               \
  DEVICE_DT_INST_DEFINE(n, dma_hc32_init, NULL, &dma_hc32_data_##n,            \
                        &dma_hc32_config_##n, PRE_KERNEL_1,                    \
                        CONFIG_DMA_INIT_PRIORITY, &dma_hc32_api_##n);

DT_INST_FOREACH_STATUS_OKAY(DMA_HC32_INST)
