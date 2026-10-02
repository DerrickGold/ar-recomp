#include "action_actor_effects.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
static bool Hex(const char *text,uint16_t *value) {
  unsigned v;int n=0;
  if(strlen(text)>4||sscanf(text,"%x%n",&v,&n)!=1||text[n]||v>65535)return false;
  *value=(uint16_t)v;return true;
}
static bool Range(const char *text,uint16_t *first,uint16_t *last) {
  unsigned a,b;int n=0;
  if(sscanf(text,"%u,%u%n",&a,&b,&n)!=2||text[n]||a>b||b>65535)return false;
  *first=a;*last=b;return true;
}
bool ActionEffectActorSelector_Set(ActionEffectActorSelector *s,const char *key,const char *value) {
  static const char *const names[]={"actor-target","actor-source","actor-parent","actor-state","actor-visual","actor-animation","actor-handler","actor-resume","actor-limit"};
  unsigned i=0;while(i<9&&strcmp(key,names[i]))++i;
  if(i==9||(s->fields&(1u<<i)))return false;
  bool ok=false;
  switch(i){
    case 0:ok=!strcmp(value,"family")||!strcmp(value,"player");s->target=!strcmp(value,"player")?2:1;break;
    case 1:ok=Hex(value,&s->source);break;
    case 2:ok=Hex(value,&s->parent);break;
    case 3:ok=Range(value,&s->state_first,&s->state_last);break;
    case 4:ok=Range(value,&s->visual_first,&s->visual_last);break;
    case 5:ok=Hex(value,&s->animation);break;
    case 6:ok=Hex(value,&s->handler);break;
    case 7:ok=Hex(value,&s->resume);break;
    case 8:{unsigned v;int n=0;ok=sscanf(value,"%u%n",&v,&n)==1&&!value[n]&&v>=1&&v<=kActionActorBindingMaxInstances;if(ok)s->limit=v;break;}
  }
  if(ok)s->fields|=1u<<i;
  return ok;
}
bool ActionEffectActorSelector_Valid(const ActionEffectActorSelector *s) {
  if(!s->fields)return true;
  return (s->fields&1)&&s->target>=1&&s->target<=2&&s->limit>=1&&s->limit<=kActionActorBindingMaxInstances&&
      (s->target!=1||((s->fields&2)&&s->source));
}
bool ActionEffectActorSelector_Matches(const ActionEffectActorSelector *s,const ActionEffectActor *a) {
  return s&&a&&a->visible&&(s->target==2?a->player:!a->player&&a->source==s->source)&&
    (!(s->fields&4)||s->parent==a->parent_source)&&
    (!(s->fields&8)||(a->state>=s->state_first&&a->state<=s->state_last))&&
    (!(s->fields&16)||(a->visual>=s->visual_first&&a->visual<=s->visual_last))&&
    (!(s->fields&32)||a->animation==s->animation)&&
    (!(s->fields&64)||a->handler==s->handler)&&
    (!(s->fields&128)||a->resume==s->resume);
}
