/**
 *******************************************************************************
 * @file  interrupts_hc32a052.c
 * @brief Interrupt management
 @verbatim
   Change Logs:
   Date             Author          Notes
   2022-07-15       MADS            First version
 @endverbatim
 *******************************************************************************
 * Copyright (C) 2022, Xiaohua Semiconductor Co., Ltd. All rights reserved.
 *
 * This software component is licensed by XHSC under BSD 3-Clause license
 * (the "License"); You may not use this file except in compliance with the
 * License. You may obtain a copy of the License at:
 *                    opensource.org/licenses/BSD-3-Clause
 *
 *******************************************************************************
 */

/*******************************************************************************
 * Include files
 ******************************************************************************/
#include "ddl.h"
#include "interrupts_hc32a052.h"

/**
 * @brief IRQ WEAK DEFINE
 */
__WEAK void SysTick_IRQHandler(void);


__WEAK void PortA_IRQHandler(void);
__WEAK void PortB_IRQHandler(void);
__WEAK void PortC_IRQHandler(void);
__WEAK void PortD_IRQHandler(void);
__WEAK void Dmac_IRQHandler(void);
__WEAK void Tim3_IRQHandler(void);
__WEAK void Usart0_IRQHandler(void);
__WEAK void Usart1_IRQHandler(void);
__WEAK void LpUart0_IRQHandler(void);
__WEAK void LpUart1_IRQHandler(void);
__WEAK void Spi0_IRQHandler(void);
__WEAK void Spi1_IRQHandler(void);
__WEAK void I2c0_IRQHandler(void);
__WEAK void I2c1_IRQHandler(void);
__WEAK void Atim0_IRQHandler(void);
__WEAK void Atim1_IRQHandler(void);
__WEAK void Atim2_IRQHandler(void);
__WEAK void LpTim_IRQHandler(void);
__WEAK void Gtim0_IRQHandler(void);
__WEAK void Gtim1_IRQHandler(void);
__WEAK void Btim0_IRQHandler(void);
__WEAK void Btim1_IRQHandler(void);
__WEAK void Btim2_IRQHandler(void);
__WEAK void Btim3_IRQHandler(void);
__WEAK void Btim4_IRQHandler(void);
__WEAK void Btim5_IRQHandler(void);
__WEAK void I2cslv_IRQHandler(void);
__WEAK void Iwdt_IRQHandler(void);
__WEAK void Wwdt_IRQHandler(void);
__WEAK void Rtc_IRQHandler(void);
__WEAK void Adc_IRQHandler(void);
__WEAK void Vc0_IRQHandler(void);
__WEAK void Vc1_IRQHandler(void);
__WEAK void Lvd_IRQHandler(void);
__WEAK void Can_IRQHandler(void);
__WEAK void Atim_Bk_IRQHandler(void);
__WEAK void Flash_IRQHandler(void);
__WEAK void Ram_IRQHandler(void);
__WEAK void Ctrim_IRQHandler(void);
__WEAK void Fault_IRQHandler(void);

/**
 * @brief NVIC 中断使能
 *
 * @param [in]  enIrq          中断号枚举类型
 * @param [in]  enLevel        中断优先级枚举类型
 * @param [in]  bEn            中断开关 TRUE:使能；FALSE:禁止
 * @retval None
 */
void EnableNvic(IRQn_Type enIrq, en_irq_level_t enLevel, boolean_t bEn)
{
    NVIC_ClearPendingIRQ(enIrq);
    NVIC_SetPriority(enIrq, (uint32_t)enLevel);
    if (TRUE == bEn)
    {
        NVIC_EnableIRQ(enIrq);
    }
    else
    {
        NVIC_DisableIRQ(enIrq);
    }
}

/**
 * @brief NVIC hardware fault 中断实现
 *
 *
 * @retval None
 */
void HardFault_Handler(void)
{
    volatile int a = 0;

    while( 0 == a)
    {
        ;
    }

}


/**
 * @brief NVIC SysTick 中断实现
 *
 * @retval None
 */
void SysTick_Handler(void)
{
   SysTick_IRQHandler();
}

/**
 * @brief GPIO PortA 中断处理函数
 *
 * @retval None
 */
void PORTA_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_PORTA)
    PortA_IRQHandler();
#endif
}

/**
 * @brief GPIO PortB 中断处理函数
 *
 * @retval None
 */
void PORTB_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_PORTB)
    PortB_IRQHandler();
#endif
}

/**
 * @brief GPIO PortC 中断处理函数
 *
 * @retval None
 */
void PORTC_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_PORTC)
    PortC_IRQHandler();
#endif
}

/**
 * @brief GPIO PortD 中断处理函数
 *
 * @retval None
 */
void PORTD_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_PORTD)
    PortD_IRQHandler();
#endif
}

/**
 * @brief DMAC  中断处理函数
 *
 * @retval None
 */
void DMAC_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_DMAC)
    Dmac_IRQHandler();
#endif
}

/**
 * @brief ATIM3 中断处理函数
 *
 * @retval None
 */
void ATIM3_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_ATIM3)
    Tim3_IRQHandler();
#endif
}

/**
 * @brief USART0 中断处理函数
 *
 * @retval None
 */
void USART0_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_USART0)
    Usart0_IRQHandler();
#endif
}

/**
 * @brief USART1 中断处理函数
 *
 * @retval None
 */
void USART1_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_USART1)
    Usart1_IRQHandler();
#endif
}

/**
 * @brief LPUART0 低功耗串口0 中断处理函数
 *
 * @retval None
 */
void LPUART0_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_LPUART0)
    LpUart0_IRQHandler();
#endif
}

/**
 * @brief LPUART1 低功耗串口1 中断处理函数
 *
 * @retval None
 */
void LPUART1_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_LPUART1)
    LpUart1_IRQHandler();
#endif
}

/**
 * @brief SPI0 中断处理函数
 *
 * @retval None
 */
void SPI0_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_SPI0)
    Spi0_IRQHandler();
#endif
}

/**
 * @brief SPI1 中断处理函数
 *
 * @retval None
 */
void SPI1_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_SPI1)
    Spi1_IRQHandler();
#endif
}

/**
 * @brief I2C0 中断处理函数
 *
 * @retval None
 */
void I2C0_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_I2C0)
    I2c0_IRQHandler();
#endif
}

/**
 * @brief I2C1 中断处理函数
 *
 * @retval None
 */
void I2C1_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_I2C1)
    I2c1_IRQHandler();
#endif
}

/**
 * @brief ATIM0 中断处理函数
 *
 * @retval None
 */
void ATIM0_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_ATIM0)
    Atim0_IRQHandler();
#endif
}

/**
 * @brief ATIM1 中断处理函数
 *
 * @retval None
 */
void ATIM1_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_ATIM1)
    Atim1_IRQHandler();
#endif
}

/**
 * @brief ATIM2 中断处理函数
 *
 * @retval None
 */
void ATIM2_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_ATIM2)
    Atim2_IRQHandler();
#endif
}

/**
 * @brief LPTIM 低功耗定时器中断处理函数
 *
 * @retval None
 */
void LPTIM_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_LPTIM)
    LpTim_IRQHandler();
#endif
}

/**
 * @brief GTIM0 BTIM0/1/2 中断处理函数
 *
 * @retval None
 */
void GTIM0_BTIM0_1_2_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_GTIM0)
    Gtim0_IRQHandler();
#endif

#if (INT_CALLBACK_ON == INT_CALLBACK_BTIM0)
    Btim0_IRQHandler();
#endif

#if (INT_CALLBACK_ON == INT_CALLBACK_BTIM1)
    Btim1_IRQHandler();
#endif

#if (INT_CALLBACK_ON == INT_CALLBACK_BTIM2)
    Btim2_IRQHandler();
#endif
}

/**
 * @brief GTIM1 BTIM3/4/5 中断处理函数
 *
 * @retval None
 */
void GTIM1_BTIM3_4_5_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_GTIM1)
    Gtim1_IRQHandler();
#endif

#if (INT_CALLBACK_ON == INT_CALLBACK_BTIM3)
    Btim3_IRQHandler();
#endif

#if (INT_CALLBACK_ON == INT_CALLBACK_BTIM4)
    Btim4_IRQHandler();
#endif

#if (INT_CALLBACK_ON == INT_CALLBACK_BTIM5)
    Btim5_IRQHandler();
#endif
}

/**
 * @brief I2CSLV 中断处理函数
 *
 * @retval None
 */
void I2CSLV_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_I2CSLV)
    I2cslv_IRQHandler();
#endif
}

/**
 * @brief WWDT 中断处理函数
 *
 * @retval None
 */
void WWDT_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_WWDT)
    Wwdt_IRQHandler();
#endif
}

/**
 * @brief IWDT 中断处理函数
 *
 * @retval None
 */
void IWDT_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_IWDT)
    Iwdt_IRQHandler();
#endif
}

/**
 * @brief RTC 中断处理函数
 *
 * @retval None
 */
void RTC_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_RTC)
    Rtc_IRQHandler();
#endif
}

/**
 * @brief ADC 中断处理函数
 *
 * @retval None
 */
void ADC_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_ADC)
    Adc_IRQHandler();
#endif
}

/**
 * @brief VC0/1 LVD 中断处理函数
 *
 * @retval None
 */
void VC0_1_LVD_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_LVD)
    Lvd_IRQHandler();
#endif

#if (INT_CALLBACK_ON == INT_CALLBACK_VC0)
    Vc0_IRQHandler();
#endif

#if (INT_CALLBACK_ON == INT_CALLBACK_VC1)
    Vc1_IRQHandler();
#endif
}

/**
 * @brief CAN 中断处理函数
 *
 * @retval None
 */
void CAN_IRQHandler(void)
{
    Can_IRQHandler();
}

/**
 * @brief ATIM_BK 中断处理函数
 *
 * @retval None
 */
void ATIM_BK_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_ATIM_BK)
    Atim_Bk_IRQHandler();
#endif
}

/**
 * @brief FLASH/RAM 中断处理函数
 *
 * @retval None
 */
void FLASH_RAM_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_FLASH)
    Flash_IRQHandler();
#endif

#if (INT_CALLBACK_ON == INT_CALLBACK_RAM)
    Ram_IRQHandler();
#endif
}

/**
 * @brief CTRIM/FAULT 中断处理函数
 *
 * @retval None
 */
void CTRIM_FAULT_IRQHandler(void)
{
#if (INT_CALLBACK_ON == INT_CALLBACK_CTRIM)
    Ctrim_IRQHandler();
#endif

#if (INT_CALLBACK_ON == INT_CALLBACK_FAULT)
    Fault_IRQHandler();
#endif
}

/******************************************************************************/
/* EOF (not truncated)                                                        */
/******************************************************************************/
