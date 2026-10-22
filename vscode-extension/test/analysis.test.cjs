const assert = require("node:assert/strict");
const test = require("node:test");
const { TextDocument } = require("vscode-languageserver-textdocument");
const { analyzeDocument, findDefinition } = require("../out/analysis.js");

function analyze(text) {
  const document = TextDocument.create("file:///sample.7ba", "7aba", 1, text);
  return { document, analysis: analyzeDocument(document) };
}

test("reports unmatched delimiters and unterminated strings", () => {
  const { analysis } = analyze('display("hello)\n}');
  assert.deepEqual(
    analysis.diagnostics.map((diagnostic) => diagnostic.code).sort(),
    ["unclosed-delimiter", "unexpected-delimiter", "unterminated-string"],
  );
});

test("reports malformed and duplicate action declarations", () => {
  const { analysis } = analyze("action greet(name) {\n}\naction greet() {\n}\naction broken(");
  assert.ok(analysis.diagnostics.some((diagnostic) => diagnostic.code === "duplicate-action"));
  assert.ok(analysis.diagnostics.some((diagnostic) => diagnostic.code === "malformed-action"));
});

test("provides class and action symbols and resolves action references", () => {
  const { document, analysis } = analyze(
    "class Animal {\n}\naction speak(sound) {\n}\nspeak(\"hi\")",
  );
  assert.deepEqual(
    analysis.symbols.map((symbol) => symbol.name),
    ["Animal", "speak"],
  );
  const definition = findDefinition(analysis, document, { line: 4, character: 2 });
  assert.equal(definition.range.start.line, 2);
  assert.equal(document.getText(definition.range), "speak");
});
