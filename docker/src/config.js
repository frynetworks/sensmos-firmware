/**
 * Configuration — every knob comes from the environment.
 *
 * Defaults mirror src/config.h in the ESP32 firmware so a container behaves like a board
 * out of the box.
 */

const DEFAULT_API_URL = 'https://api.sensmos.com/v1';
const DEFAULT_WS_URL = 'ws://api.sensmos.com:80/v1/ws';
const DEFAULT_WALLET = '0x6403bdF454Ed25502378f231CB656C698Cb74d78';

// src/config.h: BATCH_MIN_INTERVAL_MS / BATCH_FORCE_INTERVAL_MS / MON_INTERVAL_MS
const BATCH_MIN_INTERVAL_S = 60;
const BATCH_FORCE_INTERVAL_S = 180;
const MON_INTERVAL_S = 180;
const PING_INTERVAL_S = 60;

// custom_components/sensmos/const.py: DATA_MIN_KEY_LEN / DATA_MIN_INTERVAL
const INGEST_MIN_KEY_LEN = 32;
const INGEST_MIN_INTERVAL_S = 20;

const bool = (v, dflt) => (v === undefined || v === '' ? dflt : /^(1|true|yes|on)$/i.test(v));
const int = (v, dflt) => {
  if (v === undefined || v === '') return dflt;
  const n = Number.parseInt(v, 10);
  if (!Number.isFinite(n)) throw new Error(`expected an integer, got "${v}"`);
  return n;
};
const num = (v) => {
  if (v === undefined || v === '') return undefined;
  const n = Number.parseFloat(v);
  if (!Number.isFinite(n)) throw new Error(`expected a number, got "${v}"`);
  return n;
};

export function loadConfig(env = process.env, argv = process.argv.slice(2)) {
  const mode = (env.SENSMOS_MODE || 'firmware').toLowerCase();
  if (mode !== 'firmware' && mode !== 'ingest') {
    throw new Error(`SENSMOS_MODE must be "firmware" or "ingest", got "${mode}"`);
  }

  const wallet = (env.SENSMOS_WALLET || DEFAULT_WALLET).trim();
  if (mode === 'firmware' && !/^0x[0-9a-fA-F]{40}$/.test(wallet)) {
    throw new Error(`SENSMOS_WALLET must be 0x + 40 hex characters, got "${wallet}"`);
  }

  const intervalSec = int(env.SENSMOS_INTERVAL_SEC, BATCH_MIN_INTERVAL_S);
  const floor = mode === 'ingest' ? INGEST_MIN_INTERVAL_S : BATCH_MIN_INTERVAL_S;
  if (intervalSec < floor) {
    throw new Error(`SENSMOS_INTERVAL_SEC must be >= ${floor} in ${mode} mode, got ${intervalSec}`);
  }

  const ingestKey = env.SENSMOS_INGEST_KEY || '';
  if (mode === 'ingest' && ingestKey.length < INGEST_MIN_KEY_LEN) {
    throw new Error(
      `SENSMOS_INGEST_KEY must be at least ${INGEST_MIN_KEY_LEN} characters in ingest mode ` +
        `(got ${ingestKey.length})`,
    );
  }

  const lat = num(env.SENSMOS_LAT);
  const lon = num(env.SENSMOS_LON);
  if ((lat === undefined) !== (lon === undefined)) {
    throw new Error('SENSMOS_LAT and SENSMOS_LON must be set together or not at all');
  }

  return {
    mode,
    dryRun: argv.includes('--dry-run') || bool(env.SENSMOS_DRY_RUN, false),
    wallet,
    apiUrl: (env.SENSMOS_API_URL || DEFAULT_API_URL).replace(/\/+$/, ''),
    wsUrl: env.SENSMOS_WS_URL || DEFAULT_WS_URL,
    nodeName: env.SENSMOS_NODE_NAME || '',
    firmware: env.SENSMOS_FIRMWARE || '0.1.0-docker',
    intervalSec,
    forceIntervalSec: int(env.SENSMOS_FORCE_INTERVAL_SEC, BATCH_FORCE_INTERVAL_S),
    monIntervalSec: int(env.SENSMOS_MON_INTERVAL_SEC, MON_INTERVAL_S),
    pingIntervalSec: int(env.SENSMOS_PING_INTERVAL_SEC, PING_INTERVAL_S),
    keyPath: env.SENSMOS_KEY_PATH || '/data/node.key',
    mac: env.SENSMOS_MAC || '',
    simulate: bool(env.SENSMOS_SIMULATE, true),
    ingestKey,
    lat,
    lon,
    label: env.SENSMOS_LABEL || env.SENSMOS_NODE_NAME || '',
    healthPort: int(env.SENSMOS_HEALTH_PORT, 8080),
    logLevel: (env.SENSMOS_LOG_LEVEL || 'info').toLowerCase(),
    // The backend rejects a registration whose ts has drifted ("timestamp expired"), so a
    // container with a bad clock fails in a way that looks like a signature problem.
    maxClockSkewSec: int(env.SENSMOS_MAX_CLOCK_SKEW_SEC, 120),
  };
}

export const constants = {
  DEFAULT_API_URL,
  DEFAULT_WS_URL,
  DEFAULT_WALLET,
  BATCH_MIN_INTERVAL_S,
  BATCH_FORCE_INTERVAL_S,
  INGEST_MIN_KEY_LEN,
  INGEST_MIN_INTERVAL_S,
};
