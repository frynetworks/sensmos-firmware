/**
 * Health endpoint backing the container HEALTHCHECK.
 *
 * GET /healthz  200 when the node is doing its job, 503 otherwise
 * GET /status   always 200; the same document, for humans
 */

import http from 'node:http';

export function createHealthServer({ port, state }) {
  const server = http.createServer((req, res) => {
    const doc = state.snapshot();
    const path = (req.url || '/').split('?')[0];

    if (path === '/healthz') {
      const code = doc.healthy ? 200 : 503;
      res.writeHead(code, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(doc));
      return;
    }
    if (path === '/status' || path === '/') {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(doc, null, 2));
      return;
    }
    res.writeHead(404, { 'Content-Type': 'application/json' });
    res.end('{"error":"not found"}');
  });

  server.listen(port, '0.0.0.0');
  return server;
}

/** Liveness bookkeeping shared between the run loop and the health endpoint. */
export class NodeState {
  constructor({ mode, deviceId, wallet, firmware, gracePeriodSec }) {
    this.mode = mode;
    this.deviceId = deviceId;
    this.wallet = wallet;
    this.firmware = firmware;
    this.gracePeriodSec = gracePeriodSec;
    this.registered = false;
    this.registrationStatus = null;
    this.wsConnected = false;
    this.encrypted = false;
    this.batchesSent = 0;
    this.batchesFailed = 0;
    this.lastBatchAt = null;
    this.lastError = null;
    this.batchesAcked = 0;
    this.entitiesSavedLast = null;
    this.entitiesSavedTotal = 0;
    this.lastAckAt = null;
    this.startedAt = Date.now();
  }

  /** Backend confirmed a batch; `entities_saved` says how many readings it kept. */
  recordAck(doc = {}) {
    const saved = Number(doc.entities_saved) || 0;
    this.batchesAcked += 1;
    this.entitiesSavedLast = saved;
    this.entitiesSavedTotal += saved;
    this.lastAckAt = Date.now();
  }

  snapshot() {
    const now = Date.now();
    const sinceBatchSec = this.lastBatchAt ? Math.round((now - this.lastBatchAt) / 1000) : null;
    // Healthy once at least one batch has landed and batches keep flowing. Before the first
    // batch, the grace period keeps the container from being killed during startup.
    const withinGrace = (now - this.startedAt) / 1000 < this.gracePeriodSec;
    const flowing = sinceBatchSec !== null && sinceBatchSec < this.gracePeriodSec;
    const healthy =
      this.mode === 'ingest'
        ? this.batchesSent > 0 || withinGrace
        : (this.wsConnected && this.encrypted && (flowing || withinGrace));

    return {
      healthy,
      mode: this.mode,
      device_id: this.deviceId,
      owner_address: this.wallet,
      firmware: this.firmware,
      registered: this.registered,
      registration_status: this.registrationStatus,
      ws_connected: this.wsConnected,
      encrypted: this.encrypted,
      batches_sent: this.batchesSent,
      batches_failed: this.batchesFailed,
      batches_acked: this.batchesAcked,
      entities_saved_last: this.entitiesSavedLast,
      entities_saved_total: this.entitiesSavedTotal,
      seconds_since_ack: this.lastAckAt ? Math.round((now - this.lastAckAt) / 1000) : null,
      seconds_since_batch: sinceBatchSec,
      uptime_s: Math.round((now - this.startedAt) / 1000),
      last_error: this.lastError,
    };
  }
}
