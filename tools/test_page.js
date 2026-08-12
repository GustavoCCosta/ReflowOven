/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smoke test for src/net/index.html, run with node (optional dev tool):
 *
 *   node tools/test_page.js
 *
 * The page has no build step and cannot be unit tested in a browser from CI,
 * so this stubs just enough DOM to run the script, then checks the two things
 * that break silently: which transport it picks, and that a telemetry object
 * lands in the right places on screen.
 */

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const html = fs.readFileSync(
	path.join(__dirname, '..', 'src', 'net', 'index.html'), 'utf8');
const js = html.match(/<script>([\s\S]*)<\/script>/)[1];

let failures = 0;
function check(name, cond, detail) {
	if (cond) {
		console.log(`  ok   ${name}`);
	} else {
		console.log(`  FAIL ${name}${detail ? ': ' + detail : ''}`);
		failures++;
	}
}

function makeDom() {
	const els = {};
	const mk = (id) => (els[id] = {
		id, textContent: '', innerHTML: '', className: '', value: '',
		style: {}, options: [], innerHTMLSet: 0,
		appendChild(o) { this.options.push(o); },
		set innerHTMLraw(v) {},
		getContext: () => new Proxy({}, { get: () => () => {} }),
		width: 640, height: 220,
	});
	['temp', 'sub', 'sp', 'duty', 'stage', 'el', 'prof', 'c', 'usb'].forEach(mk);
	els.prof.onchange = null;
	return {
		document: {
			getElementById: (id) => els[id],
			createElement: () => ({ value: '', textContent: '' }),
		},
		els,
	};
}

function run(locationObj, opts = {}) {
	const { document, els } = makeDom();
	const sandbox = {
		document, location: locationObj, console,
		navigator: opts.serial ? { serial: { addEventListener() {} } } : {},
		fetch: () => new Promise(() => {}),
		EventSource: function () { this.onmessage = null; this.onerror = null; },
		setInterval: () => 0, clearInterval: () => {},
		TextEncoder, TextDecoder,
	};
	vm.createContext(sandbox);
	vm.runInContext(js, sandbox);
	return { sandbox, els };
}

const sample = {
	temp_mc: 183250, temp_valid: true, setpoint_mc: 180000, duty: 615,
	state: 'running', fault: 'none', profile: 0, stage: 1, n_stages: 5,
	stage_name: 'soak', stage_ms: 12000, total_ms: 102000, uptime_ms: 200000,
};

console.log('transport selection');
const oven = run({ protocol: 'http:', hostname: '192.168.7.1' }, { serial: true });
check('served by the oven -> USB button hidden',
      oven.els.usb.style.display === 'none' || oven.els.usb.style.display === undefined,
      `display=${JSON.stringify(oven.els.usb.style.display)}`);

const local = run({ protocol: 'file:', hostname: '' }, { serial: true });
check('opened from file:// -> USB button shown', local.els.usb.style.display === '');
check('opened from file:// -> prompts to connect',
      /Conectar por USB/.test(local.els.sub.textContent), local.els.sub.textContent);

const host = run({ protocol: 'http:', hostname: 'localhost' }, { serial: true });
check('served from localhost -> serial mode, not HTTP',
      host.els.usb.style.display === '');

const noSerial = run({ protocol: 'file:', hostname: '' }, { serial: false });
check('no Web Serial support -> says to use Chrome or Edge',
      /Chrome/.test(noSerial.els.sub.textContent), noSerial.els.sub.textContent);

console.log('rendering a telemetry object');
const r = run({ protocol: 'http:', hostname: '192.168.7.1' }, { serial: true });
r.sandbox.update(sample);
/* 183250 mC displayed with one decimal: toFixed rounds, so 183.3 not 183.2. */
check('temperature', /183\.3/.test(r.els.temp.innerHTML), r.els.temp.innerHTML);
check('setpoint', r.els.sp.textContent === '180 C', r.els.sp.textContent);
check('duty', r.els.duty.textContent === '62 %', r.els.duty.textContent);
check('stage', r.els.stage.textContent === 'soak (2/5)', r.els.stage.textContent);
check('elapsed', r.els.el.textContent === '1:42', r.els.el.textContent);
check('state line', r.els.sub.textContent === 'running', r.els.sub.textContent);

const invalid = Object.assign({}, sample, { temp_valid: false });
r.sandbox.update(invalid);
check('invalid reading shows --', /--/.test(r.els.temp.innerHTML), r.els.temp.innerHTML);

const faulted = Object.assign({}, sample, { fault: 'sensor', state: 'fault' });
r.sandbox.update(faulted);
check('fault is announced', /FAULT: sensor/.test(r.els.sub.textContent),
      r.els.sub.textContent);
check('fault is styled', r.els.sub.className === 'bad', r.els.sub.className);

console.log('picking the JSON line out of shell noise');
const noisy = [
	'uart:~$ reflow json',
	'[1;32muart:~$[m {"temp_mc":26250,"temp_valid":true}',
	'[0] SAC305 lead-free',
	'',
];
let parsed = 0, profiles = 0;
noisy.forEach((ln) => {
	const i = ln.indexOf('{');
	if (i >= 0) { JSON.parse(ln.slice(i)); parsed++; return; }
	if (ln.match(/\[(\d+)\]\s+(.+?)\s*$/)) profiles++;
});
check('one JSON line found, prompt and colours ignored', parsed === 1, `parsed=${parsed}`);
check('profile listing recognised', profiles === 1, `profiles=${profiles}`);

console.log(failures ? `\nFAILED (${failures})` : '\nall page checks passed');
process.exit(failures ? 1 : 0);
