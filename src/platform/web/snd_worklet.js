// snd_worklet.js -- the AudioWorklet of snd_webaudio.c, shipped as
// softworld-sound.js next to the page: the device's side of the ring, on the
// browser's audio thread
//
// The ring is in the page's memory, which the worklet shares: 16-bit stereo
// frames the mixer paints ahead of the read position. Each block takes its
// frames from the read position on and moves it, as Core Audio's callback does
// (snd_coreaudio.c). What was played is cleared, so a mixer held up (the page
// loading, or throttled while hidden) gives silence, not the ring again. Once
// a second the block's loudest sample is told (snd_info).

'use strict';

class SoftWorldRing extends AudioWorkletProcessor {
	constructor (options) {
		const o = options.processorOptions;

		super();
		// the page's memory never moves what it holds, nor its buffer once
		// shared, however it grows
		this.ring = new Int16Array(o.memory.buffer, o.ring, o.frames * 2);
		this.shared = new Uint32Array(o.memory.buffer, o.shared, 4);	// read frame, silenced, peak, seconds
		this.mask = o.frames - 1;
		this.peak = 0;
		this.counted = 0;
	}

	process (inputs, outputs) {
		const left = outputs[0][0], right = outputs[0][1] ?? left, n = left.length;
		const read = Atomics.load(this.shared, 0), silent = Atomics.load(this.shared, 1);
		let peak = this.peak;

		for (let i = 0 ; i < n ; i++) {
			const j = ((read + i) & this.mask) * 2, l = this.ring[j], r = this.ring[j + 1];

			this.ring[j] = this.ring[j + 1] = 0;
			if (silent) {
				left[i] = right[i] = 0;
				continue;
			}
			left[i] = l / 32768;
			right[i] = r / 32768;
			peak = Math.max(peak, Math.abs(l), Math.abs(r));
		}
		Atomics.store(this.shared, 0, (read + n) >>> 0);

		this.peak = peak;
		this.counted += n;
		if (this.counted >= sampleRate) {
			Atomics.store(this.shared, 2, this.peak);
			Atomics.add(this.shared, 3, 1);
			this.peak = 0;
			this.counted -= sampleRate;
		}
		return true;
	}
}

registerProcessor('softworld-ring', SoftWorldRing);
