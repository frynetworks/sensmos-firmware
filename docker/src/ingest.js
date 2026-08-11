/**
 * Software-node mode — POST /v1/ingest.
 *
 * The shape mirrors the official Home Assistant integration
 * (custom_components/sensmos/direct.py): a passkey of at least 32 characters identifies the
 * node, and `device_id = sha256("sensmos-soft:" + key)`.
 *
 * This path needs no keypair, no registration and no WebSocket — but it also carries no owner
 * address, so a node created this way is not linked to a wallet and earns nothing. Firmware
 * mode is what satisfies "linked to an account".
 */

import { sha256Hex } from './identity.js';
import { log } from './logger.js';

export function softDeviceId(key) {
  return sha256Hex(`sensmos-soft:${key}`);
}

export function buildIngestBody({ key, entities, lat, lon, label }) {
  const body = { key, entities };
  if (lat !== undefined && lon !== undefined) {
    body.lat = lat;
    body.lon = lon;
  }
  if (label) body.label = label;
  return body;
}

export async function pushIngest({ apiUrl, key, entities, lat, lon, label }) {
  const body = buildIngestBody({ key, entities, lat, lon, label });
  const res = await fetch(`${apiUrl}/ingest`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
    signal: AbortSignal.timeout(10000),
  });
  const text = await res.text();
  if (res.status === 200) {
    log.info('ingest', `sent ${entities.length} entities`);
  } else {
    log.warn('ingest', `HTTP ${res.status}: ${text.slice(0, 200)}`);
  }
  return { status: res.status, body: text, ok: res.status === 200 };
}
