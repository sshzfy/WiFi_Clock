#include "ATGM336H.h"
#include "Timer.h"

/* ============================================================
 * ATGM336H-5N 北斗+GPS 模块: NMEA 装配与解析(位置变了才通知)
 * ------------------------------------------------------------
 * 数据链路: Usart.c的USART3 RX中断 → 环形缓冲
 *           → ATGM336H_Poll()逐字节装配成行 → 校验和验证
 *           → 认出RMC就解析字段, 有变化才置ATGM_EV_UPDATE
 *
 * 为什么"每帧都收, 每秒只解析1次":
 *   模块固定1Hz发送, 每秒约12条语句共约615字节, 9600bps下占用64%。
 *   字节必须持续收走(512字节缓冲约0.83秒就满), 否则丢数据;
 *   而这12条语句里只有1条是RMC —— 每秒至多解析1次字段(十几微秒),
 *   没必要再为"多久解析一次"设节流。真正贵的是解析之后做什么
 *   (printf一行约3ms@115200), 那部分节流交给调用方。
 *
 * 一次标准正确的定位数据：
 *      $GNGGA,024948.000,3733.96252,N,11555.64279,E,1,06,3.9,59.6,M,0.0,M,,*42
 *      $GNGLL,3733.96252,N,11555.64279,E,024948.000,A,A*41
 *      $GPGSA,A,3,13,23,195,,,,,,,,,,5.9,3.9,4.4*0A
 *      $BDGSA,A,3,06,12,36,,,,,,,,,,5.9,3.9,4.4*25
 *      $GPGSV,2,1,07,12,,,28,13,30,120,29,15,27,078,,18,25,194,*41
 *      $GPGSV,2,2,07,23,85,059,29,24,,,18,195,29,166,22*4D
 *      $BDGSV,1,1,04,06,72,081,27,07,,,25,12,21,096,33,36,52,135,25*5E
 * **** $GNRMC,024948.000,A,3733.96252,N,11555.64279,E,4.86,85.35,011026,,,A*43
 *      $GNVTG,85.35,T,,M,4.86,N,9.00,K,A*1B
 *      $GNZDA,024948.000,01,10,2026,00,00*4D
 *      $GPTXT,01,01,01,ANTENNA OK*35
 *
 * 只认 $xxRMC, 只取其中5个字段:
 *   2=定位状态(A/V) 3=纬度 4=N/S 5=经度 6=E/W
 * 其余语句与字段(时间/速度/航向/日期)全部跳过; 校验失败的帧直接丢弃。
 * ============================================================ */

/* ==================== 内部状态 ==================== */

static char s_line[ATGM_LINE_MAX]; // 当前正在装配的NMEA行
static uint16_t s_line_len = 0;    // 已装配字节数
static bool s_line_drop = false;   // 本行超长, 丢弃到行尾为止

static ATGM_Info_t gps_info; // 定位结果(只被ATGM336H_Poll写)

#if (ATGM_DEBUG_ECHO == 1)
static uint32_t s_echo_bad = 0; // 错误帧回显限流
#endif

/* ==================== 工具函数 ==================== */

/**
 * @brief 取下一个逗号分隔字段
 * @param pp 指向当前字段指针的指针
 * @return 字段首址(空字段返回空串); 没有更多字段时返回NULL
 * @note  就地把逗号改成'\0', 因此返回的是独立字符串
 */
static char *ATGM_NextField(char **pp)
{
    char *start;
    char *comma;

    if (pp == NULL || *pp == NULL)
        return NULL;

    start = *pp;
    comma = strchr(start, ','); // 查找逗逗号
    if (comma != NULL)
    {
        *comma = '\0';   // 把逗号改成'\0'
        *pp = comma + 1; // 指向下一个字段
    }
    else
    {
        *pp = NULL;
    }

    return start; // 返回当前字段首址
}

/**
 * @brief NMEA经纬度 "ddmm.mmmm" / "dddmm.mmmm" 转十进制度
 */
static float ATGM_ToDegree(const char *s)
{
    float v = atof(s);                       // 2236.9453
    float deg = (float)((int)(v / 100.0f));  // 22
    return deg + (v - deg * 100.0f) / 60.0f; // 22 + 36.9453/60
}

/**
 * @brief 十六进制字符转数值
 * @return 0~15; 非法字符返回0xFF
 */
static uint8_t ATGM_Hex(char c)
{
    if (c >= '0' && c <= '9')
        return (uint8_t)(c - '0');
    if (c >= 'A' && c <= 'F')
        return (uint8_t)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f')
        return (uint8_t)(c - 'a' + 10);

    return 0xFFU; // 非法字符返回0xFF
}

/* ==================== 语句解析 ==================== */

/**
 * @brief 从 $xxRMC 取出定位状态与经纬度
 * @return true = 位置有变化或定位状态翻转(值得通知上层)
 * @note  字段: 0=语句名 1=UTC时间 2=状态A/V 3=纬度 4=N/S 5=经度 6=E/W
 *        只用到2~6, 时间/速度/航向/日期全部跳过
 */
static bool ATGM_Parse_RMC(char *line)
{
    bool was_valid = gps_info.valid; // 备份旧值, 用于判断是否翻转
    float lat = gps_info.latitude;   // 备份旧值, 用于算变化量
    float lon = gps_info.longitude;  // (未定位时下面两个if不成立, 值自然保持)
    float dlat, dlon;                // 位置变化量
    char *p = line;                  // 指向当前字段的指针, 初始指向语句名

    (void)ATGM_NextField(&p);      // 0: 语句名
    (void)ATGM_NextField(&p);      // 1: UTC时间(不用)
    char *st = ATGM_NextField(&p); // 2: 定位状态
    char *la = ATGM_NextField(&p); // 3: 纬度
    char *ns = ATGM_NextField(&p); // 4: N/S
    char *lo = ATGM_NextField(&p); // 5: 经度
    char *ew = ATGM_NextField(&p); // 6: E/W

    if (st == NULL)
        return false;

    gps_info.valid = (st[0] == 'A'); // 'A'=定位; 'V'=导航(冷启动/丢星)

    if (la != NULL && la[0] != '\0' && ns != NULL && (ns[0] == 'N' || ns[0] == 'S'))
        gps_info.latitude = ATGM_ToDegree(la) * ((ns[0] == 'S') ? -1.0f : 1.0f); // 纬度
    if (lo != NULL && lo[0] != '\0' && ew != NULL && (ew[0] == 'E' || ew[0] == 'W'))
        gps_info.longitude = ATGM_ToDegree(lo) * ((ew[0] == 'W') ? -1.0f : 1.0f); // 经度

    /* 位移超过阈值(约5.5km)才算"有更新" */
    dlat = gps_info.latitude - lat;
    dlon = gps_info.longitude - lon;
    if (dlat < 0.0f)
        dlat = -dlat;
    if (dlon < 0.0f)
        dlon = -dlon;

    return (gps_info.valid != was_valid) ||
           (dlat >= ATGM_MOVE_EPS) || (dlon >= ATGM_MOVE_EPS); // 位置有变化或定位状态翻转
}

/**
 * @brief 校验校验和并取出RMC
 * @param line 以'$'开头、以'\n'结尾的一整行
 * @return 事件位(ATGM_EV_RMC / ATGM_EV_UPDATE)
 */
static uint8_t ATGM_Parse_Line(char *line)
{
    char *star;
    const char *c;
    uint8_t h1, h2;
    uint8_t sum = 0;
#if (ATGM_DEBUG_ECHO == 1)
    char echo[ATGM_LINE_MAX]; // 解析会就地切分line, 先留一份原文备回显
#endif

    if (line[0] != '$' || strlen(line) < 7U)
        return 0;

    star = strchr(line, '*'); // 查找校验和起始符, 见到'*'才算完整帧，返回'*'的地址
    if (star == NULL || strlen(star) < 3U)
        return 0;

    /* NMEA校验和: '$'与'*'之间所有字符逐字节异或 */
    for (c = line + 1; c < star; c++)
        sum ^= (uint8_t)(*c);

    h1 = ATGM_Hex(star[1]); // 校验和高位
    h2 = ATGM_Hex(star[2]); // 校验和低位

    /* 校验和校验 */
    if (h1 == 0xFFU || h2 == 0xFFU || (uint8_t)((h1 << 4) | h2) != sum)
    {
#if (ATGM_DEBUG_ECHO == 1)
        if (s_echo_bad < 5U)
        {
            s_echo_bad++;
            printf("[GNSS] BAD: %s", line);
        }
#endif
        return 0;
    }

    /* 只认RMC(语句名在line[3..5]), 其余语句一律忽略 */
    if (strncmp(line + 3, "RMC", 3U) != 0)
        return 0;

    /* 帧完好且是RMC: 解析字段并与上次结果比较 */
#if (ATGM_DEBUG_ECHO == 1)
    strcpy(echo, line); // ATGM_Parse_RMC会就地切分line, 先备份原文
#endif

    if (!ATGM_Parse_RMC(line))
        return ATGM_EV_RMC; // 帧收到了, 但位置没有变化，或定位状态没有翻转

#if (ATGM_DEBUG_ECHO == 1)
    printf("[GNSS] %s", echo); /* echo自带"\r\n" */
#endif

    return (uint8_t)(ATGM_EV_RMC | ATGM_EV_UPDATE);
}

/* ==================== 对外接口 ==================== */

/**
 * @brief 初始化定位模块(串口部分由Usart.c负责)并清空定位结果
 */
void ATGM336H_Init(void)
{
    Usart3_Init();

    /* 初始化定位结果 */
    s_line_len = 0;
    s_line_drop = false;
    memset(&gps_info, 0, sizeof(gps_info));
#if (ATGM_DEBUG_ECHO == 1)
    s_echo_bad = 0;
#endif
}

/**
 * @brief 消化环形缓冲里的字节, 并在RMC帧上刷新定位结果
 * @return 事件位: ATGM_EV_RMC / ATGM_EV_UPDATE (可按位与)
 * @note  非阻塞。**必须至少每300ms调用一次** —— 模块每秒发约615字节,
 *        512字节的环形缓冲约0.83秒就会满, 不读就丢。
 *        RMC每秒1条, 即每秒最多解析1次; 只有位置变化或定位状态
 *        翻转时才置 ATGM_EV_UPDATE。
 */
uint8_t ATGM336H_Poll(void)
{
    uint8_t ev = 0;
    int raw = 0;

    while ((raw = Usart3_RX_Read()) >= 0)
    {
        char c = (char)raw;

        /* 见到'$'就是新的一帧; 上一行若没以'\n'收尾, 说明被截断了 */
        if (c == '$')
        {
            s_line_len = 0;
            s_line_drop = false;
        }

        /* 上一行若没以'\n'收尾, 说明被截断了, 忽略当前字节 */
        if (s_line_drop)
        {
            if (c == '\n')
                s_line_drop = false;
            continue;
        }

        /* 累加正常字节 */
        if (s_line_len < (ATGM_LINE_MAX - 1U))
        {
            s_line[s_line_len++] = c;
        }
        else
        {
            s_line_drop = true; // 超长: 不是NMEA, 整行丢弃
            continue;
        }

        /* 见到'\n'就是新的一帧, 解析并刷新定位结果 */
        if (c == '\n')
        {
            s_line[s_line_len] = '\0';
            ev |= ATGM_Parse_Line(s_line);
            s_line_len = 0;
        }
    }

    return ev;
}

/**
 * @brief 取一份当前定位结果
 * @return 定位是否有效(与out->valid相同)
 */
bool ATGM336H_GetInfo(ATGM_Info_t *out)
{
    if (out == NULL)
        return false;

    *out = gps_info;

    return gps_info.valid;
}
