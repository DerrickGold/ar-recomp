import {test} from "node:test";
import assert from "node:assert/strict";
import {createEncounterState,sampleEncounter,ENCOUNTER_PERIOD,BOSS_START} from "../web/encounters.mjs";

test("bounded actors, finite positions and continuous Master path for many loops",()=>{
  const state=createEncounterState(),master=state.master,enemy=state.enemy,effect=state.effect;
  let last;
  for(let t=0;t<ENCOUNTER_PERIOD*4;t+=1000/60) {
    assert.equal(sampleEncounter(t,state),state);
    assert.equal(state.master,master);assert.equal(state.enemy,enemy);assert.equal(state.effect,effect);
    for(const a of [master,enemy]) {
      for(const v of [a.x,a.y,a.phase,a.opacity]) assert(Number.isFinite(v));
      assert(a.y<=386&&a.y>=0);assert(a.phase>=0);assert(a.opacity>=0&&a.opacity<=1);
    }
    if(last) {assert(Math.abs(master.x-last[0])<4);assert(Math.abs(master.y-last[1])<14);}
    last=[master.x,master.y];
  }
});

test("each ordinary enemy takes one timed sword hit and stays gone",()=>{
  for(const round of [0,1,2]) {
    for(const [hit,kind] of [[3183.3333333333335,"bird"],[8950,round%2?"leaper":"club"],[14950,round%2?"club":"leaper"]]) {
      const t=round*18000+hit,before=sampleEncounter(t-1),impact=sampleEncounter(t+.01),after=sampleEncounter(t+301);
      assert.equal(before.enemy.kind,kind);assert(before.enemy.visible);assert(!before.effect.visible);
      assert.equal(impact.enemy.kind,kind);assert(impact.effect.visible);assert.equal(impact.enemy.opacity<1,true);
      assert.equal(impact.master.action,kind==="bird"?"air-sword":"sword");
      assert(Math.abs(impact.enemy.x-impact.master.x)<65,"target must meet the sword");
      assert.equal(after.enemy.visible,false);assert.equal(after.effect.visible,false);
    }
  }
});

test("boss is rare, takes three hits, and returns to the same patrol origin",()=>{
  assert.equal(sampleEncounter(BOSS_START-1).boss,false);
  for(const [hit,count] of [[4083.3333333333335,1],[8150,2],[11083.333333333334,3]]) {
    const s=sampleEncounter(BOSS_START+hit+.01);
    assert.equal(s.enemy.kind,"centaur");assert.equal(s.hits,count);assert(s.effect.visible&&s.effect.boss);
    assert(Math.abs(s.enemy.x-s.master.x)<60,"boss must be in sword reach");
    const later=sampleEncounter(BOSS_START+hit+651);
    assert.equal(later.enemy.visible,count<3);
  }
  assert.equal(sampleEncounter(ENCOUNTER_PERIOD-1).master.x,180);
  assert.equal(sampleEncounter(ENCOUNTER_PERIOD).master.x,180);
});

test("direct sampling matches stepped playback and never resurrects a defeated actor",()=>{
  const state=createEncounterState();
  for(const end of [3200,9200,15300,BOSS_START+11200,BOSS_START+16000,ENCOUNTER_PERIOD+3200]) {
    for(let t=0;t<end;t+=71) sampleEncounter(t,state);
    assert.deepEqual(sampleEncounter(end,state),sampleEncounter(end));
  }
});
