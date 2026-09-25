package builder

import (
	"os/exec"
	"strings"
	"testing"
)

func TestRegionalMediaController(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node unavailable")
	}
	code, err := frontendFiles.ReadFile("web/regional-media.js")
	if err != nil {
		t.Fatal(err)
	}
	const before = `
const assert=require('node:assert/strict');
class Element {
 constructor(){this.handlers={};this.children=[];this.hidden=false;this.files=[];this.disabled=false;this.resets=0;}
 addEventListener(name,fn){this.handlers[name]=fn;}
 append(...nodes){this.children.push(...nodes);}
 replaceChildren(...nodes){this.children=nodes;}
 querySelectorAll(){return controls;}
 reset(){this.resets++;input.files=[];}
 emit(name){return this.handlers[name]?.({preventDefault(){}});}
}
const ids=['regional-media-form','regional-media-file','regional-media-status','regional-media-donors','regional-media-refresh','panel-assets'];
const elements=Object.fromEntries(ids.map(id=>[id,new Element()]));
const form=elements[ids[0]],input=elements[ids[1]],status=elements[ids[2]],donors=elements[ids[3]],refresh=elements[ids[4]],choose=new Element();
const controls=[input,refresh];const docHandlers={};
global.document={getElementById:id=>elements[id],createElement:()=>new Element(),querySelector:()=>choose,addEventListener:(key,fn)=>docHandlers[key]=fn};
let confirms=0,allow=false,feedback=0;const reports=[];const bindings=[];
global.window={confirm:()=>{confirms++;return allow;},workshopI18n:{set:(el,key,args)=>{el.key=key;el.args=args;bindings.push(key);},text:key=>key},workshopFeedback:{clear(){},show(target,error,options){feedback++;reports.push({error,options});}}};
global.FormData=class {constructor(){this.values={};}append(k,v){this.values[k]=v;}};
const requests=[],responses=[];
global.fetch=async(path,init={})=>{requests.push({path,...init});assert(responses.length,'unexpected request');const [status,body]=responses.shift();return {status,ok:status===200,json:async()=>body};};
const rows=['us','jp','eu-en','de','fr'].map(release=>({release,status:'missing',resources:[]}));
const tick=async()=>{for(let i=0;i<5;i++)await new Promise(resolve=>setImmediate(resolve));};
`
	const after = `
(async()=>{
 responses.push([200,{donors:rows}]);await choose.emit('click');
 assert.equal(donors.children.length,5);assert.equal(status.key,'builder.media.ready');
 input.files=[{name:'private.armedia'}];responses.push([409,{code:'replace_required',release:'jp'}]);form.emit('submit');await tick();
 assert.equal(confirms,1);assert.equal(status.key,'builder.media.cancelled');assert.equal(form.resets,0);assert.equal(input.disabled,false);
 allow=true;responses.push([409,{code:'replace_required',release:'jp'}],[200,{changed:true,release:'jp'}],[200,{donors:rows}]);form.emit('submit');await tick();
 assert.equal(requests.at(-3).body.values.replace,'false');assert.equal(requests.at(-2).body.values.replace,'true');
 assert.equal(status.key,'builder.media.done');assert.equal(form.resets,1);assert.equal(input.disabled,false);
 input.files=[{name:'bad.sfc'}];responses.push([400,{error:'bad identity'}]);form.emit('submit');await tick();
 assert.equal(feedback,1);assert.equal(status.key,'builder.media.failed');assert.equal(input.files.length,1);
 assert.equal(reports.at(-1).options.operation,'Install regional media');
 responses.push([500,{error:'cannot read inventory'}]);await refresh.emit('click');
 assert.equal(reports.at(-1).options.operation,'Read regional media status');
 responses.push([200,{changed:true,release:'jp'}],[500,{error:'cannot refresh inventory'}]);form.emit('submit');await tick();
 assert.equal(status.key,'builder.media.installed_refresh_failed');
 assert.equal(reports.at(-1).error.outcome,'Regional media installed; status refresh failed.');
 responses.push([200,{donors:rows}]);await refresh.emit('click');assert.equal(status.key,'builder.media.ready');
 assert(requests.every(r=>r.path==='regional-media'));assert.equal(responses.length,0);
 docHandlers['workshop:closed']();assert(input.disabled);await choose.emit('click');assert.equal(responses.length,0);
})().catch(error=>{console.error(error);process.exitCode=1;});
`
	cmd := exec.Command(node)
	cmd.Stdin = strings.NewReader(before + string(code) + after)
	if output, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("controller: %v\n%s", err, output)
	}
}
