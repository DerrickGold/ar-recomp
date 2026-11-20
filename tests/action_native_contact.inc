#include "action/action_native_contact.h"
/* Native surface samples: standalone, lower paired, upper paired. Verify
 * both directions, empty endpoints and the native upper-neighbour priority. */
static void NativeContactChecks(void) {
  static const unsigned top[3][16]={
    {16,16,14,14,12,12,10,10,8,8,6,6,4,4,2,2},
    {16,16,15,15,14,14,13,13,12,12,11,11,10,10,9,9},
    {8,8,7,7,6,6,5,5,4,4,3,3,2,2,1,1}};
  for(unsigned collision=0;collision<16;++collision)for(unsigned left=0;left<16;++left)
    for(unsigned right=0;right<16;++right)for(unsigned x=0;x<16;++x) {
      unsigned expected;
      if(collision==6||collision==9) {
        const unsigned upper=collision==6?left:right,lower=collision==6?right:left;
        expected=top[upper==collision?2:lower==collision?1:0][collision==6?x:15-x];
      } else {
        const unsigned quarter=x<8?0:1;
        expected=collision&(1u<<quarter)?0:collision&(4u<<quarter)?8:16;
      }
      assert(ActionNativeContact_Top(collision,left,right,x)==expected);
    }
}
