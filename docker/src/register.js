/**
 * Self-registration — POST /v1/register.
 *
 * Contract as probed and documented in Fry-Networks/sensmos-esp8266 src/self_register.cpp:
 *   {}                      -> 400 {"error":"missing fields: message, pubkey, sig_esp"}
 *   stale ts                -> 400 {"error":"timestamp expired"}
 *   fresh ts + bad signature -> 401 {"error":"invalid esp signature"}
 *
 * The field is `pubkey` (not `pubkey_esp`) and `proof` is not required, so a node can claim
 * itself for an owner without the mobile app's Bluetooth attestation ceremony. Such a node is
 * owner-linked but stays `trusted: false` and earns nothing, which is the documented tradeoff.
 *
 * `message` is signed exactly as serialized — build it once, sign that string, send that string.
 */

import { log } from './logger.js';

export const MAX_REJECTS = 10;

export function buildRegistrationMessage({ deviceId, owner, ts }) {
  return `{"device_id":"${deviceId}","owner":"${owner}","ts":${ts}}`;
}

export function buildRegistrationPayload({ identity, owner, ts }) {
  const message = buildRegistrationMessage({ deviceId: identity.deviceId, owner, ts });
  return {
    message,
    pubkey: identity.pubkeyHex,
    sig_esp: identity.sign(message),
  };
}

/** Compares local time to the server's Date header; a skewed clock reads as "timestamp expired". */
export async function checkClockSkew(apiUrl, maxSkewSec) {
  try {
    const res = await fetch(`${apiUrl}/map/nodes`, {
      method: 'HEAD',
      headers: { 'X-App-Key': 'sensmos2025' },
      signal: AbortSignal.timeout(8000),
    });
    const serverDate = res.headers.get('date');
    if (!serverDate) return { checked: false };
    const skew = Math.round((Date.now() - Date.parse(serverDate)) / 1000);
    const ok = Math.abs(skew) <= maxSkewSec;
    if (!ok) {
      log.warn('clock', `local time is ${skew}s off the server — registration will be rejected`);
    }
    return { checked: true, skewSec: skew, ok };
  } catch (err) {
    log.debug('clock', `skew check skipped: ${err.message}`);
    return { checked: false };
  }
}

/**
 * One registration attempt.
 * @returns {Promise<{status: number, body: string, registered: boolean, fatal: boolean}>}
 */
export async function attemptRegistration({ apiUrl, identity, owner }) {
  const ts = Math.floor(Date.now() / 1000);
  const payload = buildRegistrationPayload({ identity, owner, ts });

  const res = await fetch(`${apiUrl}/register`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload),
    signal: AbortSignal.timeout(15000),
  });
  const body = await res.text();

  return {
    status: res.status,
    body,
    registered: res.status === 200 || res.status === 201,
    // 4xx means the payload itself is wrong; retrying it unchanged just burns attempts.
    fatal: res.status >= 400 && res.status < 500,
  };
}

/**
 * Register with bounded retries. Transient failures (5xx, network) retry with backoff;
 * rejections count against MAX_REJECTS exactly like the firmware's give-up counter.
 */
export async function register({ apiUrl, identity, owner, maxAttempts = 3, backoffMs = 10000 }) {
  let rejects = 0;
  let last = null;

  for (let attempt = 1; attempt <= maxAttempts; attempt++) {
    try {
      const result = await attemptRegistration({ apiUrl, identity, owner });
      last = result;
      if (result.registered) {
        log.info('register', `registered device ${identity.deviceId.slice(0, 16)}… to ${owner}`, {
          status: result.status,
        });
        return result;
      }
      if (result.fatal) {
        rejects += 1;
        log.warn('register', `rejected (HTTP ${result.status}): ${result.body.slice(0, 200)}`);
        if (rejects >= MAX_REJECTS) break;
      } else {
        log.warn('register', `transient failure (HTTP ${result.status}) — retrying`);
      }
    } catch (err) {
      last = { status: 0, body: err.message, registered: false, fatal: false };
      log.warn('register', `attempt ${attempt} failed: ${err.message}`);
    }
    if (attempt < maxAttempts) await new Promise((r) => setTimeout(r, backoffMs));
  }

  return last ?? { status: 0, body: 'no attempt made', registered: false, fatal: false };
}
