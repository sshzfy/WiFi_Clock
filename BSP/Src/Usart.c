#include "Usart.h"

// static Usart_Received_Callback_t Usart_Received_Callback = NULL;

volatile char rx_data;

void Usart_Init(void)
{
    // 初始化GPIOA
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource9, GPIO_AF_USART1);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource10, GPIO_AF_USART1);

    GPIO_InitTypeDef GPIO_InitStruct;
    GPIO_StructInit(&GPIO_InitStruct);

    GPIO_InitStruct.GPIO_Pin = GPIO_Pin_9 | GPIO_Pin_10;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_AF;
    GPIO_InitStruct.GPIO_Speed = GPIO_High_Speed;
    GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_UP;

    GPIO_Init(GPIOA, &GPIO_InitStruct);
    // 初始化USART1
    USART_InitTypeDef USART_InitStruct;
    USART_StructInit(&USART_InitStruct);

    USART_InitStruct.USART_BaudRate = 115200U;
    USART_InitStruct.USART_WordLength = USART_WordLength_8b;
    USART_InitStruct.USART_StopBits = USART_StopBits_1;
    USART_InitStruct.USART_Parity = USART_Parity_No;
    USART_InitStruct.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_InitStruct.USART_HardwareFlowControl = USART_HardwareFlowControl_None;

    USART_Init(USART1, &USART_InitStruct);
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);
    USART_Cmd(USART1, ENABLE);
    // 初始化NVIC
    NVIC_InitTypeDef NVIC_InitStruct;

    NVIC_InitStruct.NVIC_IRQChannel = USART1_IRQn;
    NVIC_InitStruct.NVIC_IRQChannelPreemptionPriority = 5;
    NVIC_InitStruct.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStruct.NVIC_IRQChannelCmd = ENABLE;

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);

    NVIC_Init(&NVIC_InitStruct);
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
}

// static void Usart_Write(const char str[])
// {
//     int len = strlen(str);
//     for (int i = 0; i < len; i++)
//     {
//         USART_ClearFlag(USART1, USART_FLAG_TXE);
//         USART_SendData(USART1, str[i]);
//         while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET)
//             ;
//     }
// }

// void Usart_Received_Register(Usart_Received_Callback_t callback)
// {
//     Usart_Received_Callback = callback;
// }

void USART1_IRQHandler(void)
{
    if (USART_GetITStatus(USART1, USART_IT_RXNE) == SET)
    {
        rx_data = USART_ReceiveData(USART1);
        USART_ClearITPendingBit(USART1, USART_IT_RXNE);
    }
}
