/* Camera input has no renderer/DOM dependencies. Rebase whenever the pointer
 * count changes: lifting one finger after a pinch must never jump the view. */
const ActionAuditCamera = (() => {
  const clamp=(n,a,b)=>Math.max(a,Math.min(b,n));
  function sample(points) {
    const p=[...points.values()];
    if(!p.length)return null;
    return {x:p.reduce((s,v)=>s+v.x,0)/p.length,y:p.reduce((s,v)=>s+v.y,0)/p.length,
      span:p.length>1?Math.hypot(p[1].x-p[0].x,p[1].y-p[0].y):0,count:p.length};
  }
  function move(view,before,after,units,mode='touch') {
    if(!before || !after || before.count!==after.count || after.count>2)return {...view};
    const next={...view},dx=after.x-before.x,dy=after.y-before.y;
    if(mode==='orbit' || (mode==='touch' && after.count===2)) {
      next.yaw=clamp(view.yaw+dx*.005,-.7,.7);
      next.pitch=clamp(view.pitch+dy*.005,-.7,.7);
    } else {
      next.x=view.x-dx*units;next.y=view.y-dy*units;
    }
    if(after.count>1 && before.span>2 && after.span>2)
      next.zoom=clamp(view.zoom*before.span/after.span,.5,2.5);
    return next;
  }
  function outputSize(width,height,maxAxis=768) {
    const scale=maxAxis/Math.max(1,width,height);
    return {width:Math.max(1,Math.round(width*scale)),height:Math.max(1,Math.round(height*scale))};
  }
  function canvasPoint(rect,width,height,x,y) {
    const scale=Math.min(rect.width/width,rect.height/height);
    if(!(scale>0))return null;
    const point={x:(x-rect.left-(rect.width-width*scale)/2)/scale,
      y:(y-rect.top-(rect.height-height*scale)/2)/scale};
    return point.x>=0&&point.y>=0&&point.x<width&&point.y<height?point:null;
  }
  function initialY(cells,width,height) {
    // Use the renderer's native support map, not empty padding at the bottom
    // of the tilemap. Prefer a broad floor in the room's entrance columns.
    let row=0,best=0;
    for(let y=0;y<height;++y) {
      let score=0;for(let x=0;x<Math.min(width,16);++x)score+=!!(cells[y*width+x]&16);
      if(score && score>=best){best=score;row=y;}
    }
    return best?Math.max(0,row*16-168):0;
  }
  function inspectionBounds(native) {
    return {minX:native.minX-256,maxX:native.maxX+256,
      minY:-224,maxY:native.maxY+224};
  }
  return {clamp,sample,move,outputSize,canvasPoint,initialY,inspectionBounds};
})();
