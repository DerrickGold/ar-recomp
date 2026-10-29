/* Shared C/GLSL optical approximation. The 40–42 degree primary spectrum is
 * broadened for fine waterfall spray and the low-resolution source artwork.
 * Emitter dimensions never enter these functions. */
RAINBOW_INLINE RAINBOW_VEC3 RainbowViewDirection(float x,float y,
    RAINBOW_VEC3 row0,RAINBOW_VEC3 row1,RAINBOW_VEC3 row3) {
  float ax=row0.x-x*row3.x,ay=row0.y-x*row3.y,az=row0.z-x*row3.z;
  float bx=row1.x-y*row3.x,by=row1.y-y*row3.y,bz=row1.z-y*row3.z;
  float dx=az*by-ay*bz,dy=ax*bz-az*bx,dz=ay*bx-ax*by;
  float length=RAINBOW_SQRT(dx*dx+dy*dy+dz*dz);
  if(length<.000001)return RAINBOW_V3(0,0,-1);
  return RAINBOW_V3(dx/length,dy/length,dz/length);
}
RAINBOW_INLINE float RainbowBand(float alignment,float centre) {
  float d=(alignment-centre)/.014;
  return d*d<16 ? RAINBOW_EXP(-d*d) : 0;
}
RAINBOW_INLINE RAINBOW_VEC3 RainbowSpectrum(float alignment) {
  return RAINBOW_V3(RainbowBand(alignment,.7431448),
      RainbowBand(alignment,.7547096),RainbowBand(alignment,.7660444));
}
RAINBOW_INLINE RAINBOW_VEC4 RainbowFinish(float r,float g,float b) {
  float peak=RAINBOW_MAX(r,RAINBOW_MAX(g,b));
  if(peak<.00001)return RAINBOW_V4(0,0,0,0);
  /* A little white scattered light keeps the spectral fragments in the mist
   * rather than producing opaque, saturated stripes. */
  return RAINBOW_V4(.16+.84*r/peak,.16+.84*g/peak,.16+.84*b/peak,
      .48*(1-RAINBOW_EXP(-peak)));
}
