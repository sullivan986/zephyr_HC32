## V1.0.0  Nov 15, 2023
- Initial release.

## V1.0.1  Nov 15, 2024
- 修改ATIM3样例（比较匹配CH3A触发adc ch3a-trig-adc）；
- 删除ADC的BGR1.2V采样源；
- 删除VC的BGR1.2V比较源；
- 修改Flash编程样例，编程结束后将CR.OP改为0（读模式）；
- 修改Flash库函数，Flash结束地址判断优化、Flash_LockSet函数添加回读确认、片擦函数优化；
- 修改Debug.c和Debug.h库函数问题；
- 修改Spi_Init()函数初始化的关于主机模式延后采样的配置问题；
- 删除lvd.c多余的宏定义
- 修改rtc.c超时函数和例程