/**
 * Regression tests for the data-acceptance fix.
 *
 * Bug 1: a node with no simulated sensors (or a native list without env names) ships
 *        `entities: []` every batch, so the backend saves zero entities and the map shows
 *        entity_count 0 — while every batch is still "acknowledged". The store must emit
 *        the real, measurable `uptime_s` reading whenever the backend calls it native.
 *
 * Bug 2: batch_ack carries `entities_saved`, but the node discarded the payload and
 *        counted socket writes as success. NodeState must track acks and saved-entity
 *        counts so /status distinguishes "sent" from "accepted".
 */

import test from 'node:test';
import assert from 'node:assert/strict';
import { EntityStore } from '../src/entities.js';
import { NodeState } from '../src/health.js';

test('real uptime_s is published when the backend calls it native, even with simulate off', () => {
  const store = new EntityStore({ simulate: false });
  store.setNative(['uptime_s', 'wifi_rssi', 'temp_in']);
  const ents = store.buildPublic(1700000000);
  const uptime = ents.find((e) => e.entity_id === 'pub.uptime_s');
  assert.ok(uptime, 'pub.uptime_s must be emitted from real container facts');
  assert.equal(typeof uptime.value, 'string');
  assert.ok(Number.isFinite(Number(uptime.value)));
  assert.equal(uptime.unit, 's');
  assert.equal(uptime.last_updated, 1700000000);
});

test('uptime_s is NOT published when the backend does not advertise it', () => {
  const store = new EntityStore({ simulate: false });
  store.setNative(['temp_in']);
  assert.deepEqual(store.buildPublic(1), [], 'non-native names must never be emitted');
});

test('uptime_s joins the simulated readings without disturbing them', () => {
  const store = new EntityStore({ simulate: true });
  store.setNative(['temp_in', 'uptime_s']);
  const ids = store.buildPublic(1700000000).map((e) => e.entity_id).sort();
  assert.deepEqual(ids, ['pub.temp_in', 'pub.uptime_s']);
});

test('NodeState tracks batch acks and entities_saved separately from socket writes', () => {
  const state = new NodeState({
    mode: 'firmware',
    deviceId: 'd'.repeat(64),
    wallet: '0x' + '1'.repeat(40),
    firmware: 'test',
    gracePeriodSec: 150,
  });

  state.recordAck({ entities_saved: 5 });
  state.recordAck({ entities_saved: 3 });
  state.recordAck({});

  const doc = state.snapshot();
  assert.equal(doc.batches_acked, 3);
  assert.equal(doc.entities_saved_last, 0, 'missing entities_saved counts as 0, not stale');
  assert.equal(doc.entities_saved_total, 8);
  assert.equal(typeof doc.seconds_since_ack, 'number');
});

test('NodeState snapshot exposes ack fields before any ack arrives', () => {
  const state = new NodeState({
    mode: 'firmware',
    deviceId: 'd'.repeat(64),
    wallet: '0x' + '1'.repeat(40),
    firmware: 'test',
    gracePeriodSec: 150,
  });
  const doc = state.snapshot();
  assert.equal(doc.batches_acked, 0);
  assert.equal(doc.entities_saved_total, 0);
  assert.equal(doc.entities_saved_last, null);
  assert.equal(doc.seconds_since_ack, null);
});
