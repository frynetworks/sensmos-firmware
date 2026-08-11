/**
 * Node identity — secp256k1 keypair, device_id, ECDSA signing.
 *
 * Mirrors src/identity.cpp of the ESP32 firmware:
 *   device_id = sha256_hex( hex(pubkey[65]) + hex(mac[6]) )      lowercase throughout
 *   signature = ECDSA-SHA256 over the message, DER encoded, lowercase hex
 *
 * The firmware keeps its key in NVS; a container keeps it in a mounted volume so the node
 * survives `docker compose down` with the same identity. The MAC is persisted alongside the
 * key because Docker hands out a fresh random MAC every time a container is recreated, and a
 * changing MAC would silently mint a brand-new node on every restart.
 */

import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const CURVE = 'secp256k1';

// SEC1 (RFC 5915) DER wrapper for a bare 32-byte secp256k1 scalar. Fixed template:
//   SEQUENCE { INTEGER 1, OCTET STRING(32) <priv>, [0] { OID 1.3.132.0.10 } }
const SEC1_PREFIX = Buffer.from('302e0201010420', 'hex');
const SEC1_SUFFIX = Buffer.from('a00706052b8104000a', 'hex');

/** Backend's secp256k1 public key, hardcoded in src/identity.cpp (BE_PUBKEY_HEX). */
export const BE_PUBKEY_HEX =
  '042e5d120dcd4324edb14b7a694b0868f1df9ef0f3b8ecd7580702482413f58183b6f10b55ea2b643cfc' +
  'f01880ef120db3cfadbda87f62c487b458d0a3000d5117';

export function sha256Hex(input) {
  return crypto.createHash('sha256').update(input).digest('hex');
}

function privateKeyObject(priv32) {
  const der = Buffer.concat([SEC1_PREFIX, priv32, SEC1_SUFFIX]);
  return crypto.createPrivateKey({ key: der, format: 'der', type: 'sec1' });
}

function publicKeyFromPrivate(priv32) {
  const ecdh = crypto.createECDH(CURVE);
  ecdh.setPrivateKey(priv32);
  return ecdh.getPublicKey(); // 65 bytes, uncompressed (0x04 || X || Y)
}

function generatePrivateKey() {
  // createECDH().generateKeys() produces a valid scalar for the curve.
  const ecdh = crypto.createECDH(CURVE);
  ecdh.generateKeys();
  return ecdh.getPrivateKey(null, 'buffer').length === 32
    ? ecdh.getPrivateKey()
    : Buffer.concat([Buffer.alloc(32 - ecdh.getPrivateKey().length), ecdh.getPrivateKey()]);
}

function normalizeMac(raw) {
  const hex = String(raw).replace(/[^0-9a-fA-F]/g, '').toLowerCase();
  if (hex.length !== 12) throw new Error(`MAC must be 6 bytes (12 hex chars), got "${raw}"`);
  return hex;
}

/** First non-internal interface MAC, or undefined when the container exposes none. */
function detectMac() {
  for (const addrs of Object.values(os.networkInterfaces())) {
    for (const a of addrs || []) {
      if (a.internal) continue;
      if (!a.mac || a.mac === '00:00:00:00:00:00') continue;
      return normalizeMac(a.mac);
    }
  }
  return undefined;
}

export class Identity {
  constructor({ priv, mac }) {
    this.priv = priv;
    this.pubkey = publicKeyFromPrivate(priv);
    this.pubkeyHex = this.pubkey.toString('hex');
    this.mac = mac;
    this.deviceId = sha256Hex(this.pubkeyHex + this.mac);
    this.#key = privateKeyObject(priv);
  }

  #key;

  /** ECDSA-SHA256, DER encoded, lowercase hex — matches mbedtls_ecdsa_write_signature. */
  sign(message) {
    return crypto.createSign('sha256').update(message).sign(this.#key).toString('hex');
  }

  verify(message, sigHex) {
    const pub = crypto.createPublicKey(this.#key);
    return crypto.createVerify('sha256').update(message).verify(pub, Buffer.from(sigHex, 'hex'));
  }

  /** ECDH shared secret with the backend key — the X coordinate only, as mbedTLS returns. */
  sharedSecret(bePubkeyHex = BE_PUBKEY_HEX) {
    const ecdh = crypto.createECDH(CURVE);
    ecdh.setPrivateKey(this.priv);
    return ecdh.computeSecret(Buffer.from(bePubkeyHex, 'hex'));
  }
}

/**
 * Load the identity from disk, creating it on first run.
 * @param {{keyPath: string, mac?: string}} opts
 */
export function loadIdentity({ keyPath, mac: macOverride }) {
  let stored;
  if (fs.existsSync(keyPath)) {
    const parsed = JSON.parse(fs.readFileSync(keyPath, 'utf8'));
    if (!/^[0-9a-f]{64}$/.test(parsed.privkey || '')) {
      throw new Error(`${keyPath} does not contain a 32-byte hex private key`);
    }
    stored = parsed;
  }

  // Precedence: explicit override > previously persisted > detected > derived from the key.
  // Anything but the last two would let a container recreate mint a new node id.
  const priv = stored ? Buffer.from(stored.privkey, 'hex') : generatePrivateKey();
  const mac =
    (macOverride && normalizeMac(macOverride)) ||
    stored?.mac ||
    detectMac() ||
    sha256Hex(publicKeyFromPrivate(priv).toString('hex')).slice(0, 12);

  const identity = new Identity({ priv, mac });

  const desired = { version: 1, privkey: priv.toString('hex'), mac, device_id: identity.deviceId };
  if (!stored || stored.mac !== mac || stored.privkey !== desired.privkey) {
    fs.mkdirSync(path.dirname(keyPath), { recursive: true });
    fs.writeFileSync(keyPath, `${JSON.stringify(desired, null, 2)}\n`, { mode: 0o600 });
  }

  return { identity, created: !stored };
}
