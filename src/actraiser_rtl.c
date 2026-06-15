#define _XOPEN_SOURCE 600
#include "actraiser_rtl.h"
#include "variables.h"
#include "common_cpu_infra.h"
#include "snes/snes.h"
#include "cpu_state.h"
#include "funcs.h"
#include "cpu_trace.h"
#include <stdio.h>
#include <stdbool.h>
#include <ucontext.h>
#include <stdlib.h>

extern int snes_frame_counter;

#define GAME_STACK_SIZE (2 * 1024 * 1024)

static ucontext_t g_host_ctx;
static ucontext_t g_game_ctx;
static char *g_game_stack;
static bool g_game_started;

void ActRaiser_YieldToHost(void) {
  swapcontext(&g_game_ctx, &g_host_ctx);
}

static void game_coroutine(void) {
  cpu_state_init(&g_cpu, g_ram);

  ResetHandler_M1X1(&g_cpu);
  for (;;)
    ActRaiser_YieldToHost();
}

RecompReturn ActRaiser_WaitForVblank(CpuState *cpu) {
  ActRaiser_YieldToHost();
  return RECOMP_RETURN_NORMAL;
}

void ActRaiserDrawPpuFrame(void) {
  SimpleHdma hdma_chans[3];
  Dma *dma = g_dma;

  dma_startDma(dma, g_snesrecomp_last_hdmaen, true);

  SimpleHdma_Init(&hdma_chans[0], &dma->channel[5]);
  SimpleHdma_Init(&hdma_chans[1], &dma->channel[6]);
  SimpleHdma_Init(&hdma_chans[2], &dma->channel[7]);

  int trigger = g_snes->vIrqEnabled ? g_snes->vTimer + 1 : -1;

  for (int i = 0; i <= 224; i++) {
    ppu_runLine(g_ppu, i);
    SimpleHdma_DoLine(&hdma_chans[0]);
    SimpleHdma_DoLine(&hdma_chans[1]);
    SimpleHdma_DoLine(&hdma_chans[2]);
    if (i == trigger) {
      g_snes->inIrq = true;
      cpu_push_interrupt_frame(&g_cpu);
      IrqHandler_M1X1(&g_cpu);
      trigger = g_snes->vIrqEnabled ? g_snes->vTimer + 1 : -1;
    }
  }
}

void RunOneFrameOfGame(void) {
  if (!g_game_started) {
    g_game_started = true;
    g_game_stack = malloc(GAME_STACK_SIZE);
    if (!g_game_stack) {
      fprintf(stderr, "Failed to allocate game coroutine stack\n");
      return;
    }
    getcontext(&g_game_ctx);
    g_game_ctx.uc_stack.ss_sp = g_game_stack;
    g_game_ctx.uc_stack.ss_size = GAME_STACK_SIZE;
    g_game_ctx.uc_link = &g_host_ctx;
    makecontext(&g_game_ctx, game_coroutine, 0);
  }

  g_snes->forceNmi = true;
  swapcontext(&g_host_ctx, &g_game_ctx);
  g_snes->forceNmi = false;

  g_snes->inNmi = true;
  /* NmiHandler ends in RTI, which pops a hardware interrupt frame
   * (P/PC/PB). Push the matching frame first — otherwise the RTI
   * over-pops the stack and loads garbage into cpu->P, corrupting the
   * M/X width flags of the interrupted game code. Symmetric to the
   * IRQ path in ActRaiserDrawPpuFrame. */
  cpu_push_interrupt_frame(&g_cpu);
  NmiHandler_M1X1(&g_cpu);
}
