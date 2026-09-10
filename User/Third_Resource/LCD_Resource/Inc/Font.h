#ifndef __FONT_H__
#define __FONT_H__

#include "main.h"

typedef struct
{
    const char *name;
    const uint8_t *model;
} Chinese_Font_t;

typedef struct
{
    uint16_t size; // 字体大小,高度
    const uint8_t *ascii_model;
    const Chinese_Font_t *chinese;
} Font_t;

extern const Font_t Font_12;
extern const Font_t Font_16;
extern const Font_t Font_16B;
extern const Font_t Font_22;
extern const Font_t Font_22B;
extern const Font_t Font_32;
extern const Font_t Font_32B;
extern const Font_t Font_48;
extern const Font_t Font_48B;

#endif /* __FONT_H__ */
