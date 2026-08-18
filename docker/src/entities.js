/**
 * Entity buffer — the container's answer to src/entity_store.cpp.
 *
 * Namespaces, matching the firmware:
 *   pub.*  measurements the backend recognises as native. The firmware drops any pub.* whose
 *          base name is missing from the native list the backend sends in `identified`
 *          (build_entity_payload in data_sender.cpp), so this does the same.
 *   own.*  free-form values; they travel in the batch's `user_data` object.
 *   mon.*  node telemetry; sent in its own `mon_batch` frame so it does not clobber own.*.
 *
 * Real container metrics are always reported. Synthetic environment readings are only produced
 * when SENSMOS_SIMULATE is on, and only for names the backend already treats as native — a
 * container has no thermometer, so anything it claims there is explicitly generated.
 */

import os from 'node:os';

export const ENTITY_ID_RE = /^(pub|own|mon)\.[a-z0-9_]+$/;

/** Plausible ranges for synthetic readings, keyed by native base name. */
const SIMULATED = {
  temp_in: { unit: '°C', base: 21.5, swing: 1.5, decimals: 1 },
  temp_out: { unit: '°C', base: 14.0, swing: 6.0, decimals: 1 },
  temperature: { unit: '°C', base: 21.5, swing: 1.5, decimals: 1 },
  humidity_in: { unit: '%', base: 46, swing: 6, decimals: 0 },
  humidity_out: { unit: '%', base: 62, swing: 12, decimals: 0 },
  humidity: { unit: '%', base: 46, swing: 6, decimals: 0 },
  pressure: { unit: 'hPa', base: 1013, swing: 8, decimals: 1 },
  voltage: { unit: 'V', base: 3.3, swing: 0.05, decimals: 2 },
};

/** Smooth pseudo-random walk so consecutive readings look like a sensor, not a dice roll. */
function wander(spec, phase) {
  const drift = Math.sin(phase / 7) * 0.6 + Math.sin(phase / 3.1) * 0.25 + (Math.random() - 0.5) * 0.3;
  return (spec.base + drift * spec.swing).toFixed(spec.decimals);
}

export class EntityStore {
  constructor({ simulate = true } = {}) {
    this.simulate = simulate;
    this.native = new Set();
    this.phase = Math.random() * 100;
    this.startedAt = Date.now();
  }

  /** Native entity ids advertised by the backend in `identified`. */
  setNative(ids = []) {
    this.native = new Set(ids.map((id) => String(id).replace(/^pub\./, '')).filter(Boolean));
    return this.native;
  }

  isNative(baseName) {
    return this.native.has(baseName);
  }

  uptimeSec() {
    return Math.floor((Date.now() - this.startedAt) / 1000);
  }

  /** pub.* readings: only names the backend calls native, so nothing is silently dropped. */
  buildPublic(nowSec) {
    const out = [];
    // Real, measurable fact — always reported when the backend calls it native, so a node
    // with no sensors still contributes a non-empty batch (entity_count 0 fix).
    if (this.isNative('uptime_s')) {
      out.push({
        entity_id: 'pub.uptime_s',
        value: String(this.uptimeSec()),
        unit: 's',
        last_updated: nowSec,
      });
    }
    if (!this.simulate) return out;
    this.phase += 1;
    for (const [name, spec] of Object.entries(SIMULATED)) {
      if (!this.isNative(name)) continue;
      out.push({
        entity_id: `pub.${name}`,
        value: wander(spec, this.phase),
        unit: spec.unit,
        last_updated: nowSec,
      });
    }
    return out;
  }

  /** own.* values — real, measurable facts about the container. */
  buildUserData() {
    const load = os.loadavg()[0];
    return {
      uptime_s: String(this.uptimeSec()),
      cpu_load: load.toFixed(2),
      mem_free_mb: String(Math.round(os.freemem() / 1048576)),
      platform: `${os.platform()}-${os.arch()}`,
    };
  }

  /** mon.* telemetry frame contents. */
  buildMonitoring(nowSec) {
    return [
      { entity_id: 'mon.uptime_s', value: String(this.uptimeSec()), unit: 's', last_updated: nowSec },
      {
        entity_id: 'mon.mem_free_kb',
        value: String(Math.round(os.freemem() / 1024)),
        unit: 'kB',
        last_updated: nowSec,
      },
    ];
  }
}
