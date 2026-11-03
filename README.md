# zephyr_HC32 — XHSC HC32 out-of-tree Zephyr module

把 XHSC（小华半导体）HC32 系列接入 Zephyr 的全部移植内容集中在这里，作为
**out-of-tree Zephyr module** 使用。Zephyr 主仓库与 `west.yml` 不做任何修改。

## 为什么只需要暴露一个路径

`zephyr/module.yml` 里的 `settings` 会把本目录同时注册成三个构建根：

```yaml
name: hal_xhsc
build:
  cmake: .
  kconfig: Kconfig
  settings:
    soc_root: .
    board_root: .
    dts_root: .
```

所以 app 侧只要：

```cmake
list(APPEND ZEPHYR_EXTRA_MODULES /path/to/zephyr_HC32)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
```

`SOC_ROOT` / `BOARD_ROOT` / `DTS_ROOT` 全部自动生效，不用在 app 里再写。
（同一机制也适用于 `arch_root`、`module_ext_root`、`sca_root`。）

## 目录结构

```
zephyr_HC32/
├── zephyr/module.yml                 模块声明 + 三个构建根
├── CMakeLists.txt                    按 SoC 挂载 DDL 与驱动
├── Kconfig                           CONFIG_UART_HC32 等驱动开关
├── hc32_ddl/
│   └── hc32a052/                     原厂 DDL（未改动的 BSD-3-Clause 源码）
│       ├── soc/                      hc32a052.h / base_types.h / system_*.h
│       ├── drivers/inc/              驱动头文件 + ddl_device.h（设备型号选择）
│       ├── drivers/src/              全部外设驱动 .c
│       ├── LICENSE                   BSD-3-Clause（原厂）
│       └── CMakeLists.txt            只编译当前需要的外设
├── soc/
│   └── hc32a0/
│       ├── soc.yml                   family xhsc / series hc32a0 / soc hc32a052
│       ├── Kconfig.soc               select ARM, CPU_CORTEX_M0PLUS, ...
│       ├── Kconfig.defconfig         NUM_IRQS / SYS_CLOCK_HW_CYCLES_PER_SEC
│       ├── soc.h, soc.c              CMSIS 时钟符号 + 64 MHz 时钟初始化（XTH）
│       └── CMakeLists.txt
├── boards/
│   └── xhsc/hc32a052_stk/            STK-HC32A052
│       ├── board.yml, *.yaml         HWMv2 板级元数据
│       ├── Kconfig.hc32a052_stk      select SOC_HC32A052
│       ├── hc32a052_stk_defconfig
│       └── hc32a052_stk.dts          128K Flash / 16K SRAM / USART0
├── dts/bindings/
│   ├── vendor-prefixes.txt           补上 zephyr 树里没有的 xhsc 前缀
│   ├── serial/xhsc,hc32a052-usart.yaml
│   ├── can/xhsc,hc32a052-can.yaml
│   ├── dma/xhsc,hc32a052-dma.yaml
│   └── pwm/xhsc,hc32a052-gtim.yaml
└── drivers/
    ├── serial/uart_hc32.c            Zephyr UART 驱动（轮询 + DMA 异步）
    ├── can/can_hc32.c                Zephyr CAN 驱动
    ├── dma/dma_hc32.c                Zephyr DMA 驱动（5 通道）
    └── pwm/pwm_hc32.c                Zephyr PWM 驱动（GTIM0/1）
```

当前的驱动与 SoC 支持矩阵：只有 **HC32A052**，目前有 **UART（轮询 +
DMA 异步）**、**CAN（经典 CAN 2.0A/B）**、**DMA** 和 **PWM（GTIM）**。

## 在新项目里复用

新建 app 目录，放三个文件即可：

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.20.0)
set(HC32_PORT_ROOT "/home/sullivan/dev/os/zephyr_HC32"
    CACHE PATH "XHSC HC32 port")
get_filename_component(HC32_PORT_ROOT "${HC32_PORT_ROOT}" REALPATH)
list(APPEND ZEPHYR_EXTRA_MODULES ${HC32_PORT_ROOT})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(my_app)
target_sources(app PRIVATE src/main.c)
```

```bash
west build -b hc32a052_stk /path/to/my_app
```

不用复制 HAL，也不用改 `west.yml`。

## 加新外设驱动的套路

1. `hc32_ddl/<soc>/CMakeLists.txt` 的 `zephyr_sources()` 里加上对应
   `drivers/src/xxx.c`（全部 `.c` 已随 DDL 复制进来，默认只编译了
   `ddl.c sysctrl.c gpio.c flash.c usart.c`）。
2. `dts/bindings/<class>/<vendor>,<dev>.yaml` 写绑定；板级 `.dts` 加节点，
   `reg` 取 `<soc>.h` 里的 `HC_XXX` 地址。
3. `Kconfig` 加 `config XXX_HC32`（记得 `select XXX_HAS_DRIVER`）。
4. 驱动放 `drivers/<class>/`，在顶层 `CMakeLists.txt` 用
   `zephyr_library_sources_ifdef(CONFIG_XXX_HC32 ...)` 挂上。

## 加新 SoC 的套路

1. `hc32_ddl/<soc>/` 放原厂 DDL（含自己一份 `drivers/inc/ddl_device.h` + `CMakeLists.txt`）。
2. `soc/<series>/` 加 `soc.yml` 条目、`Kconfig.soc`、`Kconfig.defconfig`、
   `soc.c`（时钟与 CMSIS 符号）。
3. `boards/<vendor>/` 加板级目录。
4. 顶层 `CMakeLists.txt` 加一行 `add_subdirectory_ifdef(CONFIG_SOC_<new> ...)`。

## 时钟

`soc_early_init_hook()`（在 `.data/.bss` 之后、`PRE_KERNEL_1` 驱动之前）把时钟配成
**64 MHz**，默认用**外部晶振 XTH**：

```
XTH 8 MHz / 1 -> PLL x64 = 512 MHz -> /8 = 64 MHz HCLK = PCLK
```

选外部晶振是因为内部 RCH 只校准到几个百分点，而 CAN 位时序要求整条总线两端的时钟
偏差加起来不超过 ±1%，RCH 根本不够；串口波特率也会随温度漂移。

### Kconfig

| 选项 | 默认 | 说明 |
|---|---|---|
| `HC32_CLOCK_PLL` | y | n = 停在 12 MHz RCH，完全不碰 PLL |
| `HC32_CLOCK_SRC_XTH` / `_RCH` | XTH | PLL 的参考源 |
| `HC32_XTH_8MHZ` / `_12MHZ` / `_16MHZ` / `_24MHZ` | 8 MHz | 板上晶振频率 |

`hc32a052_stk` 板载 **8 MHz**，默认无需改动。

### 为什么只有这 4 个频率可选

原厂 `Sysctrl_SetPLLFreq()` 强制 `384 MHz <= FVCO = fin * mul_n <= 512 MHz`，
输出又必须是 64 MHz，所以每个晶振只有特定组合成立：

| XTH | in_div | mul_n | out_div | FVCO | 输出 |
|---|---|---|---|---|---|
| 8 MHz | 1 | 64 | 8 | 512 MHz | 64 MHz |
| 12 MHz | 2 | 64 | 6 | 384 MHz | 64 MHz |
| 16 MHz | 2 | 64 | 8 | 512 MHz | 64 MHz |
| 24 MHz | 2 | 32 | 6 | 384 MHz | 64 MHz |

20 MHz 之类无法整数倍频出 64 MHz，所以没有列出。

### 带回退的时钟启动

原厂 `Sysctrl_ClkSourceEnable(SysctrlClkXTH, ...)` **无超时**地轮询起振状态位，
没有晶振或晶振损坏会直接卡死。本移植改为有上限的轮询：

- 晶振起振失败 → 回退到以 RCH 为参考的 PLL（仍是 64 MHz），`hc32_xth_ok` 置 0；
- PLL 锁定失败 → 回退到 12 MHz RCH；
- 都不会死机，应用可读 `extern uint8_t hc32_xth_ok;` 判断晶振是否正常。

`SYSTEM_XTH` 由 `CMakeLists.txt` 依据 Kconfig 自动传给 DDL
（`Sysctrl_GetHClkFreq()` 靠它换算 HCLK），改晶振频率不需要手工同步任何宏。

## CAN

`compatible = "xhsc,hc32a052-can"`，寄存器基址 `0x40030000`，IRQ 28。

### 硬件事实（RM 第 21 章）

- 经典 CAN 2.0A/B，8 字节数据，不支持 CAN FD
- 1 个发送主缓冲（PTB）+ 4 深副缓冲（STB），1 个接收 FIFO，8 组验收滤波器
- 位时序：`TQ = (PRESC+1)/f_can`，`BT = ((SEG_1+2)+(SEG_2+1)) × TQ`
- 约束：`SEG_1` 0..63、`SEG_2` 0..7、`SJW` 0..7，且 `SEG_1 ≥ SEG_2+1`、`SEG_2 ≥ SJW`
- 控制器时钟就是 **PCLK**，没有独立分频；`can_get_core_clock()` 直接返回
  `Sysctrl_GetPClkFreq()`
- 滤波器 `AIDEE=0` 表示标准/扩展帧都收，`AMASK=0` 表示 ID 全匹配

### 引脚

| 信号 | 引脚 | 复用 | 说明 |
|---|---|---|---|
| CAN_TX | PA11 | AF4 | 也可 PB09，AF5 |
| CAN_RX | PA12 | AF4 | 也可 PB08，AF3 |
| CAN_STBY | PA8 | GPIO | 拉低使能收发器 |

`can0` 节点默认 `status = "disabled"`，因为板上不一定焊了收发器。在 app 的
overlay 里打开即可（参考 `HC32_TEST/boards/hc32a052_stk.overlay`）。没有
STBY 引脚时删掉 `stby-port` / `stby-pin` 两个属性。

### 驱动能力与取舍

- **发送**：走 PTB（最高优先级），`can_send()` 在 PTB 空闲前按 `timeout` 等待，
  发送完成在中断里回调 `can_tx_callback_t`，因此非阻塞。
  只支持到 `dlc = 8`，`CAN_FRAME_IDE` / `CAN_FRAME_RTR`。
- **接收**：中断里清空 RB FIFO，逐帧分发给软件滤波器。
- **滤波器**：**用软件实现**，硬件只开一组"全收"。
  原因是 8 组硬件滤波器只能形成并集，而 Zephyr 的每个 filter 是独立的
  `(id, mask)` 对；把硬件组一对一映射会让"其余全部接受"的过滤条件
  也吃掉前面已经被别的组接受过的帧，语义对不上。驱动最多跟踪 8 个软件
  滤波器，超出返回 `-ENOMEM`。
- **错误状态**：`can_get_state()` 读 TECNT/RECNT 与 bus-off、error-passive
  标志，可返回 error-active / warning / passive / bus-off。
  打开了 `CONFIG_CAN_MANUAL_RECOVERY_MODE` 时 `recover` 会重新初始化控制器。
- **模式**：支持 normal / loopback / listen-only；CAN FD、one-shot 不支持，
  `can_set_mode()` 会返回 `-ENOTSUP`。

### 位时序换算

Zephyr 的 `struct can_timing` 是 1-based 的时间量子个数，HC32 寄存器存的是
"值减一"：

```
SEG_1 = prop_seg + phase_seg1 - 1
SEG_2 = phase_seg2 - 1
SJW   = sjw - 1
PRESC = prescaler - 1
```

两种编码的位时间长度一致（`1 + prop + phase1 + phase2 == SEG_1 + SEG_2 + 3`），
所以直接用 `can_set_bitrate()` 即可，不需要手工算寄存器值。

## DMA

RM 第 12 章。5 个同构通道，共享一条中断线（`DMAC_IRQn = 4`），CH0 固定最高
优先级。每个通道一次搬一段连续内存，源/目的宽度 8/16/32 位，地址可自增或
固定，触发源可以是软件或下表里的外设请求号。

| 通道 | 典型用途 | 触发号 |
| --- | --- | --- |
| 0 | USART0 TX | `0x40` |
| 1 | USART0 RX | `0x41` |
| 2 | USART1 TX | `0x42` |
| 3 | USART1 RX | `0x43` |
| 4 | 备用（SPI0 / ADC / ATIM …） | 见 `dmac.h` |

devicetree 里每个通道描述符是 `<&dma0 channel slot>`：

```dts
dma0: dma@40021000 {
        compatible = "xhsc,hc32a052-dma";
        reg = <0x40021000 0x400>;
        interrupts = <4 0>;
        dma-channels = <5>;
        #dma-cells = <2>;
};
```

驱动把 DDL 的 `BlockSize × TransferCnt` 模型固定成 `BlockSize = 1`、
`TransferCnt = 字节数`，所以 Zephyr 的 `dma_config/dma_start/reload` 语义可以
直接映射。**不支持** 循环传输和 gather/scatter（返回 `-ENOTSUP`）：DDL 的
`DmaMskContinuousTransfer` 在计数耗尽后只保留 ENS 位，完成中断不再按轮次
触发，没有可靠语义可用。

`dma_get_status()` 的 `pending_length` 由 `CONFA.TC + 1` 换算——TC 存的是
"传输次数 - 1"，这一点和字面直觉差 1，UART 的接收分段就是靠它算"已收到多少
字节"。

## PWM

RM 第 15 章。GTIM0/GTIM1 是 16 位向上计数器，各带 2 个比较/捕获通道。
它们与 BTIM 共用硬件：用 GTIM0 就没有 BTIM0/1/2，用 GTIM1 就没有 BTIM3/4/5，
驱动在 init 里通过 `Sysctrl_SetFunc()` 声明归属。

驱动在 init 时**固定**计数时钟：取能让 tick ≤ 1 MHz 的最快分频。64 MHz PCLK
下就是 64 分频 → 1 MHz，一个完整周期最多 65536 tick（65.5 ms），舵机 20 ms
帧有 1 µs 分辨率，同时 `pwm_get_cycles_per_sec()` 在整个运行期保持不变。

```dts
pwm0: pwm@40001800 {
        compatible = "xhsc,hc32a052-gtim";
        reg = <0x40001800 0x400>;
        #pwm-cells = <3>;
        ch0-port = <0>;  /* PA6 = GTIM0_CH0, AF2 */
        ch0-pin = <6>;
        ch0-af = <2>;
};
```

`pwm_set_cycles()` 的约定：

| 参数 | 行为 |
| --- | --- |
| `period_cycles == 0` | 停止输出，引脚强制低 |
| `pulse_cycles == 0` | 0% 占空比，引脚强制低 |
| `pulse_cycles >= period_cycles` | 100% 占空比，引脚强制高 |
| 其他 | `ARR = period - 1`，`CCR = pulse`，反向 PWM 模式 |

反向 PWM（`CNT < CCR` 输出高）是 DDL 里唯一"CCR 直接等于脉宽"的模式，正向模式
要写 `period - pulse`；`PWM_POLARITY_INVERTED` 时驱动自动换算。

更高频的 PWM（电机驱动、DC-DC）应该上 ATIM，而不是把 GTIM 的分频调低。

## UART DMA

`xhsc,hc32a052-usart` 节点加上 `dmas` / `dma-names` 后，打开
`CONFIG_UART_ASYNC_API` 就会走 DMA：

```dts
usart1: usart@40006400 {
        ...
        dmas = <&dma0 2 0x42>, <&dma0 3 0x43>;
        dma-names = "tx", "rx";
};
```

- **发送**：DMA 把缓冲区推给 TDR，完成中断再打开 USART 的 TC 中断，
  `UART_TX_DONE` 只在整个帧（含停止位）移出后才上报。
- **接收**：DMA 直接写调用者的缓冲区；USART 的"数据帧间定时器"（TOE
  `Data_Frame` 模式，即 `TOR`）在总线空闲超过 timeout 时产生中断，
  驱动把"目前收到多少字节"作为一个 `UART_RX_RDY` 上报。因此一帧数据只
  产生一次事件，不是每字节一次。
- **缓冲区满**：上报最后一段 → `UART_RX_BUF_RELEASED` →
  `UART_RX_DISABLED`，由应用重新 `uart_rx_enable()`。
- `uart_rx_buf_rsp()` 返回 `-ENOTSUP`：DMAC 不能链式切换缓冲，第二个缓冲区
  只能在第一个释放后重新武装。
- 没有 `dmas` 或没开 `CONFIG_DMA_HC32` 时，驱动只提供轮询接口，异步接口返回
  `-ENOTSUP`，控制台仍然正常。

## 注意

- **SysTick 必须先跑起来**：DDL 的 `delay1ms()` / `delay100us()` 靠
  `SysTick->VAL` 变化计时，而 `Sysctrl_ClkSourceEnable(SysctrlClkPLL, ...)`
  内部就会调 `delay100us()`。SysTick 复位后是停止的，若不在配时钟之前把它当
  自由计数器启动，**第一条时钟配置就会死循环**，之后什么都不会执行（串口自然
  没输出）。`soc_early_init_hook()` 第一步调 `hc32_systick_start()` 解决这个
  问题；Zephyr 的 SysTick 驱动会在 `PRE_KERNEL_2` 重新配置它。
- 时钟源默认是**外部晶振 XTH**，见下面「时钟」一节。`CONFIG_HC32_CLOCK_PLL=n`
  时直接用 12 MHz RCH、不走 PLL，适合首次 bring-up；UART 波特率是按实际 PCLK
  计算的，所以各配置下串口都能正常输出。
- `soc.c` 里的 `SystemCoreClockUpdate()` 是自己实现的：原厂
  `system_hc32a052.c` 会重写 SysTick 的 LOAD/VAL/CTRL，与 Zephyr 定时器冲突，
  所以那份文件**不编译**。
- 原厂 `interrupts_hc32a052.c` 也不编译；Zephyr 用自己的向量表，
  HC32 的 IRQn（PORTA=0 … CTRIM=31）与 NVIC 直接对应。
- 时钟在 `soc_early_init_hook()` 里配置（`.data/.bss` 之后、`PRE_KERNEL_1` 之前）。
  不能用 `soc_reset_hook()`：它在 `.data` 初始化之前跑，设好的
  `SystemCoreClock` 会被随后的 `.data` 拷贝覆盖。
- DDL 为 BSD-3-Clause，允许使用；若日后要上游化，需在 `REUSE.toml` 中标注。

## 许可证

- 本仓库自己的代码（SoC、board、绑定、驱动、构建脚本）：**MIT**，见 `LICENSE`。
- `hc32_ddl/` 下的原厂 DDL：**BSD-3-Clause**，见 `hc32_ddl/hc32a052/LICENSE`，
  各源文件头部也带原厂许可声明。vendor 文件尽量保持原样，便于日后与上游 DDL 对比。
