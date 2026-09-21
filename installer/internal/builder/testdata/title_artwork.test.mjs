import {test} from "node:test";
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {runInNewContext} from "node:vm";

const source=readFileSync(new URL("../web/app.js",import.meta.url),"utf8");
function section(begin,end){
  const start=source.indexOf(begin),stop=source.indexOf(end,start);
  assert.ok(start>=0&&stop>start,"production title handlers must be present");
  return source.slice(start,stop);
}
function setup(enabled=false,variant="en"){
  const control=value=>({value,listeners:{},addEventListener(event,fn){this.listeners[event]=fn;}});
  const context={titleToggle:control(""),titleVariant:control(variant),titleChange:control("0"),
    titlePreview:{},assetBaseline:{title:enabled,titleVariant:variant},assetRows:[],
    refreshAssetDirtyState(){},lastAssetConfig:{},document:{querySelector(){}},syncVariantRows(){}};
  context.titleToggle.checked=enabled;
  runInNewContext(section("function refreshTitlePreview(){","function wireAssetRow(row){")+
    section("function discardAssetChanges(){","/* Drops the PENDING")+
    section("function paintAssetConfiguration(config){","async function loadAssetConfiguration(){"),context);
  return context;
}

test("style preview and saved selection follow each other without auto-enabling",()=>{
  const s=setup();
  s.titleVariant.value="ja";s.titleVariant.listeners.change();
  assert.equal(s.titlePreview.src,"title-logo-ja.png");
  assert.equal(s.titleToggle.checked,false);
  assert.equal(s.titleChange.value,"0");
  s.titleToggle.checked=true;s.titleToggle.listeners.change();
  assert.equal(s.titleChange.value,"1");
});

test("switching back is a no-op, and discard restores both style and toggle",()=>{
  const s=setup(true,"ja");
  s.titleVariant.value="en";s.titleVariant.listeners.change();
  assert.equal(s.titleChange.value,"1");
  assert.equal(s.titlePreview.src,"title-logo.png");
  s.titleVariant.value="ja";s.titleVariant.listeners.change();
  assert.equal(s.titleChange.value,"0");
  s.titleVariant.value="en";s.titleToggle.checked=false;s.titleToggle.listeners.change();
  assert.equal(s.titleChange.value,"1");
  s.discardAssetChanges();
  assert.equal(s.titleChange.value,"0");
  assert.equal(s.titleToggle.checked,true);
  assert.equal(s.titleVariant.value,"ja");
  assert.equal(s.titlePreview.src,"title-logo-ja.png");
});

test("saved/reloaded Japanese art remains selected while on or off",()=>{
  const s=setup();
  for(const enabled of [true,false]){
    s.paintAssetConfiguration({title:{enabled,variant:"ja"},tracks:[]});
    assert.equal(s.titleToggle.checked,enabled);
    assert.equal(s.titleVariant.value,"ja");
    assert.equal(s.titlePreview.src,"title-logo-ja.png");
    assert.equal(s.titleChange.value,"0");
    assert.equal(s.assetBaseline.titleVariant,"ja");
  }
  s.paintAssetConfiguration({title:{enabled:true},tracks:[]});
  assert.equal(s.titleVariant.value,"en","old configurations default to English");
});
