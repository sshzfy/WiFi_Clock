#include "Usart.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdio.h>

/* ==================== USART1 RX 环形缓冲(供AT轮询读取) ====================
 * 说明: USART1 仅1字节RDR无FIFO。任务级轮询(RXNE)在任务睡眠/被抢占期间
 * 会溢出丢字节, 故改用RXNE中断将每个字节即时收入环形缓冲, AT任务再从缓冲读取。
 * ISR优先级=5(configMAX_SYSCALL_INTERRUPT_PRIORITY), 未调用RTOS API。
 * USART1的GPIO/波特率/RXNE中断使能由AT.c的AT_USART_Init完成, 本文件只负责
 * 中断服务 + 环形缓冲存取。
 */

#define USART1_RX_RING_SIZE 512

static char rx_ring[USART1_RX_RING_SIZE];
static volatile uint16_t rx_head = 0;
static volatile uint16_t rx_tail = 0;

void USART1_IRQHandler(void)
{
    if (USART_GetITStatus(USART1, USART_IT_RXNE) == SET)
    {
        char c = (char)USART_ReceiveData(USART1); // 读DR会同时清除RXNE/ORE
        uint16_t next = (uint16_t)((rx_tail + 1) % USART1_RX_RING_SIZE);
        if (next != rx_head) /* 非满则入队 */
        {
            rx_ring[rx_tail] = c;
            rx_tail = next;
        }
        /* 满则丢弃 */
        USART_ClearITPendingBit(USART1, USART_IT_RXNE);
    }
}

uint16_t Usart1_RX_Count(void)
{
    return (uint16_t)((rx_tail - rx_head + USART1_RX_RING_SIZE) % USART1_RX_RING_SIZE);
}

char Usart1_RX_Read(void)
{
    char c;

    if (rx_head == rx_tail)
        return '\0';
    c = rx_ring[rx_head];
    rx_head = (uint16_t)((rx_head + 1) % USART1_RX_RING_SIZE);
    return c;
}

/* ==================== USART2 调试输出(printf 经 DMA) ====================
 * 设计:
 *  - printf 每个字符先入行缓冲, 遇 '\n' 或缓冲满时经 DMA1_Stream6(USART2_TX)
 *    一次性发送, CPU 无需逐字节等 TXE。
 *  - 整行由一个互斥量保护: 每行首个字符抢锁并记录所属任务, 换行后释放,
 *    避免多任务 printf 互相穿插(整行原子输出)。
 *  - 调度器未运行 / 中断上下文中退化为逐字节轮询(ISR 内不应 printf)。
 */

#define DBG_TX_BUF_SIZE 256

static uint8_t dbg_buf[DBG_TX_BUF_SIZE];
static uint16_t dbg_len = 0;
static SemaphoreHandle_t dbg_mtx = NULL;
static TaskHandle_t dbg_line_owner = NULL;
static bool dbg_dma_ready = false;

static bool Dbg_Can_Block(void)
{
    if (xPortIsInsideInterrupt() == pdTRUE)
        return false;
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING)
        return false;
    return true;
}

static void Dbg_Byte_Poll(uint8_t c)
{
    while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET)
        ;
    USART_SendData(USART2, c);
}

/* DMA1_Stream6, Channel4 = USART2_TX */
static void Dbg_Dma_Init(void)
{
    DMA_InitTypeDef s;

    if (dbg_dma_ready)
        return;
    dbg_dma_ready = true;

    DMA_DeInit(DMA1_Stream6);
    DMA_StructInit(&s);
    s.DMA_Channel = DMA_Channel_4;
    s.DMA_PeripheralBaseAddr = (uint32_t)&(USART2->DR);
    s.DMA_Memory0BaseAddr = (uint32_t)dbg_buf;
    s.DMA_DIR = DMA_DIR_MemoryToPeripheral;
    s.DMA_BufferSize = 0;
    s.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    s.DMA_MemoryInc = DMA_MemoryInc_Enable;
    s.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    s.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    s.DMA_Mode = DMA_Mode_Normal;
    s.DMA_Priority = DMA_Priority_Low;
    s.DMA_FIFOMode = DMA_FIFOMode_Disable;
    DMA_Init(DMA1_Stream6, &s);

    USART_DMACmd(USART2, USART_DMAReq_Tx, ENABLE);
}

/* 同步发送当前行缓冲(仅任务上下文调用) */
static void Dbg_Flush(void)
{
    uint16_t n = dbg_len;

    if (n == 0)
        return;
    dbg_len = 0;

    Dbg_Dma_Init();

    DMA_Cmd(DMA1_Stream6, DISABLE);
    while (DMA_GetCmdStatus(DMA1_Stream6) != DISABLE)
        ;

    DMA1_Stream6->NDTR = n;
    DMA1_Stream6->M0AR = (uint32_t)dbg_buf;
    DMA1_Stream6->CR |= DMA_SxCR_MINC;

    USART_ClearFlag(USART2, USART_FLAG_TC);
    DMA_ClearFlag(DMA1_Stream6, DMA_FLAG_FEIF6 | DMA_FLAG_TCIF6 | DMA_FLAG_TEIF6);
    DMA_Cmd(DMA1_Stream6, ENABLE);

    /* 等所有字节由DMA搬入DR */
    while (DMA_GetFlagStatus(DMA1_Stream6, DMA_FLAG_TCIF6) == RESET)
    {
        if (Dbg_Can_Block())
            vTaskDelay(1);
    }
    DMA_ClearFlag(DMA1_Stream6, DMA_FLAG_TCIF6);

    /* 等最后一个字节真正移出(整行发送完成) */
    while (USART_GetFlagStatus(USART2, USART_FLAG_TC) == RESET)
    {
        if (Dbg_Can_Block())
            vTaskDelay(1);
    }
}

int fputc(int ch, FILE *stream)
{
    uint8_t c = (uint8_t)ch;
    (void)stream;

    /* 调度器未运行 / 中断上下文: 退化为逐字节轮询 */
    if (!Dbg_Can_Block())
    {
        Dbg_Byte_Poll(c);
        return ch;
    }

    TaskHandle_t me = xTaskGetCurrentTaskHandle();

    if (dbg_mtx == NULL)
        dbg_mtx = xSemaphoreCreateMutex();

    /* 整行互斥: 首个字符抢锁并记录行属主, 换行释放 */
    if (dbg_line_owner != me)
    {
        xSemaphoreTake(dbg_mtx, portMAX_DELAY);
        dbg_line_owner = me;
    }

    if (dbg_len >= sizeof(dbg_buf))
        Dbg_Flush();

    dbg_buf[dbg_len++] = c;

    if (c == '\n')
    {
        Dbg_Flush();
        xSemaphoreGive(dbg_mtx);
        dbg_line_owner = NULL;
    }

    return ch;
}

void Usart2_Debug_Init(void)
{
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource2, GPIO_AF_USART2);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource3, GPIO_AF_USART2);

    GPIO_InitTypeDef GPIO_InitStruct;
    GPIO_StructInit(&GPIO_InitStruct);

    GPIO_InitStruct.GPIO_Pin = GPIO_Pin_2 | GPIO_Pin_3;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_AF;
    GPIO_InitStruct.GPIO_Speed = GPIO_High_Speed;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP;

    GPIO_Init(GPIOA, &GPIO_InitStruct);

    USART_InitTypeDef USART_InitStruct;
    USART_StructInit(&USART_InitStruct);

    USART_InitStruct.USART_BaudRate = 115200U;
    USART_InitStruct.USART_WordLength = USART_WordLength_8b;
    USART_InitStruct.USART_StopBits = USART_StopBits_1;
    USART_InitStruct.USART_Parity = USART_Parity_No;
    USART_InitStruct.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_InitStruct.USART_HardwareFlowControl = USART_HardwareFlowControl_None;

    USART_Init(USART2, &USART_InitStruct);
    USART_Cmd(USART2, ENABLE);

    if (dbg_mtx == NULL)
        dbg_mtx = xSemaphoreCreateMutex();
}
