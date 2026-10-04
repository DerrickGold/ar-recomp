/* Share authoring only when the placed terrain matches. Unused metatile
 * definitions do not split a room; BG2 can stay shared when BG1 differs. */
const TERRAIN_TOKENS = ['us','jp','eu'];
const terrainFamilies = new Map();
function editFamilies(r,bg) {
  const key=`${sceneIdentity(r)}:${bg}`;
  if(terrainFamilies.has(key))return terrainFamilies.get(key);
  const base=DATA.rooms.find(b=>sceneIdentity(b)===sceneIdentity(r))||r;
  const families=[],equal=(a,b)=>a.length===b.length&&a.every((v,i)=>v===b[i]);
  for(const {profile} of TERRAIN_PROFILES) {
    const variant=terrainRoom(base,profile),layer=decodeLayer(variant,bg);
    let family=families.find(f=>!layer&&!f.layer||layer&&f.layer&&
      layer.cellsW===f.layer.cellsW&&layer.cellsH===f.layer.cellsH&&
      equal(layer.cellId,f.layer.cellId)&&equal(layer.words,f.layer.words));
    if(!family){family={profile,mask:0,room:variant,layer};families.push(family);}
    family.mask|=1<<profile;
  }
  /* Keep identities, not a second full set of decoded map surfaces. */
  families.forEach(f=>delete f.layer);
  terrainFamilies.set(key,families);return families;
}
const editFamily=(r,bg)=>editFamilies(r,bg).find(f=>f.mask&(1<<(r.terrainProfile||0)));
function terrainMask(value) {
  if(value===undefined)return 1;
  let mask=0;
  for(const token of value.split('+')) {
    const profile=TERRAIN_TOKENS.indexOf(token==='ge'?'eu':token);
    if(profile<0)return 0;
    mask|=1<<profile;
  }
  return mask;
}
const terrainField=mask=>mask===1?'':`:${TERRAIN_TOKENS.filter((_,i)=>mask&(1<<i)).join('+')}`;
function regionalKey(key) {
  const parts=key.split(':');
  return parts.length<=2&&!!terrainMask(parts[1])?parts:null;
}
const regionalRooms=(r,bg,value)=>editFamilies(r,bg)
  .filter(f=>f.mask&terrainMask(value)).map(f=>f.room);
/* Identical records across families need only one INI line. */
function regionalRecords(r,bg,records) {
  const merged=new Map();
  for(const f of editFamilies(r,bg))for(const record of records(f.room))
    merged.set(record,(merged.get(record)||0)|f.mask);
  return [...merged].map(([record,mask])=>record.includes(' =')
    ?record.replace(' =',terrainField(mask)+' ='):record+terrainField(mask));
}
function regionalPixelCount(r,bg,stamps=stampBucket(r,bg).cells,pixels=pixelBucket(r,bg)) {
  const current=keyOf(r,bg);
  return regionalRecords(r,bg,variant=>keyOf(variant,bg)===current
    ?pixelRecords(variant,bg,stamps,pixels):pixelRecords(variant,bg)).length;
}
