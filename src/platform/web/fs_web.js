// fs_web.js -- the game's files in a page (the basedir "." of sys_web_gui.c):
// the Quake directory the site serves, read as the game reads it, and what
// the game writes, kept in the browser
//
// The site lists its files (manifest.json: each path, size and modification
// time), and before the game starts the page makes their tree at /quake:
// directories, and files of those sizes whose contents come as the game reads
// them, in 1 MB pieces: those read already this session (256 MB of them), else
// the fetch worker's (fs_web_worker.js), which keeps them in the browser for 30
// days or until the site's file changes, and gets them from the site when it
// doesn't have them. The file system is synchronous, so the page's thread waits
// for the worker, spinning (a page's own thread may not sleep): a map's load
// waits for the pieces of the map and its models and sounds once.
//
// What the game writes (config.cfg, demos, screenshots, downloads) is a file
// of the page's own, in place of the site's of the same name: kept in
// IndexedDB when it is closed (every 30 s while open, and when the page
// hides), and laid over the site's tree at the next start. A file of the
// site's the game deletes stays deleted, and a file it opens to change is
// fetched whole first. Names are found whatever their case, as on Windows
// (the files of a Quake directory often are PAK0.PAK).

addToLibrary({
	$SWFS__deps: ['$FS', '$MEMFS', '$addRunDependency', '$removeRunDependency', '$addOnPreRun'],
	$SWFS__postset: 'if (!ENVIRONMENT_IS_PTHREAD) addOnPreRun(SWFS.start);',
	$SWFS: {
		CHUNK: 1 << 20,
		KEEP: 256 << 20,		// pieces kept this session
		ROOT: '/quake',

		worker: null,
		ctl: null,				// Int32Array the worker answers in: the request, the length, an error
		data: null,				// Uint8Array of the piece
		seq: 0,					// the last request
		chunks: new Map(),		// the pieces read this session, oldest first
		kept: 0,				// their bytes
		db: null,				// the IndexedDB of the page's files
		dirty: new Set(),		// files written since they were kept
		remote: new Set(),		// the site's paths, lowercased: what a deletion hides
		ops: null,

		/*
		=====================================================================
		START
		=====================================================================
		*/

		start() {
			var base = new URL(Module['swAssets'] ?? 'quake/', location.href).href;
			var manifest = new URL(Module['swManifest'] ?? 'manifest.json', location.href).href;
			var listed;

			addRunDependency('swfs');
			SWFS.ctl = new Int32Array(new SharedArrayBuffer(16));
			SWFS.data = new Uint8Array(new SharedArrayBuffer(SWFS.CHUNK));
			SWFS.worker = new Worker(locateFile('softworld-fs.js'));
			listed = new Promise((resolve, reject) => {
				SWFS.worker.onmessage = (e) => e.data.op == 'manifest' ? resolve(e.data.manifest)
					: reject(new Error(e.data.text));
				SWFS.worker.onerror = (e) => reject(new Error(e.message || "softworld-fs.js didn't start"));
			});
			SWFS.worker.postMessage({op: 'init', base, manifest, ctl: SWFS.ctl, data: SWFS.data});

			Promise.all([listed, SWFS.openOverlay()]).then(([list, records]) => {
				SWFS.worker.onmessage = null;
				SWFS.build(list, records);
				FS.chdir(SWFS.ROOT);
				navigator.storage?.persist?.().catch(() => {});
				setInterval(SWFS.flushDirty, 30000);
				return SWFS.askForGame();
			}).then(() => removeRunDependency('swfs'))
				.catch((e) => abort(`Can't reach the game's files: ${e.message}`));
		},

		// Without id1's pak0.pak (a site serving no Quake directory, as the
		// project's own page): the paks the player gives the page
		// (Module.swAskForGame, as [{name, data}]), in id1 and kept with the
		// page's own files, so the next start has them
		async askForGame() {
			if (FS.analyzePath(SWFS.ROOT + '/id1/pak0.pak').exists || !Module['swAskForGame'])
				return;
			for (const {name, data} of await Module['swAskForGame']()) {
				var [parent, base] = SWFS.parentOf('id1/' + name.toLowerCase());
				var node = SWFS.child(parent, base) ?? SWFS.createNode(parent, base, 0o100666, 0);

				SWFS.makeLocal(node, data);
				node.atime = node.mtime = node.ctime = Date.now();
				SWFS.keep(node);
			}
		},

		// the tree of the site's files, the page's own over it
		build(list, records) {
			SWFS.initOps();
			FS.mkdir(SWFS.ROOT);
			FS.mount(SWFS, {}, SWFS.ROOT);

			for (const dir of list.dirs) {
				SWFS.remote.add(dir.toLowerCase());
				SWFS.mkdirs(dir);
			}
			for (const [path, size, mtime] of list.files) {
				var [parent, name] = SWFS.parentOf(path);
				var node = SWFS.createNode(parent, name, 0o100666, 0);

				SWFS.remote.add(path.toLowerCase());
				node.stream_ops = SWFS.ops.remote;
				node.remote = {path, size, mtime, key: `${path}?${size}-${mtime}#`, last: -2};
				node.contents = null;
				node.usedBytes = size;
				node.atime = node.mtime = node.ctime = mtime * 1000;
			}

			// directories first, the outer ones before what is in them
			records.sort((a, b) => (a.kind != 'dir') - (b.kind != 'dir') || a.path.length - b.path.length);
			for (const r of records) {
				var [parent, name] = SWFS.parentOf(r.path);
				var existing = SWFS.child(parent, name);

				if (r.kind == 'dir') {
					if (!existing)
						SWFS.createNode(parent, name, 0o40777, 0);
					continue;
				}
				if (existing) {
					delete parent.contents[existing.name];
					FS.hashRemoveNode(existing);
				}
				if (r.kind == 'file') {
					var node = SWFS.createNode(parent, name, 0o100666, 0);
					node.contents = r.data;
					node.usedBytes = r.data.length;
					node.atime = node.mtime = node.ctime = r.mtime;
				}
			}
		},

		// the operations of the tree's nodes: MEMFS's, with what the page keeps
		initOps() {
			var EPERM = {{{ cDefs.EPERM }}}, ENODEV = {{{ cDefs.ENODEV }}}, ENOTEMPTY = {{{ cDefs.ENOTEMPTY }}};

			SWFS.ops = {
				dir: {
					getattr: MEMFS.node_ops.getattr,
					setattr: MEMFS.node_ops.setattr,
					lookup: MEMFS.node_ops.lookup,
					readdir: MEMFS.node_ops.readdir,
					mknod(parent, name, mode, dev) {
						var node = SWFS.createNode(parent, name, mode, dev);

						if (FS.isDir(mode))
							SWFS.keep(node);
						else
							SWFS.dirty.add(node);		// kept when closed, written or not
						return node;
					},
					rename(node, newdir, newname) {
						var oldpath = SWFS.path(node), existing = SWFS.child(newdir, newname);

						if (FS.isDir(node.mode))
							throw new FS.ErrnoError(EPERM);
						if (node.remote)
							SWFS.fetchWhole(node);
						// the name it replaces may be spelled otherwise
						if (existing && existing !== node)
							delete newdir.contents[existing.name];
						MEMFS.node_ops.rename(node, newdir, newname);
						node.parent = newdir;		// as FS.rename sets it after this
						SWFS.forget(oldpath);
						SWFS.keep(node);
					},
					unlink(parent, name) {
						var node = FS.lookupNode(parent, name);

						SWFS.forget(SWFS.path(node));
						SWFS.dirty.delete(node);
						delete parent.contents[node.name];
						parent.ctime = parent.mtime = Date.now();
					},
					rmdir(parent, name) {
						var node = FS.lookupNode(parent, name);

						for (var i in node.contents)
							throw new FS.ErrnoError(ENOTEMPTY);
						SWFS.forget(SWFS.path(node));
						delete parent.contents[node.name];
						parent.ctime = parent.mtime = Date.now();
					},
					symlink() {
						throw new FS.ErrnoError(EPERM);
					},
				},
				file: {
					getattr: MEMFS.node_ops.getattr,
					setattr(node, attr) {
						// a size set (a file opened to be written over): the site's
						// contents as far as it keeps them
						if (attr.size !== undefined && node.remote) {
							if (attr.size)
								SWFS.fetchWhole(node);
							else
								SWFS.makeLocal(node, new Uint8Array(0));
						}
						MEMFS.node_ops.setattr(node, attr);
						if (attr.size !== undefined)
							SWFS.dirty.add(node);
					},
				},
				// the page's own files
				local: {
					llseek: MEMFS.stream_ops.llseek,
					read: MEMFS.stream_ops.read,
					write(stream, buffer, offset, length, position, canOwn) {
						SWFS.dirty.add(stream.node);
						return MEMFS.stream_ops.write(stream, buffer, offset, length, position, canOwn);
					},
					mmap: MEMFS.stream_ops.mmap,
					msync: MEMFS.stream_ops.msync,
					close(stream) {
						if (SWFS.dirty.has(stream.node))
							SWFS.keep(stream.node);
					},
				},
				// the site's: fetched as read, whole when opened to be changed
				remote: {
					llseek: MEMFS.stream_ops.llseek,
					read: (stream, buffer, offset, length, position) =>
						SWFS.read(stream.node, buffer, offset, length, position),
					open(stream) {
						if ((stream.flags & {{{ cDefs.O_ACCMODE }}}) != {{{ cDefs.O_RDONLY }}}) {
							SWFS.fetchWhole(stream.node);
							stream.stream_ops = SWFS.ops.local;
						}
					},
					mmap() {
						throw new FS.ErrnoError(ENODEV);
					},
				},
			};
		},

		mount(mount) {
			return SWFS.createNode(null, '/', 0o40777, 0);
		},

		createNode(parent, name, mode, dev) {
			var node = MEMFS.createNode(parent, name, mode, dev);

			if (FS.isDir(node.mode))
				node.node_ops = SWFS.ops.dir;
			else if (FS.isFile(node.mode)) {
				node.node_ops = SWFS.ops.file;
				node.stream_ops = SWFS.ops.local;
			}
			return node;
		},

		// the child of that name, whatever its case; undefined if none
		child(parent, name) {
			try {
				return FS.lookupNode(parent, name);
			}
			catch (e) {
				return undefined;
			}
		},

		// the directory of a path under the tree (made as needed), and its name
		parentOf(path) {
			var slash = path.lastIndexOf('/');

			return [SWFS.mkdirs(path.slice(0, slash + 1)), path.slice(slash + 1)];
		},

		mkdirs(path) {
			var node = FS.lookupPath(SWFS.ROOT).node;

			for (const name of path.split('/'))
				if (name)
					node = SWFS.child(node, name) ?? SWFS.createNode(node, name, 0o40777, 0);
			return node;
		},

		// a node's path under the tree
		path(node) {
			return FS.getPath(node).slice(SWFS.ROOT.length + 1);
		},

		/*
		=====================================================================
		READING THE SITE'S FILES
		=====================================================================
		*/

		read(node, buffer, offset, length, position) {
			var end = Math.min(node.usedBytes, position + length), done = 0;
			var index, chunk, start, n;

			while (position < end) {
				index = Math.floor(position / SWFS.CHUNK);
				chunk = SWFS.chunk(node.remote, index);
				start = position - index * SWFS.CHUNK;
				n = Math.min(chunk.length - start, end - position);
				if (n <= 0)
					break;
				buffer.set(chunk.subarray(start, start + n), offset + done);
				done += n;
				position += n;
			}
			return done;
		},

		chunk(remote, index) {
			var key = remote.key + index, chunk = SWFS.chunks.get(key), oldest;

			if (chunk) {
				SWFS.chunks.delete(key);		// the newest again
				SWFS.chunks.set(key, chunk);
			}
			else {
				chunk = SWFS.fetchChunk(remote, index);
				SWFS.chunks.set(key, chunk);
				SWFS.kept += chunk.length;
				while (SWFS.kept > SWFS.KEEP) {
					oldest = SWFS.chunks.entries().next().value;
					SWFS.chunks.delete(oldest[0]);
					SWFS.kept -= oldest[1].length;
				}
			}
			// read on: the next piece comes meanwhile
			if (index == remote.last + 1 && (index + 1) * SWFS.CHUNK < remote.size)
				SWFS.worker.postMessage({op: 'prefetch', path: remote.path, size: remote.size, mtime: remote.mtime,
					index: index + 1});
			remote.last = index;
			return chunk;
		},

		// the worker's answer, waited for: the page's thread may only spin
		fetchChunk(remote, index) {
			var seq = ++SWFS.seq, started = performance.now();

			SWFS.worker.postMessage({op: 'read', seq, path: remote.path, size: remote.size, mtime: remote.mtime,
				index});
			while (Atomics.load(SWFS.ctl, 0) != seq)
				if (performance.now() - started > 60000) {
					err(`softworld: ${remote.path}: no answer in a minute`);
					throw new FS.ErrnoError({{{ cDefs.EIO }}});
				}
			if (Atomics.load(SWFS.ctl, 2))
				throw new FS.ErrnoError({{{ cDefs.EIO }}});
			return SWFS.data.slice(0, Atomics.load(SWFS.ctl, 1));
		},

		fetchWhole(node) {
			var contents = new Uint8Array(node.usedBytes);

			SWFS.read(node, contents, 0, node.usedBytes, 0);
			SWFS.makeLocal(node, contents);
		},

		makeLocal(node, contents) {
			node.remote = null;
			node.contents = contents;
			node.usedBytes = contents.length;
			node.stream_ops = SWFS.ops.local;
		},

		/*
		=====================================================================
		THE PAGE'S OWN FILES
		=====================================================================
		*/

		openOverlay() {
			return new Promise((resolve) => {
				var request;

				try {
					request = indexedDB.open('softworld', 1);
				}
				catch (e) {
					resolve([]);
					return;
				}
				request.onupgradeneeded = () => request.result.createObjectStore('files', {keyPath: 'key'});
				request.onsuccess = () => {
					var all;

					SWFS.db = request.result;
					all = SWFS.db.transaction('files').objectStore('files').getAll();
					all.onsuccess = () => resolve(all.result);
					all.onerror = () => resolve([]);
				};
				request.onerror = () => {
					err("softworld: no IndexedDB: what the game writes isn't kept");
					resolve([]);
				};
			});
		},

		store(record, remove) {
			var files;

			if (!SWFS.db)
				return;
			try {
				files = SWFS.db.transaction('files', 'readwrite').objectStore('files');
				if (remove)
					files.delete(record);
				else
					files.put(record);
			}
			catch (e) {
				err(`softworld: couldn't keep ${record.path ?? record}: ${e.message}`);
			}
		},

		// a file or directory as it is now
		keep(node) {
			var path = SWFS.path(node), key = path.toLowerCase();

			SWFS.dirty.delete(node);
			if (FS.isDir(node.mode))
				SWFS.store({key, path, kind: 'dir', mtime: node.mtime});
			else
				SWFS.store({key, path, kind: 'file', mtime: node.mtime, data: node.contents.slice(0, node.usedBytes)});
		},

		// a path deleted or renamed: the site's stays hidden
		forget(path) {
			var key = path.toLowerCase();

			if (SWFS.remote.has(key))
				SWFS.store({key, path, kind: 'gone'});
			else
				SWFS.store(key, true);
		},

		// files written and not yet closed (a log, a demo recording), kept as they are
		flushDirty() {
			for (const node of SWFS.dirty)
				if (node.parent.contents[node.name] === node)
					SWFS.keep(node);
				else
					SWFS.dirty.delete(node);
		},
	},
});
