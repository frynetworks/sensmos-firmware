import test from 'node:test';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import { WsEnc, deriveKeys, HEADER_LEN, VERSION_BYTE } from '../src/wsenc.js';

const SHARED = Buffer.alloc(32, 0xab);
const FW_NONCE = Buffer.alloc(16, 0x01);
const BE_NONCE = Buffer.alloc(16, 0x02);

/** HKDF-SHA256 written out longhand, exactly as ws_enc.cpp does it with mbedtls HMAC. */
function hkdfLonghand(shared, fwNonce, beNonce) {
  const salt = Buffer.concat([fwNonce, beNonce]);
  const prk = crypto.createHmac('sha256', salt).update(shared).digest();
  const info = Buffer.concat([Buffer.from('sensmos-ws-v1', 'utf8'), Buffer.from([0x01])]);
  const okm = crypto.createHmac('sha256', prk).update(info).digest();
  return { tx: okm.subarray(0, 16), rx: okm.subarray(16, 32) };
}

test('key derivation matches the firmware HKDF construction byte for byte', () => {
  const ours = deriveKeys(SHARED, FW_NONCE, BE_NONCE);
  const theirs = hkdfLonghand(SHARED, FW_NONCE, BE_NONCE);
  assert.deepEqual(ours.tx, theirs.tx);
  assert.deepEqual(ours.rx, theirs.rx);
  assert.equal(ours.tx.length, 16);
  assert.equal(ours.rx.length, 16);
  assert.notDeepEqual(ours.tx, ours.rx);
});

test('nonce order matters — salt is fw_nonce then be_nonce', () => {
  const forward = deriveKeys(SHARED, FW_NONCE, BE_NONCE);
  const reversed = deriveKeys(SHARED, BE_NONCE, FW_NONCE);
  assert.notDeepEqual(forward.tx, reversed.tx);
});

test('nonces must be 16 bytes', () => {
  assert.throws(() => deriveKeys(SHARED, Buffer.alloc(8), BE_NONCE), /16 bytes/);
});

test('frame layout is version | seq(8 BE) | tag(16) | ciphertext', () => {
  const enc = new WsEnc();
  enc.derive(SHARED, FW_NONCE, BE_NONCE);

  const plaintext = '{"type":"batch"}';
  const frame = enc.seal(plaintext);

  assert.equal(frame[0], VERSION_BYTE);
  assert.equal(frame.length, HEADER_LEN + Buffer.byteLength(plaintext));
  assert.equal(frame.subarray(1, 9).readBigUInt64BE(), 0n, 'first frame carries seq 0');

  const second = enc.seal(plaintext);
  assert.equal(second.subarray(1, 9).readBigUInt64BE(), 1n, 'seq increments per frame');
});

test('a peer with mirrored keys can open what we seal', () => {
  const node = new WsEnc();
  node.derive(SHARED, FW_NONCE, BE_NONCE);

  // The backend assigns tx/rx the other way round.
  const backend = new WsEnc();
  backend.derive(SHARED, FW_NONCE, BE_NONCE);
  [backend.keyTx, backend.keyRx] = [backend.keyRx, backend.keyTx];

  const payload = JSON.stringify({ type: 'batch', device_id: 'a'.repeat(64) });
  assert.equal(backend.open(node.seal(payload)).toString('utf8'), payload);
  assert.equal(node.open(backend.seal('pong')).toString('utf8'), 'pong');
});

test('AAD covers the header — flipping a header byte breaks authentication', () => {
  const node = new WsEnc();
  node.derive(SHARED, FW_NONCE, BE_NONCE);
  const peer = new WsEnc();
  peer.derive(SHARED, FW_NONCE, BE_NONCE);
  [peer.keyTx, peer.keyRx] = [peer.keyRx, peer.keyTx];

  const frame = node.seal('hello');
  frame[8] ^= 0xff; // corrupt the sequence number, which is part of both the IV and the AAD
  assert.throws(() => peer.open(frame));
});

test('ciphertext tampering is rejected by the GCM tag', () => {
  const node = new WsEnc();
  node.derive(SHARED, FW_NONCE, BE_NONCE);
  const peer = new WsEnc();
  peer.derive(SHARED, FW_NONCE, BE_NONCE);
  [peer.keyTx, peer.keyRx] = [peer.keyRx, peer.keyTx];

  const frame = node.seal('hello world');
  frame[frame.length - 1] ^= 0x01;
  assert.throws(() => peer.open(frame));
});

test('replayed or reordered frames are refused', () => {
  const node = new WsEnc();
  node.derive(SHARED, FW_NONCE, BE_NONCE);
  const peer = new WsEnc();
  peer.derive(SHARED, FW_NONCE, BE_NONCE);
  [peer.keyTx, peer.keyRx] = [peer.keyRx, peer.keyTx];

  const first = node.seal('one');
  const second = node.seal('two');
  peer.open(first);
  peer.open(second);
  assert.throws(() => peer.open(first), /replayed|reordered/);
});

test('short or wrong-version frames are refused', () => {
  const enc = new WsEnc();
  enc.derive(SHARED, FW_NONCE, BE_NONCE);
  assert.throws(() => enc.open(Buffer.alloc(10)), /malformed/);
  const bad = enc.seal('x');
  bad[0] = 0x02;
  assert.throws(() => enc.open(bad), /malformed/);
});

test('sealing before key exchange is an error, never a plaintext leak', () => {
  const enc = new WsEnc();
  assert.throws(() => enc.seal('secret'), /session key/);
});
