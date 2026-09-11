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

/*
 * A porta Web Serial stubada (RFO-T49). O harness ja fingia `navigator.serial`
 * o bastante para a pagina escolher o transporte; o que faltava era poder ABRIR
 * e FECHAR uma sessao, e alimentar bytes no leitor.
 *
 * A propriedade que isso existe para medir e de ORDEM: o laco de leitura chama
 * `usbClose()` DEPOIS de sair do `while`, entao nenhum quadro em buffer chega
 * depois para apagar o `desconectado`. Ordem e o que muda sem ninguem notar -
 * um `await` movido, um `usbClose()` antecipado - e ai a mensagem que diz ao
 * operador que ele perdeu o cabo passa a ser apagada pelo ultimo quadro.
 *
 * Por isso o leitor e uma fila que o teste controla: da para enfileirar um
 * quadro de telemetria E o fim da porta, e ver quem ganha.
 */
function makeSerial(opts) {
	const fila = [];
	let resolveEspera = null;

	const leitor = {
		read() {
			if (fila.length) {
				return Promise.resolve(fila.shift());
			}
			/* Sem nada na fila: fica pendente ate o teste empurrar. */
			return new Promise((r) => { resolveEspera = r; });
		},
		releaseLock() {},
	};

	const empurra = (item) => {
		if (resolveEspera) {
			const r = resolveEspera;
			resolveEspera = null;
			r(item);
		} else {
			fila.push(item);
		}
	};

	const porta = {
		open: () => (opts.usbOpenRejects
			? Promise.reject(new Error('acesso negado'))
			: Promise.resolve()),
		writable: { getWriter: () => ({ write() {} }) },
		readable: { getReader: () => leitor },
		/* API do teste, nao da pagina. */
		feed: (texto) => empurra({
			done: false,
			value: new TextEncoder().encode(texto),
		}),
		finish: () => empurra({ done: true }),
	};

	return {
		addEventListener() {},
		requestPort: () => (opts.usbPortRejects
			? Promise.reject(new Error('nenhuma porta escolhida'))
			: Promise.resolve(porta)),
		/* O teste alcanca a porta por aqui. */
		port: porta,
	};
}

function run(locationObj, opts = {}) {
	const { document, els } = makeDom();
	const sandbox = {
		document, location: locationObj, console,
		navigator: opts.serial
			? { serial: makeSerial(opts) }
			: {},
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
		/*
		 * The stub hands the instance back (RFO-B42). Without it the harness
		 * could only watch what the page does with data that ARRIVES, and the
		 * transport messages are written from the error path - the one the
		 * EventSource fires by itself on every reconnection attempt, with no
		 * operator action behind it.
		 */
		EventSource: function () {
			this.onmessage = null;
			this.onerror = null;
			sandbox.lastEs = this;
		},
		setInterval: () => 0, clearInterval: () => {},
		TextEncoder, TextDecoder,
	};
	vm.createContext(sandbox);
	vm.runInContext(js, sandbox);
	return { sandbox, els, serial: sandbox.navigator.serial };
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

	/*
	 * RFO-B42: a message about the TRANSPORT has the opposite lifetime, and
	 * telemetry arriving is the proof that settles it.
	 *
	 * es.onerror fires on its own, on every reconnection attempt, with no
	 * operator action behind it. RFO-B41 gave command messages an element
	 * update() never touches - which is right for a refusal and wrong here:
	 * the warning outlived the outage and the page kept saying the link was
	 * gone while telemetry ran normally behind it. That is RFO-B19 mirrored:
	 * the operator who needs to stop the oven reads 'no link', does not try
	 * the button, and goes looking for another way - and that time is time
	 * with the element on.
	 */
	console.log('a transport warning goes away when the link comes back');

	const t = run({ protocol: 'http:', hostname: '192.168.7.1' }, { serial: true });

	check('the page exposes an EventSource to fail',
	      t.sandbox.lastEs && typeof t.sandbox.lastEs.onerror === 'function',
	      `lastEs=${t.sandbox.lastEs && typeof t.sandbox.lastEs.onerror}`);

	t.sandbox.lastEs.onerror();
	check('a lost link is announced', /link lost/.test(shown(t)), shown(t));

	/* Three frames, the same budget the command assertions use: the link is
	 * demonstrably back, so nothing may still be claiming it is not. */
	t.sandbox.update(sample);
	t.sandbox.update(sample);
	t.sandbox.update(sample);
	check('the warning is gone once telemetry runs again',
	      !/link lost/.test(shown(t)), shown(t));
	check('and the state line is live', t.els.sub.textContent === 'running',
	      t.els.sub.textContent);

	/*
	 * The two classes have to coexist. This is the sequence that a fix by
	 * sharing one element cannot pass: a refusal on screen, then an outage,
	 * then the link back. The refusal is still unanswered - only the operator
	 * answers it - and the outage is over.
	 */
	const both = run({ protocol: 'http:', hostname: '192.168.7.1' },
			 { serial: true, cmdStatus: 503 });

	both.sandbox.cmd('stop');
	await settle();
	both.sandbox.lastEs.onerror();
	check('a refusal and a lost link are both on screen',
	      /desligado/.test(shown(both)) && /link lost/.test(shown(both)),
	      shown(both));

	both.sandbox.update(sample);
	both.sandbox.update(sample);
	both.sandbox.update(sample);
	check('telemetry clears the transport warning',
	      !/link lost/.test(shown(both)), shown(both));
	check('telemetry does NOT clear the command refusal (RFO-B41 holds)',
	      /desligado/.test(shown(both)), shown(both));

	/* And a transport warning must not be mistaken for an answer to a
	 * command: the outage does not wipe what the oven refused. */
	const keep = run({ protocol: 'http:', hostname: '192.168.7.1' },
			 { serial: true, cmdStatus: 401 });

	keep.sandbox.cmd('stop');
	await settle();
	keep.sandbox.lastEs.onerror();
	check('a lost link does not wipe the refusal either',
	      /token/i.test(shown(keep)), shown(keep));

	/*
	 * RFO-T49: o caminho Web Serial, que o RFO-B42 classificou e nenhum teste
	 * exercitava. Quatro mensagens de transporte vivem so aqui.
	 */
	console.log('the Web Serial transport messages');

	const usbFile = { protocol: 'file:', hostname: '' };

	/* Antes de qualquer acao: a dica de carga inicial esta na tela. */
	const dica = run(usbFile, { serial: true });
	check('the load hint is up before any action',
	      /Conectar por USB/.test(shown(dica)), shown(dica));

	/* requestPort recusado -> 'nao conectou', e a dica some. */
	const recusa = run(usbFile, { serial: true, usbPortRejects: true });
	await recusa.sandbox.usbConnect();
	await settle();
	check('a refused port says so',
	      /nao conectou/.test(shown(recusa)), shown(recusa));
	check('and the load hint is gone once the operator acted',
	      !/Conectar por USB/.test(shown(recusa)), shown(recusa));

	/* open() recusado -> mesma mensagem, outro ponto de falha. */
	const semAbrir = run(usbFile, { serial: true, usbOpenRejects: true });
	await semAbrir.sandbox.usbConnect();
	await settle();
	check('a port that refuses to open says so',
	      /nao conectou/.test(shown(semAbrir)), shown(semAbrir));

	/* Conexao boa: anuncia, e telemetria limpa o anuncio (classe transporte). */
	const usb = run(usbFile, { serial: true });
	await usb.sandbox.usbConnect();
	await settle();
	check('a connected port announces itself',
	      /conectado pela porta USB/.test(shown(usb)), shown(usb));

	usb.sandbox.update(sample);
	check('telemetry clears the connected announcement',
	      !/conectado pela porta USB/.test(shown(usb)), shown(usb));

	/*
	 * O item central. Enfileira um quadro de telemetria E o fim da porta, nessa
	 * ordem, e ve quem ganha.
	 *
	 * Com a ordem de hoje - laco processa, sai, e SO ENTAO usbClose() - o quadro
	 * e consumido antes e o 'desconectado' fica. Com o usbClose() antecipado, o
	 * quadro em buffer chega depois e apaga a mensagem que diz ao operador que
	 * ele perdeu o cabo. Injetar o quadro e o que separa os dois casos: so
	 * fechar a porta e olhar a tela mediria o caso facil.
	 */
	const queda = run(usbFile, { serial: true });
	await queda.sandbox.usbConnect();
	await settle();

	queda.serial.port.feed(JSON.stringify(sample) + '\n');
	queda.serial.port.finish();
	for (let i = 0; i < 6; i++) {
		await settle();
	}

	check('a frame buffered before the close still renders',
	      /183\.3/.test(queda.els.temp.innerHTML), queda.els.temp.innerHTML);
	check('and the disconnect notice survives it',
	      /desconectado/.test(shown(queda)), shown(queda));
	check('the USB button comes back after a disconnect',
	      queda.els.usb.style.display === '', queda.els.usb.style.display);
})().then(() => {
	console.log(failures ? `\nFAILED (${failures})` : '\nall page checks passed');
	process.exit(failures ? 1 : 0);
}, (e) => {
	console.log(`  FAIL harness: ${e && e.message}`);
	console.log(`\nFAILED (${failures + 1})`);
	process.exit(1);
});
