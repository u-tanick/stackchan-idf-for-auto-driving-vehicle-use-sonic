#!/usr/bin/env node
// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Bundle the JS DSL compiler (lexer/parser/emitter/compile/opcodes) into a
// single IIFE and inject it (+ a set of named DSL source presets) into an
// HTML template. Used by both wasm/build.sh (standalone preview shell) and
// the firmware build (wifi_config settings page) so both stay in lockstep.
//
// Usage:
//   node inject.mjs <template.html> <output.html> <name>=<path.avdsl> ...
//
// Placeholders in the template are replaced verbatim:
//   /*{{AVATAR_DSL_BUNDLE}}*/        → IIFE bundle (assigns window.AvatarDsl)
//   "{{AVATAR_DSL_PRESETS}}"         → JSON array
//                                       [{name, source}, ...]
//                                       (order = CLI argument order; the
//                                        first entry is treated by the host
//                                        page as the initial selection)
//   "{{AVATAR_DSL_DEFAULT_SOURCE}}"  → JSON string of the first preset's
//                                       source (backward-compatible alias).

import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const args = process.argv.slice(2);
if (args.length < 3) {
  process.stderr.write(
    'usage: inject.mjs <template.html> <output.html> <name>=<path.avdsl> [<name>=<path.avdsl> ...]\n');
  process.exit(1);
}
const [templatePath, outPath, ...presetArgs] = args;

const presets = presetArgs.map((a) => {
  const eq = a.indexOf('=');
  if (eq < 0) {
    process.stderr.write(`error: preset spec '${a}' missing '=' separator\n`);
    process.exit(1);
  }
  const name = a.slice(0, eq).trim();
  const path = a.slice(eq + 1).trim();
  if (!name || !path) {
    process.stderr.write(`error: preset spec '${a}' has empty name or path\n`);
    process.exit(1);
  }
  return { name, source: readFileSync(path, 'utf8') };
});

const SCRIPT_DIR = dirname(fileURLToPath(import.meta.url));
// Dependency-ordered concatenation. The keyword stripping is line-based and
// only handles single-line `import { ... } from '...'` / `export ` prefixes —
// matching the project's strict ESM style.
const SRC_FILES = ['opcodes.js', 'lexer.js', 'parser.js', 'emitter.js', 'compile.js'];

function buildBundle() {
  const parts = [];
  for (const fn of SRC_FILES) {
    let text = readFileSync(resolve(SCRIPT_DIR, fn), 'utf8');
    text = text.replace(/^import\s+\{[^}]*\}\s+from\s+['"][^'"]+['"];?\s*$/gm, '');
    text = text.replace(/^export\s+/gm, '');
    parts.push(`// === ${fn} ===\n${text}`);
  }
  return '(function(){\n' + parts.join('\n') +
    '\n// Public surface for the host page.\n' +
    'window.AvatarDsl = { compile: compile, MAGIC: MAGIC, VERSION: VERSION };\n' +
    '})();';
}

const bundle = buildBundle();
const presetsJson = JSON.stringify(presets);
const firstSourceJson = JSON.stringify(presets[0].source);

let html = readFileSync(templatePath, 'utf8');
let bundleHits = 0, presetsHits = 0, srcHits = 0;
html = html.replace(/\/\*\{\{AVATAR_DSL_BUNDLE\}\}\*\//g,
  () => { ++bundleHits; return bundle; });
html = html.replace(/"\{\{AVATAR_DSL_PRESETS\}\}"/g,
  () => { ++presetsHits; return presetsJson; });
html = html.replace(/"\{\{AVATAR_DSL_DEFAULT_SOURCE\}\}"/g,
  () => { ++srcHits; return firstSourceJson; });

// Shared settings-page helpers (tools/settings_common.js). Optional: only
// settings_wifi.html carries the placeholder (it must stay a single
// embeddable file); settings.html loads the same file as a sibling
// <script src> instead, and avatar.html doesn't use it at all.
let commonHits = 0;
html = html.replace(/\/\*\{\{SETTINGS_COMMON\}\}\*\//g, () => {
  ++commonHits;
  return readFileSync(resolve(SCRIPT_DIR, '..', 'settings_common.js'), 'utf8');
});

if (bundleHits === 0) {
  process.stderr.write(`warning: ${templatePath}: no '/*{{AVATAR_DSL_BUNDLE}}*/' placeholder found\n`);
}
if (presetsHits === 0 && srcHits === 0) {
  process.stderr.write(`warning: ${templatePath}: no preset / default-source placeholder found\n`);
}

writeFileSync(outPath, html);
process.stdout.write(
  `[avatar_dsl] injected bundle (${bundle.length}B) + ${presets.length} preset(s) ` +
  `(${presets.map((p) => p.name).join(', ')})` +
  (commonHits > 0 ? ' + settings_common.js' : '') +
  ` into ${templatePath} -> ${outPath}\n`);
