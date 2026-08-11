/**
 * WebSocket frame encryption — a port of src/ws_enc.cpp.
 *
 * Key schedule
 *   shared = ECDH(node_priv, BE_pub).X                       (32 bytes)
 *   prk    = HMAC-SHA256(key = fw_nonce || be_nonce, shared) (HKDF extract, 32-byte salt)
 *   okm    = HMAC-SHA256(key = prk, "sensmos-ws-v1" || 0x01) (HKDF expand, L = 32)
 *   tx     = okm[0..16]   node -> backend
 *   rx     = okm[16..32]  backend -> node   (the backend uses the opposite assignment)
 *
 * Frame
 *   [ version(1) = 0x01 | seq(8, big-endian) | tag(16) | ciphertext ]
 *   iv  = 0x00000000 || seq          (12 bytes)
 *   aad = version || seq             (the first 9 bytes of the frame)
 */

import crypto from 'node:crypto';

export const VERSION_BYTE = 0x01;
export const HEADER_LEN = 25; // 1 version + 8 seq + 16 tag
const INFO = 'sensmos-ws-v1';

export function deriveKeys(shared, fwNonce, beNonce) {
  if (fwNonce.length !== 16 || beNonce.length !== 16) {
    throw new Error('both nonces must be 16 bytes');
  }
  const salt = Buffer.concat([fwNonce, beNonce]);
  const okm = Buffer.from(crypto.hkdfSync('sha256', shared, salt, Buffer.from(INFO, 'utf8'), 32));
  return { tx: okm.subarray(0, 16), rx: okm.subarray(16, 32) };
}

function seqToBuffer(seq) {
  const b = Buffer.alloc(8);
  b.writeBigUInt64BE(BigInt(seq));
  return b;
}

function ivFor(seqBuf) {
  return Buffer.concat([Buffer.alloc(4), seqBuf]);
}

export class WsEnc {
  constructor() {
    this.reset();
  }

  reset() {
    this.ready = false;
    this.keyTx = null;
    this.keyRx = null;
    this.seqTx = 0n;
    this.seqRx = 0n;
    this.rxInit = false;
  }

  derive(shared, fwNonce, beNonce) {
    this.reset();
    const { tx, rx } = deriveKeys(shared, fwNonce, beNonce);
    this.keyTx = tx;
    this.keyRx = rx;
    this.ready = true;
  }

  seal(plaintext) {
    if (!this.ready) throw new Error('session key not established');
    const pt = Buffer.isBuffer(plaintext) ? plaintext : Buffer.from(plaintext, 'utf8');
    const seq = seqToBuffer(this.seqTx);
    const header = Buffer.concat([Buffer.from([VERSION_BYTE]), seq]); // aad
    const cipher = crypto.createCipheriv('aes-128-gcm', this.keyTx, ivFor(seq));
    cipher.setAAD(header);
    const ct = Buffer.concat([cipher.update(pt), cipher.final()]);
    const frame = Buffer.concat([header, cipher.getAuthTag(), ct]);
    this.seqTx += 1n;
    return frame;
  }

  open(frame) {
    if (!this.ready) throw new Error('session key not established');
    if (frame.length < HEADER_LEN || frame[0] !== VERSION_BYTE) {
      throw new Error('malformed frame');
    }
    const seqBuf = frame.subarray(1, 9);
    const seq = seqBuf.readBigUInt64BE();
    if (this.rxInit && seq <= this.seqRx) throw new Error('replayed or reordered frame');

    const decipher = crypto.createDecipheriv('aes-128-gcm', this.keyRx, ivFor(seqBuf));
    decipher.setAAD(frame.subarray(0, 9));
    decipher.setAuthTag(frame.subarray(9, 25));
    const pt = Buffer.concat([decipher.update(frame.subarray(25)), decipher.final()]);

    this.seqRx = seq;
    this.rxInit = true;
    return pt;
  }
}
