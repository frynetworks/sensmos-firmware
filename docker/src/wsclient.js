/**
 * Backend WebSocket client — a port of src/ws_client.cpp.
 *
 * Sequence
 *   1. connect, then send a plaintext `identify` frame
 *   2. backend answers `identified` carrying `enonce` (16 bytes hex) plus the native entity list
 *   3. derive the session key from ECDH + HKDF and switch to binary AES-128-GCM frames
 *   4. everything after that is sealed; plaintext arriving post-handshake is dropped as an
 *      injection attempt, exactly as the firmware does
 *
 * The signed identify string is "identify:<device_id>:<uptime_s>:e1:<enonce_hex>". The
 * timestamp really is uptime (`millis() / 1000` in the firmware), not wall-clock time.
 */

import { EventEmitter } from 'node:events';
import crypto from 'node:crypto';
import { WebSocket } from 'ws';
import { WsEnc } from './wsenc.js';
import { BE_PUBKEY_HEX } from './identity.js';
import { log } from './logger.js';

const RECONNECT_MIN_MS = 5000; // src/config.h RECONNECT_INTERVAL
const RECONNECT_MAX_MS = 60000;
const HANDSHAKE_TIMEOUT_MS = 20000;

export class SensmosWsClient extends EventEmitter {
  /**
   * @param {{url: string, identity: object, firmware: string, chip?: string, bePubkey?: string}} opts
   */
  constructor({ url, identity, firmware, chip, bePubkey = BE_PUBKEY_HEX }) {
    super();
    this.url = url;
    this.identity = identity;
    this.firmware = firmware;
    this.chip = chip || `docker ${process.version} ${process.platform}-${process.arch}`;
    this.bePubkey = bePubkey;
    this.enc = new WsEnc();
    this.ws = null;
    this.connected = false;
    this.stopped = false;
    this.attempt = 0;
    this.fwNonce = null;
    this.startedAt = Date.now();
    this.handshakeTimer = null;
    this.reconnectTimer = null;
  }

  uptimeSec() {
    return Math.floor((Date.now() - this.startedAt) / 1000);
  }

  get encrypted() {
    return this.enc.ready;
  }

  connect() {
    if (this.stopped) return;
    this.attempt += 1;
    log.info('ws', `connecting ${this.url} (attempt ${this.attempt})`);

    const ws = new WebSocket(this.url, { handshakeTimeout: 15000 });
    this.ws = ws;

    ws.on('open', () => {
      log.info('ws', 'connected — sending identify');
      this.#sendIdentify();
      this.handshakeTimer = setTimeout(() => {
        if (!this.enc.ready) {
          log.warn('ws', 'no identified within timeout — reconnecting');
          ws.close();
        }
      }, HANDSHAKE_TIMEOUT_MS);
    });

    ws.on('message', (data, isBinary) => this.#onMessage(data, isBinary));

    ws.on('close', (code, reason) => {
      this.connected = false;
      this.enc.reset();
      clearTimeout(this.handshakeTimer);
      log.warn('ws', `disconnected (code ${code})`, reason?.toString().slice(0, 120) || undefined);
      this.emit('disconnected', { code });
      this.#scheduleReconnect();
    });

    ws.on('error', (err) => log.error('ws', `socket error: ${err.message}`));
  }

  #scheduleReconnect() {
    if (this.stopped) return;
    const delay = Math.min(RECONNECT_MIN_MS * 2 ** Math.min(this.attempt - 1, 4), RECONNECT_MAX_MS);
    log.info('ws', `reconnecting in ${Math.round(delay / 1000)}s`);
    clearTimeout(this.reconnectTimer);
    this.reconnectTimer = setTimeout(() => this.connect(), delay);
  }

  #sendIdentify() {
    this.fwNonce = crypto.randomBytes(16);
    const enonceHex = this.fwNonce.toString('hex');
    const ts = this.uptimeSec();
    const msg = `identify:${this.identity.deviceId}:${ts}:e1:${enonceHex}`;

    const packet = {
      type: 'identify',
      device_id: this.identity.deviceId,
      timestamp: ts,
      msg,
      signature: this.identity.sign(msg),
      enc: 1,
      enonce: enonceHex,
      firmware: this.firmware,
      chip: this.chip,
    };
    this.ws.send(JSON.stringify(packet));
  }

  #onMessage(data, isBinary) {
    let text;
    if (isBinary) {
      if (!this.enc.ready) {
        log.warn('ws', 'binary frame before key exchange — dropped');
        return;
      }
      try {
        text = this.enc.open(Buffer.from(data)).toString('utf8');
      } catch (err) {
        log.warn('ws', `frame decrypt failed (${err.message}) — reconnecting`);
        this.ws.close();
        return;
      }
    } else {
      if (this.enc.ready) {
        log.warn('ws', 'plaintext after encryption established — dropped');
        return;
      }
      text = data.toString('utf8');
    }

    let doc;
    try {
      doc = JSON.parse(text);
    } catch {
      log.warn('ws', `unparseable frame: ${text.slice(0, 120)}`);
      return;
    }
    this.#dispatch(doc);
  }

  #dispatch(doc) {
    switch (doc.type) {
      case 'identified':
        this.#onIdentified(doc);
        break;
      case 'batch_ack':
        log.info('ws', 'batch acknowledged');
        this.emit('batch_ack', doc);
        break;
      case 'batch_error':
        log.error('ws', `batch rejected: ${JSON.stringify(doc).slice(0, 300)}`);
        this.emit('batch_error', doc);
        break;
      case 'error':
        log.error('ws', `backend error: ${JSON.stringify(doc).slice(0, 300)}`);
        this.emit('backend_error', doc);
        break;
      default:
        log.debug('ws', `unhandled frame type "${doc.type}"`);
        this.emit('frame', doc);
    }
  }

  #onIdentified(doc) {
    clearTimeout(this.handshakeTimer);
    const beHex = doc.enonce || '';
    if (beHex.length !== 32) {
      log.error('ws', 'identified without a 16-byte enonce — backend lacks encryption support');
      this.ws.close();
      return;
    }
    try {
      const shared = this.identity.sharedSecret(this.bePubkey);
      this.enc.derive(shared, this.fwNonce, Buffer.from(beHex, 'hex'));
    } catch (err) {
      log.error('ws', `key derivation failed: ${err.message}`);
      this.ws.close();
      return;
    }

    this.connected = true;
    this.attempt = 0;
    const native = (doc.entities || []).map((e) => e.entity_id).filter(Boolean);
    log.info('ws', `identified and encrypted (${native.length} native entities)`, {
      server_time: doc.server_time,
    });
    this.emit('identified', { native, serverTime: doc.server_time });
  }

  /** Sends a JSON object, sealed once the session key exists. */
  send(obj) {
    if (!this.connected || !this.ws || this.ws.readyState !== WebSocket.OPEN) return false;
    const json = JSON.stringify(obj);
    try {
      if (this.enc.ready) this.ws.send(this.enc.seal(json), { binary: true });
      else this.ws.send(json);
      return true;
    } catch (err) {
      log.error('ws', `send failed: ${err.message}`);
      return false;
    }
  }

  close() {
    this.stopped = true;
    clearTimeout(this.handshakeTimer);
    clearTimeout(this.reconnectTimer);
    if (this.ws && this.ws.readyState === WebSocket.OPEN) this.ws.close();
  }
}
