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
│       ├── soc.h, soc.c              CMSIS 时钟符号 + 64 MHz 时钟初始化
│       └── CMakeLists.txt
├── boards/
│   └── xhsc/hc32a052_stk/            STK-HC32A052
│       ├── board.yml, *.yaml         HWMv2 板级元数据
│       ├── Kconfig.hc32a052_stk      select SOC_HC32A052
│       ├── hc32a052_stk_defconfig
│       └── hc32a052_stk.dts          128K Flash / 16K SRAM / USART0
├── dts/bindings/
│   ├── vendor-prefixes.txt           补上 zephyr 树里没有的 xhsc 前缀
│   └── serial/xhsc,hc32a052-usart.yaml
└── drivers/
    └── serial/uart_hc32.c            Zephyr UART 驱动（轮询）
```

当前的驱动与 SoC 支持矩阵：只有 **HC32A052**，只有 **UART（轮询）**。

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

## 注意

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
