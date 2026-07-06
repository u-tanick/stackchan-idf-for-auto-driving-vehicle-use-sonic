// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Transport-agnostic helpers shared by the two settings pages:
//   - tools/settings.html (BLE)      — loads this as a sibling <script src>
//     (works over file:// for local dev; pages.yml copies it next to the
//     deployed settings.html)
//   - web/settings_wifi.html (HTTP)  — gets this inlined at firmware build
//     time by tools/avatar_dsl/inject.mjs via the SETTINGS_COMMON block
//     comment placeholder (the page must stay a single embeddable file)
//
// Keep this file dependency-free and side-effect-free (pages opt in via
// init()/setupTabs()); both pages assume only `window.StackchanSettings`.
(function () {
  'use strict';

  // --- Board facts -----------------------------------------------------
  // Keyed by the BoardKind byte the firmware reports (BLE BoardKind chr /
  // /api/status "board"). Mirrors board::profile_for() — update BOTH when a
  // board is added. `slug` is the per-board firmware ZIP key in
  // versions.json (release_ota uses the same strings device-side).
  const BOARDS = {
    0: { label: 'CoreS3 + M5 base',                 slug: 'cores3',    servo: true,  battery: true  },
    1: { label: 'CoreS3 + Takao base',              slug: 'cores3',    servo: true,  battery: false },
    2: { label: 'AtomS3R + Atomic ECHO BASE',       slug: 'atoms3r',   servo: false, battery: false },
    3: { label: 'AtomS3 + Atomic ECHO BASE (slim)', slug: 'atoms3',    servo: false, battery: false },
    4: { label: 'M5 StopWatch (C152)',              slug: 'stopwatch', servo: false, battery: true  },
  };

  function boardLabel(kind) {
    const b = BOARDS[kind];
    return b ? b.label : `kind ${kind}`;
  }

  // Firmware-ZIP key for a board kind. Unknown / null (pre-BoardKind
  // firmware) falls back to cores3 — the only board that existed then.
  function boardSlug(kind) {
    const b = BOARDS[kind];
    return b ? b.slug : 'cores3';
  }

  // --- Log panel ---------------------------------------------------------
  // Appends one timestamped line to #log. textContent (not innerHTML) so
  // device-supplied strings can't inject markup. The per-page default css
  // class is set via init() (BLE page historically rendered classless calls
  // as "info" blue; the Wi-Fi page as plain).
  let logDefaultClass = '';
  function log(msg, cls) {
    const el = document.getElementById('log');
    if (!el) return;
    const now = new Date();
    const ts = `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}:${String(now.getSeconds()).padStart(2, '0')}`;
    const div = document.createElement('div');
    div.className = 'entry';
    const tsSpan = document.createElement('span');
    tsSpan.className = 'ts';
    tsSpan.textContent = `[${ts}]`;
    const body = document.createElement('span');
    body.className = cls === undefined ? logDefaultClass : cls;
    body.textContent = msg;
    div.appendChild(tsSpan);
    div.appendChild(body);
    el.appendChild(div);
    el.scrollTop = el.scrollHeight;
  }

  // --- Tab bar -----------------------------------------------------------
  // Wires .tabs button[data-tab] to .tab-panel[data-tab] and persists the
  // active tab under `storageKey` (per page, so the BLE and Wi-Fi pages
  // remember their tabs independently).
  function setupTabs(storageKey) {
    const tabs = document.querySelectorAll('.tabs button[data-tab]');
    const panels = document.querySelectorAll('.tab-panel[data-tab]');
    function activate(name) {
      let found = false;
      tabs.forEach(b => {
        const on = b.dataset.tab === name;
        b.classList.toggle('active', on);
        if (on) found = true;
      });
      if (!found) return;
      panels.forEach(p => p.classList.toggle('active', p.dataset.tab === name));
      try { localStorage.setItem(storageKey, name); } catch {}
    }
    tabs.forEach(b => b.addEventListener('click', () => activate(b.dataset.tab)));
    let saved = null;
    try { saved = localStorage.getItem(storageKey); } catch {}
    if (saved) activate(saved);
  }

  function init(opts) {
    opts = opts || {};
    if (opts.logDefaultClass !== undefined) logDefaultClass = opts.logDefaultClass;
    if (opts.tabStorageKey) setupTabs(opts.tabStorageKey);
  }

  window.StackchanSettings = { BOARDS, boardLabel, boardSlug, log, setupTabs, init };
})();
