#!/usr/bin/env bun
// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Stack-chan Channel adapter for Claude Code. Spawned by `claude` as a
// subprocess via the project's `.mcp.json` and connected over stdio. Exposes
// four tools that map 1:1 to the firmware's /mcp/* REST endpoints:
//
//   say          → POST /mcp/say          (text body = hiragana/katakana)
//   set_expression → POST /mcp/expression (text body = expression name)
//   set_balloon  → POST /mcp/balloon?hold_ms=N (text body = display text)
//   get_state    → GET  /mcp/state        (JSON response)
//
// All HTTP calls carry `Authorization: Bearer <STACKCHAN_TOKEN>`. The Stack-chan
// URL is typically a Cloudflare Tunnel hostname (HTTPS), but a LAN HTTP URL
// works too for at-home testing.
//
// Configuration via env vars (set in your .env or shell):
//   STACKCHAN_URL    e.g. https://stackchan.example.com   (required)
//   STACKCHAN_TOKEN  matches CONFIG_MCP_API_TOKEN on the firmware (required)
//
// Phase 1 scope: tools only, no push events. The firmware doesn't notify the
// adapter about mic / battery / etc. — Claude has to ask via get_state.

import { Server } from '@modelcontextprotocol/sdk/server/index.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from '@modelcontextprotocol/sdk/types.js';

const STACKCHAN_URL = process.env.STACKCHAN_URL?.replace(/\/$/, '');
const STACKCHAN_TOKEN = process.env.STACKCHAN_TOKEN ?? '';
if (!STACKCHAN_URL || !STACKCHAN_TOKEN) {
  // Fatal: refuse to start. Without these the tools would 401 every time.
  console.error('stackchan-channel: STACKCHAN_URL and STACKCHAN_TOKEN env vars are required');
  process.exit(1);
}

const EXPRESSION_NAMES = ['neutral', 'happy', 'sad', 'angry', 'doubt', 'sleepy'] as const;
type ExpressionName = (typeof EXPRESSION_NAMES)[number];

async function callFirmware(
  method: 'GET' | 'POST',
  path: string,
  body?: string,
): Promise<{ status: number; text: string }> {
  const headers: Record<string, string> = {
    Authorization: `Bearer ${STACKCHAN_TOKEN}`,
  };
  if (body !== undefined) headers['Content-Type'] = 'text/plain; charset=utf-8';
  const r = await fetch(`${STACKCHAN_URL}${path}`, { method, headers, body });
  const text = await r.text();
  return { status: r.status, text };
}

function errorContent(message: string) {
  return { content: [{ type: 'text' as const, text: message }], isError: true };
}

function okContent(text: string) {
  return { content: [{ type: 'text' as const, text }] };
}

const mcp = new Server(
  { name: 'stackchan', version: '0.1.0' },
  {
    // Channel capability is what makes Claude Code treat this as a Channel
    // (vs a plain MCP server). Phase 1 has no push events, so we don't
    // actually emit notifications/claude/channel yet; the capability is
    // declared anyway so the wiring is in place for Phase 2.
    capabilities: {
      experimental: { 'claude/channel': {} },
      tools: {},
    },
    instructions:
      'Stack-chan は M5Stack 製の卓上ロボット。アバター顔の表情・吹き出し・' +
      '音声合成 (jtts) を操作できる。`say` のテキストはひらがな/カタカナ必須 ' +
      '(漢字→読み変換は無し)。表情・吹き出しはユーザーの邪魔にならない範囲で。' +
      '状態を確認したいときは get_state。',
  },
);

mcp.setRequestHandler(ListToolsRequestSchema, async () => ({
  tools: [
    {
      name: 'say',
      description:
        'Stack-chan に発話させる。テキストはひらがな/カタカナ。漢字は読み取られない (jtts は読み変換器を持たない)。発話中に再度呼ぶと前の発話完了後にキュー実行。',
      inputSchema: {
        type: 'object',
        properties: {
          text: { type: 'string', maxLength: 200, description: 'ひらがな/カタカナのテキスト' },
        },
        required: ['text'],
      },
    },
    {
      name: 'set_expression',
      description: 'Stack-chan のアバター表情を変更。',
      inputSchema: {
        type: 'object',
        properties: {
          expression: { type: 'string', enum: EXPRESSION_NAMES as unknown as string[] },
        },
        required: ['expression'],
      },
    },
    {
      name: 'set_balloon',
      description:
        'アバター下の吹き出しにテキスト表示。長文はマーキー スクロール。hold_ms 省略時はデフォルト時間。',
      inputSchema: {
        type: 'object',
        properties: {
          text: { type: 'string', maxLength: 200 },
          hold_ms: { type: 'integer', minimum: 0, default: 0 },
        },
        required: ['text'],
      },
    },
    {
      name: 'get_state',
      description: 'Stack-chan の現在状態 (FW バージョン、IP、Wi-Fi、バッテリ、ボード種別) を JSON で返す。',
      inputSchema: { type: 'object', properties: {} },
    },
  ],
}));

mcp.setRequestHandler(CallToolRequestSchema, async (req) => {
  const name = req.params.name;
  const args = (req.params.arguments ?? {}) as Record<string, unknown>;
  try {
    switch (name) {
      case 'say': {
        const text = String(args.text ?? '');
        if (!text) return errorContent('text is required');
        const r = await callFirmware('POST', '/mcp/say', text);
        if (r.status !== 200) return errorContent(`HTTP ${r.status}: ${r.text}`);
        return okContent('queued');
      }
      case 'set_expression': {
        const expr = String(args.expression ?? '');
        if (!EXPRESSION_NAMES.includes(expr as ExpressionName)) {
          return errorContent(`expression must be one of ${EXPRESSION_NAMES.join(', ')}`);
        }
        const r = await callFirmware('POST', '/mcp/expression', expr);
        if (r.status !== 200) return errorContent(`HTTP ${r.status}: ${r.text}`);
        return okContent(`expression=${expr}`);
      }
      case 'set_balloon': {
        const text = String(args.text ?? '');
        const holdMs = Number(args.hold_ms ?? 0);
        if (!text) return errorContent('text is required');
        const path = `/mcp/balloon${holdMs > 0 ? `?hold_ms=${holdMs}` : ''}`;
        const r = await callFirmware('POST', path, text);
        if (r.status !== 200) return errorContent(`HTTP ${r.status}: ${r.text}`);
        return okContent('shown');
      }
      case 'get_state': {
        const r = await callFirmware('GET', '/mcp/state');
        if (r.status !== 200) return errorContent(`HTTP ${r.status}: ${r.text}`);
        // Pass the JSON through so Claude can pick fields out.
        return okContent(r.text);
      }
      default:
        return errorContent(`unknown tool: ${name}`);
    }
  } catch (e) {
    return errorContent(`fetch error: ${e instanceof Error ? e.message : String(e)}`);
  }
});

await mcp.connect(new StdioServerTransport());
// Log to stderr — stdout is reserved for the stdio JSON-RPC transport.
console.error(`stackchan-channel ready (target=${STACKCHAN_URL})`);
