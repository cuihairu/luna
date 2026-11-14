// luna serve reply frames, as a TypeScript twin of the target's
// frame() in lua/luna/serve.lua:
//
//   "OK\n" | "ERR\n" | "EXIT\n"   status line
//   body lines...                 repr lines (or the error text)
//   "\x1e\n"                      bare RS (\30) terminator line
//
// The trailing newline after \30 keeps line-based readers from dangling
// on the last line. Pure logic, no node/vscode imports: the unit tests
// drive it with plain strings, whole chunks or byte by byte.

export type FrameStatus = "OK" | "ERR" | "EXIT";

export interface Frame {
  status: FrameStatus;
  body: string;
}

/** The bare terminator line: ASCII record separator, \30. */
export const FRAME_TERMINATOR = "\x1e";

/** The completion meta request prefix: "\1complete ". */
export const COMPLETION_META = "\x01complete";

export class FrameReader {
  private line = ""; // bytes of a not-yet-complete line
  private lines: string[] = []; // complete lines of the frame in progress

  /**
   * Feed a decoded chunk; returns every frame that completed. A frame
   * ends at a bare "\x1e" line; its status is the first line, the body
   * is the remaining lines joined with "\n" (the same shape the
   * CLI's attach reader assembles: status = acc[1], body = concat
   * from line 2).
   */
  push(chunk: string): Frame[] {
    this.line += chunk;
    const frames: Frame[] = [];
    for (;;) {
      const nl = this.line.indexOf("\n");
      if (nl < 0) {
        break;
      }
      const complete = this.line.slice(0, nl);
      this.line = this.line.slice(nl + 1);
      if (complete === FRAME_TERMINATOR) {
        frames.push(this.takeFrame());
      } else {
        this.lines.push(complete);
      }
    }
    return frames;
  }

  private takeFrame(): Frame {
    const lines = this.lines;
    this.lines = [];
    const status = lines[0] as FrameStatus;
    return { status, body: lines.slice(1).join("\n") };
  }
}
