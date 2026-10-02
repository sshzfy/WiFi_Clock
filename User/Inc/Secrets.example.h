#ifndef __SECRETS_H__
#define __SECRETS_H__

/* ============================================================
 * 敏感凭据模板(本文件入库)
 * ------------------------------------------------------------
 * 用法: 把本文件复制成同目录下的 Secrets.h, 再填入自己的真实值。
 *       Secrets.h 已被根目录 .gitignore 忽略, 不会提交; 仓库里
 *       只保留本模板, 所以新克隆的工程必须先复制一次才能编译。
 *
 *       Windows:  copy User\Inc\Secrets.example.h User\Inc\Secrets.h
 *       Git Bash: cp User/Inc/Secrets.example.h User/Inc/Secrets.h
 *
 * 源码(App.c / AT.c)只引用下面这些宏, 不要在业务代码里写明文凭据。
 * ============================================================ */

/* WiFi: ESP32-C3 只支持 2.4GHz; 密码填 "" 表示开放网络 */
#define WIFI_SSID       "SSID"
#define WIFI_PASSWORD   "PASSWORD"

/* 要绑定的 AP BSSID(MAC), 形如 "aa:bb:cc:dd:ee:ff"; 填 NULL 表示不绑定 */
#define WIFI_MAC        NULL

/* 心知天气私钥: https://www.seniverse.com 控制台里的"私钥" */
#define WEATHER_KEY     "KEY"

/* 高德"Web服务"key: https://lbs.amap.com 应用管理里创建, 逆地理编码用 */
#define AMAP_KEY        "KEY"

#endif /* __SECRETS_H__ */
