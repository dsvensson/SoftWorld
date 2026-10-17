// net_http_web.js -- the page's side of net_http_web.c: GETs by fetch(), the
// answer's status and headers kept, its body read as it comes, a piece at a
// time, into a queue the game's frame reads; each piece wakes the page's loop.
// An answer that isn't a 2xx fails once its body has come (an API says in it
// why), as the native programs' do.
//
// The page is cross-origin isolated (COEP require-corp): another site's GET
// needs its CORS headers, as the hub's API and demos send, and a header of its
// answer that isn't CORS's safe ones only where the site exposes it.

addToLibrary({
	$WebHTTP__deps: ['$SysWeb'],
	$WebHTTP: {
		RUNNING: 0,
		DONE: 1,
		FAILED: 2,

		fetches: [],
		next: 1,

		start(url, headers) {
			var id = WebHTTP.next++, init, host;
			var f = {state: WebHTTP.RUNNING, status: 0, headers: null, error: '', chunks: [], done: false,
				controller: new AbortController()};

			const fail = (why) => {
				if (f.state != WebHTTP.RUNNING)
					return;
				f.state = WebHTTP.FAILED;
				f.error = why;
				SysWeb.wake();
			};
			WebHTTP.fetches[id] = f;
			try {
				host = new URL(url).host;
				init = {headers: new Headers(), mode: 'cors', cache: 'no-store', signal: f.controller.signal};
				for (const line of headers.split(/\r?\n/)) {
					var colon = line.indexOf(':');

					if (!line)
						continue;
					if (colon <= 0)
						throw new Error(`"${line.slice(0, 64)}" isn't a header`);
					init.headers.append(line.slice(0, colon).trim(), line.slice(colon + 1).trim());
				}
			}
			catch (e) {
				fail(e.message || `${url} isn't a URL`);
				return id;
			}

			fetch(url, init).then((r) => {
				var reader = r.body ? r.body.getReader() : null;

				f.status = r.status;
				f.headers = r.headers;
				const end = () => {
					if (r.ok)
						f.done = true;
					else
						fail(`${host} answered ${r.status}`);
					SysWeb.wake();
				};
				const pump = () => reader.read().then(({done, value}) => {
					if (done)
						return end();
					f.chunks.push(value);
					SysWeb.wake();
					pump();
				}, (e) => fail(`${host} stopped answering (${e.message})`));
				SysWeb.wake();
				if (reader)
					pump();
				else
					end();
			}, (e) => fail(`${host} couldn't be reached (${e.message})`));
			return id;
		},
	},

	// a GET started: its number
	web_http_start__deps: ['$WebHTTP', '$UTF8ToString'],
	web_http_start: (url, headers) => WebHTTP.start(UTF8ToString(url), headers ? UTF8ToString(headers) : ''),

	// its body's bytes come since, up to max: 0 when none have
	web_http_read__deps: ['$WebHTTP'],
	web_http_read: (id, buf, max) => {
		var f = WebHTTP.fetches[id], done = 0, m, n;

		if (!f)
			return 0;
		while (done < max && f.chunks.length) {
			m = f.chunks[0];
			n = Math.min(m.length, max - done);
			HEAPU8.set(m.subarray(0, n), buf + done);
			done += n;
			if (n == m.length)
				f.chunks.shift();
			else
				f.chunks[0] = m.subarray(n);
		}
		return done;
	},

	// 0 running, 1 done (its body all come and read), 2 failed
	web_http_state__deps: ['$WebHTTP'],
	web_http_state: (id) => {
		var f = WebHTTP.fetches[id];

		if (!f || f.state == WebHTTP.FAILED)
			return WebHTTP.FAILED;
		return f.done && !f.chunks.length ? WebHTTP.DONE : WebHTTP.RUNNING;
	},

	web_http_status__deps: ['$WebHTTP'],
	web_http_status: (id) => WebHTTP.fetches[id]?.status ?? 0,

	// a header of its answer into out: its length, -1 if it has none (or the
	// site doesn't expose it)
	web_http_header__deps: ['$WebHTTP', '$UTF8ToString', '$stringToUTF8'],
	web_http_header: (id, name, out, size) => {
		var f = WebHTTP.fetches[id], value = f?.headers?.get(UTF8ToString(name));

		if (value === null || value === undefined)
			return -1;
		return stringToUTF8(value, out, size);
	},

	web_http_error__deps: ['$WebHTTP', '$stringToUTF8'],
	web_http_error: (id, out, size) => {
		stringToUTF8(WebHTTP.fetches[id]?.error ?? 'no such GET', out, size);
	},

	web_http_close__deps: ['$WebHTTP'],
	web_http_close: (id) => {
		var f = WebHTTP.fetches[id];

		if (!f)
			return;
		f.state = WebHTTP.FAILED;		// nothing more is told
		f.controller.abort();
		delete WebHTTP.fetches[id];
	},
});
