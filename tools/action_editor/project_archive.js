/* Portable ZIP transport for the game's two INIs. Stored output works offline
 * without a library; imports also accept ordinary deflated ZIP files. */
const ActionProjectArchive=(()=>{
  const limits={'diorama-layers.ini':16*1024*1024,'action-effects.ini':131072};
  const maxSize=20*1024*1024;
  const encoder=new TextEncoder(),decoder=new TextDecoder('utf-8',{fatal:true});
  const crcTable=Uint32Array.from({length:256},(_,index)=>{
    let value=index;
    for(let bit=0;bit<8;bit++)value=value&1?0xedb88320^(value>>>1):value>>>1;
    return value>>>0;
  });
  function crc32(bytes) {
    let value=0xffffffff;
    for(const byte of bytes)value=crcTable[(value^byte)&255]^(value>>>8);
    return (value^0xffffffff)>>>0;
  }
  function encode(files) {
    const entries=Object.entries(limits).map(([name,limit])=>{
      if(typeof files[name]!=='string')throw Error(`Project is missing ${name}.`);
      const bytes=encoder.encode(files[name]);
      if(bytes.length>limit)throw Error(`${name} exceeds its project size limit.`);
      return {name:encoder.encode(name),bytes,crc:crc32(bytes)};
    });
    const localSize=entries.reduce((size,e)=>size+30+e.name.length+e.bytes.length,0);
    const centralSize=entries.reduce((size,e)=>size+46+e.name.length,0);
    const bytes=new Uint8Array(localSize+centralSize+22),view=new DataView(bytes.buffer);
    let offset=0,central=localSize;
    for(const entry of entries) {
      view.setUint32(offset,0x04034b50,true);view.setUint16(offset+4,20,true);
      view.setUint16(offset+6,0x800,true);view.setUint16(offset+12,33,true);
      view.setUint32(offset+14,entry.crc,true);
      view.setUint32(offset+18,entry.bytes.length,true);view.setUint32(offset+22,entry.bytes.length,true);
      view.setUint16(offset+26,entry.name.length,true);
      bytes.set(entry.name,offset+30);bytes.set(entry.bytes,offset+30+entry.name.length);
      view.setUint32(central,0x02014b50,true);view.setUint16(central+4,20,true);
      view.setUint16(central+6,20,true);view.setUint16(central+8,0x800,true);
      view.setUint16(central+14,33,true);view.setUint32(central+16,entry.crc,true);
      view.setUint32(central+20,entry.bytes.length,true);view.setUint32(central+24,entry.bytes.length,true);
      view.setUint16(central+28,entry.name.length,true);view.setUint32(central+42,offset,true);
      bytes.set(entry.name,central+46);
      offset+=30+entry.name.length+entry.bytes.length;central+=46+entry.name.length;
    }
    view.setUint32(central,0x06054b50,true);view.setUint16(central+8,entries.length,true);
    view.setUint16(central+10,entries.length,true);view.setUint32(central+12,centralSize,true);
    view.setUint32(central+16,localSize,true);
    return bytes;
  }
  async function inflate(bytes,limit) {
    if(typeof DecompressionStream==='undefined')throw Error('This browser cannot open compressed ZIPs. Use a ZIP saved by the editor.');
    const reader=new Blob([bytes]).stream().pipeThrough(new DecompressionStream('deflate-raw')).getReader();
    const chunks=[];let length=0;
    try {
      for(;;) {
        const {done,value}=await reader.read();if(done)break;
        length+=value.length;
        if(length>limit)throw Error('ZIP entry exceeds its project size limit.');
        chunks.push(value);
      }
    } catch(error) {await reader.cancel();throw error;}
    const result=new Uint8Array(length);let offset=0;
    for(const chunk of chunks){result.set(chunk,offset);offset+=chunk.length;}
    return result;
  }
  async function decode(bytes) {
    if(bytes.length>maxSize)throw Error('Project ZIP exceeds 20 MiB.');
    const view=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);
    let end=-1;
    for(let i=bytes.length-22;i>=Math.max(0,bytes.length-65557);i--)
      if(view.getUint32(i,true)===0x06054b50&&i+22+view.getUint16(i+20,true)===bytes.length){end=i;break;}
    if(end<0)throw Error('Invalid or incomplete project ZIP.');
    const count=view.getUint16(end+10,true),centralSize=view.getUint32(end+12,true);
    const start=view.getUint32(end+16,true),centralEnd=start+centralSize;
    if(view.getUint16(end+4,true)||view.getUint16(end+6,true)||view.getUint16(end+8,true)!==count||
        count>32||centralEnd!==end)throw Error('Unsupported project ZIP directory.');
    const files={};let offset=start;
    for(let index=0;index<count;index++) {
      if(offset+46>centralEnd||view.getUint32(offset,true)!==0x02014b50)throw Error('Invalid ZIP entry.');
      const flags=view.getUint16(offset+8,true),method=view.getUint16(offset+10,true);
      const crc=view.getUint32(offset+16,true),packed=view.getUint32(offset+20,true),size=view.getUint32(offset+24,true);
      const nameLength=view.getUint16(offset+28,true),extra=view.getUint16(offset+30,true),comment=view.getUint16(offset+32,true);
      const disk=view.getUint16(offset+34,true),local=view.getUint32(offset+42,true);
      const next=offset+46+nameLength+extra+comment;
      if(next>centralEnd)throw Error('Incomplete ZIP entry.');
      const name=decoder.decode(bytes.subarray(offset+46,offset+46+nameLength));offset=next;
      if(!Object.hasOwn(limits,name))continue;
      if(Object.hasOwn(files,name))throw Error(`Project contains duplicate ${name}.`);
      if(flags&0x41||![0,8].includes(method)||disk)
        throw Error(`Unsupported ZIP encoding for ${name}.`);
      if(size>limits[name])throw Error(`${name} exceeds its project size limit.`);
      if(local+30>start||view.getUint32(local,true)!==0x04034b50||view.getUint16(local+8,true)!==method||view.getUint16(local+6,true)&0x41)
        throw Error(`Invalid ZIP data for ${name}.`);
      const localNameLength=view.getUint16(local+26,true),localExtra=view.getUint16(local+28,true);
      const dataStart=local+30+localNameLength+localExtra;
      if(dataStart+packed>start||decoder.decode(bytes.subarray(local+30,local+30+localNameLength))!==name)
        throw Error(`Incomplete ZIP data for ${name}.`);
      const compressed=bytes.subarray(dataStart,dataStart+packed);
      const data=method===0?compressed:await inflate(compressed,limits[name]);
      if(data.length!==size||crc32(data)!==crc)throw Error(`Corrupt ZIP data for ${name}.`);
      files[name]=decoder.decode(data);
    }
    if(offset!==centralEnd)throw Error('Invalid ZIP directory length.');
    for(const name of Object.keys(limits))if(!Object.hasOwn(files,name))
      throw Error(`Project ZIP must contain ${name} at its root.`);
    return files;
  }
  return {encode,decode,limits,maxSize};
})();
