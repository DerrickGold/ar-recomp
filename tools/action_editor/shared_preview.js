/* Snapshot v1 is a portable original-background baseline, not an effects or
 * Diorama scene. The C decoder validates it again before publishing a scene.
 * No memory views survive a WASM call; no assets are rebuilt per animation tick. */
const SharedActionPreview = (() => {
  const HEADER=112, MAX_BYTES=80*1024;
  let api=null, loadedKey='', cache=null;
  let status='Shared C baseline not bundled — JavaScript comparison available';
  const checksum=bytes=>{
    let hash=2166136261;
    for(let i=0;i<bytes.length;i++)hash=Math.imul(hash^(i>=104&&i<108?0:bytes[i]),16777619)>>>0;
    return hash;
  };
  function encode(data,blobs,r,request={}) {
    request={...(r.section==='completion'?{bgscMask:3,bgsc:0x7464}:{}),...request};
    const integer=(value,min,max,label)=>{
      if(!Number.isInteger(value)||value<min||value>max)throw Error(`Invalid ${label}`);
      return value;
    };
    for(const name of ['cameraX','cameraY','rasterCameraX'])
      integer(request[name]??0,-32768,65535,name);
    integer(request.frame??37,0,0xffffffff,'frame');
    integer(request.animationPhase??-1,-1,255,'animation phase');
    integer(request.pagePhase??-1,-1,3,'page phase');
    integer(request.bgscMask??0,0,3,'BGSC mask');
    integer(request.bgsc??0,0,65535,'BGSC');
    integer(r.terrainProfile??0,0,2,'terrain profile');
    integer(r.group,0,255,'group');integer(r.map,0,255,'room');
    integer(r.videoProfile,0,255,'video profile');
    integer(r.raster??0,0,10,'raster');
    integer(r.rasterEntryCameraX??0,0,65535,'entry camera');
    const block=(index,size)=>{
      if(index===undefined||index<0)return new Uint8Array(size);
      const bytes=blobs[index];
      if(!(bytes instanceof Uint8Array)||bytes.length!==size)throw Error('Invalid scene asset length');
      return bytes;
    };
    const present=index=>Number.isInteger(index)&&index>=0;
    if(!Array.isArray(r.video)||r.video.length!==28)throw Error('Missing native video profile');
    r.video.forEach(value=>integer(value,0,255,'video byte'));
    const assets=[block(r.chars,16384),block(r.extraChars,8192),block(r.palette,256)];
    const h=new Uint32Array(28);
    h.set([0x43535241,1,0,r.terrainProfile||0,r.group,r.map,
      (present(r.chars)?3:0)|(present(r.extraChars)?4:0)|(present(r.palette)?8:0)|16|
      (present(data.rasterWaveform)?32:0)|(present(data.rasterR4Window)?64:0)|
      (present(r.rasterWorkspace)?128:0)|(r.rasterEntryCameraX!==null&&r.rasterEntryCameraX!==undefined?256:0),
      r.videoProfile,r.raster||0,r.rasterEntryCameraX||0,
      request.cameraX||0,request.cameraY||0,request.rasterCameraX||0,
      request.frame===undefined?37:request.frame,
      request.animationPhase===undefined?0xffffffff:request.animationPhase,
      request.pagePhase===undefined?0xffffffff:request.pagePhase,
      (request.haveRasterCamera?1:0)|((request.bgscMask||0)<<1)|(request.entryFrame?8:0),
      request.bgsc||0]);
    for(let bg=0;bg<2;bg++){
      const b=r.bg[bg],size=b?b.pagesWide*b.pagesHigh*256:0;
      if(b){integer(b.pagesWide,1,64,'map width');integer(b.pagesHigh,1,64,'map height');}
      if(size>16384||size<0)throw Error('Scene map exceeds snapshot limit');
      h.set(b?[b.pagesWide,b.pagesHigh,size,3]:[0,0,0,0],18+bg*4);
      assets.push(block(b?b.metatiles:-1,2048),block(b?b.map:-1,size));
    }
    assets.push(Uint8Array.from(r.video),block(data.rasterWaveform,256),
      block(data.rasterR4Window,256),block(r.rasterWorkspace,8192));
    const size=HEADER+assets.reduce((n,b)=>n+b.length,0);
    if(size>MAX_BYTES)throw Error('Scene exceeds snapshot limit');
    h[2]=size;
    const bytes=new Uint8Array(size),view=new DataView(bytes.buffer);
    h.forEach((n,i)=>view.setUint32(i*4,n,true));
    let offset=HEADER;for(const asset of assets){bytes.set(asset,offset);offset+=asset.length;}
    view.setUint32(104,checksum(bytes),true);
    return bytes;
  }
  function load(bytes) {
    if(!api||!(bytes instanceof Uint8Array)||bytes.length>api.ActionPreview_Capacity())
      throw Error('Shared baseline is unavailable or scene is too large');
    new Uint8Array(api.memory.buffer,api.ActionPreview_Input(),bytes.length).set(bytes);
    if(!api.ActionPreview_Load(bytes.length))throw Error('Shared C baseline rejected this snapshot');
    cache=null;loadedKey='';
  }
  function render(r,x,y,frame) {
    if(!api)return null;
    const key=sceneKey(r);
    if(loadedKey!==key){load(encode(DATA,BLOBS,r));loadedKey=key;}
    const frameKey=`${key}:${x}:${y}:${frame}`;
    if(cache&&cache.key===frameKey)return cache.canvas;
    const pointer=api.ActionPreview_Render(x,y,frame);
    if(!pointer)throw Error('Shared C baseline could not render this scene');
    const canvas=cache?cache.canvas:document.createElement('canvas');
    if(canvas.width!==256)canvas.width=256;
    if(canvas.height!==224)canvas.height=224;
    const rgba=new Uint8ClampedArray(api.memory.buffer,pointer,256*224*4);
    canvas.getContext('2d').putImageData(new ImageData(rgba,256,224),0,0);
    cache={key:frameKey,canvas};return canvas;
  }
  async function initialize(bytes) {
    try{
      const module=await WebAssembly.compile(bytes);
      if(WebAssembly.Module.imports(module).length)throw Error('Unexpected shared preview runtime imports');
      const instance=await WebAssembly.instantiate(module,{});
      if(instance.exports.ActionPreview_Version()!==1)throw Error('Unsupported shared preview ABI');
      api=instance.exports;
      if(api._initialize)api._initialize();
      loadedKey='';cache=null;
      status='Shared C/WASM baseline ready · original backgrounds only';
      return api;
    }catch(error){
      api=null;loadedKey='';cache=null;
      status=`Shared C baseline unavailable: ${error.message}`;
      throw error;
    }
  }
  function canvas() {
    if(!api||tint)return null;
    try{return render(room,nativeCamera.x,nativeCamera.y,nativeFrame);}
    catch(error){status=`Shared C baseline unavailable: ${error.message}`;api=null;cache=null;loadedKey='';return null;}
  }
  return {encode,initialize,load,render,canvas,get status(){return status;},get ready(){return !!api;},
    hash:()=>api?api.ActionPreview_Hash()>>>0:null};
})();

if(window.__ACTION_PREVIEW_WASM__){
  SharedActionPreview.initialize(B64(window.__ACTION_PREVIEW_WASM__)).then(()=>{
    const refresh=()=>{refreshNativePhaseControls();draw();};
    if(document.readyState==='loading')window.addEventListener('DOMContentLoaded',refresh,{once:true});
    else refresh();
  }).catch(error=>{
    const element=document.querySelector('#sharedPreviewStatus');
    if(element)element.textContent=`Shared C baseline failed: ${error.message}. Existing map tools remain available.`;
  });
}
