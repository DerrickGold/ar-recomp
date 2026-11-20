//go:build smoketest

package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"time"

	"github.com/wailsapp/wails/v2/pkg/options"
)

// Test-only renderer probe; absent from production binaries. Run in a disposable
// workspace under an external timeout. The script closes the backend on success
// or failure. A runner must require the explicit PASS line, not just exit zero.
func enableSmokeTest(app *options.App) {
	next := app.AssetServer.Handler
	app.AssetServer.Handler = http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/__shell/smoke-config" {
			w.Header().Set("Content-Type", "application/json")
			json.NewEncoder(w).Encode(map[string]bool{"fullBuild": os.Getenv("AR_BUILDER_SMOKE_ROM") != ""})
			return
		}
		if r.URL.Path == "/__shell/smoke-rom" {
			path := os.Getenv("AR_BUILDER_SMOKE_ROM")
			if path == "" {
				http.NotFound(w, r)
				return
			}
			http.ServeFile(w, r, path)
			return
		}
		if r.URL.Path == "/__shell/smoke.wav" {
			// A generated, silent one-second PCM fixture; no retail audio.
			data := make([]byte, 44+16000)
			copy(data, "RIFF")
			binary.LittleEndian.PutUint32(data[4:], uint32(len(data)-8))
			copy(data[8:], "WAVEfmt ")
			binary.LittleEndian.PutUint32(data[16:], 16)
			binary.LittleEndian.PutUint16(data[20:], 1)
			binary.LittleEndian.PutUint16(data[22:], 1)
			binary.LittleEndian.PutUint32(data[24:], 8000)
			binary.LittleEndian.PutUint32(data[28:], 16000)
			binary.LittleEndian.PutUint16(data[32:], 2)
			binary.LittleEndian.PutUint16(data[34:], 16)
			copy(data[36:], "data")
			binary.LittleEndian.PutUint32(data[40:], 16000)
			w.Header().Set("Content-Type", "audio/wav")
			http.ServeContent(w, r, "smoke.wav", time.Time{}, bytes.NewReader(data))
			return
		}
		if r.URL.Path == "/__shell/smoke-result" && r.Method == "POST" {
			body, _ := io.ReadAll(io.LimitReader(r.Body, 4096))
			fmt.Printf("BUILDER_RENDERER_SMOKE %s\n", body)
			w.WriteHeader(http.StatusNoContent)
			return
		}
		if r.URL.Path == "/__shell/smoke.js" {
			w.Header().Set("Content-Type", "text/javascript")
			io.WriteString(w, smokeScript)
			return
		}
		if (r.URL.Path != "/" && r.URL.Path != "/__shell/output/") || r.Method != "GET" {
			next.ServeHTTP(w, r)
			return
		}
		recorded := httptest.NewRecorder()
		next.ServeHTTP(recorded, r)
		for key, values := range recorded.Header() {
			w.Header()[key] = values
		}
		w.Header().Del("Content-Length")
		w.WriteHeader(recorded.Code)
		script := "__shell/smoke.js"
		if r.URL.Path == "/__shell/output/" {
			script = "../../__shell/smoke.js"
		}
		io.WriteString(w, strings.Replace(recorded.Body.String(), "</body>", `<script src="`+script+`" defer></script></body>`, 1))
	})
}

const smokeScript = `
window.addEventListener('DOMContentLoaded', async () => {
  if (document.querySelector('#folder-form')) {
    // Disposable player harnesses exercise first-launch consent too. Never
    // bypass this in production; this entire hook requires the smoketest tag.
    try {
      const directory=document.querySelector('#directory');
      for(let i=0;i<200&&directory.disabled;i++)await new Promise(resolve=>setTimeout(resolve,25));
      if(!directory.value||directory.disabled)throw new Error('output chooser did not become ready');
      document.querySelector('#folder-form').dispatchEvent(new Event('submit',{cancelable:true}));
      const review=document.querySelector('#review');
      for(let i=0;i<200&&review.hidden;i++)await new Promise(resolve=>setTimeout(resolve,25));
      if(review.hidden)throw new Error('destination review failed: '+document.querySelector('#status').textContent);
      if(!document.querySelector('#confirm-label').hidden){const confirm=document.querySelector('#confirm');confirm.checked=true;confirm.dispatchEvent(new Event('change'));}
      const apply=document.querySelector('#apply');
      if(apply.disabled)throw new Error('reviewed destination remains disabled');
      apply.click(); // The real controller persists consent and resumes startup.
    } catch(error) {
      await fetch('../../__shell/smoke-result',{method:'POST',body:'FAIL output chooser: '+error.message});
    }
    return;
  }
  if (!document.querySelector('#build-form')) return; // Bootstrap reloads us.
  const check = (ok, message) => { if (!ok) throw new Error(message); };
  const waitFor = async (test, description) => {
    for (let n = 0; n < 200; n++) {
      const value = test();
      if (value) return value;
      await new Promise(resolve => setTimeout(resolve, 25));
    }
    throw new Error('Timed out waiting for ' + description);
  };
  // A DOM click alone also succeeds on covered or offscreen buttons. Check
  // the renderer's pointer hit target before exercising each handler.
  const reachable = async selector => {
    const el = await waitFor(() => {
      const node = document.querySelector(selector);
      return node?.getClientRects().length && !node.matches(':disabled,[aria-disabled=true]') && node;
    }, selector);
    el.scrollIntoView({block:'center', inline:'nearest', behavior:'instant'});
    await new Promise(resolve => requestAnimationFrame(resolve));
    const r = el.getBoundingClientRect();
    check(el.contains(document.elementFromPoint(r.left+r.width/2, r.top+r.height/2)), 'Covered control: '+selector);
    return el;
  };
  const click = async selector => (await reachable(selector)).click();
  let outcome;
  try {
    check(typeof window.workshopI18n === 'object', 'interface JavaScript did not load');
    check(typeof window.workshopFileInputs === 'object', 'file-input JavaScript did not load');
    const migration = await (await fetch('installation-import/state')).json();
    check(migration.enabled, 'installation import unavailable');
    // Output selection is the only startup flow, even without an import
    // receipt. Importing from another folder remains an explicit action.
    for (let i=0; i<100 && document.querySelector('#import-install-open').hidden; i++) await new Promise(resolve=>setTimeout(resolve,25));
    check(!document.querySelector('#import-install-open').hidden, 'import JavaScript did not load');
    check(!document.querySelector('#import-install').open, 'unexpected duplicate startup import dialog');
    if (document.body.dataset.bigScreen === 'true') await click('#big-screen-desktop');
    await click('#tab-build');
    await click('#import-install-open');
    check(document.querySelector('#import-install').open, 'manual import dialog did not open');
    await click('#import-install-skip');
    check(!document.querySelector('#import-install').open, 'import cancel did not close dialog');
    check((await (await fetch('installation-import/state')).json()).decided === migration.decided, 'cancelling import wrote a decision');
    const status = await (await fetch('status')).json();
    check(status.state === 'idle' && status.install.canRebuild, 'bundled build inputs unavailable');
    const language = await reachable('.workspace-bar #interface-language');
    const original = language.value;
    for (const value of [original === 'fr' ? 'en' : 'fr', original]) {
      language.value = value;
      language.dispatchEvent(new Event('change', {bubbles:true}));
      await waitFor(() => document.documentElement.lang === value && !language.disabled, 'saved language '+value);
    }
    await click('#tab-workshop');
    await click('#workshop-overview [data-nav=assets]');
    check(document.querySelector('#tab-assets').getAttribute('aria-selected') === 'true', 'tab navigation did not execute');
    await click('#asset-track-toggle');
    await click('#asset-track-list [data-track="song-09"]');
    check(document.querySelector('#asset-track-options').hidden &&
      !document.querySelector('.asset-row[data-track="song-09"]').hidden, 'music selection did not change tracks');
    await click('#tab-mods');
    const filter = await reachable('#asset-pack-type');
    filter.value = 'actraiser-manual';
    filter.dispatchEvent(new Event('change', {bubbles:true}));
    await waitFor(() => document.querySelector('#asset-pack-list .mod-empty'), 'empty manual pack list');
    check(document.querySelector('#asset-pack-manual-reader').hidden, 'manual reader offered without an installed manual');
    await click('#asset-pack-clear-filter');
    check(filter.value === 'all', 'clearing the filter did not restore all mods');
    check((await (await fetch('manual')).json()).present === false, 'a manual shipped with the bundle');
    check((await fetch('manual.pdf')).status === 404, 'absent manual did not 404');
    // Install one through the real endpoint, which is the whole player-facing
    // path: a two-page synthetic scan album the in-game reader would accept.
    const page = () => {
      const sof = [0xFF,0xC0,0x00,0x11,0x08, 0x02,0x00, 0x01,0x80, 0x03, 1,0x11,0,2,0x11,1,3,0x11,1];
      const sos = [0xFF,0xDA,0x00,0x08,0x01,0x01,0x00,0x00,0x3F,0x00];
      return [0xFF,0xD8, ...sof, ...sos, ...new Array(512).fill(0x42), 0xFF,0xD9];
    };
    const album = new Uint8Array([...new TextEncoder().encode('%PDF-1.4\n'),
      ...page(), ...page(), ...new TextEncoder().encode('\n%%EOF\n')]);
    const install = new FormData();
    install.append('manual', new Blob([album], {type: 'application/pdf'}), 'manual.pdf');
    const installed = await (await fetch('manual', {method: 'POST', body: install})).json();
    check(installed.present && installed.pages === 2, 'manual install failed: ' + JSON.stringify(installed));
    check(installed.inGame, 'a valid scan album was not accepted for the in-game reader');
    const manual = await fetch('manual.pdf', {headers: {Range: 'bytes=0-31'}});
    check(manual.ok && (await manual.text()).startsWith('%PDF'), 'PDF bytes unavailable');
    // Leave the tree as it was found.
    check((await (await fetch('manual', {method: 'DELETE'})).json()).present === false, 'manual removal failed');
    const audio = new Audio();
    audio.preload = 'auto'; audio.muted = true;
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('WAV decoder timed out')), 10000);
      audio.addEventListener('canplay', () => {clearTimeout(timer); resolve();}, {once:true});
      audio.addEventListener('error', () => {clearTimeout(timer); reject(new Error('WAV decoder unavailable: '+audio.error?.code+' '+audio.error?.message));}, {once:true});
      audio.src = '__shell/smoke.wav'; audio.load();
    });
    check(Math.abs(audio.duration - 1) < .01, 'unexpected WAV duration');
    // Exercise multipart uploads through the private HTTP bridge without a ROM
    // or build: the backend must reject this empty form with a client error.
    const invalid = await fetch('build', {method: 'POST', body: new FormData()});
    check(invalid.status === 400, 'multipart request did not reach build validation');
    await click('#tab-home');
    outcome = 'PASS renderer hit tests, installation import, status, language switching, music selection, mod filtering, manual install/serve/remove, WAV decoding, multipart validation';
    const config = await (await fetch('__shell/smoke-config')).json();
    if (config.fullBuild) {
      const form = new FormData();
      form.append('rom', await (await fetch('__shell/smoke-rom')).blob(), 'test.sfc');
      const start = await fetch('build', {method:'POST', body:form});
      check(start.ok, 'ROM build submission failed');
      await fetch('__shell/smoke-result', {method:'POST',body:'PROGRESS building the game from the private local ROM'});
      let lastPhase = '';
      for (;;) {
        await new Promise(resolve => setTimeout(resolve, 1000));
        const state = await (await fetch('status')).json();
        check(state.state !== 'failed', 'game build failed: ' + state.error);
        if (state.state === 'succeeded') { outcome += ', full game build'; break; }
        const phase = state.progress?.phaseLabel;
        if (phase && phase !== lastPhase) {
          lastPhase = phase;
          await fetch('__shell/smoke-result', {method:'POST',body:'PROGRESS '+phase});
        }
      }
    }
  } catch (error) {
    outcome = 'FAIL ' + error.message;
  }
  await fetch('__shell/smoke-result', {method: 'POST', body: outcome});
  await fetch('close', {method: 'POST'});
});
`
