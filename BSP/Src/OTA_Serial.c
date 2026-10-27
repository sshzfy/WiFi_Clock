#include "OTA_Serial.h"

#include "stm32f4xx.h"

/* ============================================================
 * OTA 离线传输串口(USART2)
 * 与 DEBUG_SUPPORT 互斥：测试时保持DEBUG_SUPPORT=0，
 * 否则两套初始化互相覆盖。
 * ------------------------------------------------------------ *
 * 用中断而不是轮询:
 *   115200 波特率下每个字节间隔约 87 us。OTA 任务会被优先级
 *   更高的 dht22(4)/ui(3) 抢占，轮询一旦错过 RXNE 就丢字节。
 *   中断把字节立刻搬进缓冲，任务再慢慢消费。
 * ============================================================ */

#define OTA_SERIAL_RX_SIZE 4096U // 2 的幂，方便用掩码取模

static volatile uint8_t s_rx[OTA_SERIAL_RX_SIZE];
static volatile uint16_t s_head = 0U; // 写指针（ISR 推进）
static volatile uint16_t s_tail = 0U; // 读指针（任务推进）
static volatile uint16_t s_ovf = 0U;  // 溢出丢字节次数

/** @brief  初始化 USART2 为 OTA 接收串口（RX 中断 + 环形缓冲） */
void OTA_Serial_Init(void)
{
    GPIO_InitTypeDef gpio;
    USART_InitTypeDef usart;
    NVIC_InitTypeDef nvic;

    /* 引脚与波特率配置和 Bootloader 的 boot_uart.c 一致 */
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);

    GPIO_PinAFConfig(GPIOA, GPIO_PinSource2, GPIO_AF_USART2);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource3, GPIO_AF_USART2);

    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin = GPIO_Pin_2 | GPIO_Pin_3;
    gpio.GPIO_Mode = GPIO_Mode_AF;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOA, &gpio);

    USART_StructInit(&usart);
    usart.USART_BaudRate = 115200U;
    usart.USART_WordLength = USART_WordLength_8b;
    usart.USART_StopBits = USART_StopBits_1;
    usart.USART_Parity = USART_Parity_No;
    usart.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_Init(USART2, &usart);

    /* RXNE：每收到一个字节触发一次中断 */
    USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);

    nvic.NVIC_IRQChannel = USART2_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 5;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);

    USART_Cmd(USART2, ENABLE);

    s_head = 0U;
    s_tail = 0U;
    s_ovf = 0U;
}

/** @brief  USART2 接收中断：字节进环形缓冲 */
void USART2_IRQHandler(void)
{
    if (USART_GetITStatus(USART2, USART_IT_RXNE) != RESET)
    {
        uint8_t c = (uint8_t)USART_ReceiveData(USART2);
        uint16_t next = (uint16_t)((s_head + 1U) & (OTA_SERIAL_RX_SIZE - 1U));

        /* next == tail 表示缓冲满：丢字节并计数 */
        if (next != s_tail)
        {
            s_rx[s_head] = c;
            s_head = next;
        }
        else
        {
            s_ovf++;
        }
    }
}

/**
 * @brief  缓冲内可读字节数
 * @note   2 的幂掩码下，(head - tail) 的无符号回绕正好等于环形差值。
 */
uint16_t OTA_Serial_Available(void)
{
    return (uint16_t)((s_head - s_tail) & (OTA_SERIAL_RX_SIZE - 1U));
}

/** @brief  读一个字节 */
int OTA_Serial_ReadByte(void)
{
    uint8_t c;

    if (s_head == s_tail)
        return -1;

    c = s_rx[s_tail];
    s_tail = (uint16_t)((s_tail + 1U) & (OTA_SERIAL_RX_SIZE - 1U));

    return (int)c;
}

/**
 * @brief  尽量多读，返回实际读到的字节数（可能为 0）
 * @param  buf 输出缓冲
 * @param  len 期望字节数
 * @return 实际读到的字节数
 */
uint16_t OTA_Serial_Read(uint8_t *buf, uint16_t len)
{
    uint16_t n = 0U;

    while (n < len && s_head != s_tail)
    {
        buf[n++] = s_rx[s_tail];
        s_tail = (uint16_t)((s_tail + 1U) & (OTA_SERIAL_RX_SIZE - 1U));
    }

    return n;
}

/** @brief  清空接收缓冲 */
void OTA_Serial_Flush(void)
{
    s_tail = s_head;
}

/** @brief  缓冲满导致丢字节的次数 */
uint16_t OTA_Serial_OverflowCount(void)
{
    return s_ovf;
}

/**
 * @brief  阻塞发送，末尾等 TC 保证最后一个字节真正发出
 * @param  buf 数据缓冲
 * @param  len 字节数
 * @note   只用于握手/结果等短包（几字节），不追求吞吐。
 */
void OTA_Serial_Send(const uint8_t *buf, uint16_t len)
{
    for (uint16_t i = 0U; i < len; i++)
    {
        while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET)
        {
        }
        USART_SendData(USART2, buf[i]);
    }

    while (USART_GetFlagStatus(USART2, USART_FLAG_TC) == RESET)
    {
    }
}
