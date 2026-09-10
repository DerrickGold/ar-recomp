// Decorative choreography only: no gameplay, randomness, wall clock, timers,
// collision simulation or growing entity lists. Sampling an absolute scene
// time also makes pause/resume and dropped frames safe at every hit boundary.
export const ENCOUNTER_PERIOD = 72000;
export const BOSS_START = 54000;
const FLOOR = 386;
// Native program $0C: the third sword-arc row begins at tick 23. Standing
// program $08 reaches its last arc row at tick 9. Times are scene milliseconds.
const AIR_CONTACT=23*1000/60, GROUND_CONTACT=9*1000/60, AIR_DURATION=40*1000/60;
const BIRD_HIT=2800+AIR_CONTACT, CLUB_HIT=8800+GROUND_CONTACT, LEAPER_HIT=14800+GROUND_CONTACT;
const BOSS_FIRST_HIT=3700+AIR_CONTACT, BOSS_SECOND_HIT=8000+GROUND_CONTACT, BOSS_LAST_HIT=10700+AIR_CONTACT;
const lerp = (a, b, t) => a + (b-a)*Math.max(0, Math.min(1, t));
const arc = (t, duration, height) => {
  const p=Math.max(0, Math.min(1, t/duration));
  return 4*height*p*(1-p);
};

export function createEncounterState() {
  const actor=()=>({kind:"",facing:"",action:"",phase:0,x:0,y:FLOOR,opacity:1,visible:true,loop:true});
  return {master:actor(),enemy:actor(),effect:{age:0,x:0,y:0,boss:false,visible:false},boss:false,hits:0};
}

function master(a,x,facing,action,phase,y=FLOOR) {
  Object.assign(a,{kind:"master",x,y,facing,action,phase,loop:action!=="sword"&&action!=="air-sword"});
}

function opponent(a,kind,x,facing,action,phase,y=FLOOR) {
  Object.assign(a,{kind,x,y,facing,action,phase});
}

function strike(state,age,x,y,boss=false,defeated=true) {
  // Small gold chips and a single short translucency change, no strobing or
  // screen flash. A defeated sprite cannot reappear after the effect expires.
  const duration=boss?650:300;
  Object.assign(state.effect,{age,x,y,boss,visible:age>=0&&age<duration});
  if(age<0) return;
  if(defeated) {
    state.enemy.visible=age<duration;
    state.enemy.opacity=Math.max(0,1-age/duration);
    state.enemy.y-=Math.min(age/duration,1)*12;
  } else if(age<160) state.enemy.opacity=.65+.35*age/160;
}

function regular(t,round,s) {
  const m=s.master,e=s.enemy;
  s.hits=t>=LEAPER_HIT?3:t>=CLUB_HIT?2:t>=BIRD_HIT?1:0;
  master(m,180,"right","idle",t);
  if(t>=1000&&t<2800) master(m,lerp(180,260,(t-1000)/1800),"right","walk",t-1000);
  else if(t>=2800&&t<3700) {
    const u=t-2800;
    master(m,lerp(260,360,u/900),"right",u<AIR_DURATION?"air-sword":"fall",u,FLOOR-arc(u,900,120));
  } else if(t>=3700&&t<4600) master(m,360,"right","idle",t-3700);
  else if(t>=4600&&t<5400) master(m,lerp(360,300,(t-4600)/800),"left","walk",t-4600);
  else if(t>=5400&&t<10200) master(m,300,"left",t>=8800&&t<9150?"sword":"idle",t-8800);
  else if(t>=10200&&t<11700) master(m,lerp(300,180,(t-10200)/1500),"left","walk",t-10200);
  else if(t>=11700) master(m,180,"right",t>=14800&&t<15150?"sword":"idle",t-14800);

  if(t<3500) {
    // $0C's actual sword arc starts after its first 21 native ticks; meet
    // the bird during that arc, not during the launch/wind-up poses.
    const hit=BIRD_HIT, u=Math.min(t,hit);
    opponent(e,"bird",lerp(730,350,(u-400)/(hit-400)),"normal","fly",u,216+12*Math.sin((hit-u)/600));
    strike(s,t-hit,350,216);
  } else if(t>=6000&&t<9300) {
    const hit=CLUB_HIT, u=Math.min(t,hit);
    // Alternate which ground enemy arrives first on later patrols.
    const kind=round%2?"leaper":"club";
    opponent(e,kind,lerp(-70,246,(u-6000)/(hit-6000)),"flipped",kind==="club"?"walk":"hop",u);
    if(kind==="leaper") e.y-=arc(((u-hit)%700+700)%700,700,20);
    strike(s,t-hit,246,FLOOR-32);
  } else if(t>=12300&&t<15300) {
    const hit=LEAPER_HIT,u=Math.min(t,hit),kind=round%2?"club":"leaper";
    opponent(e,kind,lerp(730,236,(u-12300)/(hit-12300)),"normal",kind==="club"?"walk":"hop",u);
    if(kind==="leaper") e.y-=arc(((u-hit)%700+700)%700,700,20);
    strike(s,t-hit,236,FLOOR-32);
  } else e.visible=false;
}

function boss(t,s) {
  const m=s.master,e=s.enemy;
  master(m,180,"right","idle",t);
  if(t>=1000&&t<2600) master(m,lerp(180,300,(t-1000)/1600),"right","walk",t-1000);
  else if(t>=2600&&t<3700) master(m,300,"right","idle",t);
  else if(t>=3700&&t<4800) {
    const u=t-3700;
    master(m,lerp(300,470,u/1100),"right",u<AIR_DURATION?"air-sword":"fall",u,FLOOR-arc(u,1100,170));
  } else if(t>=4800&&t<5800) master(m,470,"left","idle",t);
  else if(t>=5800&&t<6200) master(m,lerp(470,450,(t-5800)/400),"left","walk",t-5800);
  else if(t>=6200&&t<10700) master(m,450,"left",t>=8000&&t<8350?"sword":"idle",t-8000);
  else if(t>=10700&&t<11800) {
    const u=t-10700;
    // Hop backwards while facing the charging centaur for the final slash.
    master(m,lerp(450,280,u/1100),"right",u<AIR_DURATION?"air-sword":"fall",u,FLOOR-arc(u,1100,150));
  } else if(t>=11800&&t<13000) master(m,280,"right","idle",t);
  else if(t>=13000&&t<14400) master(m,lerp(280,180,(t-13000)/1400),"left","walk",t-13000);

  opponent(e,"centaur",lerp(740,470,(t-400)/2600),"normal","walk",t);
  if(t<400) e.visible=false;
  else if(t>=3000&&t<3600) opponent(e,"centaur",470,"normal","cast",t-3000);
  else if(t>=3600&&t<5200) opponent(e,"centaur",lerp(470,230,(t-3600)/1600),"normal","charge",t-3600);
  else if(t>=5200&&t<5800) opponent(e,"centaur",230,"flipped","idle",t);
  else if(t>=5800&&t<7800) opponent(e,"centaur",lerp(230,400,(t-5800)/2000),"flipped","walk",t-5800);
  else if(t>=7800&&t<8400) opponent(e,"centaur",400,"flipped","idle",t);
  else if(t>=8400&&t<9000) opponent(e,"centaur",lerp(400,380,(t-8400)/600),"flipped","idle",t);
  else if(t>=9000&&t<10600) opponent(e,"centaur",380,"flipped",t>=10000?"cast":"idle",t-10000);
  else if(t>=10600) opponent(e,"centaur",lerp(380,570,(Math.min(t,BOSS_LAST_HIT)-10600)/1600),"flipped","charge",Math.min(t,BOSS_LAST_HIT)-10600);
  if(t>=BOSS_LAST_HIT) {
    s.hits=3;strike(s,t-BOSS_LAST_HIT,e.x,230,true);
  } else if(t>=BOSS_SECOND_HIT) {
    s.hits=2;strike(s,t-BOSS_SECOND_HIT,418,310,true,false);
  } else if(t>=BOSS_FIRST_HIT) {
    s.hits=1;strike(s,t-BOSS_FIRST_HIT,400,220,true,false);
  }
  e.loop=e.action!=="cast";
}

export function sampleEncounter(time,state=createEncounterState()) {
  const t=Math.max(0,Number.isFinite(time)?time:0)%ENCOUNTER_PERIOD;
  state.boss=t>=BOSS_START;state.hits=0;
  Object.assign(state.effect,{age:0,x:0,y:0,boss:false,visible:false});
  Object.assign(state.enemy,{kind:"",facing:"",action:"",phase:0,x:0,y:FLOOR,opacity:1,visible:true,loop:true});
  state.master.opacity=1;state.master.visible=true;state.master.loop=true;
  if(state.boss) boss(t-BOSS_START,state);
  else regular(t%18000,Math.floor(t/18000),state);
  // Idle phases may precede an attack's start time. Never pass negative ticks
  // to the native clip sampler (which would select the wrong loop frame).
  state.master.phase=Math.max(0,state.master.phase);
  state.enemy.phase=Math.max(0,state.enemy.phase);
  return state;
}
