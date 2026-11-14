// The attach channel client — a TypeScript twin of the `luna --attach`
// console (run_attach in lua/luna.lua): connect to the target's unix
// socket ($LUNA_SOCK_DIR/luna-<pid>.sock, default /tmp), send ONE
// command line, ring the doorbell, and read the framed reply.
//
// The doorbell is SIGUSR1: a target blocked in its line editor only
// reaches a poll point when nudged, while a busy target is already
// polling from its instruction-count hook — a wake that lands is
// harmless either way (kernel.wake contract).
//
// Node-only (net), no vscode imports: this is also the devcore surface
// the integration tests drive against a real spawned luna target.

import * as net from "net";
import * as fs from "fs";
import * as path from "path";
import { FrameReader, Frame, FrameStatus, COMPLETION_META } from "./protocol";

export type Reply = Frame;

export interface Target {
  pid: number;
  path: string;
  alive: boolean;
}

/** The attach socket directory: $LUNA_SOCK_DIR, /tmp by default. */
export function sockDir(): string {
  return process.env.LUNA_SOCK_DIR || "/tmp";
}

/** The target's socket path: <sockDir>/luna-<pid>.sock. */
export function sockPathFor(pid: number): string {
  return path.join(sockDir(), `luna-${pid}.sock`);
}

/** kill(pid, 0) liveness probe — the same probe serve.ps uses. */
export function isAlive(pid: number): boolean {
  try {
    process.kill(pid, 0);
    return true;
  } catch (e) {
    return (e as NodeJS.ErrnoException).code === "EPERM";
  }
}

/** SIGUSR1 the target: nudge a blocked line editor to its poll point. */
export function wake(pid: number): void {
  try {
    process.kill(pid, "SIGUSR1");
  } catch {
    // ESRCH (gone) or EPERM (not ours): the busy-target poll needs no
    // wake anyway, and a failed wake must not fail the exchange
  }
}

/**
 * The attach socket directory view, like `luna ps`: every
 * luna-<pid>.sock in the sock dir, probed for liveness (a socket file
 * can outlive its process — SIGKILL, crash — and stale ones are
 * reported, not hidden).
 */
export function ps(): Target[] {
  let entries: string[];
  try {
    entries = fs.readdirSync(sockDir());
  } catch {
    return []; // unreadable sock dir: report nothing (serve.ps does)
  }
  const targets: Target[] = [];
  for (const entry of entries) {
    const m = entry.match(/^luna-(\d+)\.sock$/);
    if (!m) {
      continue;
    }
    const pid = Number(m[1]);
    targets.push({ pid, path: path.join(sockDir(), entry), alive: isAlive(pid) });
  }
  targets.sort((a, b) => a.pid - b.pid);
  return targets;
}

/**
 * One attached session. Exchanges are strictly sequential (one command
 * in flight, like the CLI); a reply timeout destroys the socket — a
 * late reply would otherwise land in the next exchange's answer slot
 * and misattribute output.
 */
export class LunaAttach {
  private sock: net.Socket | null = null;
  private reader = new FrameReader();
  private pid_ = 0;
  private pending: {
    resolve: (f: Frame) => void;
    reject: (e: Error) => void;
    timer: NodeJS.Timeout;
  } | null = null;
  private queue: Promise<unknown> = Promise.resolve();
  private lastLatencyMs: number | null = null;

  get pid(): number {
    return this.pid_;
  }

  get latencyMs(): number | null {
    return this.lastLatencyMs;
  }

  get connected(): boolean {
    return !!this.sock && !this.sock.destroyed;
  }

  connect(pid: number, timeoutMs = 5000): Promise<void> {
    if (this.connected) {
      return Promise.resolve();
    }
    return new Promise((resolve, reject) => {
      const sock = net.createConnection(sockPathFor(pid));
      sock.setEncoding("utf8");
      let settled = false;
      const timer = setTimeout(() => {
        sock.destroy();
        if (!settled) {
          settled = true;
          reject(new Error(`attach: connect timeout (${sockPathFor(pid)})`));
        }
      }, timeoutMs);
      sock.once("connect", () => {
        clearTimeout(timer);
        settled = true;
        this.sock = sock;
        this.pid_ = pid;
        this.reader = new FrameReader();
        this.lastLatencyMs = null;
        sock.on("data", (chunk: string) => {
          for (const frame of this.reader.push(chunk)) {
            const p = this.pending;
            if (p) {
              clearTimeout(p.timer);
              this.pending = null;
              p.resolve(frame);
            }
            // a frame with no waiter: nothing is in flight (the
            // sequential queue keeps one waiter at most), drop it
          }
        });
        sock.once("close", () => {
          this.sock = null;
          const p = this.pending;
          if (p) {
            clearTimeout(p.timer);
            this.pending = null;
            p.reject(new Error("attach: target closed the connection"));
          }
        });
        resolve();
      });
      sock.once("error", (e: Error) => {
        clearTimeout(timer);
        if (!settled) {
          settled = true;
          reject(new Error(`attach: ${e.message}`));
        }
      });
    });
  }

  /**
   * Send one command line, get the framed reply. The line must be a
   * single line (the serve protocol is one request = one line) — a
   * "\n" inside would be two requests.
   */
  exchange(line: string, timeoutMs = 30000): Promise<Reply> {
    if (line.includes("\n")) {
      return Promise.reject(new Error("attach: one line per exchange"));
    }
    const run = () => this.exchangeNow(line, timeoutMs);
    const next = this.queue.then(run, run);
    this.queue = next.then(() => undefined, () => undefined);
    return next;
  }

  private exchangeNow(line: string, timeoutMs: number): Promise<Reply> {
    return new Promise((resolve, reject) => {
      const sock = this.sock;
      // destroyed is synchronous (sock.destroy()), the close event that
      // nulls this.sock is not — check both
      if (!sock || sock.destroyed) {
        reject(new Error("attach: not attached"));
        return;
      }
      const started = Date.now();
      this.pending = {
        resolve: (f) => {
          this.lastLatencyMs = Date.now() - started;
          resolve(f);
        },
        reject,
        timer: setTimeout(() => {
          this.pending = null;
          // a late reply would land in the next exchange's slot:
          // tear the session down, the caller can re-attach
          sock.destroy();
          reject(new Error(`attach: no reply within ${timeoutMs}ms (target busy or gone)`));
        }, timeoutMs),
      };
      sock.write(line + "\n", (e) => {
        if (e) {
          reject(e);
          return;
        }
        wake(this.pid_);
      });
    });
  }

  /**
   * The completion meta line ("\1complete <prefix>"): candidates from
   * the target's LIVE state (user-defined variables included), the
   * same engine the attach CLI's Tab completion rides. An unavailable
   * answer degrades to an empty list — completion never throws.
   */
  async complete(prefix: string, timeoutMs = 10000): Promise<string[]> {
    if (prefix.includes("\n")) {
      return [];
    }
    try {
      const reply = await this.exchange(`${COMPLETION_META} ${prefix}`, timeoutMs);
      if (reply.status !== "OK" || !reply.body) {
        return [];
      }
      return reply.body.split("\n");
    } catch {
      return [];
    }
  }

  close(): void {
    const sock = this.sock;
    this.sock = null;
    if (sock) {
      sock.destroy();
    }
  }
}
