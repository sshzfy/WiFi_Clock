#ifndef __TASK_MONITOR_H__
#define __TASK_MONITOR_H__

#include "main.h"

#define STACK_MAP_COUNT        ((uint32_t)(sizeof(s_stack_map) / sizeof(s_stack_map[0])))
#define WORD_BYTES             ((uint32_t)sizeof(StackType_t))

typedef struct
{
    const char *name;
    uint16_t words;
} Monitor_Stack_Map_t;

void Task_Monitor_Start(void);
void Task_Monitor_Print(void);

#endif /* __TASK_MONITOR_H__ */
