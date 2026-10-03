// vid_webgl.js -- the page's side of vid_webgl.c: the canvas's WebGL 2 context,
// the present shader, and the two integer textures the frame is copied to
//
// A frame is drawn by copying the layers from the page's memory into their
// textures (the 2D only when it changed) and drawing a triangle over the
// letterboxed viewport. Without vsync a frame is drawn only when the GPU has
// finished the last (a fence), so frames don't queue on it, and the canvas
// asks to be shown as soon as it is drawn (desynchronized), where the browser
// allows. A canvas's context can't change that, so a new canvas takes the old
// one's place when vid_vsync does.

addToLibrary({
	$WebVid__deps: ['$UTF8ToString', '$stringToUTF8'],
	$WebVid: {
		gl: null,
		canvas: null,
		observer: null,
		source: '',				// present.glsl
		desync: false,			// the context's
		program: null,
		ubo: null,
		vao: null,
		view: null,				// the textures
		hud: null,
		width: 0,				// their size
		height: 0,
		cw: 0,					// the canvas's, in device pixels, as last observed
		ch: 0,
		hudforce: true,			// the 2D copied at the next draw, changed or not
		fence: null,			// the last draw's, without vsync
		lastdraw: 0,
		drawn: 0,
		skipped: 0,
		error: '',
		renderer: '',
		fullscreenPending: false,	// asked for without a gesture: the next click or key

		// a context on a new canvas in the old one's place
		create(desync) {
			var old = document.getElementById('canvas'), canvas, gl, why = '';

			canvas = document.createElement('canvas');
			canvas.id = 'canvas';
			canvas.tabIndex = -1;
			canvas.addEventListener('webglcontextcreationerror', (e) => why = e.statusMessage || '');
			gl = canvas.getContext('webgl2', {alpha: false, depth: false, stencil: false, antialias: false,
				premultipliedAlpha: false, preserveDrawingBuffer: false, desynchronized: desync,
				failIfMajorPerformanceCaveat: false});
			if (!gl) {
				WebVid.error = why || 'no WebGL 2 context';
				return false;
			}
			if (old)
				old.replaceWith(canvas);
			WebVid.observer?.disconnect();
			WebVid.gl = gl;
			WebVid.canvas = canvas;
			Module['canvas'] = canvas;
			WebVid.desync = desync;
			WebVid.fence = null;
			canvas.addEventListener('webglcontextlost', (e) => e.preventDefault());
			canvas.addEventListener('webglcontextrestored', () => WebVid.build());

			// the size in device pixels, as the layout has it
			var r = canvas.getBoundingClientRect();
			WebVid.cw = Math.round(r.width * devicePixelRatio);
			WebVid.ch = Math.round(r.height * devicePixelRatio);
			WebVid.observer = new ResizeObserver((entries) => {
				for (const e of entries)
					if (e.devicePixelContentBoxSize) {
						WebVid.cw = e.devicePixelContentBoxSize[0].inlineSize;
						WebVid.ch = e.devicePixelContentBoxSize[0].blockSize;
					}
					else {
						WebVid.cw = Math.round(e.contentRect.width * devicePixelRatio);
						WebVid.ch = Math.round(e.contentRect.height * devicePixelRatio);
					}
			});
			try {
				WebVid.observer.observe(canvas, {box: 'device-pixel-content-box'});
			}
			catch (e) {
				WebVid.observer.observe(canvas);
			}

			var info = gl.getExtension('WEBGL_debug_renderer_info');
			WebVid.renderer = (info && gl.getParameter(info.UNMASKED_RENDERER_WEBGL)) || gl.getParameter(gl.RENDERER);
			return WebVid.build();
		},

		compile(type, stage) {
			var gl = WebVid.gl, shader = gl.createShader(type);

			gl.shaderSource(shader, `#version 300 es\n#define ${stage}\n#line 1\n${WebVid.source}`);
			gl.compileShader(shader);
			if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS) && !gl.isContextLost()) {
				WebVid.error = `present.glsl (${stage}): ${gl.getShaderInfoLog(shader)}`;
				return null;
			}
			return shader;
		},

		// the program, its constants and the textures, made again for a context restored
		build() {
			var gl = WebVid.gl, vs, fs, program;

			vs = WebVid.compile(gl.VERTEX_SHADER, 'VERTEX');
			fs = WebVid.compile(gl.FRAGMENT_SHADER, 'FRAGMENT');
			if (!vs || !fs)
				return false;
			program = gl.createProgram();
			gl.attachShader(program, vs);
			gl.attachShader(program, fs);
			gl.linkProgram(program);
			if (!gl.getProgramParameter(program, gl.LINK_STATUS) && !gl.isContextLost()) {
				WebVid.error = `present.glsl: ${gl.getProgramInfoLog(program)}`;
				return false;
			}
			gl.useProgram(program);
			gl.uniform1i(gl.getUniformLocation(program, 'u_view'), 0);
			gl.uniform1i(gl.getUniformLocation(program, 'u_hud'), 1);
			gl.uniformBlockBinding(program, gl.getUniformBlockIndex(program, 'Present'), 0);
			WebVid.program = program;

			WebVid.ubo = gl.createBuffer();
			gl.bindBuffer(gl.UNIFORM_BUFFER, WebVid.ubo);
			gl.bufferData(gl.UNIFORM_BUFFER, 64, gl.DYNAMIC_DRAW);
			gl.bindBufferBase(gl.UNIFORM_BUFFER, 0, WebVid.ubo);
			WebVid.vao = gl.createVertexArray();
			gl.bindVertexArray(WebVid.vao);
			gl.disable(gl.DITHER);
			gl.disable(gl.BLEND);

			if (WebVid.width)
				WebVid.textures(WebVid.width, WebVid.height);
			return true;
		},

		textures(width, height) {
			var gl = WebVid.gl;

			for (const name of ['view', 'hud']) {
				if (WebVid[name])
					gl.deleteTexture(WebVid[name]);
				WebVid[name] = gl.createTexture();
				gl.activeTexture(name == 'view' ? gl.TEXTURE0 : gl.TEXTURE1);
				gl.bindTexture(gl.TEXTURE_2D, WebVid[name]);
				gl.texStorage2D(gl.TEXTURE_2D, 1, gl.R32UI, width, height);
				// integer textures are read by texel, never filtered
				gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
				gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
				gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
				gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
			}
			WebVid.width = width;
			WebVid.height = height;
			WebVid.hudforce = true;
		},

		fullscreen() {
			var screen = document.getElementById('screen');

			WebVid.fullscreenPending = false;
			screen.requestFullscreen({navigationUI: 'hide'}).then(() => navigator.keyboard?.lock?.().catch(() => {}),
				() => {});
		},
	},

	web_vid_init__deps: ['$WebVid'],
	web_vid_init: (source, desync) => {
		WebVid.source = UTF8ToString(source);
		return WebVid.create(!!desync);
	},

	web_vid_recreate__deps: ['$WebVid'],
	web_vid_recreate: (desync) => WebVid.create(!!desync),

	web_vid_error__deps: ['$WebVid'],
	web_vid_error: (buf, size) => stringToUTF8(WebVid.error, buf, size),

	web_vid_renderer__deps: ['$WebVid'],
	web_vid_renderer: (buf, size) => stringToUTF8(WebVid.renderer, buf, size),

	// false if the frame is bigger than the GPU's textures may be
	web_vid_settextures__deps: ['$WebVid'],
	web_vid_settextures: (width, height) => {
		var max = WebVid.gl.getParameter(WebVid.gl.MAX_TEXTURE_SIZE);

		if (width > max || height > max) {
			WebVid.error = `the frame's ${width}x${height} is more than the GPU's ${max}x${max}`;
			return false;
		}
		WebVid.textures(width, height);
		return true;
	},

	web_vid_clientsize__deps: ['$WebVid'],
	web_vid_clientsize: (width, height) => {
		{{{ makeSetValue('width', 0, 'WebVid.cw', 'i32') }}};
		{{{ makeSetValue('height', 0, 'WebVid.ch', 'i32') }}};
	},

	// draws the frame unless the last is still on the GPU (paced 0: no vsync);
	// true if it did
	web_vid_present__deps: ['$WebVid'],
	web_vid_present: (view, hud, huddirty, rowpixels, constants, x, y, width, height, paced) => {
		var gl = WebVid.gl, canvas = WebVid.canvas, now = performance.now();

		if (!gl || gl.isContextLost())
			return false;
		if (!paced && WebVid.fence) {
			if (gl.getSyncParameter(WebVid.fence, gl.SYNC_STATUS) != gl.SIGNALED && now - WebVid.lastdraw < 100) {
				WebVid.skipped++;
				return false;
			}
			gl.deleteSync(WebVid.fence);
			WebVid.fence = null;
		}

		if (canvas.width != WebVid.cw || canvas.height != WebVid.ch) {
			canvas.width = WebVid.cw;
			canvas.height = WebVid.ch;
		}
		gl.viewport(0, 0, canvas.width, canvas.height);
		gl.clearColor(0, 0, 0, 1);
		gl.clear(gl.COLOR_BUFFER_BIT);

		gl.pixelStorei(gl.UNPACK_ROW_LENGTH, rowpixels);
		gl.activeTexture(gl.TEXTURE0);
		gl.bindTexture(gl.TEXTURE_2D, WebVid.view);
		gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, WebVid.width, WebVid.height, gl.RED_INTEGER, gl.UNSIGNED_INT,
			HEAPU32, view >> 2);
		if (huddirty || WebVid.hudforce) {
			gl.activeTexture(gl.TEXTURE1);
			gl.bindTexture(gl.TEXTURE_2D, WebVid.hud);
			gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, WebVid.width, WebVid.height, gl.RED_INTEGER, gl.UNSIGNED_INT,
				HEAPU32, hud >> 2);
			WebVid.hudforce = false;
		}
		gl.bindBuffer(gl.UNIFORM_BUFFER, WebVid.ubo);
		gl.bufferSubData(gl.UNIFORM_BUFFER, 0, HEAPU8, constants, 64);

		// GL's viewport counts from the bottom
		gl.viewport(x, canvas.height - y - height, width, height);
		gl.drawArrays(gl.TRIANGLES, 0, 3);
		if (!paced)
			WebVid.fence = gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
		gl.flush();

		WebVid.lastdraw = now;
		WebVid.drawn++;
		return true;
	},

	// 1 asked for, 0 waiting for the next click or key, -1 none in this browser
	web_vid_fullscreen__deps: ['$WebVid'],
	web_vid_fullscreen: (on) => {
		if (!on) {
			WebVid.fullscreenPending = false;
			navigator.keyboard?.unlock?.();
			if (document.fullscreenElement)
				document.exitFullscreen().catch(() => {});
			return 1;
		}
		if (!document.getElementById('screen').requestFullscreen)
			return -1;
		if (navigator.userActivation && !navigator.userActivation.isActive) {
			WebVid.fullscreenPending = true;
			return 0;
		}
		WebVid.fullscreen();
		return 1;
	},

	// the frame drawn as web_vid_present draws it, but into width x height
	// pixels of a target of its own, read back as floats a channel, top row
	// first, into out (tests/test_present_webgl.c); 2 for a float target, 1 for
	// one of 8 bits where the GPU has none
	web_vid_drawto__deps: ['$WebVid'],
	web_vid_drawto: (view, hud, rowpixels, constants, width, height, out) => {
		var gl = WebVid.gl, float = !!gl.getExtension('EXT_color_buffer_float');
		var fb = gl.createFramebuffer(), rb = gl.createRenderbuffer(), pixels, row, y;

		gl.bindRenderbuffer(gl.RENDERBUFFER, rb);
		gl.renderbufferStorage(gl.RENDERBUFFER, float ? gl.RGBA32F : gl.RGBA8, width, height);
		gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
		gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, rb);

		gl.pixelStorei(gl.UNPACK_ROW_LENGTH, rowpixels);
		gl.activeTexture(gl.TEXTURE0);
		gl.bindTexture(gl.TEXTURE_2D, WebVid.view);
		gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, WebVid.width, WebVid.height, gl.RED_INTEGER, gl.UNSIGNED_INT,
			HEAPU32, view >> 2);
		gl.activeTexture(gl.TEXTURE1);
		gl.bindTexture(gl.TEXTURE_2D, WebVid.hud);
		gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, WebVid.width, WebVid.height, gl.RED_INTEGER, gl.UNSIGNED_INT,
			HEAPU32, hud >> 2);
		gl.bindBuffer(gl.UNIFORM_BUFFER, WebVid.ubo);
		gl.bufferSubData(gl.UNIFORM_BUFFER, 0, HEAPU8, constants, 64);
		gl.viewport(0, 0, width, height);
		gl.drawArrays(gl.TRIANGLES, 0, 3);

		if (float) {
			pixels = new Float32Array(width * height * 4);
			gl.readPixels(0, 0, width, height, gl.RGBA, gl.FLOAT, pixels);
		}
		else {
			pixels = new Uint8Array(width * height * 4);
			gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
			pixels = Float32Array.from(pixels, (v) => v / 255);
		}
		// GL's rows count from the bottom
		for (y = 0 ; y < height ; y++) {
			row = pixels.subarray((height - 1 - y) * width * 4, (height - y) * width * 4);
			HEAPF32.set(row, (out >> 2) + y * width * 4);
		}
		gl.bindFramebuffer(gl.FRAMEBUFFER, null);
		gl.deleteFramebuffer(fb);
		gl.deleteRenderbuffer(rb);
		WebVid.hudforce = true;
		return float ? 2 : 1;
	},

	web_vid_settitle: (text) => {
		document.title = UTF8ToString(text);
	},

	web_vid_focus: () => window.focus(),

	web_vid_describe__deps: ['$WebVid'],
	web_vid_describe: (buf, size) => {
		var gl = WebVid.gl, attrs = gl?.getContextAttributes();

		return stringToUTF8([
			`GPU: ${WebVid.renderer}`,
			`Canvas: ${WebVid.canvas.width}x${WebVid.canvas.height} device pixels, ${devicePixelRatio} a CSS pixel`,
			`Shown: ${attrs?.desynchronized ? 'as soon as drawn (desynchronized)' : "at the browser's next refresh"}`,
			`Frames: ${WebVid.drawn} drawn, ${WebVid.skipped} skipped while the GPU had the last`,
		].join('\n'), buf, size);
	},
});
