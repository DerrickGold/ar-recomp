#version 450
layout(location=0) in vec4 v_color;
layout(location=1) in vec2 v_uv;
layout(location=0) out vec4 o_color;
layout(set=2,binding=0) uniform sampler2D u_texture;
layout(set=3,binding=0,std140) uniform Context {
    vec4 eye_radius, right_scale, up_scale, forward_time;
    vec4 sun_horizon_stage, player_brightness_waves, output_size;
    vec4 light_sources[13], light_targets[13];
    vec4 native_contact; // Waterline UV, artwork availability, captured detail flags.
};
const float PI=3.14159265359;
float hash(vec2 p) { return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453); }
float noise(vec2 p) {
    vec2 i=floor(p), f=fract(p); f=f*f*(3.0-2.0*f);
    return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),
               mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);
}
float fbm(vec2 p) {
    return .57*noise(p)+.28*noise(p*2.03+13.0)+.15*noise(p*4.11-7.0);
}
vec3 ray(vec2 uv) {
    return normalize(forward_time.xyz+(uv.x*2.0-1.0)*right_scale.xyz*right_scale.w+
                     (1.0-uv.y*2.0)*up_scale.xyz*up_scale.w);
}
vec3 sky(vec3 direction) {
    float elevation=max(0.0,direction.z);
    float altitude=1.0-exp(-elevation*5.5);
    vec3 deep=mix(vec3(.36,.54,.66),vec3(.22,.37,.57),altitude);
    vec3 daylight=mix(vec3(.63,.78,.86),vec3(.34,.54,.76),altitude);
    // Frame the opening with deeper blue, then return to the earlier vista's
    // daylight palette farther away. Angular distance also gives reflected
    // sky directions the same gradient as the visible sky.
    float alignment=clamp(dot(direction,ray(sun_horizon_stage.xy)),-1.0,1.0);
    float away=smoothstep(.007,.070,1.0-alignment);
    vec3 color=mix(deep,daylight,away);
    float opening=pow(max(0.0,alignment),18.0);
    return color+vec3(.83,.80,.89)*opening*.42;
}
vec3 lightAxis(int i) { return normalize(light_targets[i].xyz-light_sources[i].xyz); }
float lightLength(int i) { return length(light_targets[i].xyz-light_sources[i].xyz); }
float lightCone(vec3 point,int i) {
    vec3 relative=point-light_sources[i].xyz;
    float along=dot(relative,lightAxis(i));
    if(along<=0.0 || along>lightLength(i)*2.0) return 0.0;
    float width=max(.001,along*light_sources[i].w);
    float radius=length(relative-lightAxis(i)*along)/width;
    float fade=1.0-smoothstep(1.30,2.0,along/lightLength(i));
    return exp(-radius*radius*2.0)*(1.0-smoothstep(.80,1.15,radius))*fade;
}
float sphereHit(vec3 origin,vec3 direction,float radius) {
    float b=dot(origin,direction), c=dot(origin,origin)-radius*radius;
    float discriminant=b*b-c;
    return discriminant>=0.0 && b<0.0 ? -b-sqrt(discriminant) : -1.0;
}
bool coneInterval(vec3 origin,vec3 ray,int i,out float start,out float end) {
    vec3 axis=lightAxis(i), relative=origin-light_sources[i].xyz;
    float axialOrigin=dot(relative,axis), axialRay=dot(ray,axis);
    float extent=lightLength(i)*2.0;
    if(abs(axialRay)<.00001) {
        if(axialOrigin<0.0 || axialOrigin>extent) return false;
        start=0.0; end=10000.0;
    } else {
        float near=-axialOrigin/axialRay, far=(extent-axialOrigin)/axialRay;
        start=max(0.0,min(near,far)); end=max(near,far);
    }
    float slope=light_sources[i].w*1.15, cosineSquared=1.0/(1.0+slope*slope);
    float a=axialRay*axialRay-cosineSquared;
    float b=2.0*(axialRay*axialOrigin-cosineSquared*dot(ray,relative));
    float c=axialOrigin*axialOrigin-cosineSquared*dot(relative,relative);
    if(abs(a)<.00001) {
        if(abs(b)<.00001) { if(c<0.0) return false; }
        else if(b>0.0) start=max(start,-c/b);
        else end=min(end,-c/b);
    } else {
        float discriminant=b*b-4.0*a*c;
        if(discriminant<0.0) { if(a<0.0) return false; }
        else {
            float r0=(-b-sqrt(discriminant))/(2.0*a), r1=(-b+sqrt(discriminant))/(2.0*a);
            float near=min(r0,r1), far=max(r0,r1);
            if(a<0.0) { start=max(start,near); end=min(end,far); }
            else if(start<=near) end=min(end,near);
            else start=max(start,far);
        }
    }
    return end>start;
}
// Analytic derivatives of eight interacting swells and capillary ripples.
// Distant normals lose fine detail instead of aliasing into sparkling rows.
vec3 wave(vec2 p,float time,float distance) {
    vec2 gradient=vec2(0); float height=0.0;
    const vec2 directions[8]=vec2[8](vec2(.22,.98),vec2(-.66,.75),vec2(.87,.49),
        vec2(-.31,.95),vec2(.94,-.34),vec2(.52,.85),vec2(-.81,.59),vec2(.39,-.92));
    const float frequency[8]=float[8](3.8,6.3,10.1,17.4,27.7,43.0,67.0,101.0);
    const float amplitude[8]=float[8](.013,.009,.006,.0035,.0019,.0010,.0005,.00022);
    for(int i=0;i<8;i++) {
        float detail=1.0-smoothstep(3.0,9.0,distance)*float(i)/8.0;
        float phase=dot(p,directions[i])*frequency[i]-time*sqrt(frequency[i]*.72)+float(i)*2.4;
        float a=amplitude[i]*detail*mix(.16,1.0,1.0-smoothstep(3.5,6.0,distance));
        height+=a*sin(phase);
        gradient+=directions[i]*a*frequency[i]*cos(phase);
    }
    return vec3(gradient,height);
}
vec4 ocean(vec2 uv) {
    vec3 rd=ray(uv), origin=eye_radius.xyz+vec3(0,0,eye_radius.w);
    float radius=eye_radius.w*.9975;
    float b=dot(origin,rd), c=dot(origin,origin)-radius*radius, discriminant=b*b-c;
    // Cover the coarse globe's limb with this material's matching sky color.
    // Palace banks stay above this narrow horizon band.
    if(discriminant<0.0 || b>=0.0)
        return vec4(sky(rd),step(sun_horizon_stage.z-.02,uv.y));
    float distance=-b-sqrt(discriminant);
    vec3 p=origin+rd*distance;
    float time=forward_time.w;
    float waves=player_brightness_waves.w;
    for(int i=0;i<2;i++) {
        vec3 displacement=wave(p.xy,time,distance)*waves;
        float error=length(p)-radius-displacement.z;
        distance-=clamp(error/min(-.025,dot(rd,normalize(p))),-1.0,1.0);
        p=origin+rd*distance;
    }
    vec3 shape=wave(p.xy,time,distance)*waves;
    vec3 base=normalize(p), n=normalize(base-vec3(shape.xy,0));
    vec3 view=-rd;
    float fresnel=.025+.975*pow(1.0-clamp(dot(n,view),0.0,1.0),5.0);
    vec3 reflection=sky(reflect(rd,n));
    // A faint native pixel material lives under the full-resolution lighting.
    // World-anchored texels follow perspective; fade subpixel detail offshore.
    float pixelDetail=output_size.z*(1.0-smoothstep(1.2,4.0,distance));
    vec2 pixelPoint=(floor(p.xy*160.0)+.5)/160.0;
    vec2 materialPoint=mix(p.xy,pixelPoint,pixelDetail);
    float variation=fbm(materialPoint*8.0+vec2(time*.12,-time*.07));
    variation=mix(variation,floor(variation*12.0+.5)/12.0,pixelDetail*.55);
    vec3 deep=vec3(.026,.20,.36), shallow=vec3(.055,.40,.58);
    vec3 water=mix(deep,shallow,.25+.40*variation);
    float wavelet=0.0;
    if(output_size.w>.5 && pixelDetail>.001) {
        float clock=time*4.0*waves, frame=mod(floor(clock),4.0);
        vec2 tile=floor(fract(pixelPoint*20.0)*8.0);
        vec2 first=(vec2(frame*8.0,0)+tile+.5)/vec2(32,8);
        vec2 next=(vec2(mod(frame+1.0,4.0)*8.0,0)+tile+.5)/vec2(32,8);
        wavelet=mix(texture(u_texture,first).r,texture(u_texture,next).r,
            smoothstep(0.0,1.0,fract(clock)));
        water*=1.0+(wavelet-.28)*.16*pixelDetail;
    }
    // Hint at the SNES's five-bit colour steps in albedo only. Fresnel,
    // displaced wave normals and specular light remain continuous.
    water=mix(water,floor(water*31.0+.5)/31.0,pixelDetail*.25);
    vec3 color=mix(water,reflection,fresnel*.76);
    float ribbon=pow(max(0.0,sin(materialPoint.y*21.0+shape.x*19.0-time*.8)),6.0);
    float sparkle=pow(max(0.0,sin(materialPoint.y*92.0+time*3.2)*
        sin(materialPoint.x*73.0-time*2.1)),12.0);
    // Daylight illuminates the entire sea even where the shafts end in air.
    // The opening's sky direction drives normal-dependent specular glints.
    vec3 daylightSun=ray(sun_horizon_stage.xy), daylightHalf=normalize(daylightSun+view);
    float daylightFacing=max(0.0,dot(n,daylightSun));
    float daylightSpec=pow(max(0.0,dot(n,daylightHalf)),180.0);
    float daylightBroad=pow(max(0.0,dot(n,daylightHalf)),24.0);
    color+=vec3(.94,.96,1.0)*daylightFacing*(.065+daylightSpec*2.4+
        daylightBroad*(.20+waves*(ribbon*.18+sparkle*.35)));
    // Shafts add local pools at their actual ocean intersections.
    for(int i=0;i<13;i++) {
        vec3 sun=normalize(light_sources[i].xyz-p), halfvector=normalize(sun+view);
        float spec=pow(max(0.0,dot(n,halfvector)),180.0);
        float broad=pow(max(0.0,dot(n,halfvector)),22.0);
        float illumination=lightCone(p,i)*max(0.0,dot(n,sun))*light_targets[i].w;
        color+=vec3(.98,.94,1.0)*illumination*(.08+spec*2.7+broad*.35+
            waves*(ribbon*(.14+.32*variation)+sparkle*.45));
    }
    float crest=smoothstep(.017,.031,shape.z)*(.2+.8*variation);
    float foam=crest*(1.0-smoothstep(1.5,5.0,distance));
    color=mix(color,vec3(.70,.85,.91),foam*.48);
    // Perspective haze joins the water to the luminous horizon.
    color=mix(color,vec3(.36,.54,.69),smoothstep(2.0,7.0,distance)*.38);
    if((int(native_contact.w)&16)!=0) {
        vec3 contactRay=ray(native_contact.xy);
        float contactDistance=sphereHit(origin,contactRay,radius);
        if(contactDistance>0.0) {
            vec2 delta=p.xy-(origin+contactRay*contactDistance).xy;
            float r=length(delta)+shape.z*.25;
            // Weak expanding rings break up in the same wave field as the sea.
            float ripples=0.0;
            for(int ring=0;ring<2;ring++) {
                float age=fract(time*.16*waves+float(ring)*.5);
                float crest=exp(-pow((r-(.11+age*.18))/.008,2.0));
                ripples+=crest*sin(age*PI)*(0.40+0.60*noise(delta*45+time*.2));
            }
            color*=1.0-.06*exp(-r*r/.012);
            color+=vec3(.34,.63,.82)*ripples*.09;
        }
    }
    return vec4(color,1);
}
vec4 shafts(vec2 uv) {
    if((int(native_contact.w)&64)==0) return vec4(0,0,0,1);
    vec3 origin=eye_radius.xyz+vec3(0,0,eye_radius.w), rd=ray(uv);
    float water=sphereHit(origin,rd,eye_radius.w*.9975);
    vec3 radiance=vec3(0);
    for(int light=0;light<13;light++) {
        // Sample the exact cone interval, ending at the receiving ocean.
        float start,end;
        if(!coneInterval(origin,rd,light,start,end)) continue;
        if(water>0.0) end=min(end,water);
        if(end<=start) continue;
        float step=(end-start)/12.0, scattering=0.0;
        vec3 scatteredColor=vec3(0);
        for(int pointIndex=0;pointIndex<12;pointIndex++) {
            float distance=start+(float(pointIndex)+.5)*step;
            vec3 point=origin+rd*distance;
            float cone=lightCone(point,light);
            // World-space wisps and view-distance extinction give each cone
            // a luminous core, soft irregular edges and atmospheric depth.
            float structure=.6*noise(vec2(dot(point,vec3(.4,.7,.1))*5.0,
                point.z*11.0-forward_time.w*.035))+
                .4*noise(point.xy*8.0+vec2(forward_time.w*.016,point.z*3.0));
            float dust=mix(.40,1.0,smoothstep(.18,.83,structure));
            float density=cone*dust*exp(-distance*.16);
            vec3 tint=mix(vec3(.99,.94,1.0),vec3(.69,.82,1.0),
                smoothstep(.95,4.5,distance));
            scattering+=density;
            scatteredColor+=tint*density;
        }
        float optical=scattering*step*.70/(lightLength(light)*light_sources[light].w);
        radiance+=scatteredColor/max(.00001,scattering)*
            (1.0-exp(-optical))*light_targets[light].w*.70;
    }
    // Broad light from the opening fills the scene between the short shafts,
    // including the native hero/platform composed underneath this pass.
    float flood=pow(max(0.0,dot(rd,ray(sun_horizon_stage.xy))),6.0)*.10;
    radiance+=vec3(1.0,.97,1.0)*flood;
    // Ease additive glare around the captured hero's torso to reveal armour detail.
    vec2 heroOffset=(uv-player_brightness_waves.xy)*output_size.xy/output_size.y;
    float heroGlare=1.0-smoothstep(.35,1.2,length(heroOffset/vec2(.055,.09)));
    return vec4(min(vec3(.40),radiance)*(1.0-.18*heroGlare)*player_brightness_waves.z,1);
}
vec4 nativeReflection(vec2 uv) {
    if(native_contact.z<.5 || (int(native_contact.w)&16)==0 || uv.y<native_contact.y)
        return vec4(0);
    float depth=uv.y-native_contact.y, time=forward_time.w;
    float strip=floor(uv.y*output_size.y*.5);
    float motion=time*player_brightness_waves.w;
    vec2 sampleUv=vec2(uv.x+sin(strip*.71+motion*1.7)*.004,
        native_contact.y-depth/.28+sin(strip*.23-motion)*.002);
    if(any(lessThan(sampleUv,vec2(0))) || any(greaterThan(sampleUv,vec2(1)))) return vec4(0);
    vec4 art=texture(u_texture,sampleUv);
    float broken=.22+.78*smoothstep(-.45,.65,sin(strip*.56-motion*1.3));
    float fade=exp(-depth*15.0)*(1.0-smoothstep(.10,.17,depth));
    return vec4(art.rgb/max(.001,art.a)*vec3(.48,.65,.86)*player_brightness_waves.z,
        art.a*broken*fade*.22);
}
vec4 nativeRim(vec2 uv) {
    if(native_contact.z<.5 || (int(native_contact.w)&32)==0) return vec4(0);
    vec2 pixel=1.5/output_size.xy;
    float a=texture(u_texture,uv).a;
    float upper=texture(u_texture,uv-vec2(0,pixel.y)).a;
    float left=texture(u_texture,uv-vec2(pixel.x,0)).a;
    float right=texture(u_texture,uv+vec2(pixel.x,0)).a;
    float edge=clamp(a-upper+.20*(2.0*a-left-right),0.0,a);
    return vec4(vec3(1.0,.65,.22)*edge*.18*player_brightness_waves.z,1);
}
void main() {
    vec2 uv=v_uv; float stage=sun_horizon_stage.w;
    vec4 result;
    if(stage<.5) {
        vec3 color=sky(ray(uv));
        // A large luminous opening, carved and rim-lit by the Palace cloud
        // volumes. Its lower rim emits the fan of shafts onto the hero/sea.
        vec2 d=(uv-vec2(.5,-.055))/vec2(.36,.30);
        float radius=length(d)+.045*(fbm(uv*17.0)-.5);
        float opening=1.0-smoothstep(.72,1.04,radius);
        vec3 radiance=mix(vec3(1.0,.96,1.0),vec3(.83,.77,.90),smoothstep(.30,.95,radius));
        color=mix(color,radiance,opening);
        color+=vec3(.65,.62,.73)*.22*exp(-pow(max(0.0,radius-.80)*1.2,2.0));
        result=vec4(color,1);
    } else if(stage<1.5) result=ocean(uv);
    else if(stage<2.5) result=shafts(uv);
    else if(stage<3.5) result=nativeReflection(uv);
    else result=nativeRim(uv);
    o_color=result*v_color;
}
