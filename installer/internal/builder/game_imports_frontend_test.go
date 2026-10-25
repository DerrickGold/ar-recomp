package builder

import (
	"os/exec"
	"strings"
	"testing"
)

func TestGameImportsController(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node unavailable")
	}
	code, err := frontendFiles.ReadFile("web/game-imports.js")
	if err != nil {
		t.Fatal(err)
	}
	const before = `
const assert=require('node:assert/strict');
class Element {
 constructor(){this.handlers={};this.children=[];this.hidden=false;this.files=[];this.disabled=false;this.value='';}
 addEventListener(name,fn){this.handlers[name]=fn;}
 replaceChildren(...nodes){this.children=nodes;}
 querySelectorAll(){return controls;}
 emit(name){return this.handlers[name]?.({preventDefault(){}});}
}
const ids=['game-imports-form','game-imports-file','game-imports-status','game-imports-refresh','game-imports-content','game-imports-help','game-imports-next','panel-assets'];
const elements=Object.fromEntries(ids.map(id=>[id,new Element()]));
const [form,input,status,refresh,choice,help,next]=ids.map(id=>elements[id]);
const category=new Element(),controls=[input,choice,refresh],events={};
global.document={getElementById:id=>elements[id],createElement:()=>new Element(),querySelector:()=>category,addEventListener:(name,fn)=>events[name]=fn};
let failures=0;
global.window={workshopI18n:{set:(el,key,args)=>{el.key=key;el.args=args;}},workshopFeedback:{clear(){},show(){failures++;}}};
global.FormData=class{constructor(){this.values={};}append(k,v){this.values[k]=v;}};
const responses=[],requests=[];
global.fetch=async(path,options={})=>{requests.push({path,...options});assert(responses.length);const [status,body]=responses.shift();return {ok:status===200,status,json:async()=>body};};
const imports=[{id:'ar2-master',labelKey:'first.label',helpKey:'first.help',nextKey:'first.next',status:'missing'},
 {id:'future-content',labelKey:'second.label',helpKey:'second.help',nextKey:'second.next',status:'invalid'}];
`
	const after = `
(async()=>{
 responses.push([200,{imports}]);await category.emit('click');
 assert.equal(choice.children.length,2);assert.equal(choice.children[1].key,'second.label');
 assert.equal(status.key,'builder.imports.missing');
 choice.value='future-content';choice.emit('change');assert.equal(help.key,'second.help');assert.equal(next.key,'second.next');assert.equal(status.key,'builder.imports.invalid');
 input.files=[{name:'source.sfc'}];input.value='source.sfc';
 responses.push([400,{error:'unsupported ROM'}]);await form.emit('submit');
 assert.equal(failures,1);assert.equal(input.value,'source.sfc');assert(!choice.disabled);
 responses.push([200,{status:'installed',changed:true}]);await form.emit('submit');
 assert.equal(requests.at(-1).body.values.content,'future-content');assert.equal(choice.value,'future-content');assert.equal(input.value,'');assert.equal(status.key,'builder.imports.installed');
 assert(requests.every(r=>r.path==='game-imports'));
 events['workshop:closed']();assert(input.disabled && choice.disabled);await category.emit('click');assert.equal(responses.length,0);
})().catch(error=>{console.error(error);process.exitCode=1;});
`
	command := exec.Command(node)
	command.Stdin = strings.NewReader(before + string(code) + after)
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("controller: %v\n%s", err, output)
	}
}
