#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "diorama/diorama_death_heim_nebula.h"

int main(void) {
  for(int top=1;top<=32;++top) {
    DioramaDeathHeimNebulaWindow w;
    assert(DioramaDeathHeimNebula_Window(624,256,top,8,360,0,0,&w));
    assert(fabsf(w.u0*640-140)<.0001f&&fabsf(w.u1*640-500)<.0001f);
    assert(fabsf(w.v0*352-top)<.0001f&&fabsf(w.v1*352-(top+224))<.0001f);
    assert(w.v0>=0&&w.v1<=256.f/352);
  }
  DioramaDeathHeimNebulaWindow w;
  assert(DioramaDeathHeimNebula_Window(624,256,17,8,360,2,-3,&w));
  assert(fabsf(w.u0*640-138)<.0001f&&fabsf(w.v0*352-20)<.0001f);
  assert(!DioramaDeathHeimNebula_Window(624,256,17,8,625,0,0,&w));
  assert(!DioramaDeathHeimNebula_Window(624,256,33,8,360,0,0,&w));
  assert(!DioramaDeathHeimNebula_Window(624,256,17,9,360,0,0,&w));
  assert(!DioramaDeathHeimNebula_Window(624,256,1,8,360,0,2,&w));
  assert(!DioramaDeathHeimNebula_Window(624,256,32,8,360,0,-1,&w));
  assert(!DioramaDeathHeimNebula_Window(624,256,17,8,360,NAN,0,&w));
  puts("Death Heim nebula source window: 32 native camera rows and invalid/motion guards passed");
  return 0;
}
