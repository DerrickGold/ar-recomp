#include "regional/action/regional_collision.h"
#include <stddef.h>
static const ArRegionalCollisionDescriptor kRules[]={
  {"kasandora_pose_extents",{0,1,0}},
  {"marahna_arrow_extents",{0,1,0}},
};
_Static_assert(sizeof(kRules)/sizeof(kRules[0])==kArRegionalCollision_Count,"describe every collision family");
const ArRegionalCollisionDescriptor *ArRegionalCollision_Descriptor(unsigned rule) {
  return rule<kArRegionalCollision_Count?&kRules[rule]:NULL;
}
bool ArRegionalCollision_Init(ArRegionalCollisionPolicy *policy,ArRegionalSource source) {
  if(!policy || (unsigned)source>=kArRegionalSource_Count)return false;
  for(unsigned i=0;i<kArRegionalCollision_Count;++i)policy->source[i]=source;
  return true;
}
bool ArRegionalCollision_Resolve(const ArRegionalCollisionPolicy *policy,ArRegionalCollisionSnapshot *snapshot) {
  if(!policy || !snapshot)return false;
  uint8_t result=0;
  for(unsigned i=0;i<kArRegionalCollision_Count;++i) {
    if((unsigned)policy->source[i]>=kArRegionalSource_Count)return false;
    result|=(uint8_t)(kRules[i].japanese[policy->source[i]]<<i);
  }
  *snapshot=result;return true;
}
bool ArRegionalCollision_GroupSource(const ArRegionalCollisionPolicy *policy,ArRegionalSource *source) {
  uint8_t bits;if(!source || !ArRegionalCollision_Resolve(policy,&bits))return false;
  if(bits && bits!=(1u<<kArRegionalCollision_Count)-1)return false;
  /* Preserve a uniform requested PAL badge even though its values alias US. */
  *source=bits?kArRegionalSource_Japan:
      policy->source[0]==kArRegionalSource_Europe && policy->source[1]==kArRegionalSource_Europe?
      kArRegionalSource_Europe:kArRegionalSource_US;
  return true;
}
bool ArRegionalCollision_Pose(ArRegionalCollisionSnapshot snapshot,unsigned family,unsigned visual,
    const ArRegionalCollisionExtents *native,ArRegionalCollisionExtents *out) {
  if(!native || !out || family>=kArRegionalCollision_Count || snapshot>>kArRegionalCollision_Count ||
      !(snapshot&(1u<<family)))return false;
  ArRegionalCollisionExtents expected,next;
  if(family==kArRegionalCollision_Arrow && (visual==0x2c || visual==0x36)) {
    expected=(ArRegionalCollisionExtents){8,8,8,0};next=(ArRegionalCollisionExtents){16,16,8,0};
  } else if(family==kArRegionalCollision_Kasandora && visual==8) {
    expected=(ArRegionalCollisionExtents){16,15,20,0};next=(ArRegionalCollisionExtents){16,16,16,0};
  } else if(family==kArRegionalCollision_Kasandora && (visual==0x0e || visual==0x0f)) {
    expected=(ArRegionalCollisionExtents){16,16,32,24};next=(ArRegionalCollisionExtents){16,16,31,24};
  } else if(family==kArRegionalCollision_Kasandora && (visual==0x16 || visual==0x17)) {
    expected=(ArRegionalCollisionExtents){28,12,24,24};next=(ArRegionalCollisionExtents){36,12,24,24};
  } else return false;
  if(native->left!=expected.left || native->right!=expected.right ||
      native->top!=expected.top || native->bottom!=expected.bottom)return false;
  *out=next;return true;
}
