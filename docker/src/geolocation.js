/**
 * IP geolocation + WS location push — port of the ESP8266 firmware pattern
 * (esp8266/src/geolocation.cpp): ip-api.com primary, ipinfo.io fallback, one
 * lookup per process, re-pushed as a `node_config` frame on every WS connect.
 * The backend applies fuzz and owns the anchor; an app GPS fix overrides this
 * coarse one.
 */

import { log } from './logger.js';

const IPAPI_URL = 'http://ip-api.com/json/?fields=status,lat,lon,query';
const IPINFO_URL = 'https://ipinfo.io/json';
const FETCH_TIMEOUT_MS = 10000;

/** Documented IP-geolocation accuracy bound (meters) — GEOLOC_ACCURACY_IP_M in the firmware. */
export const GEO_ACCURACY_IP_M = 25000;

// One acquisition attempt per process, success or failure — the firmware's
// "retry next boot" behaviour. Reconnects re-push the cached fix.
let cached = null;
let attempted = false;

/** Test seam: clears the module-level acquisition cache. */
export function resetGeoCache() {
  cached = null;
  attempted = false;
}

function validCoords(lat, lon) {
  if (typeof lat !== 'number' || typeof lon !== 'number') return false;
  if (!Number.isFinite(lat) || !Number.isFinite(lon)) return false;
  if (lat < -90 || lat > 90 || lon < -180 || lon > 180) return false;
  if (lat === 0 && lon === 0) return false; // null island = no fix
  return true;
}

const round6 = (n) => Number(n.toFixed(6)); // %.6f on the firmware wire

async function fetchJson(url, fetchImpl) {
  const doFetch = fetchImpl ?? fetch; // resolved at call time so tests can inject
  const res = await doFetch(url, { signal: AbortSignal.timeout(FETCH_TIMEOUT_MS) });
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return res.json();
}

async function fromIpApi(fetchImpl) {
  const doc = await fetchJson(IPAPI_URL, fetchImpl);
  if (doc.status !== 'success') throw new Error(`status ${doc.status ?? '(missing)'}`);
  const lat = doc.lat;
  const lon = doc.lon;
  if (!validCoords(lat, lon)) throw new Error(`invalid coords ${lat},${lon}`);
  return { lat: round6(lat), lon: round6(lon), accuracy: GEO_ACCURACY_IP_M, source: 'ip-api' };
}

async function fromIpInfo(fetchImpl) {
  const doc = await fetchJson(IPINFO_URL, fetchImpl);
  const parts = String(doc.loc ?? '').split(',');
  const lat = Number.parseFloat(parts[0]);
  const lon = Number.parseFloat(parts[1]);
  if (parts.length !== 2 || !validCoords(lat, lon)) throw new Error(`loc unparseable: ${doc.loc}`);
  return { lat: round6(lat), lon: round6(lon), accuracy: GEO_ACCURACY_IP_M, source: 'ipinfo' };
}

/**
 * Resolves the node's coarse IP-derived location, once per process.
 * @returns {{lat: number, lon: number, accuracy: number, source: string}|null}
 */
export async function getIpGeolocation({ fetchImpl } = {}) {
  if (attempted) return cached;
  attempted = true;
  for (const [name, provider] of [['ip-api.com', fromIpApi], ['ipinfo.io', fromIpInfo]]) {
    try {
      cached = await provider(fetchImpl);
      log.info('geo', `location acquired: lat=${cached.lat} lon=${cached.lon} accuracy=~${cached.accuracy}m source=${cached.source}`);
      return cached;
    } catch (err) {
      log.warn('geo', `${name} lookup failed: ${err.message}`);
    }
  }
  log.warn('geo', 'acquisition failed (both APIs) — will not retry this session');
  return null;
}

/** The exact frame the ESP firmware sends from geoloc_push_ws(). */
export function buildNodeConfig({ lat, lon }) {
  return { type: 'node_config', gps_lat: lat, gps_lon: lon, fuzz: true };
}

/**
 * Pushes the node location to the backend over the established WS session.
 * Manual coordinates (SENSMOS_LAT/LON) win over the IP lookup, mirroring the
 * firmware's manual source priority. Never rejects; a failed push retries on
 * the next WS connect because the `identified` handler calls this every time.
 * @returns {boolean} true when a frame was handed to the socket.
 */
export async function pushLocationWs(client, { manual, fetchImpl } = {}) {
  try {
    let loc = null;
    if (manual !== undefined) {
      if (validCoords(manual.lat, manual.lon)) {
        loc = { lat: round6(manual.lat), lon: round6(manual.lon), accuracy: GEO_ACCURACY_IP_M, source: 'manual' };
      } else {
        log.warn('geo', `manual coords invalid (${manual.lat},${manual.lon}) — falling back to IP lookup`);
      }
    }
    if (!loc) loc = await getIpGeolocation({ fetchImpl });
    if (!loc) {
      log.info('geo', 'no location to push');
      return false;
    }
    const ok = client.send(buildNodeConfig(loc));
    if (ok) log.info('geo', `pushed location to backend: lat=${loc.lat} lon=${loc.lon} source=${loc.source}`);
    else log.warn('geo', 'location push failed (ws send) — retries on next WS connect');
    return ok;
  } catch (err) {
    log.warn('geo', `location push failed: ${err.message}`);
    return false;
  }
}
