// net_ws_web.js -- the page's side of net_ws_web.c: its WebSockets, a packet
// a binary message (datagrams), or a stream of them (QTV), and its WebRTC
// data channels, a packet a message
//
// A datagram socket's messages go into one queue for all of them, which the
// game's frame reads with the socket's number; a stream's into its own. What
// is sent before a socket opens waits for it. A message wakes the page's loop,
// as a packet wakes the other systems' waits.
//
// A WebRTC socket is a broker's WebSocket (subprotocol rtc_client) until the
// broker finds the server (NEWPEER); then a peer connection offers one data
// channel, "quake", unordered and never sending again, as UDP; the broker
// passes the offer, the answer and the candidates as JSON (a byte for what it
// is, the peer's number in two, then the text), and is the first STUN server.
// A channel the server opens of its own (FTE's servers in a browser send on
// theirs) is read beside it. The socket opens when the channel does. It
// closes with a code of its own
// for why (4000 and up, net_ws_web.c tells them), or its broker's.

addToLibrary({
	$WebNet__deps: ['$SysWeb'],
	$WebNet: {
		CONNECTING: 0,
		OPEN: 1,
		CLOSED: 2,
		QUEUEMAX: 1024,			// datagrams waiting for the game, the oldest dropped
		PENDINGMAX: 64,			// sent before the socket opened
		RTC_CONNECTTIME: 20000,	// ms a WebRTC socket may take to open

		// the broker's messages (FTE's ICEMSG_*)
		PEERLOST: 0,
		NEWPEER: 2,
		OFFER: 3,
		CANDIDATE: 4,
		NAMEINUSE: 8,

		sockets: [],
		queue: [],				// [socket, bytes]

		// a message come for a socket
		received(id, s, data) {
			if (typeof data == 'string')
				return;
			if (s.stream)
				s.received.push(new Uint8Array(data));
			else {
				if (WebNet.queue.length >= WebNet.QUEUEMAX)
					WebNet.queue.shift();
				WebNet.queue.push([id, new Uint8Array(data)]);
			}
			SysWeb.wake();
		},

		opened(s) {
			s.state = WebNet.OPEN;
			for (const p of s.pending)
				s.send(p);
			s.pending = [];
		},

		open(url, protocol, stream) {
			var id = WebNet.sockets.length, ws, s;

			try {
				ws = protocol ? new WebSocket(url, protocol) : new WebSocket(url);
			}
			catch (e) {
				return null;		// a ws:// from an https page, or a bad URL
			}
			ws.binaryType = 'arraybuffer';
			s = {stream, state: WebNet.CONNECTING, code: 0, pending: [], received: []};
			s.send = (bytes) => ws.send(bytes);
			s.close = () => {
				ws.onclose = ws.onmessage = ws.onopen = null;
				ws.close();
			};
			ws.onopen = () => WebNet.opened(s);
			ws.onmessage = (e) => WebNet.received(id, s, e.data);
			ws.onclose = (e) => {
				s.state = WebNet.CLOSED;
				s.code = e.code;
				SysWeb.wake();
			};
			WebNet.sockets[id] = s;
			return id;
		},

		openRTC(broker, stun) {
			var id = WebNet.sockets.length, ws, s, timer;

			if (typeof RTCPeerConnection == 'undefined')
				return null;
			try {
				ws = new WebSocket(broker, 'rtc_client');
			}
			catch (e) {
				return null;		// a ws:// broker from an https page, or a bad URL
			}
			ws.binaryType = 'arraybuffer';
			s = {stream: false, state: WebNet.CONNECTING, code: 0, pending: [], received: [], pc: null, dc: null, peer: -1};

			// all of it, and then nothing more is told
			const end = () => {
				clearTimeout(timer);
				ws.onclose = ws.onmessage = null;
				ws.close();
				if (s.dc)
					s.dc.onopen = s.dc.onmessage = s.dc.onclose = null;
				if (s.pc) {
					s.pc.onicecandidate = s.pc.onconnectionstatechange = s.pc.ondatachannel = null;
					s.pc.close();
				}
			};
			const fail = (code) => {
				if (s.state == WebNet.CLOSED)
					return;
				end();
				s.state = WebNet.CLOSED;
				s.code = code;
				SysWeb.wake();
			};
			const signal = (message, text) => {
				var body = new TextEncoder().encode(text), m = new Uint8Array(3 + body.length);

				m[0] = message;
				m[1] = s.peer & 0xff;
				m[2] = (s.peer >> 8) & 0xff;
				m.set(body, 3);
				if (ws.readyState == WebSocket.OPEN)
					ws.send(m);
			};
			// the broker found the server: the relays it gives are FTE's
			// turn:host:port?user=u?auth=p, between spaces
			const start = (relays) => {
				var servers = [{urls: stun}], server;

				for (const r of relays.split(' ')) {
					if (!r.startsWith('turn:'))
						continue;
					server = {urls: r.split('?')[0]};
					for (const kv of r.split('?').slice(1)) {
						if (kv.startsWith('user='))
							server.username = kv.slice(5);
						else if (kv.startsWith('auth='))
							server.credential = kv.slice(5);
					}
					servers.push(server);
				}
				try {
					s.pc = new RTCPeerConnection({iceServers: servers});
				}
				catch (e) {
					fail(4002);
					return;
				}
				s.dc = s.pc.createDataChannel('quake', {ordered: false, maxRetransmits: 0});
				s.dc.binaryType = 'arraybuffer';
				s.dc.onopen = () => {
					clearTimeout(timer);
					WebNet.opened(s);
					SysWeb.wake();
				};
				s.dc.onmessage = (e) => WebNet.received(id, s, e.data);
				s.dc.onclose = () => fail(4003);
				// a channel the server opened of its own, read beside ours
				s.pc.ondatachannel = (e) => {
					e.channel.binaryType = 'arraybuffer';
					e.channel.onmessage = (m) => WebNet.received(id, s, m.data);
				};
				s.pc.onicecandidate = (e) => {
					if (e.candidate)
						signal(WebNet.CANDIDATE, JSON.stringify(e.candidate));
				};
				s.pc.onconnectionstatechange = () => {
					var state = s.pc.connectionState;

					if (state == 'failed' || state == 'closed' || (state == 'disconnected' && s.state == WebNet.OPEN))
						fail(s.state == WebNet.OPEN ? 4003 : 4002);
				};
				s.pc.createOffer()
					.then((d) => s.pc.setLocalDescription(d))
					.then(() => signal(WebNet.OFFER, JSON.stringify(s.pc.localDescription)))
					.catch(() => fail(4002));
			};

			ws.onmessage = (e) => {
				var b, from, text, d;

				if (typeof e.data == 'string' || e.data.byteLength < 3)
					return;
				b = new Uint8Array(e.data);
				from = (b[1] | b[2] << 8) << 16 >> 16;
				text = new TextDecoder().decode(b.subarray(3)).split('\0');
				switch (b[0]) {
				case WebNet.NEWPEER:		// the server's address, then the relays
					if (s.pc)
						break;
					s.peer = from;
					start(text[1] || '');
					break;
				case WebNet.OFFER:			// the server's answer
					if (!s.pc)
						break;
					try {
						d = JSON.parse(text[0]);
					}
					catch (x) {
						d = {type: 'answer', sdp: text[0]};
					}
					s.pc.setRemoteDescription(d).catch(() => fail(4002));
					break;
				case WebNet.CANDIDATE:
					if (!s.pc)
						break;
					try {
						d = JSON.parse(text[0]);
					}
					catch (x) {
						d = {candidate: text[0], sdpMid: '0', sdpMLineIndex: 0};
					}
					s.pc.addIceCandidate(d).catch(() => {});
					break;
				case WebNet.PEERLOST:
					if (from == -1 || from == s.peer)
						fail(4000);
					break;
				case WebNet.NAMEINUSE:
					fail(4001);
					break;
				}
			};
			// the broker may go once the channel is open
			ws.onclose = (e) => {
				if (s.state != WebNet.OPEN)
					fail(e.code || 1006);
			};
			timer = setTimeout(() => fail(4004), WebNet.RTC_CONNECTTIME);

			s.send = (bytes) => s.dc.send(bytes);
			s.close = () => {
				if (s.state != WebNet.CLOSED)
					end();
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

	// a datagram socket over WebRTC through the broker's WebSocket URL; -1 if
	// it can't be opened (no WebRTC in the browser)
	web_rtc_open__deps: ['$WebNet', '$UTF8ToString'],
	web_rtc_open: (broker, stun) => {
		var id = WebNet.openRTC(UTF8ToString(broker), UTF8ToString(stun));

		return id === null ? -1 : id;
	},

	web_ws_send__deps: ['$WebNet'],
	web_ws_send: (id, data, length) => {
		var s = WebNet.sockets[id], bytes;

		if (!s || s.state == WebNet.CLOSED)
			return false;
		bytes = HEAPU8.slice(data, data + length);
		if (s.state == WebNet.OPEN)
			s.send(bytes);
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
		s.close();
		delete WebNet.sockets[id];
		WebNet.queue = WebNet.queue.filter((e) => e[0] != id);
	},

	web_page_secure: () => location.protocol == 'https:',
});
