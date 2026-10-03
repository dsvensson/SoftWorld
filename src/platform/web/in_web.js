// in_web.js -- the page's side of in_web.c: the browser's events, queued for
// the game's frame, the mouse locked to the page while the game wants it, its
// motion and wheel, and what the browser pastes
//
// Keys go to the game by where they are (KeyboardEvent.code), typed text by
// what the layout makes of them (KeyboardEvent.key). The browser's own uses of
// keys are kept from the page (Tab, F-keys, Alt), but for pasting, which comes
// as an event after the key. The mouse is locked only after a click or a key
// (the browser's rule), so the game's wish for it is kept, and asked again then.
// Locked, motion comes raw (unadjustedMovement) where the browser has it.

addToLibrary({
	$InWeb__deps: ['$SysWeb', '$WebVid', '$stringToUTF8', '$stringToNewUTF8'],
	$InWeb: {
		KEY: 0,					// the events' types, as in_web.c has them
		BUTTON: 1,
		FOCUS: 2,
		HIDDEN: 3,
		FULLSCREEN: 4,
		USEREXIT: 5,

		queue: [],
		want: false,			// the game wants the mouse locked
		requesting: false,		// a lock asked for, not yet answered
		retry: false,			// the lock was refused: the next click or key asks again
		releasing: false,		// we let it go
		raw: false,				// the lock's motion is unaccelerated
		escape: 0,				// when Escape was last pressed
		dx: 0,					// motion not yet taken
		dy: 0,
		wheel: 0,				// wheel motion in 120ths of a step, not yet a step
		wheeltime: 0,
		pasted: null,

		push(type, down, value, captured, code) {
			InWeb.queue.push({type, down: down ? 1 : 0, value: value | 0, captured: captured ? 1 : 0, code: code ?? ''});
		},

		locked() {
			return !!document.pointerLockElement;
		},

		start() {
			var screen = document.getElementById('screen');
			var motion = 'onpointerrawupdate' in window ? 'pointerrawupdate' : 'mousemove';

			addEventListener('keydown', InWeb.key, true);
			addEventListener('keyup', InWeb.key, true);
			// a click or a key: a gesture the lock and fullscreen may be asked in
			addEventListener('pointerdown', InWeb.gesture, true);
			addEventListener('keydown', InWeb.gesture, true);

			screen.addEventListener('mousedown', InWeb.button);
			addEventListener('mouseup', InWeb.button);
			screen.addEventListener('contextmenu', (e) => e.preventDefault());
			screen.addEventListener('auxclick', (e) => e.preventDefault());
			screen.addEventListener('wheel', InWeb.wheelEvent, {passive: false});
			document.addEventListener(motion, (e) => {
				if (InWeb.locked()) {
					InWeb.dx += e.movementX;
					InWeb.dy += e.movementY;
				}
			});

			addEventListener('focus', () => InWeb.push(InWeb.FOCUS, 0, 1));
			addEventListener('blur', () => InWeb.push(InWeb.FOCUS, 0, 0));
			document.addEventListener('visibilitychange', () => InWeb.push(InWeb.HIDDEN, 0, document.hidden));
			document.addEventListener('fullscreenchange', () => InWeb.push(InWeb.FULLSCREEN, 0, !!document.fullscreenElement));
			document.addEventListener('pointerlockchange', InWeb.lockChange);
			document.addEventListener('pointerlockerror', () => {
				InWeb.requesting = false;
				InWeb.retry = true;
			});
			document.addEventListener('paste', (e) => {
				InWeb.pasted = e.clipboardData?.getData('text/plain') ?? null;
				e.preventDefault();
			});

			// as the page is now
			InWeb.push(InWeb.FOCUS, 0, document.hasFocus());
			InWeb.push(InWeb.HIDDEN, 0, document.hidden);
		},

		key(e) {
			var down = e.type == 'keydown', text = 0;

			if (e.isComposing || e.keyCode == 229)
				return;
			if (down && e.code == 'Escape')
				InWeb.escape = performance.now();

			// Alt+Enter: fullscreen, asked for while the key is a gesture
			if (down && e.code == 'Enter' && e.altKey) {
				e.preventDefault();
				if (!e.repeat) {
					if (document.fullscreenElement)
						document.exitFullscreen().catch(() => {});
					else
						WebVid.fullscreen();
				}
				return;
			}

			// the text it types: ASCII, not a Ctrl chord (Ctrl+Alt is AltGr)
			if (down && e.key.length == 1 && !(e.ctrlKey && !e.altKey) && !e.metaKey) {
				text = e.key.charCodeAt(0);
				if (text < 32 || text > 126)
					text = 0;
			}
			InWeb.push(InWeb.KEY, down, text, 0, e.code);
			if (down)
				SysWeb.wake();

			// the browser's keys are the game's, but for pasting and the system's
			// (Command) chords
			if (e.metaKey || (e.ctrlKey && e.code == 'KeyV') || (e.shiftKey && e.code == 'Insert'))
				return;
			e.preventDefault();
		},

		gesture(e) {
			if (e.type == 'keydown' && e.code == 'Escape')
				return;		// not a gesture to the browser
			if (WebVid.fullscreenPending)
				WebVid.fullscreen();
			if (InWeb.want && !InWeb.locked() && !InWeb.requesting) {
				InWeb.retry = false;
				InWeb.lock();
			}
		},

		// 0 left, 1 middle, 2 right, 3 back, 4 forward; down on the page, up anywhere
		button(e) {
			var down = e.type == 'mousedown';

			InWeb.push(InWeb.BUTTON, down, e.button, InWeb.locked());
			if (down)
				SysWeb.wake();
			if (e.button != 0 && e.button != 2)
				e.preventDefault();		// no autoscroll, no going back or forward
		},

		wheelEvent(e) {
			var now = performance.now(), d;

			e.preventDefault();
			// in 120ths of a step: a notch is 120 where the browser tells it so,
			// else 100 pixels, or 3 lines
			if (e.wheelDeltaY)
				d = -e.wheelDeltaY;
			else
				d = e.deltaY * (e.deltaMode == 1 ? 40 : e.deltaMode == 2 ? 120 : 1.2);
			// a pause, or the other way: what was left of a step is dropped
			if (now - InWeb.wheeltime > 300 || Math.sign(d) != Math.sign(InWeb.wheel))
				InWeb.wheel = 0;
			InWeb.wheeltime = now;
			InWeb.wheel += d;
			SysWeb.wake();
		},

		lock() {
			var screen = document.getElementById('screen'), p;

			InWeb.requesting = true;
			try {
				p = screen.requestPointerLock({unadjustedMovement: true});
			}
			catch (e) {
				p = null;
			}
			if (!p?.then) {
				InWeb.raw = false;		// an older browser: pointerlockchange or pointerlockerror answers
				return;
			}
			p.then(() => InWeb.raw = true, (e) => {
				if (e.name != 'NotSupportedError') {
					InWeb.requesting = false;
					InWeb.retry = true;
					return;
				}
				// no raw motion here: the system's accelerated
				InWeb.raw = false;
				screen.requestPointerLock()?.catch?.(() => {
					InWeb.requesting = false;
					InWeb.retry = true;
				});
			});
		},

		lockChange() {
			var locked = InWeb.locked();

			InWeb.requesting = false;
			if (locked) {
				InWeb.dx = InWeb.dy = 0;
				InWeb.notice();
				return;
			}
			// let go by the user (Escape) while the game held it: the menu, as
			// Escape opens it, unless the key came to the page
			if (!InWeb.releasing && InWeb.want && document.hasFocus() && !document.hidden
					&& performance.now() - InWeb.escape > 250)
				InWeb.push(InWeb.USEREXIT, 0, 0);
			InWeb.releasing = false;
			InWeb.notice();
		},

		notice() {
			Module['swNotice']?.('capture', InWeb.want && !InWeb.locked() && document.hasFocus()
				? 'Click to play' : null);
		},
	},

	web_in_start__deps: ['$InWeb'],
	web_in_start: () => InWeb.start(),

	// the next event: its type, down, value, whether the mouse was locked, and
	// the key's code; false when there is none
	web_in_next__deps: ['$InWeb'],
	web_in_next: (ev) => {
		var e = InWeb.queue.shift();

		if (!e)
			return false;
		{{{ makeSetValue('ev', 0, 'e.type', 'i32') }}};
		{{{ makeSetValue('ev', 4, 'e.down', 'i32') }}};
		{{{ makeSetValue('ev', 8, 'e.value', 'i32') }}};
		{{{ makeSetValue('ev', 12, 'e.captured', 'i32') }}};
		stringToUTF8(e.code, ev + 16, 32);
		return true;
	},

	// the game wants the mouse locked, or not; true while it is
	web_in_setcapture__deps: ['$InWeb'],
	web_in_setcapture: (want) => {
		var locked = InWeb.locked();

		want = !!want;
		if (want != InWeb.want) {
			InWeb.want = want;
			InWeb.notice();
		}
		if (want && !locked && !InWeb.requesting && !InWeb.retry)
			InWeb.lock();
		else if (!want && locked && !InWeb.releasing) {
			InWeb.releasing = true;
			document.exitPointerLock();
		}
		if (!want)
			InWeb.retry = false;
		return locked;
	},

	web_in_raw__deps: ['$InWeb'],
	web_in_raw: () => InWeb.raw,

	// the motion since it was last taken, in whole counts
	web_in_takemotion__deps: ['$InWeb'],
	web_in_takemotion: (dx, dy) => {
		var x = Math.trunc(InWeb.dx), y = Math.trunc(InWeb.dy);

		InWeb.dx -= x;
		InWeb.dy -= y;
		{{{ makeSetValue('dx', 0, 'x', 'i32') }}};
		{{{ makeSetValue('dy', 0, 'y', 'i32') }}};
	},

	// the wheel's whole steps since they were last taken, down positive
	web_in_takewheel__deps: ['$InWeb'],
	web_in_takewheel: () => {
		var steps = Math.trunc(InWeb.wheel / 120);

		InWeb.wheel -= steps * 120;
		return steps;
	},

	web_in_pasted__deps: ['$InWeb'],
	web_in_pasted: () => {
		var text = InWeb.pasted;

		InWeb.pasted = null;
		return text ? stringToNewUTF8(text) : 0;
	},
});
