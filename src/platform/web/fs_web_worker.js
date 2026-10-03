// fs_web_worker.js -- the fetch worker of fs_web.js, shipped as softworld-fs.js
// next to the page: the site's game files in 1 MB pieces, as the page's file
// system asks for them while its thread waits
//
// A piece comes from the browser's Cache Storage, else from the site in a
// Range request (kept then). The cache's keys name the file's size and
// modification time as the site lists them, so a file changed on the site is
// fetched again, and the start removes pieces of files the site no longer
// lists as they were, and pieces kept 30 days. A piece the site answers
// for another file than the one listed (its size or time changed meanwhile)
// is an error, not kept. The answer goes into the shared buffer the page
// waits on: the piece, its length and an error, then the request's number.

'use strict';

const CHUNK = 1 << 20;
const KEEP = 30 * 24 * 3600 * 1000;		// ms a piece is kept
const STORE = 'softworld-files';

let keybase, base, ctl, data, cache = null;
const inflight = new Map();				// key -> the piece's promise

onmessage = async (e) => {
	const m = e.data;
	let bytes = null, error = 0;

	if (m.op == 'init')
		return init(m);
	if (m.op == 'prefetch')
		return get(m).catch(() => {});

	// a read the page waits for
	try {
		bytes = await get(m);
	}
	catch (err) {
		console.warn(`softworld: can't read ${m.path}: ${err.message}`);
		error = 1;
	}
	if (bytes)
		data.set(bytes);
	Atomics.store(ctl, 1, bytes ? bytes.length : 0);
	Atomics.store(ctl, 2, error);
	Atomics.store(ctl, 0, m.seq);
};

function key (path, size, mtime, index) {
	return `${keybase}${encodeURIComponent(path)}?v=${size}-${mtime}&c=${index}`;
}

async function init (m) {
	let manifest;

	base = m.base;
	keybase = new URL('__sw/', base).href;
	ctl = m.ctl;
	data = m.data;
	try {
		cache = await caches.open(STORE);
	}
	catch {
		cache = null;		// none in this browsing context: the session's pieces only
	}

	// the site's list, else the one kept, when the site can't be reached
	try {
		const r = await fetch(m.manifest, {cache: 'no-cache'});
		if (!r.ok)
			throw new Error(`${r.status} ${r.statusText}`);
		manifest = await r.json();
		cache?.put(keybase + 'manifest', new Response(JSON.stringify(manifest))).catch(() => {});
	}
	catch (err) {
		const kept = await cache?.match(keybase + 'manifest');
		if (!kept) {
			postMessage({op: 'error', text: `${m.manifest}: ${err.message}`});
			return;
		}
		console.warn(`softworld: ${m.manifest}: ${err.message}; the files as they were listed last`);
		manifest = await kept.json();
	}
	postMessage({op: 'manifest', manifest});

	// the paks' headers and directories, which the game reads first
	for (const [path, size, mtime] of manifest.files)
		if (/\.pak$/i.test(path)) {
			get({path, size, mtime, index: 0}).catch(() => {});
			if (size > CHUNK)
				get({path, size, mtime, index: Math.floor((size - 1) / CHUNK)}).catch(() => {});
		}
	collect(manifest);
}

// a piece, fetched once however many ask for it at a time
function get (m) {
	const k = key(m.path, m.size, m.mtime, m.index);
	let p = inflight.get(k);

	if (!p) {
		p = load(k, m).finally(() => inflight.delete(k));
		inflight.set(k, p);
	}
	return p;
}

async function load (k, m) {
	const start = m.index * CHUNK, end = Math.min(m.size, start + CHUNK);
	const url = new URL(m.path.split('/').map(encodeURIComponent).join('/'), base);
	let r, bytes, modified, range;

	const kept = await cache?.match(k);
	if (kept)
		return new Uint8Array(await kept.arrayBuffer());
	if (end <= start)
		return new Uint8Array(0);

	// a server's errors and the network's are tried again, 3 times
	for (let tries = 0 ; ; tries++) {
		try {
			r = await fetch(url, {headers: {'Range': `bytes=${start}-${end - 1}`}, cache: 'no-store'});
			if (r.status < 500 || tries == 3)
				break;
		}
		catch (err) {
			if (tries == 3)
				throw err;
		}
		await new Promise((done) => setTimeout(done, 500 << tries));
	}
	if (!r.ok)
		throw new Error(`${r.status} ${r.statusText}`);

	bytes = new Uint8Array(await r.arrayBuffer());
	if (r.status == 206) {
		range = /\/(\d+)\s*$/.exec(r.headers.get('Content-Range') || '');
		if (!range || Number(range[1]) != m.size)
			throw new Error('changed on the site since it was listed');
	}
	else {
		// a server without ranges sent all of it
		if (bytes.length != m.size)
			throw new Error('changed on the site since it was listed');
		bytes = bytes.slice(start, end);
	}
	modified = Date.parse(r.headers.get('Last-Modified') || '');
	if (bytes.length != end - start || modified && Math.abs(modified / 1000 - m.mtime) > 1)
		throw new Error('changed on the site since it was listed');

	cache?.put(k, new Response(bytes, {headers: {'X-SoftWorld-Stored': String(Date.now())}})).catch(() => {});
	return bytes;
}

// the pieces of files no longer listed as they were, and those kept too long
async function collect (manifest) {
	const listed = new Set(manifest.files.map(([path, size, mtime]) => `${encodeURIComponent(path)}?v=${size}-${mtime}`));
	const now = Date.now();

	if (!cache)
		return;
	for (const request of await cache.keys()) {
		const name = request.url.slice(keybase.length);
		const amp = name.lastIndexOf('&c=');

		if (!request.url.startsWith(keybase) || name == 'manifest')
			continue;
		if (amp < 0 || !listed.has(name.slice(0, amp))) {
			cache.delete(request);
			continue;
		}
		const r = await cache.match(request);
		if (!r || now - Number(r.headers.get('X-SoftWorld-Stored') || 0) > KEEP)
			cache.delete(request);
	}
}
