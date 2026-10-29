#include "render/cloud_material.h"
#include "render/cloud_noise.h"

const CloudMaskLight kCloudBillowLight={-.60f,.20f,.18f,.16f,.06f,.94f,.85f};

void CloudMaterial_Billows(unsigned variant,CloudVolumeMaterial *out) {
  *out=(CloudVolumeMaterial){.lobe_count=14,.density=kCloudVolumeDensity_Billow,
    .shape_floor=-1,.shape_cutoff=-.20f,.noise_bias=.48f,.noise_gain=1.25f,
    .ramp_start=.05f,.ramp_end=.55f,.feather={.06f,.06f,.10f}};
  for(unsigned i=0;i<out->lobe_count;++i) {
    const uint32_t seed=(variant+1)*0x91e10da5u^(i+1)*0x9e3779b9u;
    if(i<7) {
      const bool crown=i>=3;
      out->lobes[i]=(CloudVolumeLobe){.x=crown?.24f+(i-3)*.17f:.20f+i*.30f,
        .y=(crown?.34f:.61f)+.13f*CloudNoise_Seed(seed),.z=.5f,
        .rx=(crown?.15f:.20f)+.06f*CloudNoise_Seed(seed^11),
        .ry=(crown?.21f:.25f)+.10f*CloudNoise_Seed(seed^37),
        .rz=(.50f+.50f*CloudNoise_Seed(seed^103))*.35f};
    } else {
      out->lobes[i]=(CloudVolumeLobe){.x=.14f+(i-7)*.12f,
        .y=.36f+.24f*CloudNoise_Seed(seed),.z=.5f,
        .rx=.09f+.05f*CloudNoise_Seed(seed^11),.ry=.12f+.08f*CloudNoise_Seed(seed^37),
        .rz=(.70f+.40f*CloudNoise_Seed(seed^103))*.35f};
    }
  }
  const uint32_t seed=(variant+1)*0x91e10da5u;
  out->noise[0]=(CloudVolumeOctave){.scale={7,7,7},.weight=.60f,.seed=seed};
  out->noise[1]=(CloudVolumeOctave){.scale={14,14,14},.weight=.27f,.seed=seed^71};
  out->noise[2]=(CloudVolumeOctave){.scale={28,28,28},.weight=.13f,.seed=seed^193};
}

void CloudMaterial_Crown(unsigned variant,CloudVolumeMaterial *out) {
  static const float shapes[4][6][4]={
    {{.15f,.65f,.13f,.16f},{.28f,.53f,.18f,.28f},{.44f,.42f,.20f,.34f},
     {.61f,.50f,.18f,.27f},{.76f,.60f,.15f,.19f},{.87f,.68f,.10f,.11f}},
    {{.13f,.68f,.10f,.12f},{.25f,.60f,.17f,.20f},{.41f,.53f,.20f,.28f},
     {.59f,.39f,.19f,.34f},{.73f,.54f,.17f,.24f},{.87f,.65f,.10f,.15f}},
    {{.13f,.64f,.10f,.14f},{.27f,.50f,.18f,.27f},{.45f,.48f,.20f,.30f},
     {.63f,.55f,.17f,.23f},{.77f,.51f,.14f,.22f},{.88f,.66f,.09f,.12f}},
    {{.13f,.68f,.11f,.12f},{.28f,.57f,.18f,.21f},{.44f,.36f,.18f,.31f},
     {.60f,.43f,.20f,.32f},{.75f,.59f,.16f,.18f},{.87f,.67f,.10f,.12f}},
  };
  variant%=4;
  *out=(CloudVolumeMaterial){.lobe_count=6,.density=kCloudVolumeDensity_Crown,
    .noise_gain=.65f,.density_gain=2.4f,.floor_start=.67f,.floor_fade=.20f};
  for(unsigned i=0;i<out->lobe_count;++i) {
    const float *p=shapes[variant][i];
    out->lobes[i]=(CloudVolumeLobe){p[0],p[1],.46f+((i+variant)%3)*.06f,p[2],p[3],p[2]*1.8f};
  }
  out->noise[0]=(CloudVolumeOctave){.scale={19,9,9},.offset={variant*3,0,0},.weight=.55f};
  out->noise[1]=(CloudVolumeOctave){.scale={43,23,21},.offset={0,variant*5,0},.weight=.18f};
}

static float Octave(const CloudVolumeMaterial *m,unsigned i,float x,float y,float z) {
  const CloudVolumeOctave *n=&m->noise[i];
  if(n->weight==0)return 0;
  x=x*n->scale[0]+n->offset[0];y=y*n->scale[1]+n->offset[1];z=z*n->scale[2]+n->offset[2];
  return n->weight*(m->density==kCloudVolumeDensity_Billow?
      CloudNoise_Seeded3(x,y,z,n->seed):CloudNoise_Weighted3(x,y,z));
}
float CloudMaterial_Density(const CloudVolumeMaterial *m,float x,float y,float z) {
  if(m->density==kCloudVolumeDensity_Crown&&
      (x<=0||x>=1||y<=0||y>=1||z<=0||z>=1))return 0;
  float shape=m->shape_floor;
  for(unsigned i=0;i<m->lobe_count;++i) {
    const CloudVolumeLobe *p=&m->lobes[i];
    const float nx=(x-p->x)/p->rx,ny=(y-p->y)/p->ry,nz=(z-p->z)/p->rz;
    shape=fmaxf(shape,1-nx*nx-ny*ny-nz*nz);
  }
  if(shape<=m->shape_cutoff)return 0;
  const float n=Octave(m,0,x,y,z)+Octave(m,1,x,y,z)+Octave(m,2,x,y,z);
  if(m->density==kCloudVolumeDensity_Crown)
    return CloudNoise_Ramp(0,1,(shape-n*m->noise_gain)*m->density_gain)*
        (1-CloudNoise_Ramp(0,1,(y-m->floor_start)/m->floor_fade));
  float value=CloudNoise_Ramp(m->ramp_start,m->ramp_end,shape+(n-m->noise_bias)*m->noise_gain);
  value*=CloudNoise_Ramp(0,m->feather[0],x)*CloudNoise_Ramp(0,m->feather[0],1-x)*
      CloudNoise_Ramp(0,m->feather[1],y)*CloudNoise_Ramp(0,m->feather[2],1-y);
  return value;
}

float CloudMaterial_Wisps(float u,float v,uint32_t seed) {
  const float x=u*2-1,y=v*2-1;
  const float edge=CloudNoise_Ramp(0,.22f,1-x*x)*CloudNoise_Ramp(0,.32f,1-y*y);
  if(edge==0)return 0;
  const float wx=u*4+CloudNoise_Seeded2(u*3,v*3,seed)*1.5f;
  const float wy=v*5+CloudNoise_Seeded2(u*3+9,v*3,seed^71)*1.3f;
  const float n=.53f*CloudNoise_Seeded2(wx,wy,seed)+.29f*CloudNoise_Seeded2(wx*2.1f,wy*2.1f,seed^31)+
      .18f*CloudNoise_Seeded2(wx*4.3f,wy*4.3f,seed^97);
  return edge*CloudNoise_Ramp(.25f,.71f,n)*(.65f+.35f*CloudNoise_Seeded2(wx*7,wy*7,seed^57));
}
uint32_t CloudMaterial_SheetTexel(int x,int y,int size,unsigned octaves) {
  float amplitude=.5f,total=0,sum=0;
  int period=4;
  for(unsigned octave=0;octave<octaves;++octave) {
    const float scale=(float)period/(float)(size-1);
    const float ox=.37f+(float)octave*.53f,oy=.61f+(float)octave*.29f;
    total+=CloudNoise_Periodic2((float)x*scale+ox,(float)y*scale+oy,period)*amplitude;
    sum+=amplitude;amplitude*=.5f;period*=2;
  }
  return CloudNoise_SheetColor(total/sum);
}

/* Called once at the public bake boundary, never per density probe. */
static bool ValidMaterial(const CloudVolumeMaterial *m) {
  if(!m||!m->lobe_count||m->lobe_count>kCloudVolumeMaxLobes||
      (m->density!=kCloudVolumeDensity_Billow&&m->density!=kCloudVolumeDensity_Crown))return false;
  for(unsigned i=0;i<m->lobe_count;++i) {
    const CloudVolumeLobe *p=&m->lobes[i];
    if(!isfinite(p->x)||!isfinite(p->y)||!isfinite(p->z)||!isfinite(p->rx)||
        !isfinite(p->ry)||!isfinite(p->rz)||p->rx<=0||p->ry<=0||p->rz<=0)return false;
  }
  for(unsigned i=0;i<3;++i) {
    const CloudVolumeOctave *n=&m->noise[i];
    if(!isfinite(n->weight))return false;
    for(unsigned j=0;j<3;++j)
      if(!isfinite(n->scale[j])||fabsf(n->scale[j])>1024||
          !isfinite(n->offset[j])||fabsf(n->offset[j])>1048576)return false;
  }
  if(!isfinite(m->shape_floor)||!isfinite(m->shape_cutoff)||!isfinite(m->noise_bias)||
      !isfinite(m->noise_gain)||!isfinite(m->ramp_start)||!isfinite(m->ramp_end)||!isfinite(m->density_gain))return false;
  if(m->density==kCloudVolumeDensity_Crown)
    return isfinite(m->floor_start)&&isfinite(m->floor_fade)&&m->floor_fade>0;
  for(unsigned i=0;i<3;++i)if(!isfinite(m->feather[i])||m->feather[i]<=0)return false;
  return m->ramp_end>m->ramp_start;
}
enum { kGridLayer=kCloudVolumeGrid*kCloudVolumeGrid };
static float LayerSample(const float *field,float x,float y,unsigned layer) {
  if(x<0||y<0||x>kCloudVolumeGrid-1||y>kCloudVolumeGrid-1)return 0;
  const unsigned ix=(unsigned)x,iy=(unsigned)y;
  const unsigned jx=ix+1<kCloudVolumeGrid?ix+1:ix,jy=iy+1<kCloudVolumeGrid?iy+1:iy;
  const float u=x-ix,v=y-iy;
  const float *p=field+layer*kGridLayer;
  const float a=p[iy*kCloudVolumeGrid+ix]*(1-u)+p[iy*kCloudVolumeGrid+jx]*u;
  const float b=p[jy*kCloudVolumeGrid+ix]*(1-u)+p[jy*kCloudVolumeGrid+jx]*u;
  return a*(1-v)+b*v;
}
bool CloudMaterial_BakeMasks(const CloudVolumeMaterial *m,const CloudMaskLight *light,
    CloudVolumeScratch *scratch,uint8_t *body,uint8_t *lit,int pitch,int width,int height) {
  if(!ValidMaterial(m)||!light||!scratch||!body||!lit||width<2||height<2||
      width>4096||height>4096||pitch<width||(size_t)pitch>SIZE_MAX/(size_t)height||
      !isfinite(light->shear_x)||!isfinite(light->shear_z)||!isfinite(light->depth_step)||
      !isfinite(light->opacity_step)||!isfinite(light->ambient)||!isfinite(light->diffuse)||
      !isfinite(light->extinction)||light->depth_step<0||light->opacity_step<0||light->extinction<0||
      light->ambient<0||light->diffuse<0||light->ambient+light->diffuse>1)return false;
  float *density=scratch->density,*optical=scratch->optical;
  for(unsigned z=0;z<kCloudVolumeGrid;++z)for(unsigned y=0;y<kCloudVolumeGrid;++y)
    for(unsigned x=0;x<kCloudVolumeGrid;++x)
      density[z*kGridLayer+y*kCloudVolumeGrid+x]=CloudMaterial_Density(m,
          x/(kCloudVolumeGrid-1.f),y/(kCloudVolumeGrid-1.f),z/(kCloudVolumeGrid-1.f));
  /* Optical depth is Y-major; each slice only reads its predecessor. */
  for(unsigned y=0;y<kCloudVolumeGrid;++y)for(unsigned z=0;z<kCloudVolumeGrid;++z)
    for(unsigned x=0;x<kCloudVolumeGrid;++x) {
      const float previous=y?LayerSample(optical,x+light->shear_x,z+light->shear_z,y-1):0;
      optical[y*kGridLayer+z*kCloudVolumeGrid+x]=previous+
          density[z*kGridLayer+y*kCloudVolumeGrid+x]*light->depth_step;
    }
  for(unsigned i=0;i<kCloudVolumeVoxels;++i) {
    density[i]=1-expf(-density[i]*light->opacity_step);
    optical[i]=light->ambient+light->diffuse*expf(-optical[i]*light->extinction);
  }
  for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
    const float u=x*(kCloudVolumeGrid-1.f)/(width-1),v=y*(kCloudVolumeGrid-1.f)/(height-1);
    const unsigned iy=(unsigned)v,jy=iy+1<kCloudVolumeGrid?iy+1:iy;
    const float t=v-iy;
    float transmission=1,radiance=0;
    for(unsigned z=0;z<kCloudVolumeGrid;++z) {
      const float alpha=LayerSample(density,u,v,z);
      const float illumination=LayerSample(optical,u,z,iy)*(1-t)+LayerSample(optical,u,z,jy)*t;
      radiance+=transmission*alpha*illumination;
      transmission*=1-alpha;
    }
    const float opacity=1-transmission;
    const float base=(opacity-radiance)/fmaxf(.001f,1-radiance);
    body[(size_t)y*pitch+x]=(uint8_t)floorf(fmaxf(0,base)*255+.5f);
    lit[(size_t)y*pitch+x]=(uint8_t)floorf(radiance*255+.5f);
  }
  return true;
}

static size_t SliceTexel(CloudSliceLayout l,int slice,int x,int y,int pitch) {
  return (size_t)(slice/l.columns*l.height+y)*pitch+slice%l.columns*l.width+x;
}
bool CloudMaterial_BakeSlices(const CloudVolumeMaterial *m,const CloudSliceLight *light,
    CloudSliceLayout l,uint32_t *out,int pitch) {
  if(!ValidMaterial(m)||!light||!out||l.width<3||l.height<3||l.width>4096||l.height>4096||
      l.slices<1||l.slices>128||l.columns<1||l.columns>129||pitch<l.width*l.columns||
      !isfinite(light->step)||!isfinite(light->extinction)||!isfinite(light->optical_step)||
      light->step<0||light->step>1||light->extinction<0||light->optical_step<0||light->probes>64)return false;
  const int rows=(l.slices+1+l.columns-1)/l.columns;
  if((size_t)pitch>SIZE_MAX/sizeof(*out)/(rows*l.height))return false;
  for(unsigned i=0;i<3;++i)
    if(!isfinite(light->direction[i])||!isfinite(light->ambient[i])||!isfinite(light->diffuse[i])||
        light->ambient[i]<0||light->diffuse[i]<0||light->ambient[i]+light->diffuse[i]>1)return false;
  const float length=hypotf(hypotf(light->direction[0],light->direction[1]),light->direction[2]);
  if(!isfinite(length)||(light->enabled&&length<.0001f))return false;
  const float step[3]={light->enabled?light->direction[0]/length*light->step:0,
      light->enabled?light->direction[1]/length*light->step:0,
      light->enabled?light->direction[2]/length*light->step:0};
  for(int y=0;y<rows*l.height;++y)for(int x=0;x<l.width*l.columns;++x)
    out[(size_t)y*pitch+x]=0x00ffffffu;
  for(int y=1;y<l.height-1;++y)for(int x=1;x<l.width-1;++x) {
    const float u=(x-.5f)/(l.width-2),v=(y-.5f)/(l.height-2);
    float accumulated[4]={0};
    for(int z=l.slices-1;z>=0;--z) {
      const float w=(z+.5f)/l.slices,density=CloudMaterial_Density(m,u,v,w);
      if(density<=0)continue;
      const float alpha=1-expf(-density*light->extinction/l.slices);
      float optical_depth=0;
      if(light->enabled)for(unsigned sample=1;sample<=light->probes;++sample)
        optical_depth+=CloudMaterial_Density(m,u+step[0]*sample,v+step[1]*sample,w+step[2]*sample)*light->optical_step;
      const float sun=expf(-optical_depth);
      float rgb[3];
      for(unsigned c=0;c<3;++c)rgb[c]=light->enabled?light->ambient[c]+light->diffuse[c]*sun:1;
      const unsigned a=(unsigned)(alpha*255+.5f);
      if(a)out[SliceTexel(l,z,x,y,pitch)]=(a<<24)|((unsigned)(rgb[0]*255+.5f)<<16)|
          ((unsigned)(rgb[1]*255+.5f)<<8)|(unsigned)(rgb[2]*255+.5f);
      for(unsigned c=0;c<3;++c)accumulated[c]=rgb[c]*alpha+accumulated[c]*(1-alpha);
      accumulated[3]=alpha+accumulated[3]*(1-alpha);
    }
    if(accumulated[3]*255>=.5f) {
      const float inverse=1/accumulated[3];
      out[SliceTexel(l,l.slices,x,y,pitch)]=((unsigned)(accumulated[3]*255+.5f)<<24)|
          ((unsigned)(fminf(1,fmaxf(0,accumulated[0]*inverse))*255+.5f)<<16)|
          ((unsigned)(fminf(1,fmaxf(0,accumulated[1]*inverse))*255+.5f)<<8)|
          (unsigned)(fminf(1,fmaxf(0,accumulated[2]*inverse))*255+.5f);
    }
  }
  return true;
}
bool CloudMaterial_Bounds(const uint32_t *pixels,int pitch,int width,int height,CloudTexelBounds *bounds) {
  if(!pixels||!bounds||width<1||height<1||pitch<width||
      (size_t)pitch>SIZE_MAX/sizeof(*pixels)/(size_t)height)return false;
  int x0=width,y0=height,x1=-1,y1=-1;
  for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
    if(!(pixels[(size_t)y*pitch+x]>>24))continue;
    if(x<x0)x0=x;if(x>x1)x1=x;if(y<y0)y0=y;if(y>y1)y1=y;
  }
  *bounds=x1<0?(CloudTexelBounds){0}:(CloudTexelBounds){x0>0?x0-1:0,y0>0?y0-1:0,
      x1+1<width?x1+1:width-1,y1+1<height?y1+1:height-1};
  return true;
}
