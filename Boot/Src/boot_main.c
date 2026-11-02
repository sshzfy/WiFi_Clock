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

typedef void (*pFunc)(void); // 跳转函数指针

/** @brief App 轻量合法性检查
 *  @note 仅检查栈顶、复位向量
 */
static bool App_QuickValid(void)
{
    const uint32_t sp = *(volatile uint32_t *)APP_BASE;        // 栈顶
    const uint32_t pc = *(volatile uint32_t *)(APP_BASE + 4U); // 复位向量

    /* 检查栈顶8字节对齐，且在 SRAM/CCM 范围内 */
    if (!((sp & 0x7U) != 0U ||
          (sp >= IRAM1_BASE && sp <= IRAM1_END) ||
          (sp >= IRAM2_BASE && sp <= IRAM2_END)))
        return false;
    /* 复位向量在 App 区内且 bit0=1 */
    if (pc < APP_BASE || pc >= APP_END || (pc & 1U) == 0U)
        return false;

    return true;
}

/** @brief  跳转到 App */
void Boot_JumpToApp(void)
{
    const uint32_t sp = *(volatile uint32_t *)APP_BASE;
    const uint32_t pc = *(volatile uint32_t *)(APP_BASE + 4U);
    pFunc jump = (pFunc)pc;

    /* 关中断 */
    __disable_irq();

    /* 关 SysTick */
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL = 0U;

    /* 清 NVIC */
    for (uint32_t i = 0U; i < sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0]); i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }

    /* 关外设 */
    USART_Cmd(USART2, DISABLE);

    /* 重新定位 VTOR */
    SCB->VTOR = APP_BASE;
    __DSB();
    __ISB();

    /* 设置 App 的栈SP
     * __set_MSP 之后不能再依赖 Bootloader 的栈 */
    __set_MSP(sp);

    /* 跳转到 App */
    jump();
}

/** @brief  Bootloader 入口 */
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

    /* 根据 state 决定行为 */
    switch (p.state)
    {
    case BOOT_ST_OTA_PENDING:
    case BOOT_ST_OTA_WRITING:
        /* App 下载完新固件并写了参数。搬运成功则直接跳转 */
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
        /* 新 App 已写入但未确认 */
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
