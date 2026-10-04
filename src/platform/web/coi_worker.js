// coi_worker.js -- shipped as softworld-coi.js next to the page: a service
// worker that hands the page the site's own responses cross-origin isolated
// (the COOP and COEP headers), for a site that can't send them, as GitHub
// Pages can't. The page's shared memory needs the isolation; the page
// (shell.html) registers this when it isn't isolated, and loads again once it
// controls the page. Other sites' responses go by as they are.

'use strict';

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (e) => e.waitUntil(self.clients.claim()));

self.addEventListener('fetch', (e) => {
	const request = e.request;

	if (new URL(request.url).origin != self.location.origin
		|| (request.cache == 'only-if-cached' && request.mode != 'same-origin'))
		return;
	e.respondWith(fetch(request).then((r) => {
		const headers = new Headers(r.headers);

		if (!r.status)		// opaque
			return r;
		headers.set('Cross-Origin-Opener-Policy', 'same-origin');
		headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
		headers.set('Cross-Origin-Resource-Policy', 'same-origin');
		return new Response(r.body, {status: r.status, statusText: r.statusText, headers});
	}));
});
