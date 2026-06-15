// Cloudflare static-asset Worker for the CoreVideo Pro web demo.
//
// Serves the built Vite SPA in ./dist under the public route prefix
// /pro/demo/ on corevideo.iamfatness.us. The renderer runs entirely on the
// deterministic mock engine bundle (no native host bridge in the browser),
// so this is a self-contained, simulated demo of the operator console.
//
// Responsibilities:
//   - Map /pro/demo/* public paths onto the asset directory root before
//     hitting the ASSETS binding (the build's base is /pro/demo/).
//   - Normalize the bare /pro/demo path to /pro/demo/.
//   - Fall back to the SPA shell (index.html) for extension-less paths.
//   - Apply hardened security headers to every response.

const ROUTE_PREFIX = "/pro/demo";

const SECURITY_HEADERS = {
  "content-security-policy":
    "default-src 'self'; img-src 'self' data: blob:; media-src 'self' blob: data:; style-src 'self' 'unsafe-inline'; script-src 'self'; connect-src 'self'; font-src 'self' data:; worker-src 'self' blob:; frame-ancestors 'none'; base-uri 'self'; form-action 'self'",
  "referrer-policy": "strict-origin-when-cross-origin",
  "x-content-type-options": "nosniff",
  "strict-transport-security": "max-age=63072000; includeSubDomains; preload",
};

function withHeaders(response) {
  const headers = new Headers(response.headers);
  for (const [key, value] of Object.entries(SECURITY_HEADERS)) {
    headers.set(key, value);
  }
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
}

function assetRequest(request, pathname) {
  const url = new URL(request.url);
  url.pathname = pathname;
  return new Request(url, request);
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    // Strip the public route prefix so it maps onto the ./dist asset root.
    let assetPath = url.pathname.startsWith(ROUTE_PREFIX)
      ? url.pathname.slice(ROUTE_PREFIX.length)
      : url.pathname;

    // Normalize the bare "/pro/demo" to "/pro/demo/" so relative asset URLs
    // resolve against the demo root.
    if (assetPath === "") {
      url.pathname = `${ROUTE_PREFIX}/`;
      return Response.redirect(url.toString(), 301);
    }
    if (assetPath === "/") {
      assetPath = "/index.html";
    }

    let response = await env.ASSETS.fetch(assetRequest(request, assetPath));

    // SPA fallback: serve the app shell for extension-less (route-like) paths.
    const hasExtension = /\.[a-zA-Z0-9]+$/.test(assetPath);
    if (response.status === 404 && !hasExtension) {
      const shell = await env.ASSETS.fetch(assetRequest(request, "/index.html"));
      response = new Response(shell.body, { status: 200, headers: shell.headers });
    }

    return withHeaders(response);
  },
};
