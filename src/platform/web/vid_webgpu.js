// vid_webgpu.js -- the canvas of vid_webgl.js that shows HDR: WebGPU's, whose
// extended tone mapping shows light brighter than SDR white, as a WebGL 2
// canvas can't yet. It can't be desynchronized, so it is the canvas only with
// vsync (vid_webgl.c chooses). The device is made the first time it is wanted,
// which WebGPU does asynchronously (vid_webgl.js shows WebGL's canvas
// meanwhile), and kept; the frame is copied into two integer textures and
// drawn by the present shader (present.wgsl) as with WebGL.

addToLibrary({
	$WebGpu__deps: ['$UTF8ToString', '$stringToUTF8'],
	$WebGpu: {
		source: '',				// present.wgsl
		state: 'none',			// none, starting, ready, or failed (WebGL's canvas only)
		error: '',				// why it failed
		adapter: '',			// the GPU, by name
		device: null,
		module: null,			// present.wgsl, compiled
		layout: null,			// the shader's bindings
		pipelines: {},			// by the format drawn to
		ubo: null,
		view: null,				// the textures
		hud: null,
		bind: null,
		width: 0,				// their size
		height: 0,
		context: null,			// the canvas's
		hudforce: true,			// the 2D copied at the next draw, changed or not
		busy: false,			// the last frame drawn is still on the GPU

		// the device, the shader and its pipeline for the canvas; once, unless
		// the device is lost
		async start() {
			var adapter, device, module;

			WebGpu.state = 'starting';
			try {
				if (!navigator.gpu)
					throw new Error('this browser has no WebGPU');
				adapter = await navigator.gpu.requestAdapter();
				if (!adapter)
					throw new Error('no GPU it can draw with');
				device = await adapter.requestDevice();
				WebGpu.adapter = adapter.info?.description || [adapter.info?.vendor, adapter.info?.architecture]
					.filter((s) => s).join(' ') || 'a GPU';
				WebGpu.layout = device.createBindGroupLayout({entries: [
					{binding: 0, visibility: GPUShaderStage.FRAGMENT, buffer: {type: 'uniform'}},
					{binding: 1, visibility: GPUShaderStage.FRAGMENT, texture: {sampleType: 'uint'}},
					{binding: 2, visibility: GPUShaderStage.FRAGMENT, texture: {sampleType: 'uint'}},
				]});
				module = device.createShaderModule({code: WebGpu.source});
				WebGpu.module = module;
				WebGpu.device = device;
				WebGpu.pipelines = {};
				await WebGpu.pipeline('rgba16float');
				WebGpu.ubo = device.createBuffer({size: 64, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST});
				if (WebGpu.width)
					WebGpu.textures(WebGpu.width, WebGpu.height);
				device.lost.then((info) => {
					if (WebGpu.device != device)
						return;
					// made again the next time it is wanted
					WebGpu.device = null;
					WebGpu.state = 'none';
					WebGpu.error = `the device was lost: ${info.message}`;
					console.warn(`WebGPU: ${WebGpu.error}`);
				});
				WebGpu.state = 'ready';
			}
			catch (e) {
				WebGpu.device = null;
				WebGpu.state = 'failed';
				WebGpu.error = e.message;
			}
		},

		// the pipeline that draws to a format, made the first time
		async pipeline(format) {
			var device = WebGpu.device, info;

			if (WebGpu.pipelines[format])
				return WebGpu.pipelines[format];
			try {
				WebGpu.pipelines[format] = await device.createRenderPipelineAsync({
					layout: device.createPipelineLayout({bindGroupLayouts: [WebGpu.layout]}),
					vertex: {module: WebGpu.module, entryPoint: 'vs'},
					fragment: {module: WebGpu.module, entryPoint: 'fs', targets: [{format}]},
					primitive: {topology: 'triangle-list'},
				});
			}
			catch (e) {
				info = await WebGpu.module.getCompilationInfo();
				throw new Error(`present.wgsl: ${info.messages.map((m) => `${m.lineNum}:${m.linePos}: ${m.message}`)
					.join('; ') || e.message}`);
			}
			return WebGpu.pipelines[format];
		},

		// false if the frame is bigger than the device's textures may be
		fits(width, height) {
			var max = WebGpu.device.limits.maxTextureDimension2D;

			if (width <= max && height <= max)
				return true;
			WebGpu.error = `the frame's ${width}x${height} is more than the GPU's ${max}x${max}`;
			return false;
		},

		textures(width, height) {
			var device = WebGpu.device;

			WebGpu.width = width;
			WebGpu.height = height;
			WebGpu.hudforce = true;
			if (!device)
				return;
			for (const name of ['view', 'hud']) {
				WebGpu[name]?.destroy();
				WebGpu[name] = device.createTexture({size: [width, height], format: 'r32uint',
					usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST});
			}
			WebGpu.bind = device.createBindGroup({layout: WebGpu.layout, entries: [
				{binding: 0, resource: {buffer: WebGpu.ubo}},
				{binding: 1, resource: WebGpu.view.createView()},
				{binding: 2, resource: WebGpu.hud.createView()},
			]});
		},

		// the canvas's context, shown in HDR; null if it can't be, with why
		attach(canvas) {
			var context = canvas.getContext('webgpu');

			if (!context || !WebGpu.fits(WebGpu.width, WebGpu.height))
				return null;
			try {
				context.configure({device: WebGpu.device, format: 'rgba16float', colorSpace: 'srgb',
					toneMapping: {mode: 'extended'}, alphaMode: 'opaque'});
			}
			catch (e) {
				WebGpu.error = e.message;
				return null;
			}
			// a browser that doesn't know the tone mapping leaves it out
			if (context.getConfiguration?.()?.toneMapping?.mode != 'extended') {
				WebGpu.error = "this browser's WebGPU canvas shows SDR alone";
				return null;
			}
			WebGpu.context = context;
			WebGpu.hudforce = true;
			WebGpu.busy = false;
			return context;
		},

		// the layers and the constants into the GPU's copies (the pointers unsigned)
		upload(view, hud, huddirty, rowpixels, constants) {
			var queue = WebGpu.device.queue, size = [WebGpu.width, WebGpu.height];
			var bytes = (WebGpu.height - 1) * rowpixels * 4 + WebGpu.width * 4, layout = {bytesPerRow: rowpixels * 4};

			queue.writeTexture({texture: WebGpu.view}, HEAPU8.subarray(view, view + bytes), layout, size);
			if (huddirty || WebGpu.hudforce) {
				queue.writeTexture({texture: WebGpu.hud}, HEAPU8.subarray(hud, hud + bytes), layout, size);
				WebGpu.hudforce = false;
			}
			queue.writeBuffer(WebGpu.ubo, 0, HEAPU8, constants, 64);
		},

		// the frame drawn into a texture over x, y, width x height, the rest black
		draw(encoder, target, format, x, y, width, height) {
			var pass = encoder.beginRenderPass({colorAttachments: [{view: target.createView(),
				clearValue: [0, 0, 0, 1], loadOp: 'clear', storeOp: 'store'}]});

			pass.setPipeline(WebGpu.pipelines[format]);
			pass.setBindGroup(0, WebGpu.bind);
			pass.setViewport(x, y, Math.min(width, target.width - x), Math.min(height, target.height - y), 0, 1);
			pass.draw(3);
			pass.end();
		},

		// the frame on the canvas; unpaced, the next isn't drawn while this is on the GPU
		present(view, hud, huddirty, rowpixels, constants, x, y, width, height, paced) {
			var device = WebGpu.device, encoder;

			WebGpu.upload(view, hud, huddirty, rowpixels, constants);
			encoder = device.createCommandEncoder();
			WebGpu.draw(encoder, WebGpu.context.getCurrentTexture(), 'rgba16float', x, y, width, height);
			device.queue.submit([encoder.finish()]);
			if (!paced) {
				WebGpu.busy = true;
				device.queue.onSubmittedWorkDone().then(() => WebGpu.busy = false);
			}
		},
	},

	// the device made for test_present_webgpu.c, without a canvas; false if it
	// couldn't be (web_gpu_error says why)
	web_gpu_start__deps: ['$WebGpu'],
	web_gpu_start__async: 'auto',
	web_gpu_start: async (source) => {
		WebGpu.source = UTF8ToString(source);
		await WebGpu.start();
		return WebGpu.state == 'ready';
	},

	web_gpu_error__deps: ['$WebGpu'],
	web_gpu_error: (buf, size) => stringToUTF8(WebGpu.error, buf, size),

	web_gpu_adapter__deps: ['$WebGpu'],
	web_gpu_adapter: (buf, size) => stringToUTF8(WebGpu.adapter, buf, size),

	web_gpu_settextures__deps: ['$WebGpu'],
	web_gpu_settextures: (width, height) => {
		if (!WebGpu.fits(width, height))
			return false;
		WebGpu.textures(width, height);
		return true;
	},

	// the frame drawn as WebGpu.present draws it, but into width x height pixels
	// of a float target of its own, read back as floats a channel, top row
	// first, into out (test_present_webgpu.c)
	web_gpu_drawto__deps: ['$WebGpu'],
	web_gpu_drawto__async: 'auto',
	web_gpu_drawto: async (view, hud, rowpixels, constants, width, height, out) => {
		var device = WebGpu.device, target, buffer, encoder, pixels, y;
		var pitch = Math.ceil(width * 16 / 256) * 256;

		// pointers, unsigned: the memory goes past 2 GB
		view >>>= 0;
		hud >>>= 0;
		constants >>>= 0;
		out >>>= 0;

		await WebGpu.pipeline('rgba32float');
		target = device.createTexture({size: [width, height], format: 'rgba32float',
			usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC});
		buffer = device.createBuffer({size: pitch * height, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ});
		WebGpu.upload(view, hud, true, rowpixels, constants);
		encoder = device.createCommandEncoder();
		WebGpu.draw(encoder, target, 'rgba32float', 0, 0, width, height);
		encoder.copyTextureToBuffer({texture: target}, {buffer, bytesPerRow: pitch}, [width, height]);
		device.queue.submit([encoder.finish()]);

		await buffer.mapAsync(GPUMapMode.READ);
		pixels = new Float32Array(buffer.getMappedRange());
		for (y = 0 ; y < height ; y++)
			HEAPF32.set(pixels.subarray(y * pitch / 4, y * pitch / 4 + width * 4), (out >>> 2) + y * width * 4);
		buffer.unmap();
		buffer.destroy();
		target.destroy();
	},
});
