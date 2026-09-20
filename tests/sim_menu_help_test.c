#include "sim/sim_menu_help.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  SimMenuHelpPage page;
  const char *text=SimMenuHelp_Item(8);
  size_t bytes=strlen(text),offset=0;
  unsigned pages=0;
  assert(SimMenuHelp_Build(&page,text,bytes,0,false));
  assert(strstr(page.text,"planted.") && !strstr(page.text,"An"));
  assert(!strncmp(text+page.source_end,"An invalid",10));
  do {
    assert(SimMenuHelp_Build(&page,text,bytes,offset,false));
    assert(page.source_start==offset && page.source_end>offset);
    assert(page.bytes==page.source_end-page.source_start);
    assert(!memcmp(page.text,text+offset,page.bytes));
    assert(!memchr(page.text,'\n',page.bytes)); /* Native soft wraps are not prose. */
    for(unsigned g=0;g<page.glyph_count;++g) {
      assert(page.row[g]<6 && page.column[g]<22);
      assert(page.source_ends[g]<=bytes);
      assert(page.text_ends[g]<=page.bytes);
    }
    offset=page.source_end; ++pages;
  } while(page.more);
  assert(offset==bytes && pages>1);
  const char *hard="An authored line.\nKeep this break.\n\nAnd this paragraph.";
  assert(SimMenuHelp_Build(&page,hard,strlen(hard),0,false));
  assert(!strcmp(page.text,hard));
  char unicode[4096]={0};
  for(unsigned i=0;i<180;++i) strcat(unicode,"e\xcc\x81");
  bytes=strlen(unicode); offset=0;
  while(offset<bytes) {
    assert(SimMenuHelp_Build(&page,unicode,bytes,offset,false));
    assert(page.source_end%3==0);
    for(unsigned g=0;g<page.glyph_count;++g) assert(page.source_ends[g]%3==0);
    offset=page.source_end;
  }
  assert(SimMenuHelp_Build(&page,"Last",4,0,true) && page.more);
  assert(!SimMenuHelp_Build(&page,"Bad\xff",4,0,false));
  assert(strcmp(SimMenuHelp_Item(12),SimMenuHelp_Item(13)));
  assert(strcmp(SimMenuHelp_Item(17),SimMenuHelp_Item(18)));
  puts("sim_menu_help: OK");
  return 0;
}
