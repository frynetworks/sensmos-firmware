/**
 * Sensmos Docker node — entry point.
 *
 * firmware mode: keypair -> self-register -> encrypted WebSocket -> signed batches, i.e. the
 *                same wire protocol an ESP32 board speaks, so the node appears under its owner.
 * ingest mode:   the far simpler POST /v1/ingest software-node path, with no owner binding.
 */

import process from 'node:process';
import { loadConfig } from './config.js';
import { log, setLevel } from './logger.js';
import { loadIdentity } from './identity.js';
import { EntityStore } from './entities.js';
import { SensmosWsClient } from './wsclient.js';
import { register, checkClockSkew, buildRegistrationPayload } from './register.js';
import { pushIngest, softDeviceId } from './ingest.js';
import { pushLocationWs } from './geolocation.js';
import { createHealthServer, NodeState } from './health.js';

const timers = new Set();
const every = (ms, fn) => {
  const t = setInterval(fn, ms);
  timers.add(t);
  return t;
};

function nowSec() {
  return Math.floor(Date.now() / 1000);
}

function buildBatch({ config, identity, store }) {
  const ts = nowSec();
  return {
    type: 'batch',
    device_id: identity.deviceId,
    owner_address: config.wallet,
    timestamp: ts,
    firmware: config.firmware,
    entities: store.buildPublic(ts),
    user_data: store.buildUserData(),
  };
}

function buildMonBatch({ config, identity, store }) {
  const ts = nowSec();
  return {
    type: 'mon_batch',
    device_id: identity.deviceId,
    owner_address: config.wallet,
    timestamp: ts,
    firmware: config.firmware,
    entities: store.buildMonitoring(ts),
  };
}

async function runDryRun(config) {
  const { identity, created } = loadIdentity({ keyPath: config.keyPath, mac: config.mac });
  const store = new EntityStore({ simulate: config.simulate });
  store.setNative(['temp_in', 'humidity_in']); // stand-in for the backend's list

  const payload = buildRegistrationPayload({
    identity,
    owner: config.wallet,
    ts: nowSec(),
  });
  const signatureValid = identity.verify(payload.message, payload.sig_esp);

  log.info('dry-run', `key ${created ? 'generated' : 'loaded'} at ${config.keyPath}`);
  log.info('dry-run', `device_id   ${identity.deviceId}`);
  log.info('dry-run', `mac         ${identity.mac}`);
  log.info('dry-run', `pubkey      ${identity.pubkeyHex.slice(0, 32)}… (${identity.pubkeyHex.length} hex chars)`);
  log.info('dry-run', `owner       ${config.wallet}`);
  log.info('dry-run', `soft id     ${softDeviceId('x'.repeat(32))} (ingest-mode derivation demo)`);
  log.info('dry-run', `register    ${JSON.stringify(payload).slice(0, 200)}…`);
  log.info('dry-run', `signature self-verifies: ${signatureValid}`);
  log.info('dry-run', `batch       ${JSON.stringify(buildBatch({ config, identity, store }))}`);

  if (!signatureValid) {
    log.error('dry-run', 'signature failed to verify — refusing to report success');
    process.exitCode = 1;
    return;
  }
  log.info('dry-run', 'no network traffic was sent');
}

async function runIngestMode(config, state) {
  const deviceId = softDeviceId(config.ingestKey);
  state.deviceId = deviceId;
  log.info('main', `ingest mode — device_id ${deviceId}`);
  log.warn('main', 'ingest mode does not bind the node to a wallet; use firmware mode for that');

  const store = new EntityStore({ simulate: config.simulate });
  // The backend does not advertise a native list on this path; the documented ids are accepted.
  store.setNative(['temp_in', 'humidity_in']);

  const push = async () => {
    const ts = nowSec();
    const entities = [
      ...store.buildPublic(ts).map(({ entity_id, value, unit }) => ({ entity_id, value, unit })),
      { entity_id: 'own.uptime_s', value: String(store.uptimeSec()), unit: 's' },
    ];
    try {
      const res = await pushIngest({
        apiUrl: config.apiUrl,
        key: config.ingestKey,
        entities,
        lat: config.lat,
        lon: config.lon,
        label: config.label,
      });
      if (res.ok) {
        state.batchesSent += 1;
        state.lastBatchAt = Date.now();
      } else {
        state.batchesFailed += 1;
        state.lastError = `ingest HTTP ${res.status}`;
      }
    } catch (err) {
      state.batchesFailed += 1;
      state.lastError = err.message;
      log.warn('ingest', `push failed: ${err.message}`);
    }
  };

  await push();
  every(config.intervalSec * 1000, push);
}

async function runFirmwareMode(config, state, identity) {
  const store = new EntityStore({ simulate: config.simulate });

  const skew = await checkClockSkew(config.apiUrl, config.maxClockSkewSec);
  if (skew.checked) log.info('clock', `offset from server: ${skew.skewSec}s`);

  const reg = await register({ apiUrl: config.apiUrl, identity, owner: config.wallet });
  state.registered = reg.registered;
  state.registrationStatus = reg.status;
  if (!reg.registered) {
    state.lastError = `registration HTTP ${reg.status}: ${reg.body.slice(0, 120)}`;
    log.error('main', `registration did not succeed (${reg.status}) — continuing to connect anyway`);
    log.error('main', `backend said: ${reg.body.slice(0, 300)}`);
  }

  const client = new SensmosWsClient({
    url: config.wsUrl,
    identity,
    firmware: config.firmware,
  });

  let batchTimer = null;
  let monTimer = null;
  let pingTimer = null;

  const sendBatch = () => {
    const ok = client.send(buildBatch({ config, identity, store }));
    if (ok) {
      state.batchesSent += 1;
      state.lastBatchAt = Date.now();
      log.info('batch', `sent (#${state.batchesSent})`);
    } else {
      state.batchesFailed += 1;
      log.warn('batch', 'not sent — socket unavailable');
    }
  };

  client.on('identified', ({ native }) => {
    state.wsConnected = true;
    state.encrypted = client.encrypted;
    const known = store.setNative(native);
    log.info('main', `backend native entities: ${[...known].join(', ') || '(none)'}`);

    clearInterval(batchTimer);
    clearInterval(monTimer);
    clearInterval(pingTimer);
    timers.delete(batchTimer);
    timers.delete(monTimer);
    timers.delete(pingTimer);

    // A batch is what marks the node alive server-side; `ping` alone does not count.
    sendBatch();
    batchTimer = every(config.intervalSec * 1000, sendBatch);
    monTimer = every(config.monIntervalSec * 1000, () =>
      client.send(buildMonBatch({ config, identity, store })),
    );
    pingTimer = every(config.pingIntervalSec * 1000, () =>
      client.send({ type: 'ping', device_id: identity.deviceId, free: 0, largest: 0 }),
    );

    // One tick later, like the firmware's deferred geoloc_push_ws(): first batch goes out first.
    setImmediate(() => {
      void pushLocationWs(client, config.lat !== undefined ? { manual: { lat: config.lat, lon: config.lon } } : undefined);
    });
  });

  client.on('disconnected', () => {
    state.wsConnected = false;
    state.encrypted = false;
  });
  client.on('batch_error', (doc) => {
    state.lastError = `batch_error ${JSON.stringify(doc).slice(0, 160)}`;
  });

  client.connect();
  return client;
}

async function main() {
  const config = loadConfig();
  setLevel(config.logLevel);

  log.info('main', `sensmos docker node starting (mode=${config.mode}, firmware=${config.firmware})`);
  if (config.nodeName) log.info('main', `node name: ${config.nodeName}`);

  if (config.dryRun) {
    await runDryRun(config);
    return;
  }

  let identity = null;
  if (config.mode === 'firmware') {
    const loaded = loadIdentity({ keyPath: config.keyPath, mac: config.mac });
    identity = loaded.identity;
    log.info('main', `identity ${loaded.created ? 'created' : 'loaded'} — device_id ${identity.deviceId}`);
    log.info('main', `owner ${config.wallet}`);
  }

  const state = new NodeState({
    mode: config.mode,
    deviceId: identity ? identity.deviceId : '',
    wallet: config.mode === 'firmware' ? config.wallet : null,
    firmware: config.firmware,
    // Two missed intervals is a real outage; anything less is normal jitter.
    gracePeriodSec: Math.max(config.intervalSec * 2 + 30, 150),
  });

  const health = createHealthServer({ port: config.healthPort, state });
  log.info('main', `health endpoint on :${config.healthPort}/healthz`);

  let client = null;
  if (config.mode === 'firmware') client = await runFirmwareMode(config, state, identity);
  else await runIngestMode(config, state);

  const shutdown = (signal) => {
    log.info('main', `${signal} received — shutting down`);
    for (const t of timers) clearInterval(t);
    if (client) client.close();
    health.close(() => process.exit(0));
    setTimeout(() => process.exit(0), 3000).unref();
  };
  process.on('SIGTERM', () => shutdown('SIGTERM'));
  process.on('SIGINT', () => shutdown('SIGINT'));
}

main().catch((err) => {
  log.error('main', `fatal: ${err.stack || err.message}`);
  process.exit(1);
});
