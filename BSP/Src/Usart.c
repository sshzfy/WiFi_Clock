#include "Usart.h"
#include "stm32f4xx.h"
#include "BuildConfig.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "Timer.h"

#include "AT.h"

/* ==================== USART1 RX 环形缓冲(供AT轮询读取) ====================
 * 任务级轮询(RXNE)在任务睡眠/被抢占期间会溢出丢字节,
 * 故改用RXNE中断将每个字节即时收入环形缓冲, AT任务再从缓冲读取。
 * ISR优先级=5(configMAX_SYSCALL_INTERRUPT_PRIORITY), 未调用RTOS API。
 * ======================================== */

#define USART1_RX_RING_SIZE 4096 // RX环形缓冲区大小 4KB

static volatile char rx_ring[USART1_RX_RING_SIZE];
static volatile uint16_t rx_head = 0U; // RX写指针
static volatile uint16_t rx_tail = 0U; // RX读指针
static volatile uint16_t rx_ovf = 0U;  // 溢出丢字节次数

void USART1_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructre;
    USART_InitTypeDef USART_InitStructure;
    NVIC_InitTypeDef NVIC_InitStructre;

    GPIO_PinAFConfig(AT_USART1_GPIO_PORT, GPIO_PinSource9, GPIO_AF_USART1);
    GPIO_PinAFConfig(AT_USART1_GPIO_PORT, GPIO_PinSource10, GPIO_AF_USART1);

    GPIO_StructInit(&GPIO_InitStructre);
    GPIO_InitStructre.GPIO_Pin = AT_USART1_GPIO_PIN_TX | AT_USART1_GPIO_PIN_RX;
    GPIO_InitStructre.GPIO_Mode = GPIO_Mode_AF; // 复用模式
    GPIO_InitStructre.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructre.GPIO_OType = GPIO_OType_PP; // 推挽输出
    GPIO_InitStructre.GPIO_PuPd = GPIO_PuPd_UP;   // 上拉
    GPIO_Init(AT_USART1_GPIO_PORT, &GPIO_InitStructre);

    USART_StructInit(&USART_InitStructure);
    USART_InitStructure.USART_BaudRate = 115200;                                    // 波特率 115200bps
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;                     // 字长 8位
    USART_InitStructure.USART_StopBits = USART_StopBits_1;                          // 停止位 1位
    USART_InitStructure.USART_Parity = USART_Parity_No;                             // 校验位 无校验
    USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;                 // 模式 RX/TX
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None; // 硬件流控制 无硬件流控制
    USART_Init(USART1, &USART_InitStructure);

    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE); // RXNE中断 → Usart.c环形缓冲

    NVIC_InitStructre.NVIC_IRQChannel = USART1_IRQn;         // 使能USART1_IRQn
    NVIC_InitStructre.NVIC_IRQChannelPreemptionPriority = 5; // 抢占优先级
    NVIC_InitStructre.NVIC_IRQChannelSubPriority = 0;        // 子优先级
    NVIC_InitStructre.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructre);

    rx_head = 0U;
    rx_tail = 0U;
    rx_ovf = 0U;

    USART_Cmd(USART1, ENABLE);
}

void USART1_IRQHandler(void)
{
    uint32_t sr = USART1->SR;
    char c = (char)USART1->DR; // 读DR: 同时清RXNE与ORE

    /* 硬件接收溢出: 至少丢了1个字节, 计入丢包统计 */
    if (sr & USART_SR_ORE)
    {
        rx_ovf++;
    }

    if (sr & USART_SR_RXNE)
    {
        uint16_t next = (uint16_t)((rx_head + 1) % USART1_RX_RING_SIZE);

        /* 非满则入队 */
        if (next != rx_tail)
        {
            rx_ring[rx_head] = c;
            rx_head = next;
        }
        else
        {
            rx_ovf++;
        }
    }
}

/** @brief 获取USART1接收环形缓冲中有效字节数
 *  @return uint16_t 有效字节数
 */
uint16_t Usart1_RX_Count(void)
{
    uint16_t tail = rx_tail;
    uint16_t head = rx_head;

    return (uint16_t)((head - tail + USART1_RX_RING_SIZE) % USART1_RX_RING_SIZE); // 计算有效字节数
}

/** @brief 从USART1接收环形缓冲中读取一个字节
 *  @return int 字节值(-1:缓冲区为空)
 */
int Usart1_RX_Read(void)
{
    int c;

    /* 环形缓冲区为空 */
    if (rx_head == rx_tail)
        return -1;

    c = (uint8_t)rx_ring[rx_tail];                             // 读取读指针字符
    rx_tail = (uint16_t)((rx_tail + 1) % USART1_RX_RING_SIZE); // 更新读指针

    return c;
}

/** @brief  缓冲满导致丢字节的次数 */
uint16_t Usart1_RX_Ovf(void)
{
    return rx_ovf;
}

/** @brief 丢弃USART1接收环形缓冲中尚未取走的全部字节 */
void Usart1_RX_Flush(void)
{
    /* USART1 中断优先级 = 5 = configMAX_SYSCALL_INTERRUPT_PRIORITY,
     * 可被 taskENTER_CRITICAL() 屏蔽, 临界区内不会与 ISR 争抢指针 */
    taskENTER_CRITICAL();
    rx_head = rx_tail;
    taskEXIT_CRITICAL();
}

#if (DEBUG_SUPPORT == 1)

/* ==================== USART2 调试输出(printf 经 DMA) ====================
 * - printf 每个字符先入行缓冲, 遇 '\n' 或缓冲满时经 DMA 一次性发送。
 * - 整行由一个互斥量保护: 每行首个字符抢锁并记录所属任务, 换行后释放,
 *   避免多任务 printf 互相穿插(整行原子输出)。
 * - 调度器未运行 / 中断上下文中退化为逐字节轮询(ISR 内不应 printf)。
 * ======================================== */

#define DBG_TX_BUF_SIZE 256 // 行缓冲区大小 256字节

static uint8_t dbg_buf[DBG_TX_BUF_SIZE];   // 行缓冲区
static uint16_t dbg_len = 0;               // 行缓冲区有效字符数
static SemaphoreHandle_t dbg_mtx = NULL;   // 行缓冲区互斥量,保护整行输出
static TaskHandle_t dbg_line_owner = NULL; // 当前持有锁的任务句柄
static bool dbg_dma_ready = false;         // DMA是否初始化

static bool Dbg_Can_Block(void);
static void Dbg_Byte_Poll(uint8_t c);
static void Dbg_Dma_Init(void);
static void Dbg_Flush(void);

/**
 * @brief 判断当前上下文是否允许调用 FreeRTOS 的阻塞 API
 * @return true 可以阻塞发送, false 不能阻塞发送
 * */
static bool Dbg_Can_Block(void)
{
    if (xPortIsInsideInterrupt() == pdTRUE)
        return false;
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING)
        return false;

    return true;
}

/**
 * @brief 逐字节轮询发送字符 c
 * @param c 字符
 * */
static void Dbg_Byte_Poll(uint8_t c)
{
    while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET)
    {
    }
    USART_SendData(USART2, c);
}

static void Dbg_Dma_Init(void)
{
    if (dbg_dma_ready)
        return;

    DMA_DeInit(DMA1_Stream6);
    DMA_InitTypeDef DMA_InitStructure;

    DMA_StructInit(&DMA_InitStructure);
    DMA_InitStructure.DMA_Channel = DMA_Channel_4;                          // 通道4对应USART2_TX
    DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)&(USART2->DR);     // 从USART2_DR寄存器读取
    DMA_InitStructure.DMA_Memory0BaseAddr = (uint32_t)dbg_buf;              // 从行缓冲区读取
    DMA_InitStructure.DMA_DIR = DMA_DIR_MemoryToPeripheral;                 // 从内存到外设
    DMA_InitStructure.DMA_BufferSize = 0;                                   // 占位，动态计算缓冲区大小
    DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;        // 外设地址不增加
    DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;                 // 内存地址增加
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte; // 外设数据大小为字节
    DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;         // 内存数据大小为字节
    DMA_InitStructure.DMA_Mode = DMA_Mode_Normal;                           // 正常模式
    DMA_InitStructure.DMA_Priority = DMA_Priority_Low;                      // 低优先级
    DMA_InitStructure.DMA_FIFOMode = DMA_FIFOMode_Disable;                  // 禁用FIFO
    DMA_Init(DMA1_Stream6, &DMA_InitStructure);

    USART_DMACmd(USART2, USART_DMAReq_Tx, ENABLE);

    dbg_dma_ready = true;
}

/**
 * @brief 同步发送当前行缓冲(仅任务上下文调用)
 * @note 仅在任务上下文调用, 不能在中断上下文调用
 * */
static void Dbg_Flush(void)
{
    uint16_t n = dbg_len; // 存储有效字符数

    if (n == 0)
        return;

    dbg_len = 0; // 立即重置长度（持有锁，安全），防止下次重复搬运

    /* 修改 DMA 参数 */
    Dbg_Dma_Init();
    DMA_Cmd(DMA1_Stream6, DISABLE); // 禁用 DMA1_Stream6, 允许安全修改 DMA 参数
    uint64_t _t0 = TIM5_Get_ms();
    while (DMA_GetCmdStatus(DMA1_Stream6) != DISABLE)
    {
        if (TIM5_Get_ms() - _t0 > 1000U)
            return; /* 1 秒保护：异常时放弃本行输出, 防止整系统卡死 */
    }

    DMA1_Stream6->NDTR = n;                 // 设置要搬运的字节数
    DMA1_Stream6->M0AR = (uint32_t)dbg_buf; // 设置内存地址为行缓冲区
    DMA1_Stream6->CR |= DMA_SxCR_MINC;      // 确保内存地址递增（逐字节读取）

    /* 清除残留标志，防止旧状态干扰后续 while 循环对本次传输完成的判断 */
    USART_ClearFlag(USART2, USART_FLAG_TC);
    DMA_ClearFlag(DMA1_Stream6, DMA_FLAG_FEIF6 | DMA_FLAG_TCIF6 | DMA_FLAG_TEIF6);
    DMA_Cmd(DMA1_Stream6, ENABLE); // 启用 DMA1_Stream6, 开始传输

    /* 等所有字节由 DMA 搬入 USART2_DR 寄存器,传输未完成则让出 CPU */
    bool dma_tc = false;
    uint64_t _t1 = TIM5_Get_ms();
    while (DMA_GetFlagStatus(DMA1_Stream6, DMA_FLAG_TCIF6) == RESET)
    {
        if (Dbg_Can_Block())
            vTaskDelay(1);
        if (TIM5_Get_ms() - _t1 > 2000U)
            break; /* 2 秒保护 */
    }
    dma_tc = (DMA_GetFlagStatus(DMA1_Stream6, DMA_FLAG_TCIF6) == SET); // 确认 DMA 传输完成
    DMA_ClearFlag(DMA1_Stream6, DMA_FLAG_TCIF6);                       // 清除 DMA1_Stream6_TCIF6标志位, 确保下一次发送从头开始

    /* DMA 传输未完成，停流避免 dbg_buf 和后续 printf 冲突 */
    if (!dma_tc)
    {
        DMA_Cmd(DMA1_Stream6, DISABLE); // 禁用 DMA1_Stream6
        return;
    }

    /* 等最后一个字节真正移出(整行发送完成), 传输未完成则让出CPU */
    uint64_t _t2 = TIM5_Get_ms();
    while (USART_GetFlagStatus(USART2, USART_FLAG_TC) == RESET)
    {
        if (Dbg_Can_Block())
            vTaskDelay(1);
        if (TIM5_Get_ms() - _t2 > 2000U)
            break; /* 2 秒保护 */
    }
}

int fputc(int ch, FILE *stream)
{
    uint8_t c = (uint8_t)ch;
    (void)stream;

    /* 调度器未运行 / 中断上下文: 退化为逐字节轮询
     * 互斥量在 Usart2_Debug_Init() 里创建, 正常流程不会走到这里的轮询分支。 */
    if (!Dbg_Can_Block() || dbg_mtx == NULL)
    {
        Dbg_Byte_Poll(c);
        return ch;
    }

    TaskHandle_t me = xTaskGetCurrentTaskHandle(); // 获取当前任务（调用printf函数的任务）句柄

    /* 整行互斥: 首个字符抢锁并记录行属主, 换行释放 */
    if (dbg_line_owner != me)
    {
        xSemaphoreTake(dbg_mtx, portMAX_DELAY); // 抢锁, 将锁的所有权交给当前任务
                                                // 如果锁被占用, 则阻塞等待任务释放锁
        dbg_line_owner = me;                    // 登记属主，后续字符免检
    }

    /* 缓冲满则提前刷出（但仍持有锁，行未结束） */
    if (dbg_len >= sizeof(dbg_buf))
        Dbg_Flush();

    /* 行缓冲区未满, 写入字符到行缓冲区 */
    dbg_buf[dbg_len++] = c;

    /* 遇到换行符：先重置行属主为 NULL, 再释放锁：
     * 防止任务 B 在 Give 之后立刻拿到锁并登记自己为 owner
     * 随后任务 A 的 dbg_line_owner = NULL 将其覆盖，导致死锁*/
    if (c == '\n')
    {
        Dbg_Flush();
        dbg_line_owner = NULL;
        xSemaphoreGive(dbg_mtx);
    }

    return ch;
}

void Usart2_Debug_Init(void)
{
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource2, GPIO_AF_USART2);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource3, GPIO_AF_USART2);

    GPIO_InitTypeDef GPIO_InitStructure;
    GPIO_StructInit(&GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2 | GPIO_Pin_3;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    USART_InitTypeDef USART_InitStructure;
    USART_StructInit(&USART_InitStructure);
    USART_InitStructure.USART_BaudRate = 115200U;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_Init(USART2, &USART_InitStructure);
    USART_Cmd(USART2, ENABLE);

    DBG_PRINTF("[SYS]Build Date:%s %s\r\n", __DATE__, __TIME__);

    /* 初始化互斥量 */
    if (dbg_mtx == NULL)
        dbg_mtx = xSemaphoreCreateMutex();
}
#else
void Usart2_Debug_Init(void)
{
    /* 调试打印已关闭: 不初始化 USART2, 不占用 PA2/PA3 */
}
#endif
