#include "regional/towns/regional_score_feedback.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  assert(!ArRegionalScore_Descriptor((ArRegionalScoreRule)-1));
  assert(!ArRegionalScore_Descriptor(kArRegionalScore_Count));
  for(unsigned combination=0; combination<81; ++combination) {
    ArRegionalScorePolicy policy;
    unsigned digits=combination;
    for(unsigned i=0; i<kArRegionalScore_Count; ++i) { policy.source[i]=(ArRegionalSource)(digits%3); digits/=3; }
    ArRegionalScoreSnapshot snapshot;
    assert(ArRegionalScore_Resolve(&policy,&snapshot));
    bool same=true, equivalent=true;
    for(unsigned i=1;i<kArRegionalScore_Count;++i) {
      same &= policy.source[0]==policy.source[i];
      equivalent &= snapshot.japanese[0]==snapshot.japanese[i];
    }
    ArRegionalSource source=kArRegionalSource_Count;
    assert(ArRegionalScore_GroupSource(&policy,&source)==equivalent);
    if(equivalent)assert(source==(same?policy.source[0]:snapshot.japanese[0]?kArRegionalSource_Japan:kArRegionalSource_US));
    else assert(source==kArRegionalSource_Count);
    for(unsigned i=0; i<kArRegionalScore_Count; ++i) {
      assert(snapshot.japanese[i]==(policy.source[i]==kArRegionalSource_Japan));
      const ArRegionalScoreDescriptor *d=ArRegionalScore_Descriptor((ArRegionalScoreRule)i);
      assert(d && d->japanese[0]==0 && d->japanese[1]==1 && d->japanese[2]==0);
      for(unsigned j=0; j<i; ++j)assert(strcmp(d->key,ArRegionalScore_Descriptor((ArRegionalScoreRule)j)->key));
    }
    const ArRegionalScoreSnapshot before=snapshot;
    policy.source[2]=kArRegionalSource_Count;
    assert(!ArRegionalScore_Resolve(&policy,&snapshot) && !memcmp(&snapshot,&before,sizeof(snapshot)));
  }
  for(unsigned source=0;source<3;++source)for(unsigned input=0;input<65536;++input) {
    unsigned score=0,place=1; bool valid=true;
    for(unsigned n=0;n<4;++n) { unsigned digit=(input>>(4*n))&15; valid&=digit<10; score+=place*digit;place*=10; }
    uint16_t units=0xffff;
    assert(ArRegionalScore_Convert((ArRegionalSource)source,(uint16_t)input,&units)==valid);
    assert(units==(valid ? source==1 ? (score<650?0:(score-650)/32*10) : score/10*2 : 0xffff));
    ArRegionalScoreDestination destination=kArRegionalScoreDestination_None;
    assert(ArRegionalScore_Destination((ArRegionalSource)source,(uint16_t)input,&destination));
    assert(destination==(input==2?kArRegionalScoreDestination_Growth:
        source==1||input==1?kArRegionalScoreDestination_Stocks:kArRegionalScoreDestination_None));
  }
  ArRegionalScorePolicy policy={0}, before=policy;
  assert(!ArRegionalScore_Init(&policy,kArRegionalSource_Count) && !memcmp(&policy,&before,sizeof(policy)));
  for(unsigned source=0;source<3;++source) {
    assert(ArRegionalScore_Init(&policy,(ArRegionalSource)source));
    for(unsigned i=0;i<kArRegionalScore_Count;++i)assert(policy.source[i]==(ArRegionalSource)source);
  }
  uint16_t units=0xffff; ArRegionalScoreDestination destination=kArRegionalScoreDestination_Growth;
  assert(!ArRegionalScore_Convert(kArRegionalSource_Count,0,&units) && units==0xffff);
  assert(!ArRegionalScore_Destination(kArRegionalSource_Count,1,&destination) && destination==kArRegionalScoreDestination_Growth);
  puts("score feedback: independent policies, all BCD inputs and completion counts passed");
  return 0;
}
