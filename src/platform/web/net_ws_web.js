// net_ws_web.js -- the page's side of net_ws_web.c: its WebSockets, a packet
// a binary message (datagrams), or a stream of them (QTV)
//
// A datagram socket's messages go into one queue for all of them, which the
// game's frame reads with the socket's number; a stream's into its own. What
// is sent before a socket opens waits for it. A message wakes the page's loop,
// as a packet wakes the other systems' waits.
//
// Later, other transports by the URL's scheme (an RTCDataChannel, unordered
// and unreliable) can take a socket's place here, the game's side as it is.

addToLibrary({
	$WebNet__deps: ['$SysWeb'],
	$WebNet: {
		CONNECTING: 0,
		OPEN: 1,
		CLOSED: 2,
		QUEUEMAX: 1024,			// datagrams waiting for the game, the oldest dropped
		PENDINGMAX: 64,			// sent before the socket opened

		sockets: [],
		queue: [],				// [socket, bytes]

		open(url, protocol, stream) {
			var id = WebNet.sockets.length, ws, s;

			try {
				ws = protocol ? new WebSocket(url, protocol) : new WebSocket(url);
			}
			catch (e) {
				return null;		// a ws:// from an https page, or a bad URL
			}
			ws.binaryType = 'arraybuffer';
			s = {ws, stream, state: WebNet.CONNECTING, code: 0, pending: [], received: []};
			ws.onopen = () => {
				s.state = WebNet.OPEN;
				for (const p of s.pending)
					ws.send(p);
				s.pending = [];
			};
			ws.onmessage = (e) => {
				if (typeof e.data == 'string')
					return;
				if (stream)
					s.received.push(new Uint8Array(e.data));
				else {
					if (WebNet.queue.length >= WebNet.QUEUEMAX)
						WebNet.queue.shift();
					WebNet.queue.push([id, new Uint8Array(e.data)]);
				}
				SysWeb.wake();
			};
			ws.onclose = (e) => {
				s.state = WebNet.CLOSED;
				s.code = e.code;
				SysWeb.wake();
			};
			WebNet.sockets[id] = s;
			return id;
		},
	},

	// a socket's number, -1 if it can't be opened
	web_ws_open__deps: ['$WebNet', '$UTF8ToString'],
	web_ws_open: (url, protocol, stream) => {
		var id = WebNet.open(UTF8ToString(url), protocol ? UTF8ToString(protocol) : '', !!stream);

		return id === null ? -1 : id;
	},

	web_ws_send__deps: ['$WebNet'],
	web_ws_send: (id, data, length) => {
		var s = WebNet.sockets[id], bytes;

		if (!s || s.state == WebNet.CLOSED)
			return false;
		bytes = HEAPU8.slice(data, data + length);
		if (s.state == WebNet.OPEN)
			s.ws.send(bytes);
		else if (s.pending.length < WebNet.PENDINGMAX)
			s.pending.push(bytes);
		return true;
	},

	// 0 connecting, 1 open, 2 closed; how it closed in code
	web_ws_state__deps: ['$WebNet'],
	web_ws_state: (id, code) => {
		var s = WebNet.sockets[id];

		if (!s)
			return WebNet.CLOSED;
		{{{ makeSetValue('code', 0, 's.code', 'i32') }}};
		return s.state;
	},

	// the next datagram of any socket: its length, and the socket's number in id;
	// 0 when none waits, -1 for one longer than max (dropped)
	web_ws_recv__deps: ['$WebNet'],
	web_ws_recv: (buf, max, id) => {
		var next = WebNet.queue.shift();

		if (!next)
			return 0;
		{{{ makeSetValue('id', 0, 'next[0]', 'i32') }}};
		if (next[1].length > max)
			return -1;
		HEAPU8.set(next[1], buf);
		return next[1].length;
	},

	// a stream's bytes, up to max: 0 when none have come, -1 when it closed
	web_ws_read__deps: ['$WebNet'],
	web_ws_read: (id, buf, max) => {
		var s = WebNet.sockets[id], done = 0, m, n;

		if (!s)
			return -1;
		while (done < max && s.received.length) {
			m = s.received[0];
			n = Math.min(m.length, max - done);
			HEAPU8.set(m.subarray(0, n), buf + done);
			done += n;
			if (n == m.length)
				s.received.shift();
			else
				s.received[0] = m.subarray(n);
		}
		if (!done && s.state == WebNet.CLOSED)
			return -1;
		return done;
	},

	web_ws_close__deps: ['$WebNet'],
	web_ws_close: (id) => {
		var s = WebNet.sockets[id];

		if (!s)
			return;
		s.ws.onclose = s.ws.onmessage = s.ws.onopen = null;
		s.ws.close();
		delete WebNet.sockets[id];
		WebNet.queue = WebNet.queue.filter((e) => e[0] != id);
	},

	web_page_secure: () => location.protocol == 'https:',
});
