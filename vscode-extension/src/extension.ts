import * as vscode from "vscode";
import { spawn } from "node:child_process";
import * as path from "node:path";
import { LanguageClient, ServerOptions, TransportKind } from "vscode-languageclient/node";

let languageClient: LanguageClient | undefined;

function normalizeSpacing(input: string): string {
  let text = input.trim();
  const replacements: Array<[RegExp, string]> = [
    [/\s*==\s*/g, " == "],
    [/\s*!=\s*/g, " != "],
    [/\s*<=\s*/g, " <= "],
    [/\s*>=\s*/g, " >= "],
    [/\s*=\s*/g, " = "],
    [/\s*\+\s*/g, " + "],
    [/\s*-\s*/g, " - "],
    [/\s*\/\s*/g, " / "],
    [/\s*\*\s*/g, " * "],
    [/\s*<\s*/g, " < "],
    [/\s*>\s*/g, " > "],
    [/\s*\(\s*/g, "("],
    [/\s*\)\s*/g, ")"],
    [/\s*\{\s*/g, " { "],
    [/\s*\}\s*/g, " }"],
    [/\s*,\s*/g, ", "],
    [/\s*;\s*/g, "; "],
  ];

  for (const [pattern, replacement] of replacements) {
    text = text.replace(pattern, replacement);
  }

  return text.replace(/\s{2,}/g, " ");
}

function formatLine(line: string): string {
  const raw = line.trim();
  if (raw.length === 0) {
    return "";
  }
  return normalizeSpacing(raw);
}

function formatDocument(document: vscode.TextDocument): string {
  const lines = document.getText().split(/\r?\n/);
  const output: string[] = [];
  let depth = 0;

  for (let i = 0; i < lines.length; i++) {
    const original = lines[i];
    const text = original.trim();
    if (text.length === 0) {
      if (output.length > 0 && output[output.length - 1] !== "") {
        output.push("");
      }
      continue;
    }

    const clean = formatLine(original);
    const leading = " ".repeat(depth * 4);
    const normalized = leading + clean;

    if (clean.endsWith("{")) {
      output.push(normalized);
      depth += 1;
      continue;
    }

    if (clean === "}") {
      if (depth > 0) {
        depth -= 1;
      }
      output.push(" ".repeat(depth * 4) + "}");
      continue;
    }

    output.push(normalized);
  }

  return output.join("\n");
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
  const outputChannel = vscode.window.createOutputChannel("7ABA");
  const serverModule = context.asAbsolutePath(path.join("out", "server.js"));
  const serverOptions: ServerOptions = {
    run: { module: serverModule, transport: TransportKind.ipc },
    debug: {
      module: serverModule,
      transport: TransportKind.ipc,
      options: { execArgv: ["--nolazy", "--inspect=6009"] },
    },
  };
  languageClient = new LanguageClient(
    "7abaLanguageServer",
    "7ABA Language Server",
    serverOptions,
    {
      documentSelector: [{ language: "7aba", scheme: "file" }],
    },
  );
  context.subscriptions.push(languageClient);
  await languageClient.start();

  const provider: vscode.DocumentFormattingEditProvider = {
    provideDocumentFormattingEdits(
      document: vscode.TextDocument,
    ): vscode.TextEdit[] {
      const fullRange = new vscode.Range(
        document.lineAt(0).range.start,
        document.lineAt(Math.max(0, document.lineCount - 1)).range.end,
      );
      const formatted = formatDocument(document);
      return [vscode.TextEdit.replace(fullRange, formatted)];
    },
  };

  context.subscriptions.push(
    vscode.languages.registerDocumentFormattingEditProvider("7aba", provider),
    outputChannel,
    vscode.commands.registerCommand(
      "7aba.runFile",
      async (resource?: vscode.Uri) => {
        const document = resource
          ? await vscode.workspace.openTextDocument(resource)
          : vscode.window.activeTextEditor?.document;

        if (
          !document ||
          document.languageId !== "7aba" ||
          document.uri.scheme !== "file"
        ) {
          void vscode.window.showErrorMessage(
            "Open a saved 7ABA file before running it.",
          );
          return;
        }

        if (document.isDirty && !(await document.save())) {
          return;
        }

        const configuredPath = vscode.workspace
          .getConfiguration("7aba")
          .get<string>("executablePath", "")
          .trim();
        const executable =
          configuredPath ||
          context.asAbsolutePath(path.join("bin", "7aba.exe"));

        outputChannel.clear();
        outputChannel.appendLine(
          `Running ${path.basename(document.uri.fsPath)}...`,
        );
        outputChannel.show(true);

        const process = spawn(executable, ["run", document.uri.fsPath], {
          cwd: path.dirname(document.uri.fsPath),
          windowsHide: true,
        });

        process.stdout.on("data", (data: Buffer) =>
          outputChannel.append(data.toString()),
        );
        process.stderr.on("data", (data: Buffer) =>
          outputChannel.append(data.toString()),
        );
        process.on("error", (error: Error) => {
          outputChannel.appendLine(`\nFailed to start 7ABA: ${error.message}`);
          void vscode.window.showErrorMessage(
            `Failed to start 7ABA: ${error.message}`,
          );
        });
        process.on("close", (code: number | null) => {
          outputChannel.appendLine(
            `\n7ABA exited with code ${code ?? "unknown"}.`,
          );
        });
      },
    ),
  );
}

export async function deactivate(): Promise<void> {
  if (languageClient) {
    await languageClient.stop();
    languageClient = undefined;
  }
}
