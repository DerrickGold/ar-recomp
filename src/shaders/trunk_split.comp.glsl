#version 450
layout(local_size_x=8,local_size_y=8,local_size_z=1) in;
layout(set=0,binding=0) uniform sampler2D nativeLow;
layout(set=0,binding=1) uniform sampler2D nativeHigh;
layout(set=0,binding=2) uniform sampler2D nativeFar;
layout(std430,set=0,binding=3) readonly buffer Art {uint data[];} art;
layout(rgba8,set=1,binding=0) writeonly uniform image2D lowOut;
layout(rgba8,set=1,binding=1) writeonly uniform image2D highOut;
layout(rgba8,set=1,binding=2) writeonly uniform image2D farOut;
layout(std140,set=2,binding=0) uniform Context {ivec4 capture;ivec4 asset;vec4 fade;} settings;
uint byteAt(int base,int index) {return (art.data[base+(index>>2)]>>((index&3)*8))&255u;}
void main() {
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);
  if(p.x>=640||p.y>=352)return;
  vec4 low=texelFetch(nativeLow,p,0),high=vec4(0),far=vec4(0);
  vec4 above=texelFetch(nativeHigh,p,0),behind=texelFetch(nativeFar,p,0);
  if(low.a==0)low=behind;
  if(above.a>0)low=above;
  int wx=p.x+settings.capture.z,y=p.y+settings.capture.w;
  if(p.y<settings.capture.y&&y>=0&&y<settings.asset.x) {
    if(wx>=320&&wx<704) {
      int at=y*48+((wx-320)>>3);uint bit=1u<<uint(wx&7);
      if((byteAt(settings.asset.z,at)&bit)!=0u){high=low;low=vec4(0);}
      if((byteAt(settings.asset.y,at)&bit)!=0u)low=high=vec4(0);
    }
    int x=wx-352;
    if(x>=0&&x<320) {
      uint color=art.data[settings.asset.w+(y&63)*320+x];
      far=vec4(vec3((color>>16)&255u,(color>>8)&255u,color&255u)*settings.fade.x,float(color>>24))/255.0;
    }
  }
  imageStore(lowOut,p,low);imageStore(highOut,p,high);imageStore(farOut,p,far);
}
