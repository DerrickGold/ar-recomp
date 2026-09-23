#include "actraiser/actraiser_population_conversion.h"
#include "actraiser/actraiser_cell_map.h"
#include "actraiser/actraiser_story_snapshot.h"
#include "snesrecomp/support/utf8_fs.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define REMOVE_DIR(p) _rmdir(p)
#else
#include <unistd.h>
#define REMOVE_DIR(p) rmdir(p)
#endif

static uint8_t wram[0x20000], rom[65536], sram[kActRaiserSramSize];
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 address) {
  (void)cpu;
  if(bank==3)return rom[address];
  if(bank==0x70){assert(address<sizeof(sram));return sram[address];}
  assert(bank==0 || bank==0x7f);return wram[(bank==0x7f?0x10000:0)+address];
}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 address) {
  return cpu_read8(cpu,bank,address)|(uint16)cpu_read8(cpu,bank,address+1)<<8;
}
void cpu_write8(CpuState *cpu,uint8 bank,uint16 address,uint8 value) {
  (void)cpu;assert(bank==0 || bank==0x7f);wram[(bank==0x7f?0x10000:0)+address]=value;
}
void cpu_write16(CpuState *cpu,uint8 bank,uint16 address,uint16 value) {
  cpu_write8(cpu,bank,address,value);cpu_write8(cpu,bank,address+1,value>>8);
}
/* Extension bridge codec has separate exhaustive coverage. This fixture has
 * no sidecar bridges; the native bridge record remains part of the real census. */
unsigned ActRaiserBridgeExtension_Count(CpuState *cpu,uint8_t bank,unsigned town) {
  (void)cpu;(void)bank;(void)town;return 0;
}
bool ActRaiserRegional_CopySupport(ArRegionalSupportSnapshot *snapshot) {
  const ArRegionalSupportPolicy native={{0}};return ArRegionalSupport_Resolve(&native,snapshot);
}
static void Word(uint8_t *bytes,unsigned at,unsigned value){bytes[at]=value;bytes[at+1]=value>>8;}
static void Record(unsigned town,unsigned slot,unsigned flags,unsigned x,unsigned y) {
  const unsigned at=0x16be7+town*512+slot*4;
  wram[at]=x;wram[at+1]=y;wram[at+2]=flags;wram[at+3]=1;
  const unsigned type=flags&15,width=type==2?2:1,mark=type==0?0xe0:type==2?0xe4:0xe1;
  for(unsigned dy=0;dy<width;++dy)for(unsigned dx=0;dx<width;++dx)
    wram[0x12000+ActRaiser_CellMarkIndex(town,x+dx,y+dy)]=mark;
}
static CpuState Setup(void) {
  memset(wram,0,sizeof(wram));memset(sram,0,sizeof(sram));memset(rom,0,sizeof(rom));
  Word(wram,0x18,0x0700);wram[0x216]=1;Word(wram,0x291,10);
  memcpy(wram+0x288,"ELISE",5);
  memset(wram+0x177e7,0x5a,0x400);
  memset(wram+0x16800,0x25,0x300);memset(wram+0x19250,0x91,0x300);
  for(unsigned town=0;town<6;++town) {
    Word(rom,0xdc74+2*town,0x6be7+town*512);
    Word(wram,0x16b18+town*2,2);Word(wram,0x22e + town*2,3);
    Word(wram,0x21c+town*2,10);Word(wram,0x16b26+town*2,64);
    Word(wram,0x17cc9+town*2,3);Word(wram,0x17cd5+town*2,15);Word(wram,0x17ce1+town*2,7);
    Record(town,0,0xa0,2,2);Record(town,1,0x82,4,4);Record(town,2,0x81,8,8);
  }
  Save_RecomputeChecksum(sram);
  return (CpuState){.A=0xabcd,.X=0xf25f,.Y=0xf270,.PB=1,.DB=1,.S=0x1e00,.P=CPU_P_M,.m_flag=1};
}
static bool Identity(void *context,uint8_t id[16]){(void)context;memset(id,7,16);return true;}
typedef struct Faults {SaveCommitHost inner;unsigned calls,fail_at;bool recovery_fail,name_fail;const char *name_block;} Faults;
static bool Prepare(void *context,SaveError *error){Faults *f=context;return f->inner.prepare_story(f->inner.context,error);}
static bool Commit(void *context,SaveFileFormat format,const char *path,const uint8_t *expected,
    const uint8_t *image,SaveCommitKind kind,const char *import,SaveError *error) {
  Faults *f=context;++f->calls;
  if(f->calls==f->fail_at)return false;
  const bool ok=f->inner.commit(f->inner.context,format,path,expected,image,kind,import,error);
  if(ok && f->calls==2 && f->name_fail)assert(sr_mkdir(f->name_block)==0);
  return ok;
}
static void Reload(void *context){Faults *f=context;f->inner.reloaded(f->inner.context);}
static bool Recovery(void *context,const char *source,const char *destination,const uint8_t *image,SaveError *error) {
  Faults *f=context;
  return !f->recovery_fail && f->inner.copy_recovery(f->inner.context,source,destination,image,error);
}
static void Remove(const char *path){
  char extra[256];remove(path);
  snprintf(extra,sizeof(extra),"%s.archeckpoint",path);remove(extra);
  snprintf(extra,sizeof(extra),"%s.arname",path);remove(extra);
}
static void Run(SaveBackend backend,unsigned failure,unsigned source) {
  const char *native="population-conversion.srm",*ini="population-conversion.ini",*folder="population-recovery";
  const char *path=backend==kSaveBackend_Ini?ini:native;
  const SaveFileFormat format=backend==kSaveBackend_Ini?kSaveFileFormat_Ini:kSaveFileFormat_NativeSrm;
  const char *recovered="population-recovery/save.srm";
  Remove(native);Remove(ini);Remove(recovered);REMOVE_DIR(folder);
  CpuState cpu=Setup(), original_cpu=cpu;
  SaveError error={{0}};
  assert(Save_WriteFile(format,path,sram,&error));
  assert(SaveSystem_Attach(sram,sizeof(sram),backend,native,ini,&error) && SaveSystem_LoadActive(&error));
  ArRegionalCampaign campaign;ArRegionalCampaign_Init(&campaign,0,Identity,NULL);
  ArRegionalCostPolicy defaults={{0}};assert(ArRegionalCampaign_NewGame(&campaign,&defaults,&error));
  if(source==0)assert(ArRegionalSession_SetPopulationProfile(&campaign.active,campaign.active.revision,1));
  const ArRegionalSession previous=campaign.active;
  char name_block[256];snprintf(name_block,sizeof(name_block),"%s.arname.tmp",path);
  Faults faults={.inner=ArRegionalCampaign_SaveHost(&campaign),.fail_at=failure==1?1:failure==3?2:0,
      .recovery_fail=failure==2,.name_fail=failure==4,.name_block=name_block};
  SaveCommitHost host={.context=&faults,.prepare_story=Prepare,.commit=Commit,.reloaded=Reload,.copy_recovery=Recovery};
  assert(SaveSystem_SetCommitHost(&host) && SaveSystem_SetLocalizedPlayerName("Élise","ELISE"));
  uint8_t before[sizeof(wram)], checkpoint[kActRaiserSramSize], disk[kActRaiserSramSize];
  memcpy(before,wram,sizeof(wram));assert(ActRaiserStorySnapshot_Capture(&cpu,checkpoint));
  ActRaiserPopulationPreview preview;
  cpu.PB=0;assert(ActRaiserPopulation_Preview(&cpu,&campaign,source,&preview)==kActRaiserPopulation_Unsafe);cpu.PB=1;
  Word(wram,0x18,0x0100);assert(ActRaiserPopulation_Preview(&cpu,&campaign,source,&preview)==kActRaiserPopulation_Unsafe);Word(wram,0x18,0x0700);
  assert(ActRaiserPopulation_Preview(&cpu,&campaign,source,&preview)==kActRaiserPopulation_Ready);
  assert(preview.redevelop==(source!=2) && preview.town.affected_towns==(source==2?0:63));
  for(unsigned town=0;town<6;++town)assert(preview.town.removed[town]==(source==2?0:2) && preview.town.growth_credit[town]==(source==2?0:8));
  if(source!=2) {
    ++wram[0x19efa];assert(ActRaiserPopulation_Commit(&cpu,&campaign,&preview,folder,&error)==kActRaiserPopulation_Stale);--wram[0x19efa];
  }
  ++campaign.active.revision;assert(ActRaiserPopulation_Commit(&cpu,&campaign,&preview,folder,&error)==kActRaiserPopulation_Stale);--campaign.active.revision;
  assert(!faults.calls && !sr_path_exists(folder));
  const ActRaiserPopulationResult result=ActRaiserPopulation_Commit(&cpu,&campaign,&preview,folder,&error);
  const ActRaiserPopulationResult expected=failure==1?kActRaiserPopulation_CheckpointFailed:failure==2?kActRaiserPopulation_RecoveryFailed:
      failure==3?kActRaiserPopulation_RolledBack:failure==4?kActRaiserPopulation_NamePending:kActRaiserPopulation_Committed;
  if(result!=expected)fprintf(stderr,"conversion result %d != %d: %s\n",result,expected,error.message);
  assert(result==expected && !memcmp(&cpu,&original_cpu,sizeof(cpu)));
  if(failure>=1 && failure<=3) {
    assert(!memcmp(wram,before,sizeof(wram)) && !memcmp(&campaign.active,&previous,sizeof(previous)));
    if(failure>1)assert(Save_LoadFile(format,path,disk,&error) && !memcmp(disk,checkpoint,sizeof(disk)));
  } else {
    assert(campaign.active.effective.support.source[0]==source);
    for(unsigned town=0;source!=2 && town<6;++town) {
      assert(!wram[0x16be9+town*512] && !wram[0x16bed+town*512] && wram[0x16bf1+town*512]==0x81);
      Word(before,0x21c+town*2,2);Word(before,0x16b26+town*2,source==1?16:32);
      Word(before,0x19efa+town*2,8);
      for(unsigned slot=0;slot<2;++slot)before[0x16be9+town*512+4*slot]=0;
      before[0x12000+ActRaiser_CellMarkIndex(town,2,2)]=8;
      for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx)before[0x12000+ActRaiser_CellMarkIndex(town,4+dx,4+dy)]=8;
    }
    if(source!=2) {
      memset(before+0x177e7,0,16);memset(before+0x19758,0xff,14);
      Word(before,0x218,12);Word(before,0x21a,2);
    }
    Word(before,0x297,source==1?1800:2200);
    assert(!memcmp(wram,before,sizeof(wram))); /* Includes unchanged roads, plots, clocks, awards, story and actors. */
    assert(ActRaiserPopulation_Commit(&cpu,&campaign,&preview,folder,&error)==kActRaiserPopulation_Stale);
    assert(ActRaiserPopulation_Preview(&cpu,&campaign,source,&preview)==kActRaiserPopulation_Unchanged);
    assert(faults.calls==2);
    if(failure==4)assert(REMOVE_DIR(name_block)==0 && SaveSystem_AutoPersistIfChanged(&error));
    assert(Save_LoadFile(format,path,disk,&error) && !memcmp(disk,sram,sizeof(disk)));
    ArRegionalSession saved;assert(ArRegionalSession_Load(&saved,0,path,disk,&error)==kSaveCheckpoint_Ready);
    assert(!memcmp(&saved,&campaign.active,sizeof(saved)));
    /* A loaded converted save contains no replayable demolition command. */
    assert(SaveSystem_LoadActive(&error) && ArRegionalCampaign_Continue(&campaign,path,sram,&error));
    assert(ActRaiserPopulation_Preview(&cpu,&campaign,source,&preview)==kActRaiserPopulation_Unchanged);
  }
  if(failure!=1 && failure!=2) {
    assert(Save_LoadFile(kSaveFileFormat_NativeSrm,recovered,disk,&error) && !memcmp(disk,checkpoint,sizeof(disk)));
    ArRegionalSession saved;assert(ArRegionalSession_Load(&saved,0,recovered,disk,&error)==kSaveCheckpoint_Ready);
    assert(!memcmp(&saved,&previous,sizeof(saved)));
  }
  Remove(native);Remove(ini);Remove(recovered);if(failure!=1)assert(REMOVE_DIR(folder)==0);
}
int main(void) {
  for(unsigned backend=0;backend<2;++backend)for(unsigned failure=0;failure<5;++failure)for(unsigned source=0;source<3;++source)
    Run(backend,failure,source);
  puts("population conversion: both directions/backends, stale previews, recovery, rollback and cold reload passed");
  return 0;
}
