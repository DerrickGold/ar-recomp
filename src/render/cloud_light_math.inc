/* Shared C/GLSL cloud transmission. Coordinates are normalized to an authored
 * rectangle. One field drives the rim, air streaks and lower cloud reflection;
 * camera position never starts an animation or changes an opening's identity. */
CLOUD_INLINE float CloudLightSmooth(float a, float b, float x) {
  float t=CLOUD_CLAMP((x-a)/(b-a),0.0,1.0);
  return t*t*(3.0-2.0*t);
}
CLOUD_INLINE float CloudLightGauss(float x) { return CLOUD_EXP(-x*x); }
/* A fixed world-space feather, independent of cloud motion or camera. Dimming
 * is applied before illumination and overlaps merge by their strongest value. */
CLOUD_INLINE float CloudLightDimming(float u,float v,float amount) {
  return amount*CloudLightSmooth(0.0,.1,u)*(1.0-CloudLightSmooth(.9,1.0,u))*
      CloudLightSmooth(0.0,.1,v)*(1.0-CloudLightSmooth(.9,1.0,v));
}
CLOUD_INLINE CLOUD_UINT_TYPE CloudLightHash(CLOUD_UINT_TYPE value) {
  value^=value>>16;
  value*=0x7feb352du;
  value^=value>>15;
  value*=0x846ca68bu;
  return value^(value>>16);
}
/* Independent positions allow clusters and quiet gaps instead of a jittered
 * grid. Adding an opening leaves the existing ones in place. Integer hashing
 * keeps the layout identical on native GPUs, the CPU and WebGL. */
CLOUD_INLINE CloudLightOpening CloudLightOpeningAt(CLOUD_UINT_TYPE seed,int index) {
  CLOUD_UINT_TYPE a=CloudLightHash(seed+CLOUD_UINT(index)*0x9e3779b9u);
  CLOUD_UINT_TYPE b=CloudLightHash(a+0x68bc21ebu);
  float position=CLOUD_FLOAT(a&65535u)/65535.0;
  float size=CLOUD_FLOAT(a>>16)/65535.0;
  float power=CLOUD_FLOAT(b&65535u)/65535.0;
  float height=CLOUD_FLOAT(b>>16)/65535.0;
  return CLOUD_OPENING(.13+.74*position,.018+.058*size,
      .38+.82*power,(height-.5)*.06,.42+.24*size);
}
CLOUD_INLINE float CloudLightEdge(float u,float seed,float edge_position) {
  float offset=CLOUD_FLOAT(CLOUD_UINT(seed)&1023u)*.006135923;
  return edge_position+.018*CLOUD_SIN(u*31.0+offset)+.008*CLOUD_SIN(u*77.0+offset*2.1);
}
/* Start as wide as the luminous cloud opening, then spread continuously.
 * The editor uses the same envelope for its source/landing-width guides. */
CLOUD_INLINE float CloudLightBeamWidth(float width,float depth) {
  return width*(1.45+CLOUD_MAX(0.0,depth)*2.4);
}
CLOUD_INLINE CloudLightSample CloudLightWeights(float u,float v,float aspect,
    float slope,float phase,float seed,int strands,float softness,
    float falloff,float cover,float edge_position,float echo_position) {
  CloudLightSample out_sample=CLOUD_SAMPLE(0.0,0.0,0.0,0.0);
  if(u<=0.0||u>=1.0||v<=0.0||v>=1.0)return out_sample;
  float border=CloudLightSmooth(0.0,.13,u)*(1.0-CloudLightSmooth(.87,1.0,u));
  float edge=CloudLightEdge(u,seed,edge_position);
  float rim=.004+softness*.009;
  float seed_phase=CLOUD_FLOAT(CLOUD_UINT(seed)&1023u)*.006135923;
  for(int i=0;i<5;++i) {
    if(i>=strands)break;
    CloudLightOpening opening=CloudLightOpeningAt(CLOUD_UINT(seed),i);
    float centre=opening.centre,width=opening.width;
    float dy=v-edge-opening.offset;
    // A drifting, smooth cloud veil breaks each opening differently. This
    // same transmission follows its ray and reflected patch, without pulsing
    // the entire scene in unison or moving the light source with the camera.
    float veil=.5+.5*CLOUD_SIN(phase+centre*9.0+seed_phase);
    veil*=.65+.35*CLOUD_SIN(phase*2.0-centre*13.0+seed_phase);
    float transmission=opening.power*(1.0-cover*veil);
    float coverage=transmission*CloudLightGauss((u-centre)/(width*1.45));
    out_sample.edge+=coverage*CloudLightGauss(dy/rim);
    out_sample.halo+=coverage*CloudLightGauss((dy-.025)/(.035+softness*.025));
    // A continuous shaft fills the opening and spreads into the sky. Keep
    // its axis fixed while the source fade follows the wavy cloud contact;
    // moving the axis with the edge bends the shaft as it travels sideways.
    float ray_dy=v-CloudLightEdge(centre,seed,edge_position)-opening.offset;
    float origin=u-ray_dy*slope*aspect;
    float across=(origin-centre)/CloudLightBeamWidth(width,ray_dy);
    out_sample.streak+=transmission*CloudLightGauss(across)*
        CloudLightSmooth(-.01,.035,dy)*
        CLOUD_EXP(-CLOUD_MAX(0.0,ray_dy)/(opening.reach*falloff))*
        (1.0-CloudLightSmooth(opening.reach*.7,opening.reach,ray_dy));
    float reflection=centre+(echo_position-edge_position-opening.offset)*slope*aspect*.25;
    out_sample.echo+=transmission*CloudLightGauss((u-reflection)/(width*2.0));
  }
  out_sample.edge*=border;
  out_sample.halo*=border;
  out_sample.streak*=border;
  out_sample.echo*=border*CloudLightGauss((v-echo_position)/.09)*
      CloudLightSmooth(.0,.08,v)*(1.0-CloudLightSmooth(.92,1.0,v));
  return out_sample;
}
