# serve.py -- serves a web build of SoftWorld and a Quake directory to a
# browser on this machine
#
#   python src/platform/web/serve.py build/web/Release C:\quake
#
# and then http://localhost:8000/ (a command line after a ?, as
# softworld.html?+map dm4). The page comes from the build's directory, the
# game's files from the Quake directory under /quake/ (links followed), listed
# in /manifest.json: each file's path, size and modification time, read again
# at each request. Files are sent in the ranges the page asks for, and every
# response is cross-origin isolated (COOP and COEP), as the page's shared
# memory needs. Browsers allow that over http on localhost alone; from another
# machine the page needs https.

import argparse
import email.utils
import json
import mimetypes
import os
import re
import sys
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlsplit

TYPES = {'.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.wasm': 'application/wasm',
	'.json': 'application/json', '.cfg': 'text/plain; charset=utf-8'}


# the files and directories under root, with '/' between names
def manifest (root):
	files, dirs = [], []
	for dirpath, dirnames, filenames in os.walk(root, followlinks=True):
		dirnames[:] = sorted(d for d in dirnames if not d.startswith('.'))
		rel = os.path.relpath(dirpath, root).replace(os.sep, '/')
		prefix = '' if rel == '.' else rel + '/'
		if prefix:
			dirs.append(rel)
		for name in sorted(filenames):
			if name.startswith('.'):
				continue
			try:
				st = os.stat(os.path.join(dirpath, name))
			except OSError:
				continue
			files.append([prefix + name, st.st_size, int(st.st_mtime)])
	return {'files': files, 'dirs': dirs}


class Handler (BaseHTTPRequestHandler):
	build = basedir = None

	def headers_common (self):
		self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
		self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
		self.send_header('Cross-Origin-Resource-Policy', 'same-origin')
		self.send_header('Cache-Control', 'no-cache')

	def fail (self, status):
		self.send_response(status)
		self.headers_common()
		self.send_header('Content-Length', '0')
		self.end_headers()

	def do_HEAD (self):
		self.do_GET(body=False)

	def do_GET (self, body=True):
		path = unquote(urlsplit(self.path).path)
		if path == '/':
			self.send_response(HTTPStatus.FOUND)
			self.headers_common()
			self.send_header('Location', '/softworld.html')
			self.send_header('Content-Length', '0')
			self.end_headers()
			return
		if path == '/manifest.json':
			data = json.dumps(manifest(self.basedir), separators=(',', ':')).encode()
			self.send_response(HTTPStatus.OK)
			self.headers_common()
			self.send_header('Content-Type', TYPES['.json'])
			self.send_header('Content-Length', str(len(data)))
			self.end_headers()
			if body:
				self.wfile.write(data)
			return

		root = self.build
		if path.startswith('/quake/'):
			root, path = self.basedir, path[len('/quake'):]
		names = path.split('/')[1:]
		# nothing outside the directory: no '..', no drive or backslashes
		if not names or any(n in ('', '.', '..') or '\\' in n or ':' in n for n in names):
			self.fail(HTTPStatus.NOT_FOUND)
			return
		self.send_file(os.path.join(root, *names), body)

	def send_file (self, name, body):
		try:
			f = open(name, 'rb')
		except OSError:
			self.fail(HTTPStatus.NOT_FOUND)
			return
		with f:
			st = os.fstat(f.fileno())
			if not os.path.isfile(name):
				self.fail(HTTPStatus.NOT_FOUND)
				return
			size, mtime = st.st_size, int(st.st_mtime)
			start, end, status = 0, size, HTTPStatus.OK

			since = self.headers.get('If-Modified-Since')
			if since and 'Range' not in self.headers:
				try:
					if email.utils.parsedate_to_datetime(since).timestamp() >= mtime:
						self.send_response(HTTPStatus.NOT_MODIFIED)
						self.headers_common()
						self.end_headers()
						return
				except (TypeError, ValueError):
					pass

			# one range: a-b, a- or -n
			m = re.fullmatch(r'bytes=(\d*)-(\d*)', self.headers.get('Range', '').strip())
			if m and (m[1] or m[2]):
				if m[1]:
					start, end = int(m[1]), min(int(m[2]) + 1, size) if m[2] else size
				else:
					start, end = max(size - int(m[2]), 0), size
				if start >= end:
					self.send_response(HTTPStatus.REQUESTED_RANGE_NOT_SATISFIABLE)
					self.headers_common()
					self.send_header('Content-Range', f'bytes */{size}')
					self.send_header('Content-Length', '0')
					self.end_headers()
					return
				status = HTTPStatus.PARTIAL_CONTENT

			self.send_response(status)
			self.headers_common()
			ext = os.path.splitext(name)[1].lower()
			self.send_header('Content-Type', TYPES.get(ext) or mimetypes.guess_type(name)[0] or 'application/octet-stream')
			self.send_header('Last-Modified', email.utils.formatdate(mtime, usegmt=True))
			self.send_header('Accept-Ranges', 'bytes')
			if status == HTTPStatus.PARTIAL_CONTENT:
				self.send_header('Content-Range', f'bytes {start}-{end - 1}/{size}')
			self.send_header('Content-Length', str(end - start))
			self.end_headers()
			if not body:
				return
			f.seek(start)
			left = end - start
			while left > 0:
				block = f.read(min(left, 1 << 20))
				if not block:
					break
				self.wfile.write(block)
				left -= len(block)

	def log_message (self, format, *args):
		if self.server.verbose:
			super().log_message(format, *args)


def main ():
	parser = argparse.ArgumentParser(description='Serves a web build of SoftWorld and a Quake directory.')
	parser.add_argument('build', help="the build's directory, with softworld.html (build/web/Release)")
	parser.add_argument('basedir', help='the Quake directory, with id1')
	parser.add_argument('--port', type=int, default=8000)
	parser.add_argument('--bind', default='127.0.0.1', help='the address to listen on (127.0.0.1)')
	parser.add_argument('--verbose', action='store_true', help='prints each request')
	args = parser.parse_args()

	for d in (args.build, args.basedir):
		if not os.path.isdir(d):
			sys.exit(f'{d}: not a directory')
	Handler.build, Handler.basedir = os.path.abspath(args.build), os.path.abspath(args.basedir)
	server = ThreadingHTTPServer((args.bind, args.port), Handler)
	server.verbose = args.verbose
	host = 'localhost' if args.bind in ('127.0.0.1', '0.0.0.0', '::') else args.bind
	print(f'SoftWorld on http://{host}:{args.port}/ (Ctrl+C stops it)')
	try:
		server.serve_forever()
	except KeyboardInterrupt:
		pass


if __name__ == '__main__':
	main()
