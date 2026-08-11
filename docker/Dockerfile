# Sensmos node, containerized. Same wire protocol as the ESP32 firmware, no hardware required.
#
# Build:  docker build -t sensmos-node:latest .
# Run:    docker compose up -d

# ---------- build ----------
FROM node:22-alpine AS build
WORKDIR /app

# The tests need no dev dependencies — they run on node:test — so this is the production tree.
COPY package.json package-lock.json ./
RUN npm ci --omit=dev

COPY src/ ./src/
COPY test/ ./test/

# A container whose crypto is wrong registers nothing, so the tests gate the image.
RUN node --test "test/*.test.js"

# ---------- runtime ----------
FROM node:22-alpine AS runtime

# tini reaps zombies and forwards SIGTERM, so `docker stop` is a clean shutdown, not a kill.
RUN apk add --no-cache tini

ENV NODE_ENV=production \
    SENSMOS_KEY_PATH=/data/node.key \
    SENSMOS_HEALTH_PORT=8080

WORKDIR /app
COPY --from=build /app/node_modules ./node_modules
COPY --from=build /app/src ./src
COPY package.json ./

# The keypair lives here; keep the volume and the node keeps its identity across recreates.
RUN mkdir -p /data && chown -R node:node /data /app
VOLUME ["/data"]

USER node
EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=5s --start-period=45s --retries=3 \
  CMD node -e "fetch('http://127.0.0.1:'+(process.env.SENSMOS_HEALTH_PORT||8080)+'/healthz').then(r=>process.exit(r.ok?0:1)).catch(()=>process.exit(1))"

ENTRYPOINT ["/sbin/tini", "--"]
CMD ["node", "src/index.js"]
