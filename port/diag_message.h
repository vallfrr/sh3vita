#pragma once
#include <stdint.h>
/* Bounded FIFO for a single diagnostic window; lifetime is owned by caller. */
typedef struct DiagMessageQueue DiagMessageQueue;
typedef struct { uint32_t hwnd, message, wparam, lparam, time, x, y; } DiagMessage;
DiagMessageQueue *diag_message_create(unsigned capacity);
void diag_message_destroy(DiagMessageQueue *queue);
/* 0 success, 1 full, -1 invalid/error. */
int diag_message_post(DiagMessageQueue *queue,const DiagMessage *message);
/* 1 found, 0 empty, -1 invalid/error; remove=0 leaves the head untouched. */
int diag_message_peek(DiagMessageQueue *queue,DiagMessage *message,int remove);
