import {
  createConnection,
  ProposedFeatures,
  TextDocuments,
  TextDocumentSyncKind,
} from "vscode-languageserver/node";
import { TextDocument } from "vscode-languageserver-textdocument";
import { analyzeDocument, findDefinition } from "./analysis";

const connection = createConnection(ProposedFeatures.all);
const documents = new TextDocuments(TextDocument);
const analyses = new Map<string, ReturnType<typeof analyzeDocument>>();

connection.onInitialize(() => ({
  capabilities: {
    textDocumentSync: TextDocumentSyncKind.Incremental,
    documentSymbolProvider: true,
    definitionProvider: true,
  },
}));

function updateDocument(document: TextDocument): void {
  const analysis = analyzeDocument(document);
  analyses.set(document.uri, analysis);
  connection.sendDiagnostics({ uri: document.uri, diagnostics: analysis.diagnostics });
}

documents.onDidOpen((event) => updateDocument(event.document));
documents.onDidChangeContent((event) => updateDocument(event.document));
documents.onDidClose((event) => {
  analyses.delete(event.document.uri);
  connection.sendDiagnostics({ uri: event.document.uri, diagnostics: [] });
});

connection.onDocumentSymbol((params) => {
  const document = documents.get(params.textDocument.uri);
  if (!document) {
    return [];
  }
  const analysis = analyses.get(document.uri) ?? analyzeDocument(document);
  return analysis.symbols;
});

connection.onDefinition((params) => {
  const document = documents.get(params.textDocument.uri);
  if (!document) {
    return undefined;
  }
  const analysis = analyses.get(document.uri) ?? analyzeDocument(document);
  return findDefinition(analysis, document, params.position);
});

documents.listen(connection);
connection.listen();
