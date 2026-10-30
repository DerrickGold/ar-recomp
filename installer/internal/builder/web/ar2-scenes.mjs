// Decorative paths for the imported Master. These never execute level code
// or touch gameplay state; sampling absolute time keeps pause/resume stable.
export const MASTER_ACTIONS = ["idle","walk","sword","jump","air-sword","fall","glide-start","glide"];

export function sampleMasterPatrol(time) {
  const t = Math.max(0,time)%20000, left = t >= 10000, phase = t%10000;
  const travel = Math.min(1,Math.max(0,(phase-1000)/3000));
  const state = {x:left?430-220*travel:210+220*travel, y:386, facing:left?"left":"right", action:"idle", phase:0, loop:true};
  if (phase >= 1000 && phase < 4000) Object.assign(state,{action:"walk",phase:phase-1000});
  else if (phase >= 4500 && phase < 4817) Object.assign(state,{action:"sword",phase:phase-4500,loop:false});
  else if (phase >= 6000 && phase < 7400) {
    const jump = phase-6000, fraction = jump/1400;
    state.y -= 4*130*fraction*(1-fraction);
    Object.assign(state, jump<200 ? {action:"jump",phase:jump,loop:false} :
      jump>=350 && jump<667 ? {action:"air-sword",phase:jump-350,loop:false} : {action:"fall",phase:jump,loop:false});
  }
  return state;
}

export function sampleMasterGlide(time) {
  const t = Math.max(0,time)%24000, left = t >= 12000, phase = t%12000;
  const travel = .5-.5*Math.cos(phase/12000*Math.PI);
  return {x:left?470-300*travel:170+300*travel, y:270+Math.sin(t/24000*Math.PI*2)*24,
    facing:left?"left":"right",action:phase<400?"glide-start":"glide",phase,loop:phase>=400};
}
