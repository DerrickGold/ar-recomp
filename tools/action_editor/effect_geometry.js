/* Static, unoccluded map footprints. Native ray parameters come from the C
 * recipe; sway, soft edges, particles and terrain clipping stay in preview. */
const EffectGeometry=(()=>{
  const rad=Math.PI/180;
  const isRay=e=>e.guide?.[0]>0||e.kind==='light-fan';
  function origin(e){return {x:e.x,y:e.y+(e.guide?.[0]===2?e.guide[4]*(e.scaleY||1):0)};}
  function slope(e) {
    const base=e.guide?.[1]||0,angle=(e.angle||0)*rad;
    return (e.guide?.[0]===1?-Math.tan(Math.atan(base)+angle):
      base/(e.lengthScale||1)+Math.tan(angle))*(e.scaleX||1)/(e.scaleY||1);
  }
  function direction(e) {
    if(e.guide?.[0])return Math.atan(slope(e));
    return (e.angle||0)*rad;
  }
  function rayPoint(e,t,side=0) {
    const o=origin(e);
    if(e.guide?.[0]) {
      const factor=e.guide[2]+(e.guide[3]-e.guide[2])*t;
      return {x:o.x+slope(e)*t*e.height+side*e.width*.5*factor,y:o.y+t*e.height};
    }
    const a=((e.angle||0)+side*(e.fan||0)*.5)*rad;
    return {x:o.x+side*e.width*.5+Math.sin(a)*e.height*t,y:o.y+Math.cos(a)*e.height*t};
  }
  function outline(e) {
    if(isRay(e))return [rayPoint(e,0,-1),rayPoint(e,0,1),rayPoint(e,1,1),rayPoint(e,1,-1)];
    if(['soft-light','halo','free-mist','waterfall-spray','cloud-bank','torch','wall-torch'].includes(e.kind))
      return Array.from({length:24},(_,i)=>({x:e.x+Math.cos(i*Math.PI/12)*e.width/2,y:e.y+Math.sin(i*Math.PI/12)*e.height/2}));
    const y=e.y-(e.anchorY??.5)*e.height;
    return [{x:e.x-e.width/2,y},{x:e.x+e.width/2,y},{x:e.x+e.width/2,y:y+e.height},{x:e.x-e.width/2,y:y+e.height}];
  }
  function contains(e,p) {
    const path=outline(e);let inside=false;
    for(let i=0,j=path.length-1;i<path.length;j=i++) {
      const a=path[i],b=path[j];
      if((a.y>p.y)!==(b.y>p.y)&&p.x<(b.x-a.x)*(p.y-a.y)/(b.y-a.y)+a.x)inside=!inside;
    }
    return inside;
  }
  function rotation(e,scale) {
    if(!e.rotatable)return null;
    const o=origin(e),a=direction(e),distance=48/scale;
    return {x:o.x+Math.sin(a)*distance,y:o.y+Math.cos(a)*distance,rotate:true};
  }
  function angleAt(e,p) {
    const o=origin(e),dx=(p.x-o.x)/(e.scaleX||1),dy=(p.y-o.y)/(e.scaleY||1);
    const a=Math.atan2(dx,dy)/rad;
    if(e.guide?.[0]===1)return -a-Math.atan(e.guide[1])/rad;
    if(e.guide?.[0]===2)return Math.atan(Math.tan(a*rad)-e.guide[1]/(e.lengthScale||1))/rad;
    return a;
  }
  return {isRay,origin,direction,rayPoint,outline,contains,rotation,angleAt};
})();
