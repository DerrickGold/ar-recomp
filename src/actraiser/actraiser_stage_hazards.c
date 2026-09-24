#include "actraiser_stage_hazards.h"
#include "actraiser_regional_runtime.h"
#include "actraiser_game.h"
#include "actraiser_hle_fatal.h"

enum { kCountScratch = 0x00, kBoxes = 0x1ae4, kStride = 10 };

static bool Profile(CpuState *cpu, ArRegionalHazards *selected) {
  if (!cpu || cpu->emulation || cpu->m_flag || cpu->x_flag || cpu->D ||
      cpu->PB || cpu->DB != 0x0a || cpu->_flag_D || (cpu->P & CPU_P_D)) return false;
  const uint8_t profile = ActRaiserRegional_HazardSnapshot();
  if (!profile) return false;
  const uint16_t scene = cpu_read16(cpu,0,kActRaiserWram_MapGroup);
  ArRegionalHazards native;
  if (!ArRegionalHazards_Copy(0,scene,&native) ||
      !ArRegionalHazards_Copy(profile,scene,selected) ||
      cpu_read16(cpu,0,kCountScratch) != native.count ||
      cpu->X != native.count*kStride || cpu_read8(cpu,0x0a,cpu->Y) != 0xff)
    return false;
  /* A post-expansion seam: do not replace another subsystem's buffer or a
   * modified/unknown source stream. The native cursor and caller stack stay
   * native; following actor placements are never parsed from a donor ROM. */
  bool changed = native.count != selected->count;
  for (unsigned i = 0; i < native.count; ++i) {
    const ArRegionalHazardBox *b = &native.boxes[i];
    const uint16_t words[] = {b->left,b->width,b->top,b->height,b->damage_or_flag};
    const ArRegionalHazardBox *s = &selected->boxes[i];
    changed |= b->left!=s->left || b->width!=s->width || b->top!=s->top ||
        b->height!=s->height || b->damage_or_flag!=s->damage_or_flag;
    for (unsigned j = 0; j < 5; ++j)
      if (cpu_read16(cpu,0,(uint16_t)(kBoxes+i*kStride+j*2)) != words[j]) return false;
  }
  return changed;
}

bool ActRaiser_StageHazardsEntry(CpuState *cpu) {
  ArRegionalHazards selected;
  return Profile(cpu,&selected);
}

RecompReturn ActRaiser_StageHazards(CpuState *cpu) {
  ArRegionalHazards selected;
  if (!Profile(cpu,&selected)) ActRaiserHleFatal("Invalid regional hazard expansion");
  for (unsigned i = 0; i < selected.count; ++i) {
    const ArRegionalHazardBox *b = &selected.boxes[i];
    const uint16_t words[] = {b->left,b->width,b->top,b->height,b->damage_or_flag};
    for (unsigned j = 0; j < 5; ++j)
      cpu_write16(cpu,0,(uint16_t)(kBoxes+i*kStride+j*2),words[j]);
  }
  cpu_write16(cpu,0,kCountScratch,(uint16_t)selected.count);
  /* Reproduce only LDA $00. Native STA count / INY / PLX / RTS owns the
   * continuation, source cursor and return frame. Unused record tails retain
   * native semantics: count, not clearing, bounds every collision traversal. */
  cpu->A = (uint16_t)selected.count;
  cpu->P = (uint8_t)((cpu->P & ~(CPU_P_N|CPU_P_Z)) | (cpu->A ? 0 : CPU_P_Z));
  cpu_p_to_mirrors(cpu);
  cpu_hle_tailcall_request(0x00940e,0x00940c);
  return RECOMP_RETURN_TAILCALL;
}
