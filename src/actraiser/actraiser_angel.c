#include "actraiser_angel.h"

#include <stdlib.h>

#include "actraiser_cpu_hle_internal.h"
#include "actraiser_hle_fatal.h"
#include "actraiser_native_call.h"
#include "actraiser_game.h"

extern RecompReturn bank_01_B23B_M0X0(CpuState *cpu);
extern RecompReturn bank_01_B41A_M0X0(CpuState *cpu);
extern RecompReturn bank_01_B4AF_M0X0(CpuState *cpu);
extern RecompReturn bank_01_B4AF_M1X0(CpuState *cpu);
extern RecompReturn bank_01_B4C6_M0X0(CpuState *cpu);
extern RecompReturn bank_01_AC70_M0X0(CpuState *cpu);
extern RecompReturn bank_01_CFF2_M0X0(CpuState *cpu);

enum {
  kAngelRecord = kActRaiserWram_SimAngelRecord,
  kPositionX = 0x0a,
  kPositionY = 0x0c,
  kState = 0x12,
  kVelocityX = 0x1a,
  kVelocityY = 0x1c,
  kFacing = 0x22,
  kPicker = 0x9215,
  kMaximumPosition = 0x1f0,
  kNativeSpeed = 2,
};

static uint32_t s_stick;
static int s_fraction[2];
static uint16_t s_expected[2];
static uint8_t s_map_group, s_map_number;
static bool s_position_valid, s_analog_motion;

static void ResetRemainder(void) {
  s_fraction[0] = s_fraction[1] = 0;
  s_position_valid = false;
}

void ActRaiserAngel_Reset(void) {
  s_stick = 0;
  ResetRemainder();
  s_analog_motion = false;
  s_map_group = s_map_number = 0xff;
}

void ActRaiserAngel_SetInput(uint32_t stick, uint8_t map_group, uint8_t map_number) {
  if (!ActRaiser_IsSimulationTown(map_group, map_number)) stick = 0;
  if (!stick || map_group != s_map_group || map_number != s_map_number) ResetRemainder();
  s_stick = stick;
  s_map_group = map_group;
  s_map_number = map_number;
  s_analog_motion = false;
}

static void Require(CpuState *cpu) {
  if (!cpu || cpu->emulation || cpu->PB != 1 || cpu->DB > 1 || cpu->D ||
      cpu->_flag_D)
    ActRaiserHleFatal("Angel HLE requires the native bank-1 movement ABI");
  cpu_mirrors_to_p(cpu);
}

bool ActRaiserAngel_DecodeEntry(CpuState *cpu) {
  return cpu && !cpu->emulation && cpu->PB == 1 && cpu->DB <= 1 && !cpu->D &&
      !cpu->_flag_D;
}

bool ActRaiserAngel_MoveEntry(CpuState *cpu) {
  return ActRaiserAngel_DecodeEntry(cpu) && !cpu->x_flag;
}

static void A8(CpuState *cpu, uint8_t value) {
  cpu_write_a8(cpu, value);
  ActRaiserCpuHle_SetNegativeZero8(cpu, value);
}

static void A16(CpuState *cpu, uint16_t value) {
  cpu->A = value;
  ActRaiserCpuHle_SetNegativeZero16(cpu, value);
}

static void Width(CpuState *cpu, bool narrow) {
  cpu->P = narrow ? cpu->P | CPU_P_M : cpu->P & ~CPU_P_M;
  cpu->m_flag = narrow;
}

static void Add(CpuState *cpu, uint16_t value) {
  const uint16_t before = cpu->A;
  const uint32_t sum = (uint32_t)before + value;
  cpu->_flag_C = sum > UINT16_MAX;
  cpu->_flag_V = (~(before ^ value) & (before ^ (uint16_t)sum) & 0x8000u) != 0;
  cpu->P = (cpu->P & ~(CPU_P_C | CPU_P_V)) |
      (cpu->_flag_C ? CPU_P_C : 0) | (cpu->_flag_V ? CPU_P_V : 0);
  A16(cpu, (uint16_t)sum);
}

static void Compare(CpuState *cpu, uint16_t value) {
  cpu->_flag_C = cpu->A >= value;
  cpu->P = (cpu->P & ~CPU_P_C) | (cpu->_flag_C ? CPU_P_C : 0);
  ActRaiserCpuHle_SetNegativeZero16(cpu, (uint16_t)(cpu->A - value));
}

static RecompReturn Escape(RecompReturn result) {
  return result >= RECOMP_RETURN_TAILCALL ? result : (RecompReturn)(result - 1);
}

static RecompReturn Call(CpuState *cpu, ActRaiserNativeLeaf leaf,
                        uint16_t caller, bool long_call) {
  return ActRaiserNativeCall(cpu, leaf, 1, caller, long_call);
}

/* Four facing values remain the native up/right/down/left animation and
 * arrow selectors. A diagonal tie retains a matching facing to avoid chatter. */
static unsigned Cardinal(CpuState *cpu) {
  const int x = ActRaiserAngel_StickX(s_stick), y = ActRaiserAngel_StickY(s_stick);
  const unsigned horizontal = x < 0 ? 3 : 1;
  const unsigned vertical = y < 0 ? 0 : 2;
  const unsigned previous = cpu_read8(cpu, 1, kAngelRecord + kFacing);
  /* Hold fire to strafe. This also holds across the normal tick between
   * shooting animations; arrows always use the native cardinal facing. */
  if (previous < 4 && (cpu_read8(cpu, 0, 0xa1) & 0x40) &&
      !cpu_read8(cpu, 0x7f, kPicker)) return previous;
  if (abs(x) == abs(y) && (previous == horizontal || previous == vertical))
    return previous;
  return abs(x) > abs(y) ? horizontal : vertical;
}

static RecompReturn Decode(CpuState *cpu, bool controller_input) {
  static const int8_t directions[16] = {
    -1, 2, 6, -1, 4, 3, 5, -1, 0, 1, 7, 0, 2, 2, 2, 0,
  };
  static const int8_t dx[8] = {0, 2, 2, 2, 0, -2, -2, -2};
  static const int8_t dy[8] = {-2, -2, 0, 2, 2, 2, 0, -2};
  static const uint8_t facing[8] = {0, 0, 1, 2, 2, 2, 3, 0};
  Require(cpu);
  const unsigned buttons = controller_input ? cpu_read8(cpu, 0, 0xa1) : cpu->A;
  cpu_write8(cpu, 0, cpu->S--, cpu->P);
  cpu->P |= CPU_P_M | CPU_P_X;
  cpu_p_to_mirrors(cpu);
  cpu->X &= 0xff;
  cpu->Y &= 0xff;
  A8(cpu, (uint8_t)(buttons & 15));
  cpu->X = cpu->A & 15;
  ActRaiserCpuHle_SetNegativeZero8(cpu, (uint8_t)cpu->X);
  int direction = directions[cpu->X];
  s_analog_motion = controller_input && (s_stick & AR_ANGEL_STICK_ENABLED);
  if (s_analog_motion) {
    const bool moving = ActRaiserAngel_StickX(s_stick) || ActRaiserAngel_StickY(s_stick);
    direction = moving ? (int)Cardinal(cpu) * 2 : -1;
  } else {
    ResetRemainder();
  }
  A8(cpu, (uint8_t)direction);
  const bool stopped = direction < 0;
  if (!stopped) {
    cpu->X = (uint16_t)direction;
    ActRaiserCpuHle_SetNegativeZero8(cpu, (uint8_t)cpu->X);
    A8(cpu, cpu_read8(cpu, 0x7f, kPicker));
    A8(cpu, cpu->_flag_Z ? facing[direction] : 5);
    cpu_write8(cpu, 1, kAngelRecord + kFacing, (uint8_t)cpu->A);
    cpu->Y = 0;
    A8(cpu, (uint8_t)dx[direction]);
    cpu_write16(cpu, 1, kAngelRecord + kVelocityX, (uint16_t)(int16_t)dx[direction]);
    cpu->Y = 0;
    A8(cpu, (uint8_t)dy[direction]);
    cpu_write16(cpu, 1, kAngelRecord + kVelocityY, (uint16_t)(int16_t)dy[direction]);
    if (dy[direction] < 0) cpu->Y = 0xff;
    ActRaiserCpuHle_SetNegativeZero8(cpu, (uint8_t)cpu->Y);
  }
  /* Keep the original PHP on the stack: regional recovery owns its existing
   * eligible-call clock and the native PLP/RTL continuation. */
  if (!cpu_hle_tailcall_request(stopped ? 0x019c34 : 0x019c30,
                               controller_input ? 0x019beb : 0x019bf4))
    ActRaiserHleFatal("Angel decoder requires its generated continuation");
  return RECOMP_RETURN_TAILCALL;
}

RecompReturn ActRaiser_AngelDecodeInput(CpuState *cpu) {
  return Decode(cpu, true);
}

RecompReturn ActRaiser_AngelDecodeDirection(CpuState *cpu) {
  return Decode(cpu, false);
}

static int Step(CpuState *cpu, unsigned axis) {
  const unsigned position = axis ? kPositionY : kPositionX;
  const int current = cpu_read16(cpu, 1, kAngelRecord + position);
  if (!s_position_valid || current != s_expected[axis]) s_fraction[axis] = 0;
  const int velocity = (axis ? ActRaiserAngel_StickY(s_stick) :
      ActRaiserAngel_StickX(s_stick)) * kNativeSpeed;
  if (!velocity || (s_fraction[axis] < 0 && velocity > 0) ||
      (s_fraction[axis] > 0 && velocity < 0)) s_fraction[axis] = 0;
  const int total = s_fraction[axis] + velocity;
  int delta = total / kAngelStickScale;
  s_fraction[axis] = total % kAngelStickScale;
  if (current + delta < 0 || current + delta > kMaximumPosition) {
    delta = current + delta < 0 ? -current : kMaximumPosition - current;
    s_fraction[axis] = 0;
  }
  s_expected[axis] = (uint16_t)(current + delta);
  return delta;
}

/* Return the Y-step result, as in the original routine. Knockback ends when
 * Y leaves the map; rejecting an X step alone does not end it. */
static bool Integrate(CpuState *cpu, bool analog) {
  bool accepted = false;
  for (unsigned axis = 0; axis < 2; ++axis) {
    const unsigned position = axis ? kPositionY : kPositionX;
    const unsigned velocity = axis ? kVelocityY : kVelocityX;
    const int delta = analog ? Step(cpu, axis) :
        (int16_t)cpu_read16(cpu, 1, cpu->X + velocity);
    A16(cpu, cpu_read16(cpu, 1, cpu->X + position));
    Add(cpu, (uint16_t)delta);
    accepted = !cpu->_flag_N;
    if (accepted) {
      Compare(cpu, kMaximumPosition + 1);
      accepted = !cpu->_flag_C;
      if (accepted) cpu_write16(cpu, 1, cpu->X + position, cpu->A);
    }
  }
  s_position_valid = analog;
  return accepted;
}

static RecompReturn AnimateAndMove(CpuState *cpu, bool update_facing) {
  RecompReturn result;
  if (update_facing) {
    result = Call(cpu, cpu->m_flag ? bank_01_B4AF_M1X0 : bank_01_B4AF_M0X0,
                  0xb2b8, false);
    if (result != RECOMP_RETURN_NORMAL) return Escape(result);
  }
  result = Call(cpu, bank_01_AC70_M0X0, 0xb2bb, false);
  if (result != RECOMP_RETURN_NORMAL) return Escape(result);
  Integrate(cpu, s_analog_motion);
  cpu->S += 2;
  return RECOMP_RETURN_NORMAL;
}

RecompReturn ActRaiser_AngelMove(CpuState *cpu) {
  Require(cpu);
  return AnimateAndMove(cpu, true);
}

RecompReturn ActRaiser_AngelNormalTail(CpuState *cpu) {
  Require(cpu);
  cpu->X = ActRaiserCpuHle_PopWord(cpu);
  ActRaiserCpuHle_SetNegativeZero16(cpu, cpu->X);
  A8(cpu, cpu_read8(cpu, 0x7f, kPicker));
  if (!cpu->_flag_Z) return AnimateAndMove(cpu, true);
  A8(cpu, cpu_read8(cpu, 0, 0xa1));
  cpu->_flag_Z = !(cpu->A & 0x40);
  cpu->P = (cpu->P & ~CPU_P_Z) | (cpu->_flag_Z ? CPU_P_Z : 0);
  if (cpu->_flag_Z) return AnimateAndMove(cpu, true);
  A8(cpu, cpu_read8(cpu, 0, 0x0b1c));
  if (!cpu->_flag_Z) return AnimateAndMove(cpu, true);
  Width(cpu, true);
  A8(cpu, cpu_read8(cpu, 1, kActRaiserWram_AngelCurrentHp));
  Width(cpu, false);
  if (cpu->_flag_Z) return AnimateAndMove(cpu, true);
  A16(cpu, 1);
  cpu_write16(cpu, 1, cpu->X + kState, cpu->A);
  A16(cpu, cpu_read16(cpu, 1, cpu->X + kFacing) & 0xff);
  Add(cpu, 6);
  A16(cpu, cpu->A & 0xff);
  RecompReturn result = Call(cpu, bank_01_CFF2_M0X0, 0xb2b2, false);
  if (result != RECOMP_RETURN_NORMAL) return Escape(result);
  return AnimateAndMove(cpu, false);
}

RecompReturn ActRaiser_AngelKnockback(CpuState *cpu) {
  Require(cpu);
  ResetRemainder();
  A16(cpu, (uint16_t)(cpu_read16(cpu, 1, cpu->X + 0x18) - 1));
  if (!cpu->_flag_Z) {
    cpu_write16(cpu, 1, cpu->X + 0x18, cpu->A);
    if (Integrate(cpu, false)) {
      cpu->S += 2;
      return RECOMP_RETURN_NORMAL;
    }
  }
  A16(cpu, 0);
  cpu_write16(cpu, 1, cpu->X + kState, 0);
  A16(cpu, 60);
  cpu_write16(cpu, 1, cpu->X + 0x18, 60);
  cpu->S += 2;
  return RECOMP_RETURN_NORMAL;
}

RecompReturn ActRaiser_AngelService(CpuState *cpu) {
  Require(cpu);
  const uint8_t saved_p = cpu->P;
  cpu_write8(cpu, 0, cpu->S--, saved_p);
  Width(cpu, false);
  A16(cpu, cpu_read16(cpu, 0x7f, 0x9750));
  RecompReturn result;
  if (cpu->_flag_Z) {
    const unsigned state = cpu_read16(cpu, 1, kAngelRecord + kState) & 0x7fff;
    if (state >= 3) ResetRemainder();
    result = Call(cpu, bank_01_B23B_M0X0, 0xb226, false);
    if (result != RECOMP_RETURN_NORMAL) return Escape(result);
    if ((state == 1 || state == 2) && (s_stick & AR_ANGEL_STICK_ENABLED)) {
      /* Fire animation and arrow creation own facing. Only movement changes
       * here; preserve the native callee's complete CPU result and arrow data. */
      const CpuState saved = *cpu;
      cpu->X = kAngelRecord;
      Integrate(cpu, true);
      *cpu = saved;
    }
    result = Call(cpu, bank_01_B41A_M0X0, 0xb229, false);
    if (result != RECOMP_RETURN_NORMAL) return Escape(result);
    result = Call(cpu, bank_01_B4C6_M0X0, 0xb22d, true);
    if (result != RECOMP_RETURN_NORMAL) return Escape(result);
  } else {
    ResetRemainder();
    cpu->X = kAngelRecord;
    ActRaiserCpuHle_SetNegativeZero16(cpu, cpu->X);
    result = Call(cpu, bank_01_B4AF_M0X0, 0xb235, false);
    if (result != RECOMP_RETURN_NORMAL) return Escape(result);
    result = Call(cpu, bank_01_AC70_M0X0, 0xb238, false);
    if (result != RECOMP_RETURN_NORMAL) return Escape(result);
  }
  cpu->P = cpu_read8(cpu, 0, ++cpu->S);
  cpu_p_to_mirrors(cpu);
  if (cpu->x_flag) {
    cpu->X &= 0xff;
    cpu->Y &= 0xff;
  }
  cpu->S += 3;
  return RECOMP_RETURN_NORMAL;
}
