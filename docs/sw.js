const CACHE_NAME = "kh-climatesense-v1.2.2";
const ASSETS = ["./", "./index.html", "./manifest.json", "./icon-192.png", "./icon-512.png", "./weather-bg.jpg"];

self.addEventListener("install", e => {
  e.waitUntil(caches.open(CACHE_NAME).then(c => c.addAll(ASSETS)).then(() => self.skipWaiting()));
});

self.addEventListener("activate", e => {
  e.waitUntil(self.clients.claim());
});

self.addEventListener("fetch", e => {
  const url = e.request.url;
  if (url.includes("firebasedatabase.app") || url.includes("script.google.com")) {
    return;
  }
  e.respondWith(fetch(e.request).catch(() => caches.match(e.request)));
});
