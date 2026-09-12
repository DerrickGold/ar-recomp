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
  let outcome;
  try {
    check(typeof window.workshopI18n === 'object', 'interface JavaScript did not load');
    check(typeof window.workshopFileInputs === 'object', 'file-input JavaScript did not load');
    const migration = await (await fetch('installation-import/state')).json();
    check(migration.enabled, 'installation import unavailable');
    // Exercise the real first-launch prompt, then persist a deliberate skip.
    // Warm launches must retain the manual import action without prompting.
    for (let i=0; i<100 && document.querySelector('#import-install-open').hidden; i++) await new Promise(resolve=>setTimeout(resolve,25));
    check(!document.querySelector('#import-install-open').hidden, 'import JavaScript did not load');
    if (!migration.decided) {
      check(document.querySelector('#import-install').open, 'first-launch import dialog missing');
      document.querySelector('#import-install-skip').click();
      for (let i=0; i<100 && document.querySelector('#import-install').open; i++) await new Promise(resolve=>setTimeout(resolve,25));
      check(!document.querySelector('#import-install').open, 'import skip did not close dialog');
      check((await (await fetch('installation-import/state')).json()).decided, 'import decision not persisted');
    }
    const status = await (await fetch('status')).json();
    check(status.state === 'idle' && status.install.canRebuild, 'bundled build inputs unavailable');
    const preferences = await fetch('interface/preferences', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({language: window.workshopI18n.locale})});
    check(preferences.ok, 'interface preferences unavailable');
    await preferences.json();
    document.querySelector('#tab-assets').click();
    check(document.querySelector('#tab-assets').getAttribute('aria-selected') === 'true', 'tab navigation did not execute');
    document.querySelector('#tab-manual').click();
    check(document.querySelector('#manual-frame').getAttribute('src') === 'manual.pdf', 'manual navigation failed');
    const manual = await fetch('manual.pdf', {headers: {Range: 'bytes=0-31'}});
    check(manual.ok && (await manual.text()).startsWith('%PDF'), 'PDF bytes unavailable');
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
    document.querySelector('#tab-home').click();
    outcome = 'PASS JavaScript, installation import prompt, status, preferences, tabs, PDF bytes, WAV decoding, multipart validation';
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
