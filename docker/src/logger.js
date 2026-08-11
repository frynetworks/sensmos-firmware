/** Structured stdout logging — Docker's log driver is the only sink. */

const LEVELS = { debug: 10, info: 20, warn: 30, error: 40 };
let threshold = LEVELS.info;

export function setLevel(level) {
  threshold = LEVELS[level] ?? LEVELS.info;
}

function emit(level, tag, msg, extra) {
  if (LEVELS[level] < threshold) return;
  const ts = new Date().toISOString();
  const suffix = extra === undefined ? '' : ` ${typeof extra === 'string' ? extra : JSON.stringify(extra)}`;
  const line = `${ts} ${level.toUpperCase().padEnd(5)} [${tag}] ${msg}${suffix}`;
  (level === 'error' || level === 'warn' ? process.stderr : process.stdout).write(`${line}\n`);
}

export const log = {
  debug: (tag, msg, extra) => emit('debug', tag, msg, extra),
  info: (tag, msg, extra) => emit('info', tag, msg, extra),
  warn: (tag, msg, extra) => emit('warn', tag, msg, extra),
  error: (tag, msg, extra) => emit('error', tag, msg, extra),
};
