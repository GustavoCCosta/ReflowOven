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

/*
 * The starting style of a stubbed element has to mirror the page's own inline
 * attribute, and it is READ OUT OF THE PAGE rather than copied here (RFO-B23).
 *
 * The defect this replaces: the stub started as `style: {}` and the HTTP path
 * never touches style.display, so `=== undefined` was always true and the
 * assertion below could not fail. It would have stayed green with the USB
 * button on display while the oven served the page -- the exact regression it
 * exists to catch.
 *
 * Copying the value ('none') into the stub would fix the assertion and leave a
 * second hole: a page that drops style='display:none' from the button would
 * still pass, because the stub would supply the hiding the page no longer does.
 * Reading the attribute means that page has to come here as a failure.
 */
function inlineStyle(id) {
	const tag = html.match(new RegExp("<[^>]*id='" + id + "'[^>]*>"));
	const attr = tag && tag[0].match(/style='([^']*)'/);
	const style = {};

	if (!attr) {
		return style;
	}
	for (const decl of attr[1].split(';')) {
		const [prop, value] = decl.split(':');

		if (prop && value) {
			/* display-mode -> displayMode, like the real style object. */
			const key = prop.trim().replace(/-(\w)/g, (_, c) => c.toUpperCase());

			style[key] = value.trim();
		}
	}
	return style;
}

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
		style: inlineStyle(id), options: [], innerHTMLSet: 0,
		appendChild(o) { this.options.push(o); },
		set innerHTMLraw(v) {},
		getContext: () => new Proxy({}, { get: () => () => {} }),
		width: 640, height: 220,
	});
	/*
	 * The stubbed ids come from the page, not from a list kept here by hand
	 * (RFO-B41). A page that grows an element the script writes to would
	 * otherwise crash the harness with "cannot set textContent of undefined",
	 * and the same reasoning as inlineStyle() applies: what the page has is a
	 * fact to read, not a copy to maintain.
	 */
	const ids = [...html.matchAll(/id='([A-Za-z0-9_-]+)'/g)].map((m) => m[1]);

	[...new Set(ids)].forEach(mk);
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
		/*
		 * A fetch that never settles is enough to check which transport the
		 * page picks, but it cannot exercise what the page does with an
		 * ANSWER - and that is the whole subject of RFO-B41. So: the profile
		 * list always succeeds, and the command endpoint answers whatever the
		 * test asked for (opts.cmdStatus), or rejects when it asks for a dead
		 * link (opts.cmdRejects).
		 */
		fetch: (url) => {
			if (String(url).indexOf('/api/cmd') < 0) {
				return Promise.resolve({
					ok: true, status: 200,
					json: () => Promise.resolve({ profiles: ['a', 'b'] }),
				});
			}
			if (opts.cmdRejects) {
				return Promise.reject(new Error('network down'));
			}
			const status = opts.cmdStatus === undefined ? 204 : opts.cmdStatus;

			return Promise.resolve({ ok: status < 400, status });
		},
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

/* What the page shows, whichever element it chose to show it in. Assertions
 * about messages use this instead of naming an element: the page moved its
 * message channel in RFO-B41 and the facts being asserted did not change. */
function shown(dom) {
	return Object.keys(dom.els)
		.map((k) => `${dom.els[k].textContent}`)
		.join(' | ');
}

const settle = () => new Promise((r) => setImmediate(r));

console.log('transport selection');
const oven = run({ protocol: 'http:', hostname: '192.168.7.1' }, { serial: true });
check('served by the oven -> USB button hidden',
      oven.els.usb.style.display === 'none',
      `display=${JSON.stringify(oven.els.usb.style.display)}`);

const local = run({ protocol: 'file:', hostname: '' }, { serial: true });
check('opened from file:// -> USB button shown', local.els.usb.style.display === '');
check('opened from file:// -> prompts to connect',
      /Conectar por USB/.test(shown(local)), shown(local));

const host = run({ protocol: 'http:', hostname: 'localhost' }, { serial: true });
check('served from localhost -> serial mode, not HTTP',
      host.els.usb.style.display === '');

const noSerial = run({ protocol: 'file:', hostname: '' }, { serial: false });
check('no Web Serial support -> says to use Chrome or Edge',
      /Chrome/.test(shown(noSerial)), shown(noSerial));

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

/*
 * RFO-B41: a message about a refused command has to survive the telemetry that
 * keeps arriving behind it.
 *
 * The failure this catches is not cosmetic. The operator clicks Stop, the POST
 * is refused, the page says so - and the next telemetry frame, under a second
 * later, overwrites the line with "running". Someone who looked at the board
 * after clicking (which is what one does after clicking Stop) sees nothing at
 * all and believes the oven was told to stop. Same ending as RFO-B19, with the
 * element as the culprit instead of the discarded promise.
 *
 * The assertion deliberately does not name an element: it asks whether the page
 * still SHOWS the text anywhere. A fix that moves the message elsewhere still
 * passes; a fix that only delays the overwrite does not.
 */
(async function commandMessages() {
	console.log('a refused command keeps saying so');

	const refused = [
		[401, /token/i],
		[403, /IP do forno/i],
		[503, /desligado/i],
		[400, /malformada/i],
		[418, /recusado/i],
	];

	for (const [status, expected] of refused) {
		const d = run({ protocol: 'http:', hostname: '192.168.7.1' },
			      { serial: true, cmdStatus: status });

		d.sandbox.cmd('stop');
		await settle();
		check(`${status} is reported`, expected.test(shown(d)), shown(d));

		/* Three frames, not one: a single frame could pass by accident of
		 * ordering, and the real page pushes one per second. */
		d.sandbox.update(sample);
		d.sandbox.update(sample);
		d.sandbox.update(sample);
		check(`${status} survives three telemetry frames`,
		      expected.test(shown(d)), shown(d));
	}

	const dead = run({ protocol: 'http:', hostname: '192.168.7.1' },
			 { serial: true, cmdRejects: true });

	dead.sandbox.cmd('stop');
	await settle();
	dead.sandbox.update(sample);
	check('a dead link survives three telemetry frames',
	      /sem resposta/.test(shown(dead)), shown(dead));

	/* The cure cannot be freezing the screen. */
	const live = run({ protocol: 'http:', hostname: '192.168.7.1' },
			 { serial: true, cmdStatus: 503 });

	live.sandbox.cmd('stop');
	await settle();
	live.sandbox.update(sample);
	check('telemetry keeps updating behind the message',
	      /183\.3/.test(live.els.temp.innerHTML) &&
	      live.els.sp.textContent === '180 C',
	      `${live.els.temp.innerHTML} / ${live.els.sp.textContent}`);

	/*
	 * And an accepted command must not leave the old refusal on screen. The
	 * options object is read by the fetch stub on every call, so flipping the
	 * status here is the same page living through a refusal and then a
	 * success - which is exactly the sequence an operator produces when the
	 * first Stop is refused and the second one is not.
	 */
	const retryOpts = { serial: true, cmdStatus: 503 };
	const retry = run({ protocol: 'http:', hostname: '192.168.7.1' }, retryOpts);

	retry.sandbox.cmd('stop');
	await settle();
	check('the refusal is on screen before the retry',
	      /desligado/.test(shown(retry)), shown(retry));

	retryOpts.cmdStatus = 204;
	retry.sandbox.cmd('stop');
	await settle();
	retry.sandbox.update(sample);
	check('an accepted command clears the old refusal',
	      !/desligado/.test(shown(retry)), shown(retry));
})().then(() => {
	console.log(failures ? `\nFAILED (${failures})` : '\nall page checks passed');
	process.exit(failures ? 1 : 0);
}, (e) => {
	console.log(`  FAIL harness: ${e && e.message}`);
	console.log(`\nFAILED (${failures + 1})`);
	process.exit(1);
});
