/* Ordered native-cell regions resolve to disjoint rectangles for authoring.
 * Splits retain an explicit curve domain, so touching artwork inside a curved
 * wall does not restart the curve at the edited cell. An attached pull is one
 * atomic bend token with its own explicit source-axis endpoints; copying a
 * payload or carving artwork must keep that token unchanged. Group transforms
 * likewise retain their shared pivot verbatim; only an explicit Copy operation
 * rebases a pivot to a different selection. */
const VoxelRecords=(()=>{
  const clone=value=>JSON.parse(JSON.stringify(value));
  const bounds=r=>({x:r.x,y:r.y,w:r.w||1,h:r.h||1});
  const covers=(r,x,y)=>x>=r.x&&y>=r.y&&x<r.x+(r.w||1)&&y<r.y+(r.h||1);
  const resolve=(records,x,y)=>{for(let i=records.length-1;i>=0;i--)if(covers(records[i],x,y))return records[i];return null;};
  function payload(r) {
    const out={...r};delete out.x;delete out.y;delete out.w;delete out.h;delete out._order;
    if(out.shape&&out.shape!=='box'&&!out['shape-span'])out['shape-span']=`${r.y*16},${(r.h||1)*16}`;
    return out;
  }
  const signature=r=>JSON.stringify(Object.fromEntries(Object.entries(payload(r)).sort(([a],[b])=>a.localeCompare(b))));
  const place=(r,b)=>({...payload(r),x:b.x,y:b.y,...(b.w>1?{w:b.w}:{}),...(b.h>1?{h:b.h}:{})});
  function subtract(r,cut) {
    const a=bounds(r),b=bounds(cut),x0=Math.max(a.x,b.x),y0=Math.max(a.y,b.y),x1=Math.min(a.x+a.w,b.x+b.w),y1=Math.min(a.y+a.h,b.y+b.h);
    if(x0>=x1||y0>=y1)return [r];
    const pieces=[];
    if(a.y<y0)pieces.push({x:a.x,y:a.y,w:a.w,h:y0-a.y});
    if(y1<a.y+a.h)pieces.push({x:a.x,y:y1,w:a.w,h:a.y+a.h-y1});
    if(a.x<x0)pieces.push({x:a.x,y:y0,w:x0-a.x,h:y1-y0});
    if(x1<a.x+a.w)pieces.push({x:x1,y:y0,w:a.x+a.w-x1,h:y1-y0});
    return pieces.map(b=>place(r,b));
  }
  function compact(records) {
    let result=records.map(clone),previous=-1;
    while(previous!==result.length) {
      previous=result.length;
      for(const horizontal of [true,false]) {
        const groups=new Map();
        for(const r of result) {
          const b=bounds(r),id=signature(r)+':'+(horizontal?`${b.y}:${b.h}`:`${b.x}:${b.w}`);
          if(!groups.has(id))groups.set(id,[]);groups.get(id).push(r);
        }
        result=[];
        for(const rows of groups.values()) {
          rows.sort((a,b)=>horizontal?a.x-b.x:a.y-b.y);let first=rows[0];
          for(let i=1;i<=rows.length;i++) {
            const a=bounds(first),b=i<rows.length?bounds(rows[i]):null;
            if(b&&(horizontal?a.x+a.w===b.x:a.y+a.h===b.y))
              first=place(first,{...a,w:a.w+(horizontal?b.w:0),h:a.h+(horizontal?0:b.h)});
            else {result.push(first);first=rows[i];}
          }
        }
      }
    }
    return result.sort((a,b)=>a.y-b.y||a.x-b.x||(a.h||1)-(b.h||1)||(a.w||1)-(b.w||1));
  }
  function put(records,record) {
    const out=records.flatMap(old=>subtract(old,record));
    if(Object.keys(payload(record)).length)out.push(clone(record));
    return out;
  }
  function normalize(records) {
    let out=[];for(const record of Array.isArray(records)?records:Object.values(records||{}))out=put(out,record);
    return compact(out);
  }
  function append(records,record) {
    const b=bounds(record);
    return records.filter(r=>{const a=bounds(r);return a.x!==b.x||a.y!==b.y||a.w!==b.w||a.h!==b.h;}).concat(clone(record));
  }
  function pack(cells) {
    const rows=new Map();
    for(const r of cells) {
      const id=`${r.y}:${signature(r)}`;if(!rows.has(id))rows.set(id,[]);rows.get(id).push(r);
    }
    const runs=[];
    for(const row of rows.values()) {
      row.sort((a,b)=>a.x-b.x);let first=row[0],end=first.x+1;
      for(let i=1;i<=row.length;i++) {
        if(i<row.length&&row[i].x===end){end++;continue;}
        runs.push(place(first,{x:first.x,y:first.y,w:end-first.x,h:1}));
        if(i<row.length){first=row[i];end=first.x+1;}
      }
    }
    return compact(runs);
  }
  function update(records,cells) {
    const changes=pack(cells);let out=normalize(records);
    for(const r of changes)out=put(out,r);
    out=compact(out);
    if(out.length<=256)return out;
    // A valid 256-record source can describe thousands of fragments by
    // overlaying holes. Preserve ordered rectangles instead of exploding it.
    out=records;for(const r of changes)out=append(out,r);return out;
  }
  return {bounds,covers,resolve,payload,signature,place,subtract,compact,put,normalize,append,pack,update};
})();
