# Sensmos node in Docker

Runs a Sensmos node in a container on Linux, Windows or macOS — anywhere Docker runs. It speaks
the same wire protocol as the ESP32 firmware in this repository, so the node registers under your
account and appears on the network without any hardware.

The ESP32 sources in this repository are untouched. Everything here lives under `docker/`.

## Quick start

```bash
cd docker
cp .env.example .env          # optional; the defaults already work
docker compose up -d
docker compose logs -f        # watch it register and start batching
```

Within a minute or two the log shows:

```
[main]     identity created — device_id 8f3c…
[register] registered device 8f3c…  to 0x6403…
[ws]       connected — sending identify
[ws]       identified and encrypted (7 native entities)
[batch]    sent (#1)
```

Confirm the node from outside the container:

```bash
curl -s -H "X-App-Key: sensmos2025" \
  https://api.sensmos.com/v1/nodes/by-owner/0x6403bdF454Ed25502378f231CB656C698Cb74d78
```

Your `device_id` should be in the list with `"ws_online": true`.

## How it works

| Step | What happens |
|---|---|
| Identity | Generates a secp256k1 keypair on first run and stores it in `/data/node.key`. `device_id = sha256(hex(pubkey) + hex(mac))`, the same derivation the firmware uses. |
| Registration | `POST /v1/register` with `{message, pubkey, sig_esp}`, where `message` names your wallet and the signature proves the device key. This is the self-registration path — no phone, no Bluetooth. |
| Session | Connects over WebSocket, sends a signed `identify`, and derives an AES-128-GCM session key from ECDH + HKDF. Everything after the handshake is encrypted and sequence-protected. |
| Data | Sends a `batch` every 60 s. A batch is also what marks the node alive; a node that only pings is treated as offline. |

## Configuration

Every setting is an environment variable — see [`.env.example`](.env.example) for the annotated
list. The ones that matter most:

| Variable | Default | Meaning |
|---|---|---|
| `SENSMOS_MODE` | `firmware` | `firmware` for the full protocol, `ingest` for the simple software-node path |
| `SENSMOS_WALLET` | `0x6403…4d78` | the account the node registers under |
| `SENSMOS_KEY_PATH` | `/data/node.key` | where the identity is stored |
| `SENSMOS_INTERVAL_SEC` | `60` | seconds between batches (60 is the protocol minimum) |
| `SENSMOS_SIMULATE` | `true` | publish generated sensor readings alongside real container metrics |

### Two modes

**`firmware`** is the default and the one that binds a node to an account. The node is
owner-linked but reports `trusted: false` and earns nothing, because trust requires the physical
Bluetooth attestation ceremony with the mobile app — a container cannot perform it.

**`ingest`** posts to `/v1/ingest` with a passkey of 32+ characters, matching the official Home
Assistant integration. Set `SENSMOS_INGEST_KEY`. There is no wallet binding in this mode.

### About the data

A container has no thermometer. With `SENSMOS_SIMULATE=true` the node publishes generated
readings, and only for entity names the backend itself advertises as native. Real container facts
— uptime, load average, free memory — are always reported as they are. Set
`SENSMOS_SIMULATE=false` to publish nothing but the real metrics.

## Running more than one node

Each node needs its own key volume, otherwise two containers share one identity:

```bash
docker run -d --name sensmos-node-2 \
  -e SENSMOS_NODE_NAME=second-node \
  -e SENSMOS_WALLET=0x6403bdF454Ed25502378f231CB656C698Cb74d78 \
  -v sensmos-data-2:/data \
  sensmos-node:latest
```

## Health

The container exposes `GET /healthz` on port 8080, which the Docker healthcheck uses.
`GET /status` returns the same document formatted for reading:

```bash
curl -s localhost:8080/status
```

```json
{
  "healthy": true,
  "device_id": "8f3c…",
  "owner_address": "0x6403…",
  "registered": true,
  "ws_connected": true,
  "encrypted": true,
  "batches_sent": 12,
  "seconds_since_batch": 14
}
```

The port is bound to loopback by default. It reports node state and is not authenticated, so
think before publishing it.

## Development

```bash
npm install
npm test                  # 29 unit tests: crypto, framing, payload construction
npm run dry-run           # builds and signs a real payload, sends nothing
```

`npm run dry-run` is the fastest way to check a configuration: it prints the `device_id`, the
registration payload and a sample batch, verifies its own signature, and exits without touching
the network.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `registration HTTP 400: timestamp expired` | The container clock is wrong. The backend checks freshness before it checks the signature. |
| `registration HTTP 401: invalid esp signature` | The key file is corrupt, or the message was altered after signing. |
| `identified without a 16-byte enonce` | The backend answered without encryption support; the client refuses to continue in plaintext. |
| Node registers, then a second node appears after a restart | The `/data` volume was not persisted, so a new keypair — and a new `device_id` — was generated. |

## Notes

- The only runtime dependency is `ws`. All cryptography uses Node's built-in `node:crypto`.
- The container runs as a non-root user, with `tini` as PID 1 so `docker stop` shuts it down
  cleanly.
- Registering a node writes a permanent record against your account. Removing the container does
  not remove the node.
