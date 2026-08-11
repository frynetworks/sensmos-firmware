import test from 'node:test';
import assert from 'node:assert/strict';
import { Identity } from '../src/identity.js';
import { buildRegistrationMessage, buildRegistrationPayload } from '../src/register.js';
import { softDeviceId, buildIngestBody } from '../src/ingest.js';
import { EntityStore, ENTITY_ID_RE } from '../src/entities.js';
import { loadConfig, constants } from '../src/config.js';

const PRIV = Buffer.alloc(32, 0x11);
const MAC = 'aabbccddeeff';
const OWNER = '0x6403bdF454Ed25502378f231CB656C698Cb74d78';
const identity = () => new Identity({ priv: PRIV, mac: MAC });

test('registration message is the exact string the backend expects', () => {
  const msg = buildRegistrationMessage({ deviceId: 'a'.repeat(64), owner: OWNER, ts: 1700000000 });
  assert.equal(msg, `{"device_id":"${'a'.repeat(64)}","owner":"${OWNER}","ts":1700000000}`);
  // Field order is part of the contract: the signature covers this literal string.
  assert.ok(msg.indexOf('"device_id"') < msg.indexOf('"owner"'));
  assert.ok(msg.indexOf('"owner"') < msg.indexOf('"ts"'));
  assert.equal(JSON.parse(msg).owner, OWNER);
});

test('registration payload carries message, pubkey and sig_esp, and self-verifies', () => {
  const id = identity();
  const payload = buildRegistrationPayload({ identity: id, owner: OWNER, ts: 1700000000 });

  assert.deepEqual(Object.keys(payload).sort(), ['message', 'pubkey', 'sig_esp']);
  assert.equal(payload.pubkey, id.pubkeyHex);
  assert.equal(payload.pubkey.length, 130);
  assert.ok(id.verify(payload.message, payload.sig_esp));
  // `proof` and `pubkey_esp` belong to the mobile-app flow and must not appear here.
  assert.ok(!('proof' in payload));
  assert.ok(!('pubkey_esp' in payload));
});

test('the signed message binds this device to this owner', () => {
  const id = identity();
  const mine = buildRegistrationPayload({ identity: id, owner: OWNER, ts: 1700000000 });
  const other = buildRegistrationMessage({
    deviceId: id.deviceId,
    owner: '0x0000000000000000000000000000000000000000',
    ts: 1700000000,
  });
  assert.ok(!id.verify(other, mine.sig_esp), 'signature must not transfer to another owner');
});

test('software-node device_id follows sha256("sensmos-soft:" + key)', () => {
  const key = 'k'.repeat(32);
  assert.match(softDeviceId(key), /^[0-9a-f]{64}$/);
  assert.notEqual(softDeviceId(key), softDeviceId(`${key}x`));
});

test('ingest body omits location and label when unset', () => {
  const entities = [{ entity_id: 'pub.temp_in', value: '21.4', unit: '°C' }];
  const bare = buildIngestBody({ key: 'k'.repeat(32), entities });
  assert.deepEqual(Object.keys(bare), ['key', 'entities']);

  const full = buildIngestBody({ key: 'k'.repeat(32), entities, lat: 1.5, lon: 2.5, label: 'x' });
  assert.equal(full.lat, 1.5);
  assert.equal(full.label, 'x');
});

test('pub.* entities are only emitted for backend-native names', () => {
  const store = new EntityStore({ simulate: true });

  store.setNative([]);
  assert.deepEqual(store.buildPublic(1700000000), [], 'nothing native means nothing published');

  store.setNative(['pub.temp_in', 'humidity_in']);
  const ents = store.buildPublic(1700000000);
  const ids = ents.map((e) => e.entity_id).sort();
  assert.deepEqual(ids, ['pub.humidity_in', 'pub.temp_in']);

  for (const e of ents) {
    assert.match(e.entity_id, ENTITY_ID_RE);
    assert.equal(typeof e.value, 'string', 'values must be strings, not numbers');
    assert.equal(typeof e.unit, 'string');
    assert.equal(e.last_updated, 1700000000);
    assert.ok(Number.isFinite(Number(e.value)));
  }
});

test('simulation can be switched off entirely', () => {
  const store = new EntityStore({ simulate: false });
  store.setNative(['temp_in']);
  assert.deepEqual(store.buildPublic(1), []);
});

test('user_data and mon entities report real container facts as strings', () => {
  const store = new EntityStore({ simulate: false });
  const user = store.buildUserData();
  for (const v of Object.values(user)) assert.equal(typeof v, 'string');
  assert.ok('uptime_s' in user);

  const mon = store.buildMonitoring(1700000000);
  assert.ok(mon.every((e) => e.entity_id.startsWith('mon.')));
  assert.ok(mon.every((e) => typeof e.value === 'string'));
});

test('config defaults track the firmware constants', () => {
  const cfg = loadConfig({}, []);
  assert.equal(cfg.mode, 'firmware');
  assert.equal(cfg.wallet, constants.DEFAULT_WALLET);
  assert.equal(cfg.intervalSec, constants.BATCH_MIN_INTERVAL_S);
  assert.equal(cfg.forceIntervalSec, constants.BATCH_FORCE_INTERVAL_S);
  assert.equal(cfg.apiUrl, constants.DEFAULT_API_URL);
});

test('config rejects a malformed wallet, short ingest key and sub-minimum interval', () => {
  assert.throws(() => loadConfig({ SENSMOS_WALLET: '0xnope' }, []), /40 hex/);
  assert.throws(
    () => loadConfig({ SENSMOS_MODE: 'ingest', SENSMOS_INGEST_KEY: 'short' }, []),
    /at least 32 characters/,
  );
  assert.throws(() => loadConfig({ SENSMOS_INTERVAL_SEC: '5' }, []), />= 60/);
  assert.throws(() => loadConfig({ SENSMOS_MODE: 'nope' }, []), /must be "firmware" or "ingest"/);
  assert.throws(() => loadConfig({ SENSMOS_LAT: '10' }, []), /together/);
});

test('--dry-run is recognised from argv and from the environment', () => {
  assert.equal(loadConfig({}, ['--dry-run']).dryRun, true);
  assert.equal(loadConfig({ SENSMOS_DRY_RUN: 'true' }, []).dryRun, true);
  assert.equal(loadConfig({}, []).dryRun, false);
});
