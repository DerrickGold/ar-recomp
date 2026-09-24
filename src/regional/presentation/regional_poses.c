#include "regional/presentation/regional_poses.h"
#include <stddef.h>
static const ArRegionalPoseDescriptor kRules[]={
  {"aitos_shared_pose_order",{0,1,0}},
  {"aitos_humanoid_pose_order",{0,1,0}},
};
_Static_assert(kArRegionalPose_Count==2,"pose snapshot bits");
const ArRegionalPoseDescriptor *ArRegionalPoses_Descriptor(unsigned rule) {return rule<kArRegionalPose_Count?&kRules[rule]:NULL;}
bool ArRegionalPoses_Resolve(const ArRegionalPosePolicy *policy,uint8_t *snapshot) {
  if(!policy || !snapshot)return false;
  uint8_t value=0;
  for(unsigned i=0;i<kArRegionalPose_Count;++i) {
    if((unsigned)policy->source[i]>=kArRegionalSource_Count)return false;
    value|=(uint8_t)(kRules[i].enabled[policy->source[i]]<<i);
  }
  *snapshot=value;return true;
}
bool ArRegionalPoses_Visual(uint8_t snapshot,unsigned state,unsigned row,uint8_t native,uint8_t *visual) {
  if(!visual || (snapshot&~3u) || row>=4 || (state!=10 && state!=45))return false;
  const unsigned rule=state==10?kArRegionalPose_AitosShared:kArRegionalPose_AitosHumanoid;
  const uint8_t expected[]={0,4,6,2};
  if(native!=expected[row]+(state==10))return false;
  *visual=(snapshot&(1u<<rule)) && (row==0 || row==3)?(uint8_t)(native+(row==0?2:-2)):native;
  return true;
}
