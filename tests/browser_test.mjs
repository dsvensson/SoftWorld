// browser_test.mjs -- runs a test page (linked with --emrun) in a headless
// Chrome of its own, for ctest:
//
//   node browser_test.mjs <chrome> <test.html> [seconds]
//
// The page's directory is served on localhost with the cross-origin isolation
// its shared memory needs. Chrome runs with a profile of its own (a browser
// apart from any other running), headless, drawing in software (SwiftShader),
// with sound that plays without a gesture and is muted, and only that browser
// is ended. What the page prints (emrun's reports) is printed in order, and
// the page's exit code is this one's: 1 if it doesn't exit in time, 77 (skipped)
// if Chrome doesn't run it.

import child_process from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';

const [chrome, page, seconds = '60'] = process.argv.slice(2);
const root = path.resolve(path.dirname(page));
const types = {'.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm'};
const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'softworld-test-'));
const lines = new Map();
let next = 1, browser = null, finished = false;

// emrun's reports: ^out^n^text, ^err^n^text (numbered, posted at once), ^exit^code
function report (body) {
	const exit = /^\^exit\^(-?\d+)/.exec(body);
	const line = /^\^(out|err)\^(\d+)\^(.*)$/s.exec(body);

	if (exit)
		finish(Number(exit[1]));
	else if (line) {
		lines.set(Number(line[2]), [line[1], decodeURIComponent(line[3])]);
		for (let l ; (l = lines.get(next)) ; lines.delete(next++))
			(l[0] == 'out' ? process.stdout : process.stderr).write(l[1] + '\n');
	}
}

function finish (code) {
	if (finished)
		return;
	finished = true;
	server.close();
	if (browser && browser.exitCode === null) {
		// this browser alone, with its own processes
		if (process.platform == 'win32')
			child_process.spawnSync('taskkill', ['/PID', String(browser.pid), '/T', '/F'], {stdio: 'ignore'});
		else
			try {
				process.kill(-browser.pid, 'SIGKILL');
			}
			catch {}
	}
	// its profile, once it has let go of it
	for (let tries = 0 ; tries < 20 ; tries++)
		try {
			fs.rmSync(profile, {recursive: true, force: true});
			break;
		}
		catch {
			Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 100);
		}
	process.exit(code);
}

const server = http.createServer((request, response) => {
	const url = new URL(request.url, 'http://localhost');
	let body = '';

	if (request.method == 'POST') {
		request.on('data', (data) => body += data);
		request.on('end', () => {
			response.end();
			report(body);
		});
		return;
	}
	const file = path.resolve(root, '.' + decodeURIComponent(url.pathname));
	if (!file.startsWith(root + path.sep)) {
		response.writeHead(404).end();
		return;
	}
	fs.readFile(file, (err, data) => {
		if (err) {
			response.writeHead(404).end();
			return;
		}
		response.writeHead(200, {
			'Content-Type': types[path.extname(file)] ?? 'application/octet-stream',
			'Cross-Origin-Opener-Policy': 'same-origin',
			'Cross-Origin-Embedder-Policy': 'require-corp',
			'Cross-Origin-Resource-Policy': 'same-origin',
			'Cache-Control': 'no-store',
		}).end(data);
	});
});

server.listen(0, '127.0.0.1', () => {
	const url = `http://localhost:${server.address().port}/${encodeURIComponent(path.basename(page))}`;

	browser = child_process.spawn(chrome, [`--user-data-dir=${profile}`, '--headless=new', '--no-first-run',
		'--no-default-browser-check', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
		'--autoplay-policy=no-user-gesture-required', '--mute-audio', url],
		{stdio: 'ignore', detached: process.platform != 'win32'});
	browser.on('error', (err) => {
		console.log(`${chrome}: ${err.message}, skipped`);
		finish(77);
	});
	// the page closes its window as it exits, and its report may come after
	browser.on('exit', () => setTimeout(() => {
		if (!finished) {
			console.log('Chrome ended before the page did, skipped');
			finish(77);
		}
	}, 1000));
	setTimeout(() => {
		console.log(`the page didn't finish in ${seconds} s`);
		finish(1);
	}, Number(seconds) * 1000);
});
