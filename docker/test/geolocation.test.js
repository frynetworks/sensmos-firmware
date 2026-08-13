import test from 'node:test';
import assert from 'node:assert/strict';

import {
  getIpGeolocation,
  pushLocationWs,
  buildNodeConfig,
  resetGeoCache,
  GEO_ACCURACY_IP_M,
} from '../src/geolocation.js';

// Safety net: these tests must never touch the network — node --test runs each
// file in its own child process, so clobbering the global is contained here.
globalThis.fetch = () => {
  throw new Error('test hit the network');
};

const okJson = (body) => ({ ok: true, status: 200, json: async () => body });
const httpErr = (status) => ({ ok: false, status, json: async () => ({}) });
const badJson = () => ({
  ok: true,
  status: 200,
  json: async () => {
    throw new SyntaxError('Unexpected end of JSON input');
  },
});

/** Fake fetch that records calls and shifts queued results (Error instances throw). */
function recordingFetch(...results) {
  const calls = [];
  const fn = async (url, opts) => {
    calls.push({ url: String(url), opts });
    const r = results.shift();
    if (r === undefined) throw new Error('recordingFetch exhausted');
    if (r instanceof Error) throw r;
    return r;
  };
  fn.calls = calls;
  return fn;
}

const fakeClient = (sendResult = true) => ({
  sent: [],
  send(obj) {
    this.sent.push(obj);
    return sendResult;
  },
});

const IPAPI_OK = { status: 'success', lat: 44.0121, lon: -92.4802, query: '1.2.3.4' };

test('ip-api success parses lat/lon with constant accuracy', async () => {
  resetGeoCache();
  const f = recordingFetch(okJson(IPAPI_OK));
  const loc = await getIpGeolocation({ fetchImpl: f });
  assert.deepEqual(loc, { lat: 44.0121, lon: -92.4802, accuracy: GEO_ACCURACY_IP_M, source: 'ip-api' });
  assert.equal(f.calls.length, 1);
  assert.ok(f.calls[0].url.includes('ip-api.com'));
});

test('ip-api status "fail" falls back to ipinfo loc parsing', async () => {
  resetGeoCache();
  const f = recordingFetch(okJson({ status: 'fail' }), okJson({ loc: '52.7357,20.7072' }));
  const loc = await getIpGeolocation({ fetchImpl: f });
  assert.deepEqual(loc, { lat: 52.7357, lon: 20.7072, accuracy: GEO_ACCURACY_IP_M, source: 'ipinfo' });
  assert.equal(f.calls.length, 2);
  assert.ok(f.calls[1].url.includes('ipinfo.io'));
});

test('network error on ip-api falls back to ipinfo', async () => {
  resetGeoCache();
  const f = recordingFetch(new TypeError('fetch failed'), okJson({ loc: '52.7357,20.7072' }));
  const loc = await getIpGeolocation({ fetchImpl: f });
  assert.equal(loc.source, 'ipinfo');
  assert.equal(loc.lat, 52.7357);
});

test('timeout on ip-api falls back to ipinfo', async () => {
  resetGeoCache();
  const timeout = new Error('The operation was aborted due to timeout');
  timeout.name = 'TimeoutError';
  const f = recordingFetch(timeout, okJson({ loc: '1.5,2.5' }));
  const loc = await getIpGeolocation({ fetchImpl: f });
  assert.deepEqual(loc, { lat: 1.5, lon: 2.5, accuracy: GEO_ACCURACY_IP_M, source: 'ipinfo' });
});

test('both providers failing returns null without throwing', async () => {
  resetGeoCache();
  const f = recordingFetch(httpErr(500), httpErr(429));
  const loc = await getIpGeolocation({ fetchImpl: f });
  assert.equal(loc, null);
  assert.equal(f.calls.length, 2);
});

test('malformed JSON from ip-api falls back to ipinfo', async () => {
  resetGeoCache();
  const f = recordingFetch(badJson(), okJson({ loc: '10.5,-20.25' }));
  const loc = await getIpGeolocation({ fetchImpl: f });
  assert.equal(loc.source, 'ipinfo');
});

test('empty response bodies from both providers return null', async () => {
  resetGeoCache();
  const f = recordingFetch(okJson({}), okJson({}));
  const loc = await getIpGeolocation({ fetchImpl: f });
  assert.equal(loc, null);
});

test('out-of-range ip-api coordinates are rejected, ipinfo used', async () => {
  resetGeoCache();
  const f = recordingFetch(okJson({ status: 'success', lat: 99, lon: 10 }), okJson({ loc: '50.0,10.0' }));
  const loc = await getIpGeolocation({ fetchImpl: f });
  assert.deepEqual(loc, { lat: 50, lon: 10, accuracy: GEO_ACCURACY_IP_M, source: 'ipinfo' });
});

test('null-island (0,0) from both providers returns null', async () => {
  resetGeoCache();
  const f = recordingFetch(okJson({ status: 'success', lat: 0, lon: 0 }), okJson({ loc: '0,0' }));
  const loc = await getIpGeolocation({ fetchImpl: f });
  assert.equal(loc, null);
});

test('unparseable ipinfo loc returns null when ip-api already failed', async () => {
  resetGeoCache();
  const f = recordingFetch(okJson({ status: 'fail' }), okJson({ loc: 'somewhere' }));
  const loc = await getIpGeolocation({ fetchImpl: f });
  assert.equal(loc, null);
});

test('result is cached — success and failure both skip refetching', async () => {
  resetGeoCache();
  const first = recordingFetch(okJson(IPAPI_OK));
  const loc1 = await getIpGeolocation({ fetchImpl: first });
  assert.equal(loc1.source, 'ip-api');
  const second = recordingFetch(okJson({ status: 'success', lat: 11, lon: 22 }));
  const loc2 = await getIpGeolocation({ fetchImpl: second });
  assert.deepEqual(loc2, loc1);
  assert.equal(second.calls.length, 0);

  resetGeoCache();
  const failing = recordingFetch(httpErr(500), httpErr(500));
  assert.equal(await getIpGeolocation({ fetchImpl: failing }), null);
  const third = recordingFetch(okJson(IPAPI_OK));
  assert.equal(await getIpGeolocation({ fetchImpl: third }), null);
  assert.equal(third.calls.length, 0);
});

test('push sends the firmware node_config frame with bare-number coords', async () => {
  resetGeoCache();
  const f = recordingFetch(okJson(IPAPI_OK));
  const client = fakeClient();
  const ok = await pushLocationWs(client, { fetchImpl: f });
  assert.equal(ok, true);
  assert.equal(client.sent.length, 1);
  assert.deepEqual(client.sent[0], {
    type: 'node_config',
    gps_lat: 44.0121,
    gps_lon: -92.4802,
    fuzz: true,
  });
  assert.equal(typeof client.sent[0].gps_lat, 'number');
  assert.ok(JSON.stringify(client.sent[0]).includes('"gps_lat":44.0121'));
});

test('push with no obtainable location sends nothing and returns false', async () => {
  resetGeoCache();
  const f = recordingFetch(httpErr(500), httpErr(500));
  const client = fakeClient();
  const ok = await pushLocationWs(client, { fetchImpl: f });
  assert.equal(ok, false);
  assert.equal(client.sent.length, 0);
});

test('push returns false without throwing when the socket send fails', async () => {
  resetGeoCache();
  const f = recordingFetch(okJson(IPAPI_OK));
  const client = fakeClient(false);
  const ok = await pushLocationWs(client, { fetchImpl: f });
  assert.equal(ok, false);
  assert.equal(client.sent.length, 1);
});

test('repeat push (reconnect) re-sends the cached fix without refetching', async () => {
  resetGeoCache();
  const f = recordingFetch(okJson(IPAPI_OK));
  const client = fakeClient();
  assert.equal(await pushLocationWs(client, { fetchImpl: f }), true);
  assert.equal(await pushLocationWs(client, { fetchImpl: f }), true);
  assert.equal(client.sent.length, 2);
  assert.deepEqual(client.sent[0], client.sent[1]);
  assert.equal(f.calls.length, 1);
});

test('manual coordinates win over IP lookup and are rounded to 6 dp', async () => {
  resetGeoCache();
  const f = recordingFetch();
  const client = fakeClient();
  const ok = await pushLocationWs(client, { manual: { lat: 1.2345678, lon: 2 }, fetchImpl: f });
  assert.equal(ok, true);
  assert.equal(f.calls.length, 0);
  assert.deepEqual(client.sent[0], {
    type: 'node_config',
    gps_lat: 1.234568,
    gps_lon: 2,
    fuzz: true,
  });
});

test('buildNodeConfig mirrors the firmware frame exactly', () => {
  const frame = buildNodeConfig({ lat: -33.8688, lon: 151.2093 });
  assert.deepEqual(frame, { type: 'node_config', gps_lat: -33.8688, gps_lon: 151.2093, fuzz: true });
});
