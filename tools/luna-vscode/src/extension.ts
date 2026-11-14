// The luna extension surface. Thin shell over src/core:
//
//   - completions + hover from the GENERATED symbol snapshot
//     (scripts/regen.sh — the real runtime's own introspection; never
//     a hand-written table), merged live with the attached target's
//     own completion engine over the serve protocol
//   - run file/selection: spawn `luna`, stream to an output channel
//   - attach: pick a target from the sock dir, drive it from any lua
//     editor (ctrl+enter sends the line), replies land in a REPL doc;
//     the serve protocol is the ONLY execution channel — attach is a
//     bridge to the target's live state, not a second runtime

import * as vscode from "vscode";
import * as fs from "fs";
import * as path from "path";
import * as cp from "child_process";
import {
  parseSymbols,
  moduleNames,
  memberNames,
  resolveChain,
  requireAliases,
  chainAround,
  trailingChain,
  requirePrefix,
  SymbolTable,
  SymbolNode,
} from "./core/symbols";
import { LunaAttach, ps, sockDir, Target } from "./core/attach";

const ATTACHED_CONTEXT = "luna.attached";

// the generated snapshot rides the package root (out/ is one level down)
function loadSymbols(): SymbolTable {
  const jsonPath = path.join(__dirname, "..", "symbols.json");
  return parseSymbols(fs.readFileSync(jsonPath, "utf8"));
}

function nodeDetail(node: SymbolNode): string {
  const parts: string[] = [];
  if (node.sig) {
    parts.push(node.sig);
  }
  if (node.at) {
    parts.push(node.at);
  }
  return parts.join("  ") || node.kind;
}

function nodeDoc(node: SymbolNode, docs: Record<string, string>, docPath?: string): vscode.MarkdownString | undefined {
  const doc = node.doc ?? (docPath ? docs[docPath] : undefined);
  const md = new vscode.MarkdownString();
  const face = node.sig ? ["```lua", node.sig, "```"].join("\n") : `\`${node.kind}\``;
  md.appendText(face);
  if (doc) {
    md.appendText("\n\n");
    md.appendText(doc);
  }
  return md;
}

export function activate(context: vscode.ExtensionContext): void {
  const table = loadSymbols();
  const channel = vscode.window.createOutputChannel("luna");
  const status = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 100);
  let attach: LunaAttach | null = null;
  let replDoc: vscode.TextDocument | null = null;

  const runtimePath = () =>
    vscode.workspace.getConfiguration("luna").get<string>("runtimePath", "luna");
  const attachTimeoutMs = () =>
    vscode.workspace.getConfiguration("luna").get<number>("attachTimeoutMs", 30000);

  const setAttached = (on: boolean) =>
    vscode.commands.executeCommand("setContext", ATTACHED_CONTEXT, on);

  // -- run file / selection ------------------------------------------------

  const runProcess = (args: string[]) => {
    const bin = runtimePath();
    channel.appendLine(`$ ${bin} ${args.join(" ")}`);
    channel.show(true);
    const child = cp.spawn(bin, args, { stdio: ["ignore", "pipe", "pipe"] });
    child.stdout.on("data", (d: Buffer) => channel.append(d.toString()));
    child.stderr.on("data", (d: Buffer) => channel.append(d.toString()));
    child.on("error", (e: Error) => channel.appendLine(`spawn failed: ${e.message}`));
    child.on("close", (code: number) => channel.appendLine(`[exit ${code}]`));
  };

  const runFile = async () => {
    const editor = vscode.window.activeTextEditor;
    if (!editor || editor.document.uri.scheme !== "file") {
      vscode.window.showErrorMessage("luna: run file needs a saved lua file");
      return;
    }
    try {
      await editor.document.save();
    } catch {
      // an unsavable doc still runs from its last saved state
    }
    runProcess([editor.document.fileName]);
  };

  const runSelection = () => {
    const editor = vscode.window.activeTextEditor;
    if (!editor) {
      vscode.window.showErrorMessage("luna: run selection needs an editor");
      return;
    }
    const text = editor.document.getText(editor.selection);
    if (!text.trim()) {
      vscode.window.showErrorMessage("luna: nothing selected");
      return;
    }
    runProcess(["-e", text]);
  };

  // -- attach --------------------------------------------------------------

  const statusText = () => {
    if (!attach) {
      return "";
    }
    const lat = attach.latencyMs;
    return `luna: ${attach.pid}` + (lat != null ? ` (${lat}ms)` : "");
  };

  const ensureReplDoc = async (): Promise<vscode.TextDocument> => {
    if (replDoc) {
      return replDoc;
    }
    replDoc = await vscode.workspace.openTextDocument({
      content: "-- luna attach REPL — ctrl+enter in any lua editor sends the\n" +
        "-- line to the attached target; replies land here\n",
      language: "lua",
    });
    return replDoc;
  };

  const appendRepl = async (text: string) => {
    const doc = await ensureReplDoc();
    const edit = new vscode.WorkspaceEdit();
    edit.insert(doc.uri, doc.positionAt(doc.getText().length), text);
    await vscode.workspace.applyEdit(edit);
    await vscode.window.showTextDocument(doc, { preserveFocus: true, viewColumn: 2 });
  };

  const attachTo = async (target: Target) => {
    if (attach) {
      attach.close();
    }
    const session = new LunaAttach();
    try {
      await session.connect(target.pid, attachTimeoutMs());
    } catch (e) {
      vscode.window.showErrorMessage(`luna: attach failed: ${(e as Error).message}`);
      return;
    }
    attach = session;
    await setAttached(true);
    status.text = statusText();
    status.command = "luna.detach";
    status.tooltip = `attached to luna pid ${target.pid}; detach to close`;
    status.show();
    await appendRepl(`-- attached to ${target.pid}\n`);
    vscode.window.showInformationMessage(`luna: attached to pid ${target.pid}`);
  };

  const attachCmd = async () => {
    const targets = ps().filter((t) => t.alive);
    if (targets.length === 0) {
      vscode.window.showInformationMessage(`luna: no live attach targets in ${sockDir()}`);
      return;
    }
    const picked = await vscode.window.showQuickPick(
      targets.map((t) => ({
        label: `pid ${t.pid}`,
        description: t.path,
        target: t,
      })),
      { placeHolder: "pick a luna process to attach to" },
    );
    if (picked) {
      await attachTo(picked.target);
    }
  };

  const sendLine = async () => {
    if (!attach || !attach.connected) {
      vscode.window.showErrorMessage("luna: not attached (run 'luna: Attach to luna Process')");
      return;
    }
    const editor = vscode.window.activeTextEditor;
    if (!editor) {
      return;
    }
    // the selection if any, else the line at the cursor; one line per
    // exchange (the serve protocol is one request = one line)
    let text = editor.document.getText(editor.selection).trim();
    if (!text || text.includes("\n")) {
      text = editor.document.lineAt(editor.selection.active.line).text.trim();
    }
    if (!text) {
      return;
    }
    try {
      const reply = await attach.exchange(text, attachTimeoutMs());
      status.text = statusText();
      const header = reply.status === "OK" ? "> " : "! error: ";
      const body = reply.status === "EXIT" ? "the target exited" : reply.body;
      await appendRepl(`\n-- ${header}${text}\n${body}\n`);
    } catch (e) {
      // a timeout tears the session down; surface it and clear the UI
      await setAttached(false);
      status.hide();
      attach = null;
      vscode.window.showErrorMessage(`luna: ${(e as Error).message}`);
    }
  };

  const detach = async () => {
    if (attach) {
      attach.close();
    }
    attach = null;
    await setAttached(false);
    status.hide();
  };

  // -- completions + hover (static snapshot, merged live) ------------------

  // track 3, every branch: the attached target's own completion engine
  // — user globals the static snapshot cannot know about
  const mergeLive = async (
    prefix: string,
    items: vscode.CompletionItem[],
  ): Promise<vscode.CompletionItem[]> => {
    if (attach?.connected) {
      try {
        const live = await attach.complete(prefix, 2000);
        const seen = new Set(items.map((i) => i.label));
        for (const cand of live) {
          if (cand && !seen.has(cand)) {
            seen.add(cand);
            const item = new vscode.CompletionItem(
              cand,
              vscode.CompletionItemKind.Value,
            );
            item.detail = "from the attached luna process";
            items.push(item);
          }
        }
      } catch {
        // live completion is best-effort; the snapshot already answered
      }
    }
    return items;
  };

  const completionProvider: vscode.CompletionItemProvider = {
    async provideCompletionItems(document, position) {
      const prefix = document.getText(
        new vscode.Range(position.with({ character: 0 }), position),
      );

      // track 1: require strings — the module list from the snapshot
      const req = requirePrefix(prefix);
      if (req !== null) {
        return moduleNames(table).map(
          (m) => new vscode.CompletionItem(m, vscode.CompletionItemKind.Module),
        );
      }

      // track 2: dotted chains — require aliases expand into snapshot
      // paths (local loop = require "loop" makes loop.setT resolvable)
      const { chain, typed } = trailingChain(prefix);
      const aliases = requireAliases(document.getText());
      const items: vscode.CompletionItem[] = [];
      if (chain.length === 0) {
        // bare prefix: offer module names and luna globals as heads
        for (const m of moduleNames(table)) {
          if (typed === "" || m.startsWith(typed)) {
            items.push(new vscode.CompletionItem(m, vscode.CompletionItemKind.Module));
          }
        }
        for (const g of Object.keys(table.globals).sort()) {
          if (typed === "" || g.startsWith(typed)) {
            items.push(
              new vscode.CompletionItem(g, vscode.CompletionItemKind.Class),
            );
          }
        }
        return mergeLive(prefix, items);
      }
      const head = aliases[chain[0]];
      const resolved = head
        ? resolveChain(table, [...head.split("."), ...chain.slice(1)])
        : resolveChain(table, chain);
      if (resolved?.members) {
        for (const name of memberNames(resolved)) {
          const child = resolved.members[name];
          const item = new vscode.CompletionItem(
            name,
            child.kind === "function"
              ? vscode.CompletionItemKind.Function
              : vscode.CompletionItemKind.Field,
          );
          item.detail = nodeDetail(child);
          item.documentation = nodeDoc(child, table.docs);
          items.push(item);
        }
      }
      return mergeLive(prefix, items);
    },
  };

  const hoverProvider: vscode.HoverProvider = {
    provideHover(document, position) {
      const line = document.lineAt(position.line).text;
      const chain = chainAround(line, position.character);
      if (!chain || chain.length === 0) {
        return undefined;
      }
      const aliases = requireAliases(document.getText());
      const head = aliases[chain[0]];
      const node = head
        ? resolveChain(table, [...head.split("."), ...chain.slice(1)])
        : resolveChain(table, chain);
      if (!node) {
        return undefined;
      }
      const docPath = head
        ? [...head.split("."), ...chain.slice(1)].join(".")
        : chain.join(".");
      return new vscode.Hover([
        nodeDetail(node),
        nodeDoc(node, table.docs, docPath) ?? "",
      ]);
    },
  };

  // -- wiring ---------------------------------------------------------------

  context.subscriptions.push(
    vscode.languages.registerCompletionItemProvider("lua", completionProvider, ".", ":"),
    vscode.languages.registerHoverProvider("lua", hoverProvider),
    vscode.commands.registerCommand("luna.runFile", runFile),
    vscode.commands.registerCommand("luna.runSelection", runSelection),
    vscode.commands.registerCommand("luna.attach", attachCmd),
    vscode.commands.registerCommand("luna.attach.sendLine", sendLine),
    vscode.commands.registerCommand("luna.detach", detach),
    channel,
    status,
    { dispose: () => detach() },
  );
}

export function deactivate(): void {
  // subscriptions own the detach
}
