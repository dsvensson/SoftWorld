// snd_webaudio.js -- the page's side of snd_webaudio.c: the AudioContext, and
// the AudioWorklet (snd_worklet.js, softworld-sound.js next to the page) that
// plays the ring from the page's shared memory
//
// A page's sound waits for a click or a key (the browsers' autoplay rule):
// until then the context is suspended, the worklet takes nothing, and the
// mixer stops as far ahead as it paints. The first click or key starts it,
// the one that locks the mouse.

addToLibrary({
	$WebSnd: {
		ctx: null,
		node: null,
		failed: '',
	},

	// the sample rate, 0 if there is no sound (web_snd_error says why)
	web_snd_init__deps: ['$WebSnd'],
	web_snd_init: (ring, frames, shared) => {
		var resume;

		if (!globalThis.AudioWorkletNode) {
			WebSnd.failed = 'this browser has no AudioWorklet';
			return 0;
		}
		try {
			WebSnd.ctx = new AudioContext({latencyHint: 'interactive'});
		}
		catch (e) {
			WebSnd.failed = e.message;
			return 0;
		}
		WebSnd.ctx.audioWorklet.addModule(locateFile('softworld-sound.js')).then(() => {
			WebSnd.node = new AudioWorkletNode(WebSnd.ctx, 'softworld-ring', {numberOfInputs: 0,
				numberOfOutputs: 1, outputChannelCount: [2],
				processorOptions: {'memory': wasmMemory, 'ring': ring, 'frames': frames, 'shared': shared}});
			WebSnd.node.connect(WebSnd.ctx.destination);
		}).catch((e) => WebSnd.failed = `softworld-sound.js: ${e.message ?? e}`);

		resume = () => {
			if (WebSnd.ctx && WebSnd.ctx.state != 'running' && WebSnd.ctx.state != 'closed')
				WebSnd.ctx.resume().catch(() => {});
		};
		addEventListener('pointerdown', resume, true);
		addEventListener('keydown', resume, true);
		// shown again, after the browser stopped it (Safari's 'interrupted')
		document.addEventListener('visibilitychange', () => {
			if (!document.hidden && navigator.userActivation?.hasBeenActive)
				resume();
		});
		return WebSnd.ctx.sampleRate;
	},

	// 0 starting, 1 waiting for a click or a key, 2 playing, 3 failed
	web_snd_state__deps: ['$WebSnd'],
	web_snd_state: () => {
		if (WebSnd.failed)
			return 3;
		if (!WebSnd.node)
			return 0;
		return WebSnd.ctx.state == 'running' ? 2 : 1;
	},

	web_snd_error__deps: ['$WebSnd', '$stringToUTF8'],
	web_snd_error: (buf, size) => stringToUTF8(WebSnd.failed, buf, size),

	// the latency the browser tells, in ms
	web_snd_latency__deps: ['$WebSnd'],
	web_snd_latency: () => {
		var ctx = WebSnd.ctx;

		return ctx ? ((ctx.baseLatency ?? 0) + (ctx.outputLatency ?? 0)) * 1000 : 0;
	},

	web_snd_shutdown__deps: ['$WebSnd'],
	web_snd_shutdown: () => {
		WebSnd.node?.disconnect();
		WebSnd.ctx?.close().catch(() => {});
		WebSnd.node = WebSnd.ctx = null;
	},
});
