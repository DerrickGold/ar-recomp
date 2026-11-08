#include "diorama/diorama_jungle_terminal.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <string.h>
int main(void) {
  VoxelJungleTerminal s={.left=-16,.top=-96,.right=512,.bottom=304,.count=4,
    .runs={{.start=-96,.end=216,.edge=1,.color=0x954a,.palette_index=123},
      {.start=216,.end=304,.edge=1,.color=0xa0e5,.palette_index=81,.high_priority=true},
      {.start=-16,.end=512,.edge=2,.color=0xa0e5,.palette_index=81,.high_priority=true},
      {.start=208,.end=304,.edge=3,.color=0xa0e5,.palette_index=81,.high_priority=true}}};
  const JungleTerminalRect views[]={ {-55,-41,551,338},{-200,-200,900,700},{0,0,500,300} };
  for(unsigned v=0;v<3;++v) {
    JungleTerminalRect r[4];bool have[4];
    for(unsigned i=0;i<4;++i)have[i]=JungleTerminalResolve(&s,i,views[v],r+i);
    /* Independent raster ownership oracle: source-over source overlap may
     * replace its own final pixel, but two extension rectangles never overlap. */
    for(float y=-100.5f;y<380;y+=1)for(float x=-90.5f;x<580;x+=1) {
      unsigned owners=0;for(unsigned i=0;i<4;++i)if(have[i])
        owners+=x>=r[i].x0&&x<r[i].x1&&y>=r[i].y0&&y<r[i].y1;
      assert(owners<=1);
      const bool visible=x>=views[v].x0&&x<views[v].x1&&y>=views[v].y0&&y<views[v].y1;
      const bool bottom=views[v].y1>304&&y>=303&&y<368&&x>=-80&&x<576;
      const bool right=views[v].x1>512&&x>=511&&x<576&&y>=-96&&y<(views[v].y1>304?303:304);
      const bool left=views[v].x0<-16&&x>=-80&&x<-15&&y>=208&&y<(views[v].y1>304?303:304);
      assert(!!owners==(visible&&(bottom||right||left)));
    }
  }
  /* Missing lower-left source cannot be supplied by its intact right corner. */
  s.count=3;JungleTerminalRect q;assert(JungleTerminalResolve(&s,2,views[0],&q));assert(q.x0==-16);
  s.runs[2].color=0;assert(!JungleTerminalResolve(&s,2,views[0],&q));
  s.runs[2].color=0x8000;assert(JungleTerminalResolve(&s,2,views[0],&q));assert(q.x1==512);
  s.runs[2].color=0xa0e5;s.runs[2].palette_index=80;
  assert(JungleTerminalResolve(&s,2,views[0],&q));assert(q.x1==512); /* SameRGB is not source identity. */
  assert(!JungleTerminalResolve(&s,9,views[0],&q));
  assert(!JungleTerminalResolve(&s,0,(JungleTerminalRect){NAN,0,1,1},&q));
  puts("Marahna terminal: exact finite apron, one native-pixel overlap, disjoint corners and missing/black source identity passed");
  return 0;
}
