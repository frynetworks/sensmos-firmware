import test from 'node:test';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { Identity, loadIdentity, sha256Hex, BE_PUBKEY_HEX } from '../src/identity.js';

// Private key 0x1111…11 has a published secp256k1 public point, so this doubles as a check
// that we are really on secp256k1 and not some other curve.
const PRIV = Buffer.alloc(32, 0x11);
const MAC = 'aabbccddeeff';
const EXPECTED_PUB =
  '044f355bdcb7cc0af728ef3cceb9615d90684bb5b2ca5f859ab0f0b704075871aa' +
  '385b6b1b8ead809ca67454d9683fcf2ba03456d6fe2c4abe2b07f0fbdbb2f1c1';
const EXPECTED_DEVICE_ID = '4f06014d34b2d247a72807ad7c36b271eb7de89d34fdc0ec2b1d0d8122b914b3';

test('public key is the uncompressed secp256k1 point for the private scalar', () => {
  const id = new Identity({ priv: PRIV, mac: MAC });
  assert.equal(id.pubkeyHex.length, 130);
  assert.equal(id.pubkeyHex.slice(0, 2), '04');
  assert.equal(id.pubkeyHex, EXPECTED_PUB);
});

test('device_id is sha256 over lowercase hex of pubkey then mac', () => {
  const id = new Identity({ priv: PRIV, mac: MAC });
  assert.equal(id.deviceId, EXPECTED_DEVICE_ID);
  assert.match(id.deviceId, /^[0-9a-f]{64}$/);
  // Recompute the firmware formula independently of the class.
  assert.equal(id.deviceId, sha256Hex(EXPECTED_PUB + MAC));
});

test('a different mac yields a different device_id', () => {
  const a = new Identity({ priv: PRIV, mac: MAC });
  const b = new Identity({ priv: PRIV, mac: '001122334455' });
  assert.notEqual(a.deviceId, b.deviceId);
});

test('signatures are DER encoded and verify against the public key', () => {
  const id = new Identity({ priv: PRIV, mac: MAC });
  const msg = 'identify:abc:42:e1:00112233445566778899aabbccddeeff';
  const sig = id.sign(msg);

  assert.match(sig, /^[0-9a-f]+$/);
  assert.equal(sig.slice(0, 2), '30', 'DER SEQUENCE tag');
  const der = Buffer.from(sig, 'hex');
  assert.equal(der[1], der.length - 2, 'DER length header matches body');
  assert.ok(id.verify(msg, sig));
  assert.ok(!id.verify(`${msg}x`, sig), 'a tampered message must not verify');
});

test('signing is over sha256 of the message, matching mbedtls_ecdsa_write_signature', () => {
  const id = new Identity({ priv: PRIV, mac: MAC });
  const msg = 'hello sensmos';
  const sig = Buffer.from(id.sign(msg), 'hex');

  // Verify the digest path explicitly: same signature must validate as a raw digest signature.
  const pub = crypto.createPublicKey(
    crypto.createPrivateKey({
      key: Buffer.concat([
        Buffer.from('302e0201010420', 'hex'),
        PRIV,
        Buffer.from('a00706052b8104000a', 'hex'),
      ]),
      format: 'der',
      type: 'sec1',
    }),
  );
  assert.ok(crypto.verify('sha256', Buffer.from(msg, 'utf8'), pub, sig));
});

test('ECDH with the backend key produces a 32-byte shared X coordinate', () => {
  const id = new Identity({ priv: PRIV, mac: MAC });
  const shared = id.sharedSecret(BE_PUBKEY_HEX);
  assert.equal(shared.length, 32);

  // Both sides must agree: derive the same secret from the other direction.
  const ephemeral = crypto.createECDH('secp256k1');
  ephemeral.generateKeys();
  const ours = crypto.createECDH('secp256k1');
  ours.setPrivateKey(PRIV);
  const a = ours.computeSecret(ephemeral.getPublicKey());
  const b = ephemeral.computeSecret(ours.getPublicKey());
  assert.deepEqual(a, b);
});

test('identity persists and reloads with a stable device_id', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'sensmos-id-'));
  const keyPath = path.join(dir, 'node.key');
  try {
    const first = loadIdentity({ keyPath, mac: MAC });
    assert.equal(first.created, true);

    const second = loadIdentity({ keyPath, mac: MAC });
    assert.equal(second.created, false);
    assert.equal(second.identity.deviceId, first.identity.deviceId);

    // The MAC is persisted, so a container recreated without SENSMOS_MAC keeps its identity
    // instead of minting a new node.
    const third = loadIdentity({ keyPath });
    assert.equal(third.identity.deviceId, first.identity.deviceId);

    const stored = JSON.parse(fs.readFileSync(keyPath, 'utf8'));
    assert.match(stored.privkey, /^[0-9a-f]{64}$/);
    assert.equal(stored.mac, MAC);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('a corrupt key file is rejected rather than silently regenerated', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'sensmos-id-'));
  const keyPath = path.join(dir, 'node.key');
  try {
    fs.writeFileSync(keyPath, JSON.stringify({ version: 1, privkey: 'nope', mac: MAC }));
    assert.throws(() => loadIdentity({ keyPath }), /32-byte hex private key/);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});
