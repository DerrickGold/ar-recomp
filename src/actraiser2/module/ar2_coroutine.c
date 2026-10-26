#define _XOPEN_SOURCE 600
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "ar2_coroutine.h"
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
static void *host, *game;
static bool converted;
static void (*start)(void);
static void CALLBACK Run(void *unused) {
  (void)unused;
  start();
}
bool Ar2Coroutine_Create(void (*entry)(void)) {
  start = entry;
  converted = !IsThreadAFiber();
  host = converted ? ConvertThreadToFiber(NULL) : GetCurrentFiber();
  game = host ? CreateFiber(8 * 1024 * 1024, Run, NULL) : NULL;
  return game != NULL;
}
void Ar2Coroutine_Resume(void) { SwitchToFiber(game); }
void Ar2Coroutine_Yield(void) { SwitchToFiber(host); }
void Ar2Coroutine_Destroy(void) {
  if (game) DeleteFiber(game);
  if (converted && host) ConvertFiberToThread();
  host = game = NULL;
  converted = false;
}
#else
#include <ucontext.h>
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
static ucontext_t host, game;
static void *stack;
bool Ar2Coroutine_Create(void (*entry)(void)) {
  stack = malloc(8 * 1024 * 1024);
  if (!stack || getcontext(&game)) return false;
  game.uc_stack.ss_sp = stack;
  game.uc_stack.ss_size = 8 * 1024 * 1024;
  game.uc_link = &host;
  makecontext(&game, entry, 0);
  return true;
}
void Ar2Coroutine_Resume(void) {
  if (swapcontext(&host, &game)) abort();
}
void Ar2Coroutine_Yield(void) {
  if (swapcontext(&game, &host)) abort();
}
void Ar2Coroutine_Destroy(void) {
  free(stack);
  stack = NULL;
}
#endif
