/* Handles edit the existing field contract, not an extra rendering transform.
 * Collision witnesses, particle budgets and receiver settings stay untouched. */
const EffectRegions=(()=>{
  const owners={
    'castle-mist':'castle', 'castle-sky':'castle',
    'temple-dust':'atmosphere', 'cave-light':'atmosphere',
    'moon-cloud':'moon',
    'blood-mist':'marsh', 'blood-water':'marsh'
  };
  const vector=(r,key)=>String(r[key]??'').trim().split(/\s+/).map(Number);
  const box=(left,top,right,bottom)=>({x:(left+right)/2,y:(top+bottom)/2,width:right-left,height:bottom-top});
  function bounds(kind,r,index=0) {
    if(kind==='castle-mist') {
      const [left,right,floor]=vector(r,'floor'),shape=vector(r,'mist-shape');
      return {x:(left+right)/2,y:floor,width:right-left,height:2*(shape[0]+shape[1])+2,anchorY:1};
    }
    if(kind==='castle-sky') {
      const [x,y]=vector(r,'sky-anchor'),[rx,ry]=vector(r,'sky-shape');
      return {x,y,width:rx*2,height:ry*2};
    }
    if(kind==='temple-dust')return box(...vector(r,`area-${index+1}`));
    if(kind==='cave-light') {
      const [x,y,rx,ry,lean]=vector(r,`ambient-${index+1}`);
      return {x,y,width:2*(rx+Math.abs(lean)*ry),height:ry*2};
    }
    if(kind==='moon-cloud') {
      const anchor=vector(r,'anchor'),motion=vector(r,'cloud-motion'),shape=vector(r,'cloud-shape');
      return {x:anchor[0]+motion[2],y:anchor[1]+motion[5],width:shape[0]*2,height:shape[1]*2};
    }
    if(kind==='blood-mist'||kind==='blood-water') {
      const [left,right]=vector(r,`span-${index+1}`),surface=vector(r,'surface');
      const [top,bottom]=kind==='blood-mist'?vector(r,'mist-window').map(y=>y+surface[0]):surface.slice(1,3);
      return box(left,top,right,bottom);
    }
    return null;
  }
  function limits(kind,r,index=0) {
    const b=bounds(kind,r,index),result={maxWidth:16384,maxHeight:16384,resizeHeight:true,moveY:true};
    if(kind==='castle-mist')result.maxHeight=128;
    if(kind==='castle-sky') {
      const extents=[...vector(r,'sky-window'),...vector(r,'sky-clip')];
      result.maxWidth=Math.min(768,b.width*768/Math.max(1,...extents.filter((_,i)=>i%2===0).map(Math.abs)));
      result.maxHeight=Math.min(768,b.height*768/Math.max(1,...extents.filter((_,i)=>i%2===1).map(Math.abs)));
    }
    if(kind==='moon-cloud') {
      const w=vector(r,'cloud-window'),motion=vector(r,'cloud-motion');
      const scale=(lo,hi,centre,span)=>Math.min(span/(hi-lo),
        ...[lo,hi].map(edge=>edge===centre?Infinity:(edge>centre?384-centre:-384-centre)/(edge-centre)));
      result.maxWidth=Math.min(768,b.width*scale(w[0],w[2],motion[2],768));
      result.maxHeight=Math.min(768,b.height*scale(w[1],w[3],motion[5],384));
    }
    if(kind==='blood-water'||kind==='blood-mist') {
      const world=vector(r,'bounds'),count=Number(r['span-count']);
      const left=index?vector(r,`span-${index}`)[1]:world[0];
      const right=index+1<count?vector(r,`span-${index+2}`)[0]:world[2];
      result.maxWidth=right-left;result.minWidth=2*vector(r,'surface')[3]+1;
      result.left=left;result.right=right;
      if(kind==='blood-water') {result.resizeHeight=false;result.moveY=false;result.maxHeight=b.height;}
      else {
        const shape=vector(r,'mist-shape');
        result.maxHeight=Math.min(80,b.height*24/Math.max(.001,shape[2]+shape[3]));
      }
    }
    // Whole-pixel handle limits also leave room for float serialization at
    // the C validator's inclusive clip/shape boundaries.
    result.maxWidth=Math.floor(result.maxWidth);result.maxHeight=Math.floor(result.maxHeight);
    return result;
  }
  function patch(kind,r,index,draft) {
    const before=bounds(kind,r,index),after={...before,...draft},result={};
    if(!before||![after.x,after.y,after.width,after.height].every(Number.isFinite)||after.width<4||after.height<4)
      throw Error('Effect regions require finite dimensions of at least 4 pixels.');
    const range=limits(kind,r,index);
    after.width=Math.max(range.minWidth??4,Math.min(after.width,range.maxWidth));
    after.height=range.resizeHeight?Math.min(after.height,range.maxHeight):before.height;
    if(range.left!==undefined)after.x=Math.max(range.left+after.width/2,Math.min(range.right-after.width/2,after.x));
    if(!range.moveY)after.y=before.y;
    const sx=after.width/before.width,sy=after.height/before.height;
    const write=(key,values)=>result[key]=values.map(v=>Number(v.toFixed(5))).join(' ');
    if(kind==='castle-mist') {
      write('floor',[Math.round(after.x-after.width/2),Math.round(after.x+after.width/2),Math.round(after.y)]);
      const shape=vector(r,'mist-shape'),scale=(after.height-2)/(before.height-2);
      shape[0]*=scale;shape[1]*=scale;write('mist-shape',shape);
    } else if(kind==='castle-sky') {
      write('sky-anchor',[Math.round(after.x),Math.round(after.y)]);
      write('sky-shape',[after.width/2,after.height/2]);
      // Grow the capture/clip apron with the source, so enlarging a halo does
      // not just enlarge a handle over geometry still clipped to its old size.
      for(const key of ['sky-window','sky-clip'])write(key,vector(r,key).map((v,i)=>v*(i%2?sy:sx)));
    } else if(kind==='temple-dust') {
      write(`area-${index+1}`,[after.x-after.width/2,after.y-after.height/2,after.x+after.width/2,after.y+after.height/2]);
    } else if(kind==='cave-light') {
      const v=vector(r,`ambient-${index+1}`);
      v[0]=after.x;v[1]=after.y;v[2]*=sx;v[3]*=sy;v[4]*=sx/sy;
      write(`ambient-${index+1}`,v);
    } else if(kind==='moon-cloud') {
      const anchor=vector(r,'anchor'),motion=vector(r,'cloud-motion'),shape=vector(r,'cloud-shape');
      const dx=after.x-before.x,dy=after.y-before.y;
      write('cloud-window',vector(r,'cloud-window').map((v,i)=>
        (v-(i%2?motion[5]:motion[2]))*(i%2?sy:sx)+(i%2?motion[5]+dy:motion[2]+dx)));
      motion[2]=after.x-anchor[0];motion[5]=after.y-anchor[1];
      motion[3]*=sx;motion[6]*=sy;shape[2]*=sx;
      shape[0]=after.width/2;shape[1]=after.height/2;
      write('cloud-motion',motion);write('cloud-shape',shape);
    } else {
      write(`span-${index+1}`,[Math.round(after.x-after.width/2),Math.round(after.x+after.width/2)]);
      const surface=vector(r,'surface');
      if(kind==='blood-mist') {
        const oldCentre=before.y-surface[0],centre=Math.max(-48+after.height/2,Math.min(32-after.height/2,after.y-surface[0]));
        write('mist-window',[centre-after.height/2,centre+after.height/2]);
        const shape=vector(r,'mist-shape');shape[2]*=sy;shape[3]*=sy;write('mist-shape',shape);
        const motion=vector(r,'mist-motion');
        motion[3]=centre+(motion[3]-oldCentre)*sy;motion[4]*=sy;motion[5]*=sy;write('mist-motion',motion);
      }
    }
    return result;
  }
  return {owner:kind=>owners[kind],bounds,limits,patch};
})();
