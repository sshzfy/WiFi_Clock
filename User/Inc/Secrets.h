#ifndef __SECRETS_H__
#define __SECRETS_H__

/* ============================================================
 * 敏感凭据(本文件不入库)
 * ------------------------------------------------------------
 * 本文件保存 WiFi 与第三方接口的真实凭据, 已写进根目录 .gitignore:
 * 仓库里只有同目录下的 Secrets.example.h 模板, 真实值留在本地。
 *
 * 源码(App.c / AT.c)只引用下面这些宏, 任何地方都不再出现明文凭据;
 * 换 WiFi 或换 key 只改本文件, 不必碰业务代码。
 *
 * 注意: macOS/Linux 下是 User/Inc/Secrets.h, 新克隆的工程需要先
 *       copy User\Inc\Secrets.example.h User\Inc\Secrets.h 再填写。
 * ============================================================ */

/* WiFi: ESP32-C3 只支持 2.4GHz; 密码填 "" 表示开放网络 */
#define WIFI_SSID       "Laptop-S"
#define WIFI_PASSWORD   "Sun507109!"

/* 要绑定的 AP BSSID(MAC), 形如 "aa:bb:cc:dd:ee:ff"; 填 NULL 表示不绑定 */
#define WIFI_MAC        NULL

/* 心知天气私钥: https://www.seniverse.com 控制台里的"私钥" */
#define WEATHER_KEY     "SgM2NZE2Sghy4FOFh"

/* 高德"Web服务"key: https://lbs.amap.com 应用管理里创建, 逆地理编码用 */
#define AMAP_KEY        "23ca4864d87b04bab193347fb5f3f089"

#endif /* __SECRETS_H__ */
