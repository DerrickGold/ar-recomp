#include "sim/sim_menu_help.h"
#include "localization/unicode_grapheme.h"
#include <string.h>

bool SimMenuHelp_Build(SimMenuHelpPage *p,const char *s,size_t bytes,
                       size_t start,bool more_authored_pages) {
  if (!p || !s || start>bytes) return false;
  memset(p,0,sizeof(*p)); p->source_start=start;
  unsigned row=0,col=0;
  size_t at=start;
  while(at<bytes && row<6 && p->glyph_count<kSimMenuHelpGlyphs) {
    uint32_t scalar; size_t end;
    if (!ArUnicodeGrapheme_Next(s,bytes,at,&scalar,&end)) return false;
    if (scalar=='\n' || scalar=='\r') {
      at=end; col=0; ++row; continue;
    }
    /* Wrap a word as a unit where it fits; long words break at graphemes. */
    if (col && scalar!=' ' && (at==start || s[at-1]==' ')) {
      unsigned length=0; size_t cursor=at;
      while(cursor<bytes && length<=22) {
        uint32_t next_scalar; size_t next;
        if (!ArUnicodeGrapheme_Next(s,bytes,cursor,&next_scalar,&next)) return false;
        if(next_scalar==' ' || next_scalar=='\n' || next_scalar=='\r') break;
        ++length; cursor=next;
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
    p->source_ends[g]=end; p->text_ends[g]=end-start;
    p->scalars[g]=scalar; p->row[g]=row; p->column[g]=col++;
    at=end;
  }
  /* Only these added Help pages are automatically divided. Keep a tiny
   * sentence opening (e.g. Wheat's "An") with the rest of its sentence when
   * it overflows the native grid. Authored breaks and all ROM dialogue are
   * untouched; both renderers still receive one identical source boundary. */
  if (at<bytes && p->glyph_count>kSimMenuHelpGlyphs*3/4) {
    for (unsigned g=p->glyph_count;g>kSimMenuHelpGlyphs*3/4;--g) {
      const uint32_t scalar=p->scalars[g-1];
      if (scalar!='.' && scalar!='!' && scalar!='?') continue;
      size_t boundary=p->source_ends[g-1];
      if (boundary>=at || s[boundary]!=' ' || p->glyph_count-g>12) break;
      while(boundary<at && s[boundary]==' ') ++boundary;
      if (boundary<at && !memchr(s+boundary,'\n',at-boundary) &&
          !memchr(s+boundary,'\r',at-boundary)) {
        at=boundary; p->glyph_count=g;
      }
      break;
    }
  }
  /* Native tile wrapping decides the shared page boundary, not the enhanced
   * paragraph's line breaks. Keep the original slice, including only authored
   * breaks, so proportional fonts can wrap it to their real dialogue width. */
  p->bytes=at-start;
  if(p->bytes>=sizeof(p->text)) return false;
  memcpy(p->text,s+start,p->bytes);
  p->text[p->bytes]=0; p->source_end=at;
  p->more=at<bytes || more_authored_pages; p->active=true;
  return true;
}

const char *SimMenuHelp_Category(unsigned category) {
  static const char *const text[]={
    "Return to the Sky Palace or move the palace to another region.",
    "Guide the development of your people, or listen to their requests.",
    "Use SP to perform a miracle. Describe explains a miracle without using it.",
    "Receive offerings at the cathedral, or choose an offering you already hold to use.",
    "View the status of the Master or compare the cities.",
    "Record your progress or adjust the speed of messages."};
  return category<6?text[category]:"";
}
const char *SimMenuHelp_Action(unsigned action) {
  static const char *const text[]={
    "Leave the town and return to the Sky Palace.",
    "Enter the world view to move the Sky Palace to another region.",
    "Choose a direction for your people to develop. Select the next part of town where you want them to build.",
    "Listen to the current message or request from your people.",
    "", "", "", "", "",
    "Visit the cathedral to receive an offering from your people. Choose what to take from the offerings available there.",
    "Choose an offering you hold. Some take effect after a message; others ask you to choose a place in town.",
    "View the level, life, magic and possessions of the Master.",
    "View the cities and their populations.",
    "Record your progress and choose whether to continue playing.",
    "Choose how quickly messages appear."};
  return action>=1 && action<=15?text[action-1]:"";
}
const char *SimMenuHelp_Item(unsigned item) {
  if(item>=1 && item<=4) return "Use this magic during action stages. It cannot be used from the offering menu in town.";
  switch(item) {
    case 5:return "Increases the maximum life of the Master when received.";
    case 6:return "Increases the magic of the Master when received.";
    case 7:return "Choose a place in town to offer bread. It is used only at a suitable location. Cancelling the selection leaves it in your inventory.";
    case 8:return "Choose a place in town to plant wheat. The selected terrain and the situation in town determine whether it can be planted. An invalid place or cancellation leaves the offering in your inventory.";
    case 9:return "Use the herb in the town where it is needed. Your people will explain its effect before using it.";
    case 10:return "Help your people build a bridge. It can be used when the town needs this help.";
    case 11:return "Play music for your people. Its effect depends on the current situation in town.";
    case 12:return "Keep this ancient tablet among your possessions. It has no effect when used directly in town.";
    case 13:return "Give this ancient tablet to your people. Their message will explain its effect.";
    case 14:return "Choose a lair in town for the effect of the skull. Cancelling the selection leaves it in your inventory.";
    case 15:return "Give the fleece to your people to trigger its effect in town.";
    case 16:case 17:return "Keep this bomb among your possessions. It cannot be used directly in town.";
    case 18:return "Use this bomb against the monsters in town. It takes effect immediately and is consumed when used.";
    case 19:return "Use the compass in the town where it is needed.";
    case 20:return "Strengthen the angel for combat. The offering is consumed as its effect is applied.";
    default:return "";
  }
}
