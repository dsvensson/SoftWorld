// sys_web.js -- the page's side of sys_web_gui.c: the loop that runs the
// game's frames, the notices over it, and what the page does when it hides
//
// A frame returns how long until the next is due, or -1 for the next
// animation frame (vsync). A long wait is a timer, set early by as late as
// timers have fired; the rest of it, and no wait at all, a task posted to the
// page itself, so input still comes between: as Sys_WaitUntil spins its last
// quarter millisecond on the other systems. Hidden, the page gets no animation
// frames, and a timer takes their place.

addToLibrary({
	$SysWeb__deps: ['$callUserCallback', 'Sys_WebFrame', 'Sys_WebHidden', 'Sys_WebCommand', '$SWFS',
		'$stringToNewUTF8', 'free'],
	$SysWeb: {
		timer: 0,			// a pending setTimeout
		raf: 0,				// a pending animation frame
		posted: false,		// a task posted to the channel
		channel: null,
		expected: 0,		// when the timer is due, performance.now's
		late: 1,			// ms timers fire late, on average
		woken: false,		// something came that the next frame takes now
		stopped: false,

		start() {
			SysWeb.channel = new MessageChannel();
			SysWeb.channel.port1.onmessage = () => {
				SysWeb.posted = false;
				SysWeb.tick();
			};
			document.addEventListener('visibilitychange', () => {
				if (document.hidden)
					SysWeb.hidden();
				SysWeb.reschedule();
			});
			addEventListener('pagehide', SysWeb.hidden);
			Module['command'] = (text) => {
				var p = stringToNewUTF8(text);
				callUserCallback(() => _Sys_WebCommand(p));
				_free(p);
				SysWeb.wake();
			};
			SysWeb.tick();
		},

		cancel() {
			if (SysWeb.timer)
				clearTimeout(SysWeb.timer);
			if (SysWeb.raf)
				cancelAnimationFrame(SysWeb.raf);
			SysWeb.timer = SysWeb.raf = 0;
		},

		tick() {
			var wait = -1, woken = SysWeb.woken;

			if (SysWeb.stopped || ABORT)
				return;
			SysWeb.cancel();
			SysWeb.woken = false;
			callUserCallback(() => {
				wait = _Sys_WebFrame(woken ? 1 : 0);
			});
			if (!SysWeb.stopped && !ABORT)
				SysWeb.schedule(wait);
		},

		schedule(wait) {
			var ms;

			if (wait < 0) {
				if (document.hidden)
					SysWeb.timer = setTimeout(SysWeb.tick, 16);
				else
					SysWeb.raf = requestAnimationFrame(() => {
						SysWeb.raf = 0;
						SysWeb.tick();
					});
				return;
			}
			ms = wait * 1000;
			if (ms > SysWeb.late + 1) {
				SysWeb.expected = performance.now() + ms - SysWeb.late;
				SysWeb.timer = setTimeout(SysWeb.timed, ms - SysWeb.late);
			}
			else
				SysWeb.post();
		},

		// the timer fired: how late, for the next one
		timed() {
			var late = performance.now() - SysWeb.expected;

			SysWeb.timer = 0;
			SysWeb.late += (Math.min(Math.max(late, 0), 8) - SysWeb.late) * 0.1;
			SysWeb.late = Math.max(SysWeb.late, 0.5);
			SysWeb.tick();
		},

		post() {
			if (!SysWeb.posted) {
				SysWeb.posted = true;
				SysWeb.channel.port2.postMessage(0);
			}
		},

		// input or a packet: the frame waiting for its time runs now; one waiting
		// for an animation frame takes it then
		wake() {
			if (SysWeb.stopped || SysWeb.raf)
				return;
			SysWeb.woken = true;
			if (SysWeb.timer) {
				clearTimeout(SysWeb.timer);
				SysWeb.timer = 0;
				SysWeb.post();
			}
		},

		// the page was hidden or shown: an animation frame waited for may not come
		reschedule() {
			if (SysWeb.stopped || !SysWeb.raf && !SysWeb.timer)
				return;
			SysWeb.cancel();
			SysWeb.post();
		},

		// the page may be closed without another frame: config.cfg, and what
		// the files open for writing hold, are kept now
		hidden() {
			if (SysWeb.stopped || ABORT)
				return;
			callUserCallback(() => _Sys_WebHidden());
			SWFS.flushDirty();
		},
	},

	web_start_loop__deps: ['$SysWeb'],
	web_start_loop: () => SysWeb.start(),

	web_wake__deps: ['$SysWeb'],
	web_wake: () => SysWeb.wake(),

	web_notice__deps: ['$UTF8ToString'],
	web_notice: (slot, text) => {
		Module['swNotice']?.(UTF8ToString(slot), text ? UTF8ToString(text) : null);
	},

	web_stop__deps: ['$SysWeb', '$UTF8ToString'],
	web_stop: (text, error) => {
		SysWeb.stopped = true;
		SysWeb.cancel();
		SWFS.flushDirty();
		if (document.pointerLockElement)
			document.exitPointerLock();
		if (document.fullscreenElement)
			document.exitFullscreen().catch(() => {});
		Module['swNotice']?.('error', UTF8ToString(text), !!error);
	},
});
