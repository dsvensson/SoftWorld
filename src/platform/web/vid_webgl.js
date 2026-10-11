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
//
// A desynchronized canvas may be drawn in the buffer on the screen (Chrome's,
// with an overlay), so nothing is cleared before a frame: the canvas keeps
// what was drawn (preserveDrawingBuffer), and the bars around the viewport are
// cleared only when the canvas or the viewport change. A clear at each frame,
// the browser's or ours, was on the screen now and then: the view flickered
// black.
//
// With the CRT shader (vid_crt.h) the frame is drawn at its own size into a
// texture of the shader's, then its passes, the last on the canvas: crt.glsl's
// programs, a texture and framebuffer each pass draws in, made when it's turned
// on (web_vid_crt_init) and as vid_webgl.c sizes them, and again for a context
// restored or made anew.

addToLibrary({
	$WebVid__deps: ['$UTF8ToString', '$stringToUTF8'],
	$WebVid: {
		gl: null,
		canvas: null,
		observer: null,
		source: '',				// present.glsl
		desync: false,			// the context's
		program: null,
		upright: null,			// present.glsl's u_upright
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
		cleared: '',			// the canvas's size and the viewport when it was last cleared
		lastdraw: 0,
		drawn: 0,
		skipped: 0,
		error: '',
		renderer: '',
		fullscreenPending: false,	// asked for without a gesture: the next click or key
		crt: null,				// the CRT shader's, while it's on

		// a context on a new canvas in the old one's place
		create(desync) {
			var old = document.getElementById('canvas'), canvas, gl, why = '';

			canvas = document.createElement('canvas');
			canvas.id = 'canvas';
			canvas.tabIndex = -1;
			canvas.addEventListener('webglcontextcreationerror', (e) => why = e.statusMessage || '');
			gl = canvas.getContext('webgl2', {alpha: false, depth: false, stencil: false, antialias: false,
				premultipliedAlpha: false, preserveDrawingBuffer: desync, desynchronized: desync,
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

		compile(type, stage, source = WebVid.source, name = 'present.glsl') {
			var gl = WebVid.gl, shader = gl.createShader(type);

			gl.shaderSource(shader, `#version 300 es\n#define ${stage}\n#line 1\n${source}`);
			gl.compileShader(shader);
			if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS) && !gl.isContextLost()) {
				WebVid.error = `${name} (${stage.replace(/\n#define /g, ' ')}): ${gl.getShaderInfoLog(shader)}`;
				return null;
			}
			return shader;
		},

		link(vs, fs, name) {
			var gl = WebVid.gl, program = gl.createProgram();

			gl.attachShader(program, vs);
			gl.attachShader(program, fs);
			gl.linkProgram(program);
			if (!gl.getProgramParameter(program, gl.LINK_STATUS) && !gl.isContextLost()) {
				WebVid.error = `${name}: ${gl.getProgramInfoLog(program)}`;
				return null;
			}
			return program;
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
			WebVid.upright = gl.getUniformLocation(program, 'u_upright');

			WebVid.ubo = gl.createBuffer();
			gl.bindBuffer(gl.UNIFORM_BUFFER, WebVid.ubo);
			gl.bufferData(gl.UNIFORM_BUFFER, 64, gl.DYNAMIC_DRAW);
			gl.bindBufferBase(gl.UNIFORM_BUFFER, 0, WebVid.ubo);
			WebVid.vao = gl.createVertexArray();
			gl.bindVertexArray(WebVid.vao);
			gl.disable(gl.DITHER);
			gl.disable(gl.BLEND);
			WebVid.cleared = '';

			if (WebVid.width)
				WebVid.textures(WebVid.width, WebVid.height);
			return !WebVid.crt || WebVid.crtbuild();
		},

		// the CRT shader's programs, buffers and textures, made again for a context
		// restored or made anew: a program a pass, the last's drawing on the canvas
		crtbuild() {
			var gl = WebVid.gl, crt = WebVid.crt, vs, screen, fs, program, i, k, last = crt.names.length - 1;

			crt.float = !!gl.getExtension('EXT_color_buffer_float');
			vs = WebVid.compile(gl.VERTEX_SHADER, 'VERTEX', crt.source, 'crt.glsl');
			screen = WebVid.compile(gl.VERTEX_SHADER, 'VERTEX\n#define SCREEN', crt.source, 'crt.glsl');
			if (!vs || !screen)
				return false;
			crt.programs = [];
			for (i = 0 ; i <= last ; i++) {
				fs = WebVid.compile(gl.FRAGMENT_SHADER, `FRAGMENT\n#define PASS_${crt.names[i].toUpperCase()}`, crt.source,
					'crt.glsl');
				program = fs && WebVid.link(i == last ? screen : vs, fs, `crt.glsl's ${crt.names[i]}`);
				if (!program)
					return false;
				gl.useProgram(program);
				gl.uniformBlockBinding(program, gl.getUniformBlockIndex(program, 'ParamsBlock'), 1);
				gl.uniformBlockBinding(program, gl.getUniformBlockIndex(program, 'PassBlock'), 2);
				for (k = 0 ; k < 8 ; k++)
					gl.uniform1i(gl.getUniformLocation(program, `T${k}`), k);
				crt.programs.push(program);
			}
			gl.useProgram(WebVid.program);

			// the parameters, and each pass's sizes where a range of a buffer may start
			crt.params = gl.createBuffer();
			gl.bindBuffer(gl.UNIFORM_BUFFER, crt.params);
			gl.bufferData(gl.UNIFORM_BUFFER, crt.paramssize, gl.DYNAMIC_DRAW);
			crt.stride = Math.ceil(64 / gl.getParameter(gl.UNIFORM_BUFFER_OFFSET_ALIGNMENT))
				* gl.getParameter(gl.UNIFORM_BUFFER_OFFSET_ALIGNMENT);
			crt.passes = gl.createBuffer();
			gl.bindBuffer(gl.UNIFORM_BUFFER, crt.passes);
			gl.bufferData(gl.UNIFORM_BUFFER, crt.stride * crt.names.length, gl.DYNAMIC_DRAW);

			crt.textures = [];
			crt.specs.forEach((spec, t) => spec && WebVid.crttexture(t));
			return true;
		},

		// a texture of the CRT shader's as its spec has it: one the passes draw in,
		// with its framebuffer (cleared, for one read the frame after), or a LUT
		crttexture(t) {
			var gl = WebVid.gl, crt = WebVid.crt, spec = crt.specs[t], old = crt.textures[t], tex;

			if (old) {
				gl.deleteTexture(old.texture);
				gl.deleteFramebuffer(old.framebuffer);
			}
			tex = crt.textures[t] = {texture: gl.createTexture(), framebuffer: null, width: spec.width,
				height: spec.height};
			gl.activeTexture(gl.TEXTURE0);
			gl.bindTexture(gl.TEXTURE_2D, tex.texture);
			// half floats where the GPU draws them (EXT_color_buffer_float), else 8 bits
			gl.texStorage2D(gl.TEXTURE_2D, spec.levels, spec.half && crt.float ? gl.RGBA16F : gl.RGBA8, spec.width,
				spec.height);
			gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, spec.levels > 1 ? gl.LINEAR_MIPMAP_LINEAR
				: spec.linear ? gl.LINEAR : gl.NEAREST);
			gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, spec.linear ? gl.LINEAR : gl.NEAREST);
			gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
			gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
			if (spec.pixels) {
				gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 0);
				gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, spec.width, spec.height, gl.RGBA, gl.UNSIGNED_BYTE, spec.pixels);
			}
			else {
				tex.framebuffer = gl.createFramebuffer();
				gl.bindFramebuffer(gl.FRAMEBUFFER, tex.framebuffer);
				gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex.texture, 0);
				if (spec.feedback) {
					gl.clearColor(0, 0, 0, 0);
					gl.clear(gl.COLOR_BUFFER_BIT);
				}
				gl.bindFramebuffer(gl.FRAMEBUFFER, null);
			}
			gl.bindTexture(gl.TEXTURE_2D, WebVid.view);
		},

		// the frame through the CRT shader, its layers just copied: drawn as
		// present.glsl shows it at its own size into the shader's first texture,
		// then the passes, the last into x, y (from the bottom), width x height of
		// framebuffer (null the canvas). table is the passes' textures this frame,
		// 8 a pass: what it draws in (-1 the canvas), and what it reads (-1 after)
		crtdraw(constants, params, passes, table, framebuffer, x, y, width, height) {
			var gl = WebVid.gl, crt = WebVid.crt, i, k, t, out, original = crt.textures[0];

			gl.useProgram(WebVid.program);
			gl.uniform1f(WebVid.upright, 1);
			gl.bindBuffer(gl.UNIFORM_BUFFER, WebVid.ubo);
			gl.bufferSubData(gl.UNIFORM_BUFFER, 0, HEAPU8, constants, 64);
			gl.bindFramebuffer(gl.FRAMEBUFFER, original.framebuffer);
			gl.viewport(0, 0, original.width, original.height);
			gl.drawArrays(gl.TRIANGLES, 0, 3);
			gl.uniform1f(WebVid.upright, 0);

			gl.bindBuffer(gl.UNIFORM_BUFFER, crt.params);
			gl.bufferSubData(gl.UNIFORM_BUFFER, 0, HEAPU8, params, crt.paramssize);
			gl.bindBuffer(gl.UNIFORM_BUFFER, crt.passes);
			for (i = 0 ; i < crt.names.length ; i++)
				gl.bufferSubData(gl.UNIFORM_BUFFER, i * crt.stride, HEAPU8, passes + i * 64, 64);
			gl.bindBufferBase(gl.UNIFORM_BUFFER, 1, crt.params);

			for (i = 0 ; i < crt.names.length ; i++) {
				gl.useProgram(crt.programs[i]);
				gl.bindBufferRange(gl.UNIFORM_BUFFER, 2, crt.passes, i * crt.stride, 64);
				for (k = 1 ; k < 8 && (t = HEAP32[(table >> 2) + i * 8 + k]) >= 0 ; k++) {
					gl.activeTexture(gl.TEXTURE0 + k - 1);
					gl.bindTexture(gl.TEXTURE_2D, crt.textures[t].texture);
				}
				out = HEAP32[(table >> 2) + i * 8];
				if (out < 0) {
					gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
					gl.viewport(x, y, width, height);
				}
				else {
					gl.bindFramebuffer(gl.FRAMEBUFFER, crt.textures[out].framebuffer);
					gl.viewport(0, 0, crt.textures[out].width, crt.textures[out].height);
				}
				gl.drawArrays(gl.TRIANGLES, 0, 3);
				// avg-lum reads the mip chain of what the pass drew
				if (i == crt.mipspass) {
					gl.bindTexture(gl.TEXTURE_2D, crt.textures[out].texture);
					gl.generateMipmap(gl.TEXTURE_2D);
				}
			}

			// present.glsl's, as it has them
			gl.useProgram(WebVid.program);
			gl.activeTexture(gl.TEXTURE1);
			gl.bindTexture(gl.TEXTURE_2D, WebVid.hud);
			gl.activeTexture(gl.TEXTURE0);
			gl.bindTexture(gl.TEXTURE_2D, WebVid.view);
			gl.bindFramebuffer(gl.FRAMEBUFFER, null);
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
	web_vid_present: (view, hud, huddirty, rowpixels, constants, x, y, width, height, paced, crtconstants, crtparams,
		crtpasses, crttable) => {
		var gl = WebVid.gl, canvas = WebVid.canvas, now = performance.now(), layout;

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
		// the bars, when the canvas or the viewport changed: the frame is drawn
		// over the viewport's pixels whole, and a canvas that doesn't keep them
		// (with vsync) is cleared for each frame by the browser
		layout = `${canvas.width}x${canvas.height} ${x},${y} ${width}x${height}`;
		if (layout != WebVid.cleared) {
			gl.viewport(0, 0, canvas.width, canvas.height);
			gl.clearColor(0, 0, 0, 1);
			gl.clear(gl.COLOR_BUFFER_BIT);
			WebVid.cleared = layout;
		}

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
		// GL's viewport counts from the bottom
		if (crttable)
			WebVid.crtdraw(crtconstants, crtparams, crtpasses, crttable, null, x, canvas.height - y - height, width,
				height);
		else {
			gl.bindBuffer(gl.UNIFORM_BUFFER, WebVid.ubo);
			gl.bufferSubData(gl.UNIFORM_BUFFER, 0, HEAPU8, constants, 64);
			gl.viewport(x, canvas.height - y - height, width, height);
			gl.drawArrays(gl.TRIANGLES, 0, 3);
		}
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

	// the CRT shader turned on: crt.glsl with the files it includes, the passes'
	// names (a comma between), the pass after which the mip chain is made, and
	// the parameters' size; its textures come with web_vid_crt_texture
	web_vid_crt_init__deps: ['$WebVid'],
	web_vid_crt_init: (glsl, core, params, names, mipspass, paramssize) => {
		var source = UTF8ToString(glsl).replace(/^#version .*$/m, '');

		source = source.replace('#include "crt_core.inc"', UTF8ToString(core))
			.replace('#include "crt_params.h"', UTF8ToString(params));
		WebVid.crt = {source: source, names: UTF8ToString(names).split(','), mipspass: mipspass,
			paramssize: paramssize, specs: [], textures: []};
		if (WebVid.crtbuild())
			return true;
		WebVid.crt = null;
		return false;
	},

	// a texture of the CRT shader's, made anew: width x height, half floats or 8
	// bits, filtered bilinearly or not, levels of mips, cleared or a LUT's pixels
	web_vid_crt_texture__deps: ['$WebVid'],
	web_vid_crt_texture: (t, width, height, half, linear, levels, feedback, pixels) => {
		var crt = WebVid.crt, old = crt.specs[t];

		if (old && !pixels && old.width == width && old.height == height)
			return;
		crt.specs[t] = {width: width, height: height, half: !!half, linear: !!linear, levels: levels,
			feedback: !!feedback, pixels: pixels ? HEAPU8.slice(pixels, pixels + width * height * 4) : null};
		WebVid.crttexture(t);
	},

	// the CRT shader turned off
	web_vid_crt_free__deps: ['$WebVid'],
	web_vid_crt_free: () => {
		var gl = WebVid.gl, crt = WebVid.crt;

		if (!crt)
			return;
		crt.programs?.forEach((p) => gl.deleteProgram(p));
		crt.textures.forEach((t) => {
			gl.deleteTexture(t.texture);
			gl.deleteFramebuffer(t.framebuffer);
		});
		gl.deleteBuffer(crt.params);
		gl.deleteBuffer(crt.passes);
		WebVid.crt = null;
	},

	// the frame through the CRT shader as web_vid_present draws it, but into
	// width x height pixels of a float target of its own, read back as
	// web_vid_drawto does (tests/test_present_webgl.c)
	web_vid_crt_drawto__deps: ['$WebVid'],
	web_vid_crt_drawto: (view, hud, rowpixels, constants, params, passes, table, width, height, out) => {
		var gl = WebVid.gl, fb = gl.createFramebuffer(), rb = gl.createRenderbuffer(), pixels, y;

		gl.bindRenderbuffer(gl.RENDERBUFFER, rb);
		gl.renderbufferStorage(gl.RENDERBUFFER, WebVid.crt.float ? gl.RGBA32F : gl.RGBA8, width, height);
		gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
		gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, rb);
		gl.clearColor(0, 0, 0, 1);
		gl.clear(gl.COLOR_BUFFER_BIT);

		gl.pixelStorei(gl.UNPACK_ROW_LENGTH, rowpixels);
		gl.activeTexture(gl.TEXTURE1);
		gl.bindTexture(gl.TEXTURE_2D, WebVid.hud);
		gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, WebVid.width, WebVid.height, gl.RED_INTEGER, gl.UNSIGNED_INT,
			HEAPU32, hud >> 2);
		gl.activeTexture(gl.TEXTURE0);
		gl.bindTexture(gl.TEXTURE_2D, WebVid.view);
		gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, WebVid.width, WebVid.height, gl.RED_INTEGER, gl.UNSIGNED_INT,
			HEAPU32, view >> 2);
		WebVid.crtdraw(constants, params, passes, table, fb, 0, 0, width, height);

		gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
		if (WebVid.crt.float) {
			pixels = new Float32Array(width * height * 4);
			gl.readPixels(0, 0, width, height, gl.RGBA, gl.FLOAT, pixels);
		}
		else {
			pixels = new Uint8Array(width * height * 4);
			gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
			pixels = Float32Array.from(pixels, (v) => v / 255);
		}
		// GL's rows count from the bottom
		for (y = 0 ; y < height ; y++)
			HEAPF32.set(pixels.subarray((height - 1 - y) * width * 4, (height - y) * width * 4), (out >> 2) + y * width * 4);
		gl.bindFramebuffer(gl.FRAMEBUFFER, null);
		gl.deleteFramebuffer(fb);
		gl.deleteRenderbuffer(rb);
		WebVid.hudforce = true;
		return WebVid.crt.float ? 2 : 1;
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
