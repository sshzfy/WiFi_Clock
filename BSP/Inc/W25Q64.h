#ifndef __W25Q64_H__
#define __W25Q64_H__

#include "main.h"
#include "stm32f4xx.h"
#include <stdbool.h>
#include <stdint.h>


#define W25Q64_BUSY_TIMEOUT     0x08000000U // 忙等待超时(轮询次数上限), 防止芯片异常/未焊接时死等

/* 定义W25Q64 Flash的GPIO引脚配置 */
#define W25Q64_GPIO_PORT    GPIOA      // 选择GPIOA端口(SPI1)
#define W25Q64_CS_PIN       GPIO_Pin_4 // 选择CS引脚为GPIOA_4
#define W25Q64_CLK_PIN      GPIO_Pin_5 // 选择时钟引脚为GPIOA_5
#define W25Q64_MISO_PIN     GPIO_Pin_6 // 选择MISO引脚为GPIOA_6
#define W25Q64_MOSI_PIN     GPIO_Pin_7 // 选择MOSI引脚为GPIOA_7

/* 芯片容量与结构参数 */
#define W25Q64_FLASH_SIZE       (8U * 1024U * 1024U)                     // 总容量 8MB
#define W25Q64_SECTOR_SIZE      4096U                                    // 扇区 4KB (最小擦除单位)
#define W25Q64_BLOCK32_SIZE     32768U                                   // 32KB 块
#define W25Q64_BLOCK_SIZE       65536U                                   // 64KB 块
#define W25Q64_PAGE_SIZE        256U                                     // 页 256字节
#define W25Q64_SECTOR_COUNT     (W25Q64_FLASH_SIZE / W25Q64_SECTOR_SIZE) // 可擦除单元总数(以4KB扇区为单位, 8MB/4KB = 2048)

/* 指令定义 */
#define W25Q64_CMD_WRITE_ENABLE     0x06 //写使能
#define W25Q64_CMD_WRITE_DISABLE    0x04 //写禁止
#define W25Q64_CMD_READ_SR1         0x05 //读状态寄存器1
#define W25Q64_CMD_READ_SR2         0x35 //读状态寄存器2
#define W25Q64_CMD_WRITE_SR         0x01 //写状态寄存器
#define W25Q64_CMD_READ_DATA        0x03 //读数据（标准）
#define W25Q64_CMD_FAST_READ        0x0B //快速读（带dummy）
#define W25Q64_CMD_PAGE_PROGRAM     0x02 //页编程（最多256字节）
#define W25Q64_CMD_SECTOR_ERASE     0x20 //4KB 扇区擦除
#define W25Q64_CMD_BLOCK32_ERASE    0x52 //32KB 块擦除
#define W25Q64_CMD_BLOCK64_ERASE    0xD8 //64KB 块擦除
#define W25Q64_CMD_CHIP_ERASE       0xC7 //整片擦除
#define W25Q64_CMD_JEDEC_ID         0x9F //读 JEDEC ID
#define W25Q64_CMD_READ_ID          0x90 //读厂商/器件ID
#define W25Q64_CMD_POWER_DOWN       0xB9 //进入掉电模式
#define W25Q64_CMD_RELEASE_PD       0xAB //释放掉电模式

/* 状态寄存器位定义 */
#define W25Q64_SR1_BUSY    (1 << 0) //忙标志位
#define W25Q64_SR1_WEL     (1 << 1) //写使能锁存位
#define W25Q64_SR1_BP0     (1 << 2) //块保护位0
#define W25Q64_SR1_BP1     (1 << 3) //块保护位1
#define W25Q64_SR1_BP2     (1 << 4) //块保护位2
#define W25Q64_SR1_TB      (1 << 5) //顶部/底部保护
#define W25Q64_SR1_SEC     (1 << 6) //扇区/块保护
#define W25Q64_SR2_QE      (1 << 1) //Quad使能位（SR2的S9）

/* 函数声明 */
/* 写类接口返回 true 表示本次操作在超时前完成, false 表示超时(芯片未响应) */
void W25Q64_Init(void);
uint16_t W25Q64_ReadId(void);
uint32_t W25Q64_ReadJedecId(void);
uint8_t W25Q64_IsBusy(void);
uint32_t W25Q64_GetBusyTimeoutCount(void);
void W25Q64_Read(uint32_t addr, uint8_t *buf, uint32_t len);
bool W25Q64_PageProgram(uint32_t addr, const uint8_t *buf, uint32_t len);
bool W25Q64_SectorErase(uint32_t addr);
bool W25Q64_Block32Erase(uint32_t addr);
bool W25Q64_ChipErase(void);

#endif /* __W25Q64_H__ */
