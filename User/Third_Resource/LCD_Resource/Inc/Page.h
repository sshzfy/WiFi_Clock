#ifndef __PAGE_H__
#define __PAGE_H__

#include "main.h"
#include "delay.h"
#include "ST7789.h"
#include "Image.h"
#include "Font.h"
#include "App.h"

typedef struct
{
    uint16_t x_start;
    uint16_t y_start;
    uint16_t width;
    uint16_t height;
} Modle_Size_t;

typedef struct
{
    int code;
    const Image_t *icon;
    const char *chinese;
} Weather_Map_t;

void Boot_Page_Display(void);
void Main_Page_Display(void);
void Main_Page_Clock_Update(void);
void Main_Page_Weather_Update(void);
void Main_Page_Room_Update(void);

#endif /* __PAGE_H__ */
