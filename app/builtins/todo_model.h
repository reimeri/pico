#ifndef PICO_BUILTIN_TODO_MODEL_H
#define PICO_BUILTIN_TODO_MODEL_H

#include <stdbool.h>

#define PICO_TODO_MAX 30
#define PICO_TODO_ID_MAX 64
#define PICO_TODO_TEXT_MAX 300
#define PICO_TODO_EXPLANATION_MAX 300
#define PICO_TODO_STATE_VERSION 1

typedef enum PicoTodoStatus {
    PICO_TODO_PENDING = 0,
    PICO_TODO_IN_PROGRESS,
    PICO_TODO_COMPLETED,
} PicoTodoStatus;

typedef struct PicoTodoItem {
    char id[PICO_TODO_ID_MAX + 1];
    char *text;
    PicoTodoStatus status;
} PicoTodoItem;

typedef struct PicoTodoList {
    PicoTodoItem items[PICO_TODO_MAX];
    int count;
    char *explanation;
} PicoTodoList;

void PicoTodoList_Init(PicoTodoList *list);
void PicoTodoList_Free(PicoTodoList *list);
void PicoTodoList_Swap(PicoTodoList *a, PicoTodoList *b);
int PicoTodoList_Completed(const PicoTodoList *list);
bool PicoTodoList_AllCompleted(const PicoTodoList *list);

bool PicoTodoList_ParseArgs(const char *json, PicoTodoList *out, char **error);
bool PicoTodoList_ParseDetails(const char *json, PicoTodoList *out, char **error);
char *PicoTodoList_DetailsJson(const PicoTodoList *list);
char *PicoTodoList_FormatAgent(const PicoTodoList *list);
char *PicoTodoList_FormatReminder(const PicoTodoList *list);
const char *PicoTodoStatus_Name(PicoTodoStatus status);

#endif
