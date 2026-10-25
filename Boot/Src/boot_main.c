#include "boot_layout.h"
#include "boot_param.h"
#include "boot_uart.h"
#include "boot_jump.h"
#include "boot_update.h"

#include "stm32f4xx.h"

/* ============================================================
 * Bootloader 入口
 * ------------------------------------------------------------
 * 烧在 0x08000000，是上电后执行的第一段代码。
 *
 * 启动流程：读参数区 -> 按 state 决定行为：
 *   IDLE / RECOVERY        快速检查 App，有效则跳转
 *   OTA_PENDING / WRITING  从 firmware.bin 搬运，失败尝试回滚
 *   ROLLBACK               从 backup.bin 恢复
 *   APP_UNCONFIRMED        未确认则 boot_try++，达 3 次回滚
 * ============================================================ */

typedef void (*pFunc)(void);

/**
 * @brief  App 轻量合法性检查
 * @return true = 看起来像一个合法 App
 * @note   不做整包 CRC，保证正常启动在几十毫秒内完成。
 *         检查三项：
 *           1. 向量表第 0 字（栈顶）在 SRAM/CCM 范围内；
 *           2. 向量表第 1 字（复位向量）在 App 区内且 bit0=1；
 *           3. 前 8 个字不能全是 0xFF（说明从未烧录）。
 *         整包 CRC 留给搬运后的回读校验（boot_update.c）。
 */
static bool App_QuickValid(void)
{
    uint32_t sp = *(volatile uint32_t *)APP_BASE;
    uint32_t pc = *(volatile uint32_t *)(APP_BASE + 4U);
    bool all_erased = true;

    if (!((sp >= 0x20000000UL && sp <= 0x20020000UL) ||
          (sp >= 0x10000000UL && sp <= 0x10010000UL)))
        return false;

    if (pc < APP_BASE || pc >= APP_END || (pc & 1U) == 0U)
        return false;

    for (uint32_t i = 0U; i < 8U; i++)
    {
        if (*(volatile uint32_t *)(APP_BASE + i * 4U) != 0xFFFFFFFFUL)
        {
            all_erased = false;
            break;
        }
    }

    return !all_erased;
}

/**
 * @brief  跳转到 App
 * @note   跳转顺序不能乱：
 *           关中断 -> 关 SysTick -> 清 NVIC -> 关外设 ->
 *           设 VTOR -> 设 MSP -> 开中断 -> 跳转。
 *         不设 VTOR，App 中断会打到 Bootloader 的向量表；
 *         不设 MSP，App 会继续用 Bootloader 的栈；
 *         最后不开中断，App 的 SysTick/FreeRTOS 起不来。
 *         导出给 boot_update.c：搬运完成后由它直接跳转。
 */
void Boot_JumpToApp(void)
{
    uint32_t sp = *(volatile uint32_t *)APP_BASE;
    uint32_t pc = *(volatile uint32_t *)(APP_BASE + 4U);
    pFunc jump = (pFunc)pc;

    __disable_irq();

    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL = 0U;

    for (uint32_t i = 0U; i < 8U; i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }

    USART_Cmd(USART2, DISABLE);

    SCB->VTOR = APP_BASE;
    __DSB();
    __ISB();

    /* __set_MSP 之后不能再依赖 Bootloader 的栈 */
    __set_MSP(sp);
    __enable_irq();
    jump();
}

/**
 * @brief  Bootloader 入口
 * @note   启动文件已调用 SystemInit()，进来时时钟 168 MHz。
 */
int main(void)
{
    boot_param_t p;

    Boot_Uart_Init();
    Boot_Uart_Puts("\n[BOOT] WiFi_Clock bootloader\n");

    Boot_Param_Default(&p);
    if (Boot_Param_Load(&p))
    {
        Boot_Uart_Puts("[BOOT] param: state=");
        Boot_Uart_PutU32(p.state);
        Boot_Uart_Puts(" seq=");
        Boot_Uart_PutU32(p.seq);
        Boot_Uart_Puts(" ver=");
        Boot_Uart_PutHex(p.app_version);
        Boot_Uart_Puts("\n");
    }
    else
    {
        Boot_Uart_Puts("[BOOT] param: empty\n");
    }

    switch (p.state)
    {
    case BOOT_ST_OTA_PENDING:
    case BOOT_ST_OTA_WRITING:
        /* App 下载完新固件并写了参数。搬运成功则直接跳转（不返回） */
        Boot_Uart_Puts("[BOOT] start update\n");
        if (Boot_Update_FromFile(OTA_FW_PATH))
            break;
        Boot_Uart_Puts("[BOOT] update failed, try rollback\n");
        if (Boot_Rollback_FromFile(OTA_BACKUP_PATH))
            break;
        Boot_Uart_Puts("[BOOT] no usable firmware, halt\n");
        for (;;)
        {
        }

    case BOOT_ST_ROLLBACK:
        Boot_Uart_Puts("[BOOT] rollback\n");
        if (Boot_Rollback_FromFile(OTA_BACKUP_PATH))
            break;
        Boot_Uart_Puts("[BOOT] rollback failed, halt\n");
        for (;;)
        {
        }

    case BOOT_ST_APP_UNCONFIRMED:
        /* 新 App 已写入但未确认。
         * 快速检查失败（搬运被破坏），或已尝试 3 次仍未确认 -> 回滚 */
        if (!App_QuickValid() || p.boot_try >= 3U)
        {
            Boot_Uart_Puts("[BOOT] app unconfirmed, rollback\n");
            if (Boot_Rollback_FromFile(OTA_BACKUP_PATH))
                break;
            Boot_Uart_Puts("[BOOT] rollback failed, halt\n");
            for (;;)
            {
            }
        }
        p.boot_try++;
        p.seq++; // 递增序号，否则 Load 会读回旧记录
        (void)Boot_Param_Save(&p);
        Boot_Uart_Puts("[BOOT] app valid, jump\n");
        Boot_JumpToApp();
        for (;;)
        {
        }

    case BOOT_ST_RECOVERY:
    default:
        /* IDLE：正常启动。RECOVERY 的串口救援 Phase 3 再实现，
         * 现在先按 IDLE 处理，App 无效就停住。 */
        if (!App_QuickValid())
        {
            Boot_Uart_Puts("[BOOT] app invalid, halt\n");
            for (;;)
            {
            }
        }
        Boot_Uart_Puts("[BOOT] app valid, jump\n");
        Boot_JumpToApp();
        for (;;)
        {
        }
    }
}
