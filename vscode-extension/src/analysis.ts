import {
  Diagnostic,
  DiagnosticSeverity,
  DocumentSymbol,
  Location,
  Position,
  Range,
  SymbolKind,
} from "vscode-languageserver/node";
import { TextDocument } from "vscode-languageserver-textdocument";

export interface DocumentAnalysis {
  diagnostics: Diagnostic[];
  symbols: DocumentSymbol[];
  definitions: Map<string, Location>;
  identifiers: Array<{ name: string; start: number; end: number }>;
}

interface Delimiter {
  char: string;
  offset: number;
}

function rangeAt(document: TextDocument, start: number, end: number): Range {
  return Range.create(document.positionAt(start), document.positionAt(end));
}

function diagnosticAt(
  document: TextDocument,
  start: number,
  message: string,
  code: string,
  severity: DiagnosticSeverity = DiagnosticSeverity.Error,
): Diagnostic {
  return {
    severity,
    range: rangeAt(document, start, Math.min(start + 1, document.getText().length)),
    message,
    source: "7aba",
    code,
  };
}

function scanDelimiters(
  document: TextDocument,
  diagnostics: Diagnostic[],
): { pairs: Map<number, number>; identifiers: DocumentAnalysis["identifiers"] } {
  const text = document.getText();
  const opening = new Map<string, string>([
    ["(", ")"],
    ["[", "]"],
    ["{", "}"],
  ]);
  const closing = new Set(["}", ")", "]"]);
  const stack: Delimiter[] = [];
  const pairs = new Map<number, number>();
  const identifiers: DocumentAnalysis["identifiers"] = [];
  let inString = false;
  let stringStart = -1;
  let inComment = false;

  for (let offset = 0; offset < text.length; offset++) {
    const character = text[offset];
    if (inComment) {
      if (character === "\n" || character === "\r") {
        inComment = false;
      } else {
        continue;
      }
    }
    if (inString) {
      if (character === "\\") {
        offset++;
        continue;
      }
      if (character === '"') {
        inString = false;
        stringStart = -1;
      } else if (character === "\n" || character === "\r") {
        diagnostics.push(
          diagnosticAt(document, stringStart, "Unterminated string literal.", "unterminated-string"),
        );
        inString = false;
        stringStart = -1;
      }
      continue;
    }
    if (character === "#") {
      inComment = true;
    } else if (character === '"') {
      inString = true;
      stringStart = offset;
    } else if (/[A-Za-z_]/.test(character)) {
      const start = offset;
      while (offset + 1 < text.length && /[A-Za-z0-9_]/.test(text[offset + 1])) {
        offset++;
      }
      identifiers.push({ name: text.slice(start, offset + 1), start, end: offset + 1 });
    } else if (opening.has(character)) {
      stack.push({ char: character, offset });
    } else if (closing.has(character)) {
      const top = stack[stack.length - 1];
      if (!top || opening.get(top.char) !== character) {
        diagnostics.push(
          diagnosticAt(document, offset, `Unexpected closing delimiter '${character}'.`, "unexpected-delimiter"),
        );
      } else {
        stack.pop();
        pairs.set(top.offset, offset);
      }
    }
  }

  if (inString && stringStart >= 0) {
    diagnostics.push(
      diagnosticAt(document, stringStart, "Unterminated string literal.", "unterminated-string"),
    );
  }
  for (const delimiter of stack) {
    diagnostics.push(
      diagnosticAt(
        document,
        delimiter.offset,
        `Unclosed delimiter '${delimiter.char}'.`,
        "unclosed-delimiter",
      ),
    );
  }
  return { pairs, identifiers };
}

export function analyzeDocument(document: TextDocument): DocumentAnalysis {
  const diagnostics: Diagnostic[] = [];
  const { pairs, identifiers } = scanDelimiters(document, diagnostics);
  const text = document.getText();
  const definitions = new Map<string, Location>();
  const symbols: DocumentSymbol[] = [];
  const seenActions = new Map<string, number>();
  const seenClasses = new Map<string, number>();
  const lines = text.split(/\r?\n/);

  for (const [lineIndex, line] of lines.entries()) {
    const lineOffset = document.offsetAt(Position.create(lineIndex, 0));
    const actionMatch = /^\s*action\b(.*)$/.exec(line);
    const classMatch = /^\s*class\b(.*)$/.exec(line);
    let declaration: { kind: SymbolKind; name: string; nameOffset: number; nameEnd: number } | undefined;

    if (actionMatch) {
      const parsed = /^\s*action\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)\s*(?:\{|$)/.exec(line);
      if (!parsed) {
        const keyword = line.indexOf("action");
        diagnostics.push(
          diagnosticAt(document, lineOffset + keyword, "Malformed action declaration; expected 'action name(parameters) {'.", "malformed-action"),
        );
      } else {
        const name = parsed[1];
        const nameStart = lineOffset + line.indexOf(name, line.indexOf("action") + 6);
        declaration = {
          kind: SymbolKind.Function,
          name,
          nameOffset: nameStart,
          nameEnd: nameStart + name.length,
        };
        const previous = seenActions.get(name);
        if (previous !== undefined) {
          diagnostics.push(
            diagnosticAt(document, nameStart, `Action '${name}' is already declared.`, "duplicate-action"),
          );
        } else {
          seenActions.set(name, nameStart);
        }
      }
    } else if (classMatch) {
      const parsed = /^\s*class\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s+extends\s+[A-Za-z_][A-Za-z0-9_]*)?\s*(?:\{|$)/.exec(line);
      if (!parsed) {
        const keyword = line.indexOf("class");
        diagnostics.push(
          diagnosticAt(document, lineOffset + keyword, "Malformed class declaration; expected 'class Name [extends Base] {'.", "malformed-class"),
        );
      } else {
        const name = parsed[1];
        const nameStart = lineOffset + line.indexOf(name, line.indexOf("class") + 5);
        declaration = {
          kind: SymbolKind.Class,
          name,
          nameOffset: nameStart,
          nameEnd: nameStart + name.length,
        };
        const previous = seenClasses.get(name);
        if (previous !== undefined) {
          diagnostics.push(
            diagnosticAt(document, nameStart, `Class '${name}' is already declared.`, "duplicate-class"),
          );
        } else {
          seenClasses.set(name, nameStart);
        }
      }
    }

    if (declaration) {
      const location: Location = {
        uri: document.uri,
        range: rangeAt(document, declaration.nameOffset, declaration.nameEnd),
      };
      if (!definitions.has(declaration.name)) {
        definitions.set(declaration.name, location);
      }
      const lineStart = lineOffset;
      const lineEnd = lineStart + line.length;
      const braceOffset = text.indexOf("{", declaration.nameEnd);
      const matchingClose =
        braceOffset >= declaration.nameEnd && braceOffset <= lineEnd
          ? pairs.get(braceOffset)
          : undefined;
      const symbolRange = rangeAt(
        document,
        lineStart,
        matchingClose === undefined ? lineEnd : matchingClose + 1,
      );
      symbols.push({
        name: declaration.name,
        kind: declaration.kind,
        range: symbolRange,
        selectionRange: location.range,
      });
    }
  }

  return { diagnostics, symbols, definitions, identifiers };
}

export function findDefinition(
  analysis: DocumentAnalysis,
  document: TextDocument,
  position: Position,
): Location | undefined {
  const offset = document.offsetAt(position);
  const token = analysis.identifiers.find(
    (identifier) => offset >= identifier.start && offset <= identifier.end,
  );
  return token ? analysis.definitions.get(token.name) : undefined;
}
