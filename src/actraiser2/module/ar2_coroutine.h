#ifndef AR_AR2_COROUTINE_H
#define AR_AR2_COROUTINE_H
/* Module-private stack switching, owned by the calling thread. */
#include <stdbool.h>
bool Ar2Coroutine_Create(void (*entry)(void));
void Ar2Coroutine_Resume(void);
void Ar2Coroutine_Yield(void);
void Ar2Coroutine_Destroy(void);
#endif
