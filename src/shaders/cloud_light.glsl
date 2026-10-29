struct CloudLightSample { float edge; float halo; float streak; float echo; };
struct CloudLightOpening { float centre; float width; float power; float offset; float reach; };
#define CLOUD_INLINE
#define CLOUD_CLAMP clamp
#define CLOUD_MAX max
#define CLOUD_EXP exp
#define CLOUD_SIN sin
#define CLOUD_FLOAT float
#define CLOUD_UINT_TYPE uint
#define CLOUD_UINT uint
#define CLOUD_OPENING CloudLightOpening
#define CLOUD_SAMPLE CloudLightSample
#include "../render/cloud_light_math.inc"
#undef CLOUD_INLINE
#undef CLOUD_CLAMP
#undef CLOUD_MAX
#undef CLOUD_EXP
#undef CLOUD_SIN
#undef CLOUD_FLOAT
#undef CLOUD_UINT_TYPE
#undef CLOUD_UINT
#undef CLOUD_OPENING
#undef CLOUD_SAMPLE
// Read the uniform array directly. Passing it by value to this helper creates
// a per-fragment private array in SPIR-V and is costly on integrated GPUs.
// Six vec4s per region, matching DioramaCloudLightRegion. This flattened
// layout also keeps the native/WASM uniform bridge explicit and verifiable.
vec3 cloud_lighting(vec3 original,vec2 uv,vec4 source,vec4 meta,vec4 background_dim) {
    vec2 world=uv*source.zw+source.xy;
    vec3 result=original;
    if(meta.y>0.0) {
        float dim=0.0;
        for(int i=0;i<4;++i) {
            if(float(i)>=meta.x)break;
            vec4 bounds=CLOUD_REGIONS[i*6];
            vec2 p=(world-bounds.xy)/bounds.zw;
            dim=max(dim,CloudLightDimming(p.x,p.y,background_dim[i]));
        }
        result*=1.0-dim;
    }
    for(int i=0;i<4;++i) {
        if(float(i)>=meta.x)break;
        int at=i*6;
        vec4 bounds=CLOUD_REGIONS[at],core=CLOUD_REGIONS[at+1],shape=CLOUD_REGIONS[at+2];
        vec4 motion=CLOUD_REGIONS[at+3],color=CLOUD_REGIONS[at+4],spill=CLOUD_REGIONS[at+5];
        vec2 p=(world-bounds.xy)/bounds.zw;
        if(any(lessThanEqual(p,vec2(0)))||any(greaterThanEqual(p,vec2(1))))continue;
        CloudLightSample s=CloudLightWeights(p.x,p.y,bounds.w/bounds.z,
            motion.x,motion.y,motion.z,int(motion.w),spill.w,shape.x,shape.y,shape.z,shape.w);
        float luma=dot(original,vec3(.2126,.7152,.0722));
        // Cloud material catches textured rim/halo light. Air scattering must
        // also remain visible over the warm horizon and dark gaps below it;
        // using the grey-cloud mask there erased the rays leaving each opening.
        float grey=1.0-smoothstep(.05,.18,original.r-original.b);
        float grain=.05+.95*smoothstep(.06,.34,luma);
        float halo=s.halo*core.y*(.62+.38*grain)*grey;
        float streak=s.streak*core.z;
        float rim=s.edge*core.x*grain*grey;
        float echo=s.echo*core.w*smoothstep(.34,.70,luma);
        result=mix(result,spill.rgb,1.0-exp(-halo*color.a));
        result=mix(result,mix(spill.rgb,color.rgb,.8),1.0-exp(-streak*color.a));
        result=mix(result,color.rgb,1.0-exp(-rim*color.a));
        result=mix(result,mix(spill.rgb,color.rgb,.65),1.0-exp(-echo*color.a));
    }
    return result;
}
