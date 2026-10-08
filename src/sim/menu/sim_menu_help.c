#include "sim/menu/sim_menu_help.h"
#include "localization/unicode_grapheme.h"
#include <string.h>
#include <stdio.h>

bool SimMenuHelp_Build(SimMenuHelpPage *p,const char *s,size_t bytes,
                       size_t start,bool more_authored_pages,bool enhanced) {
  if (!p || !s || start>bytes) return false;
  memset(p, 0, sizeof(*p));
  p->source_start = start;
  p->enhanced = enhanced;
  if (enhanced) {
    if (bytes-start>=sizeof(p->text)) return false;
    for (size_t at=start,next;at<bytes;at=next)
      if (!ArUnicodeGrapheme_Next(s,bytes,at,NULL,&next)) return false;
    p->bytes=bytes-start;
    memcpy(p->text,s+start,p->bytes);
    p->text[p->bytes]=0;
    p->source_end=bytes;
    p->more=more_authored_pages;
    p->active=true;
    return true;
  }
  unsigned row=0,col=0;
  size_t at=start;
  while(at<bytes && row<6 && p->glyph_count<kSimMenuHelpGlyphs) {
    uint32_t scalar;
    size_t end;
    if (!ArUnicodeGrapheme_Next(s,bytes,at,&scalar,&end)) return false;
    if (scalar=='\n' || scalar=='\r') {
      at = end;
      col = 0;
      ++row;
      continue;
    }
    /* Wrap a word as a unit where it fits; long words break at graphemes. */
    if (col && scalar!=' ' && (at==start || s[at-1]==' ')) {
      unsigned length = 0;
      size_t cursor = at;
      while(cursor<bytes && length<=22) {
        uint32_t next_scalar;
        size_t next;
        if (!ArUnicodeGrapheme_Next(s,bytes,cursor,&next_scalar,&next)) return false;
        if(next_scalar==' ' || next_scalar=='\n' || next_scalar=='\r') break;
        ++length;
        cursor = next;
      }
      if (length<=22 && col+length>22) col=22;
    }
    if(col==22) {
      if(++row==6) break;
      col=0;
    }
    if (!col && scalar==' ') { at=end; continue; }
    if(end-start>=sizeof(p->text)) return false;
    const unsigned g=p->glyph_count++;
    p->source_ends[g] = end;
    p->text_ends[g] = end - start;
    p->scalars[g] = scalar;
    p->row[g] = row;
    p->column[g] = col++;
    at=end;
  }
  /* Only these added Help pages are automatically divided. Keep a tiny
   * sentence opening (e.g. Wheat's "An") with the rest of its sentence when
   * it overflows the native grid. Authored breaks and all ROM dialogue are
   * untouched; this branch is only used by the native glyph renderer. */
  if (at<bytes && p->glyph_count>kSimMenuHelpGlyphs*3/4) {
    for (unsigned g=p->glyph_count;g>kSimMenuHelpGlyphs*3/4;--g) {
      const uint32_t scalar=p->scalars[g-1];
      if (scalar!='.' && scalar!='!' && scalar!='?') continue;
      size_t boundary=p->source_ends[g-1];
      if (boundary>=at || s[boundary]!=' ' || p->glyph_count-g>12) break;
      while(boundary<at && s[boundary]==' ') ++boundary;
      if (boundary<at && !memchr(s+boundary,'\n',at-boundary) &&
          !memchr(s+boundary,'\r',at-boundary)) {
        at = boundary;
        p->glyph_count = g;
      }
      break;
    }
  }
  /* Keep exact source offsets while the native tile grid wraps glyphs. */
  p->bytes=at-start;
  if(p->bytes>=sizeof(p->text)) return false;
  memcpy(p->text,s+start,p->bytes);
  p->text[p->bytes] = 0;
  p->source_end = at;
  p->more = at < bytes || more_authored_pages;
  p->active = true;
  return true;
}

#include "sim/menu/sim_menu_help_data.inc"
static const char *HelpText(const char *kind, unsigned value) {
  char id[64];
  snprintf(id, sizeof(id), !strcmp(kind,"category") ? "sim.help.%s.%u" : "sim.help.%s.%02u", kind,value);
  for (size_t i=0;i<sizeof(kSimMenuHelpText)/sizeof(kSimMenuHelpText[0]);++i)
    if (!strcmp(id,kSimMenuHelpText[i].id)) return kSimMenuHelpText[i].text;
  return "";
}
const char *SimMenuHelp_Category(unsigned category) { return HelpText("category",category); }
const char *SimMenuHelp_Action(unsigned action) { return HelpText("action",action); }
const char *SimMenuHelp_Item(unsigned item) { return HelpText("item",item); }
