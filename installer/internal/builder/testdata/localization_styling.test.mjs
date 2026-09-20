import {test} from "node:test";
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {runInNewContext} from "node:vm";
const context={window:{},Intl};
runInNewContext(readFileSync(new URL("../localization_styling.js",import.meta.url),"utf8"),context);
const wrap=context.window.workshopInlineStyle.wrap;
function apply(source,start=0,end=source.length,open="<i>",close="</i>"){
 const edit=wrap(source,start,end,open,close);
 return {body:source.slice(0,start)+edit.text+source.slice(end),...edit};
}

test("selection and cursor edits retain text, values and a useful selection",()=>{
 const selected=apply("Hello {master_name:02}!",0,22);
 assert.equal(selected.body,"<i>Hello {master_name:02}</i>!");
 assert.equal(selected.body.slice(selected.selectStart,selected.selectEnd),"Hello {master_name:02}");
 const cursor=apply("Hello",2,2);
 assert.equal(cursor.body,"He<i></i>llo");assert.equal(cursor.selectStart,5);assert.equal(cursor.selectEnd,5);
 assert.equal(apply("",0,0).body,"<i></i>");
});

test("commands, comments, line boundaries and table separators are never wrapped",()=>{
 const source="@anchor reset_text_cursor.00\n# note\nOne | {value}\n@line\nTwo\n@end\n";
 assert.equal(apply(source).body,"@anchor reset_text_cursor.00\n# note\n<i>One </i>|<i> {value}</i>\n@line\n<i>Two</i>\n@end\n");
 assert.throws(()=>apply(source,3,3),/selection/);
 assert.throws(()=>apply("@end\n"),/selection/);
 assert.equal(apply("One\n  \t\nTwo").body,"<i>One</i>\n  \t\n<i>Two</i>");
});

test("overrides go inside existing tags, including tags split over source lines",()=>{
 assert.equal(apply('<i>Voilà</i>',0,12,'<span italic="false">','</span>').body,'<i><span italic="false">Voilà</span></i>');
 const source='<span\n font="hud">Hello</span> there';
 assert.equal(apply(source).body,'<span\n font="hud"><i>Hello</i></span><i> there</i>');
 const mixed='<i>Hello</i> world';
 assert.equal(apply(mixed,5,17).body,'<i>He<i>llo</i></i><i> worl</i>d');
});

test("syntax tokens and Unicode graphemes cannot be split",()=>{
 for(const [text,start,end] of [
  ["{master_name}",1,5],['<span color="#FFD050">hey</span>',3,24],
  ["\\<literal",1,3],["{{literal}}",1,3],["e\u0301clair",1,4],
  ["A😀B",2,3],["👩‍👩‍👧‍👦!",0,2]
 ]) assert.throws(()=>apply(text,start,end),/selection/,text);
 for(const text of ["e\u0301clair","👩‍👩‍👧‍👦!","日本語", "\\<literal", "{{literal}}"])
  assert.equal(apply(text).body,"<i>"+text+"</i>");
 assert.equal(apply("@@literal").body,"@@<i>literal</i>");
 assert.equal(apply("\\#literal").body,"\\#<i>literal</i>");
});
