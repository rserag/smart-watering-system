import { test, expect, type Page, type WebSocketRoute } from '@playwright/test';
import type { Device } from '../app/garden-shared';
import { DIAGNOSTICS_OPERATOR, type DebugAction, type DebugCapture, type DebugSnapshot } from '../app/diagnostics-shared';

function fixtureSnapshot(): DebugSnapshot {
  return {
    type: 'debug.snapshot', schemaVersion: 1, deviceId: 'diagnostic-garden', bootId: 'diagnostic-boot', sequence: 1, uptimeMs: 121000, configRevision: 3,
    session: { mode: 'observe', sessionId: null, owner: null, leaseRemainingMs: 0, automationHeld: false },
    hardware: { board: 'esp32dev', relayPins: [27, 26, 25, 33], sensorPins: [34, 35, 36, 39], tankPin: 23, relayActiveHigh: true, oled: { sda: 21, scl: 22, address: 60, available: true } },
    tank: { physicalLow: false, effectiveLow: false, low: false, pendingLow: false, debounceRemainingMs: 0, overrideLow: null },
    timings: { loopDurationUs: 24, maxLoopDurationUs: 132, sampleDurationUs: 4600 },
    zones: Array.from({ length: 4 }, (_, index) => ({ id: index + 1, physicalRaw: 2100 + index * 80, raw: 2100 + index * 80, samples: Array.from({ length: 16 }, (_, sample) => 2100 + index * 80 + sample - 8), minRaw: 2092 + index * 80, maxRaw: 2107 + index * 80, filteredRaw: 2050 + index * 80, relativeMoisturePercent: 41 - index * 6, unclampedPercent: 40.7 - index * 6, sensorValid: true, validSamples: 3, invalidSamples: 0, dryCount: 0, wetCount: 0, sampleAgeMs: 120, phase: 'monitoring', fault: null, enabled: index === 0, paused: false, relayOn: false, virtualRelayOn: false, decisionReason: 'above_start_threshold', phaseElapsedMs: 110000, pulseRemainingMs: 0, wateringOnMsThisCycle: 0, overrideRaw: null, dryRaw: 2600, wetRaw: 1250, startWateringPercent: 30, stopWateringPercent: 63, pulseMs: 8000, soakMs: 90000, maxOnMs: 60000 })),
  };
}

async function garden(page: Page, options: { operator?: boolean; supported?: boolean } = {}) {
  const snapshot = fixtureSnapshot();
  const device: Device = { id: snapshot.deviceId, online: true, firmwareVersion: options.supported === false ? '0.7.0' : '0.8.0', bootId: snapshot.bootId, schemaVersion: 1, configRevision: 3, automaticWateringEnabled: true, lastSeenAt: new Date().toISOString(), wifiRssi: -54, mainTankLow: false, mainTankLastChangedAt: new Date().toISOString(), zones: snapshot.zones.map(zone => ({ id: zone.id, name: zone.id === 1 ? 'Tomatoes' : null, raw: zone.raw!, filteredRaw: zone.filteredRaw!, moisturePercent: zone.relativeMoisturePercent!, sensorValid: true, phase: zone.phase, relayOn: false, wateringOnMsThisCycle: 0, fault: null, lastWateredAt: null })), directTelegram: true, telegramDebugEnabled: false, telegramConfigured: true, telegramHourlySilent: false, telegramPendingMessages: 0, telegramLastSendSucceeded: true, telegramWorkerRunning: true, telegramTimeReady: true, telegramLastFailureStage: null };
  const state = {
    snapshot, device, receivedAt: new Date().toISOString(), operator: options.operator !== false, supported: options.supported !== false,
    writes: [] as { path: string; body: Record<string, unknown> }[], actions: [] as DebugAction[], captures: [] as DebugCapture[], sockets: [] as WebSocketRoute[],
    nextRequest: 0, failure: '', renewUnconfirmed: false, cached: false, offline: false, leaseEnds: 0, pulse: null as { requestId: string; zoneId: number; ends: number } | null,
    getGate: Promise.resolve(), inFlight: 0, maxInFlight: 0, reads: 0,
  };
  const record = (command: string, status: string, requestId: string, parameters: Record<string, unknown> = {}, result?: Record<string, unknown>) => {
    state.actions.unshift({ id: state.actions.length + 1, requestId, command, status, createdAt: new Date().toISOString(), bootId: state.snapshot.bootId!, parameters, recordType: status === 'accepted' ? 'command' : 'event', result });
  };
  const stop = () => {
    if (state.pulse) record('debug.output.pulse', 'cancelled', state.pulse.requestId);
    state.pulse = null;
    state.snapshot.zones.forEach(zone => { zone.relayOn = false; zone.virtualRelayOn = false; zone.pulseRemainingMs = 0; });
  };
  const clear = () => {
    state.snapshot.zones.forEach(zone => { zone.overrideRaw = null; zone.raw = zone.physicalRaw; zone.sensorValid = true; });
    state.snapshot.tank.overrideLow = null; state.snapshot.tank.effectiveLow = state.snapshot.tank.physicalLow; state.snapshot.tank.low = state.snapshot.tank.physicalLow;
  };
  const touch = () => {
    if (!state.cached) { state.receivedAt = new Date().toISOString(); state.snapshot.sequence!++; }
    if (state.snapshot.session.mode !== 'observe') state.snapshot.session.leaseRemainingMs = Math.max(0, state.leaseEnds - Date.now());
    if (state.pulse) {
      const zone = state.snapshot.zones[state.pulse.zoneId - 1]; zone.pulseRemainingMs = Math.max(0, state.pulse.ends - Date.now());
      if (zone.pulseRemainingMs === 0) { record('debug.output.pulse', 'completed', state.pulse.requestId); state.pulse = null; zone.relayOn = false; zone.virtualRelayOn = false; }
    }
  };
  await page.route('**/api/**', async route => {
    const path = new URL(route.request().url()).pathname;
    if (path === '/api/me') return route.fulfill({ json: { authenticated: true, authMode: 'development', name: 'Diagnostic operator', email: state.operator ? DIAGNOSTICS_OPERATOR : 'viewer@example.com', canControl: state.operator } });
    if (path === '/api/devices') return route.fulfill({ json: [state.device] });
    if (path.endsWith('/diagnostics')) {
      state.reads++; state.inFlight++; state.maxInFlight = Math.max(state.maxInFlight, state.inFlight);
      try { await state.getGate; touch(); return await route.fulfill({ json: { canControl: state.operator, supported: state.supported, online: !state.offline, snapshot: state.snapshot, receivedAt: state.receivedAt, actions: state.actions, captures: state.captures.map(({ id, name, createdAt, sampleCount }) => ({ id, name, createdAt, sampleCount })) } }); } finally { state.inFlight--; }
    }
    if (path.includes('/debug/captures/') && route.request().method() === 'GET') return route.fulfill({ json: state.captures.find(capture => capture.id === path.split('/').at(-1)) });
    if (path.endsWith('/debug/captures')) {
      expect(state.operator).toBe(true);
      const { name } = route.request().postDataJSON();
      const capture: DebugCapture = { id: `capture-${state.captures.length + 1}`, name, createdAt: new Date().toISOString(), sampleCount: 1, snapshots: [{ snapshot: structuredClone(state.snapshot), receivedAt: state.receivedAt }], actions: state.actions.slice() };
      state.captures.unshift(capture); return route.fulfill({ json: capture });
    }
    if (path.includes('/debug/')) {
      expect(state.operator).toBe(true);
      expect(route.request().method()).toBe('POST');
      const body = route.request().postDataJSON() as Record<string, unknown>;
      state.writes.push({ path, body });
      const requestId = `request-${++state.nextRequest}`;
      const command = path.endsWith('/actions') ? String(body.command) : path.endsWith('/renew') ? 'debug.session.renew' : path.endsWith('/end') ? 'debug.session.end' : 'debug.session.start';
      if (command === 'debug.session.renew' && state.renewUnconfirmed) {
        // The controller executed renewal, but the backend could not confirm
        // its follow-up snapshot and returned an unconfirmed HTTP outcome.
        state.leaseEnds = Date.now() + 30000;
        record(command, 'completed', requestId); touch();
        return route.fulfill({ status: 504, json: { detail: { status: 'unconfirmed', requestId, message: 'Session renewal read-back could not be confirmed.' } } });
      }
      if (state.failure) { record(command, 'rejected', requestId); return route.fulfill({ status: 409, json: { detail: state.failure } }); }
      const parameters = (body.parameters ?? {}) as Record<string, unknown>;
      if (command === 'debug.session.start') {
        stop(); clear(); state.leaseEnds = Date.now() + 30000;
        state.snapshot.session = { mode: body.mode as 'hardware' | 'simulation', sessionId: 'fixture-session', owner: DIAGNOSTICS_OPERATOR, leaseRemainingMs: 30000, automationHeld: true };
      } else if (command === 'debug.session.renew') state.leaseEnds = Date.now() + 30000;
      else if (command === 'debug.session.end') { stop(); clear(); state.snapshot.session = { mode: 'observe', sessionId: null, owner: null, leaseRemainingMs: 0, automationHeld: true }; }
      else if (command === 'debug.automation.resume') state.snapshot.session.automationHeld = false;
      else if (command === 'debug.output.stop') stop();
      else if (command === 'debug.output.pulse') {
        const zoneId = Number(parameters.zoneId); state.pulse = { requestId, zoneId, ends: Date.now() + Number(parameters.durationMs) };
        state.snapshot.zones[zoneId - 1].relayOn = state.snapshot.session.mode === 'hardware'; state.snapshot.zones[zoneId - 1].virtualRelayOn = state.snapshot.session.mode === 'simulation'; record(command, 'started', requestId, parameters);
      } else if (command === 'debug.input.set') {
        expect(state.snapshot.session.mode).toBe('simulation');
        if (parameters.input === 'soil') { const zone = state.snapshot.zones[Number(parameters.zoneId) - 1]; zone.overrideRaw = Number(parameters.raw); zone.raw = zone.overrideRaw; zone.sensorValid = zone.raw >= 50 && zone.raw <= 4050; }
        else { state.snapshot.tank.overrideLow = Boolean(parameters.low); state.snapshot.tank.effectiveLow = Boolean(parameters.low); state.snapshot.tank.low = Boolean(parameters.low); }
      } else if (command === 'debug.input.clear') { expect(state.snapshot.session.mode).toBe('simulation'); clear(); }
      else if (command === 'debug.i2c.scan') record(command, 'completed', requestId, parameters, { addresses: [60] });
      else if (command === 'debug.oled.test') { record(command, 'started', requestId, parameters); record(command, 'completed', requestId, parameters); }
      record(command, 'accepted', requestId, parameters); touch();
      return route.fulfill({ json: { requestId, command, status: 'accepted', ...(state.snapshot.session.sessionId ? { sessionId: state.snapshot.session.sessionId } : {}), snapshot: state.snapshot, receivedAt: state.receivedAt } });
    }
    if (path.endsWith('/telegram/deliveries')) return route.fulfill({ json: [] });
    throw new Error(`Unexpected API request ${route.request().method()} ${path}`);
  });
  await page.routeWebSocket('**/ws/dashboard', socket => { state.sockets.push(socket); socket.send(JSON.stringify({ type: 'snapshot', devices: [state.device] })); });
  await page.goto('/?view=diagnostics');
  await expect(page.getByRole('heading', { name: 'Diagnostics & manual testing' })).toBeVisible();
  await expect(page.getByRole('button', { name: 'Start simulation', exact: true })).toBeEnabled({ timeout: options.operator === false || options.supported === false ? 1 : 5000 }).catch(async error => { if (options.operator !== false && options.supported !== false) throw error; });
  return state;
}

test('observation links all 30 board labels, pipeline, traces and zone selection', async ({ page }) => {
  await garden(page);
  await expect(page.getByRole('heading', { name: '30-pin board & wiring' })).toBeVisible();
  await expect(page.getByLabel('ESP32 board, 15 header pins per side, USB-C at top and antenna at bottom').getByRole('button')).toHaveCount(30);
  await page.getByRole('button', { name: 'D26, GPIO 26, Zone 2 relay output. Inspect connection.' }).click();
  await expect(page.getByRole('button', { name: /Zone 2.*35 ADC.*26 relay/ })).toHaveAttribute('aria-pressed', 'true');
  await expect(page.getByRole('heading', { name: 'From pin to watering decision' })).toBeVisible();
  await expect(page.getByLabel('16 physical ADC conversions')).toBeVisible();
  await page.getByRole('button', { name: 'Freeze chart' }).click();
  await expect(page.getByRole('button', { name: 'Resume live chart' })).toBeVisible();
  await page.screenshot({ path: `test-results/diagnostics-observe-${test.info().project.name}.png`, fullPage: true });
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
});

test('simulation keeps physical signals separate, marks invalid derived data and Stop clears injections', async ({ page }) => {
  const state = await garden(page);
  await page.getByRole('button', { name: 'Start simulation', exact: true }).click();
  await expect(page.getByText('SIMULATION · physical relays stay LOW.', { exact: false })).toBeVisible();
  await page.getByRole('spinbutton', { name: 'Soil ADC override' }).fill('0');
  await page.getByRole('button', { name: 'Inject soil ADC', exact: true }).click();
  await expect(page.getByText('Last valid percentage; not a fresh reading')).toBeVisible();
  expect(state.snapshot.zones[0].physicalRaw).toBe(2100); expect(state.snapshot.zones[0].raw).toBe(0);
  await page.getByRole('button', { name: 'Inject tank state', exact: true }).click();
  await page.getByRole('button', { name: 'Run pulse', exact: true }).click();
  await expect.poll(() => state.snapshot.zones[0].virtualRelayOn).toBe(true);
  expect(state.snapshot.zones[0].relayOn).toBe(false);
  await page.screenshot({ path: `test-results/diagnostics-simulation-${test.info().project.name}.png`, fullPage: true });
  await page.getByRole('button', { name: 'Stop all tests', exact: true }).click();
  await expect.poll(() => state.snapshot.zones[0].overrideRaw).toBe(null);
  expect(state.snapshot.tank.overrideLow).toBe(null); expect(state.snapshot.zones[0].virtualRelayOn).toBe(false);
  await page.getByRole('button', { name: 'End session', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Resume normal automation' })).toBeVisible();
  expect(state.snapshot.session.automationHeld).toBe(true);
});

test('hardware session uses bounded pulse and ends without simulation-only clear', async ({ page }) => {
  const state = await garden(page);
  await page.getByRole('button', { name: 'Start hardware test', exact: true }).click();
  await page.getByRole('spinbutton', { name: 'Relay duration (ms)' }).fill('60001');
  await expect(page.getByRole('button', { name: 'Run pulse', exact: true })).toBeDisabled();
  await page.getByRole('spinbutton', { name: 'Relay duration (ms)' }).fill('5000');
  await page.getByRole('button', { name: 'Run pulse', exact: true }).click();
  await expect.poll(() => state.snapshot.zones[0].relayOn).toBe(true);
  await expect(page.getByRole('button', { name: 'Release soil', exact: true })).toBeDisabled();
  await page.getByRole('button', { name: 'End session', exact: true }).click();
  await expect.poll(() => state.snapshot.session.mode).toBe('observe');
  expect(state.snapshot.zones[0].relayOn).toBe(false);
  expect(state.writes.some(write => write.body.command === 'debug.input.clear')).toBe(false);
});

test('read-only identity and old firmware disable mutation but retain observation and export', async ({ page }) => {
  const state = await garden(page, { operator: false });
  await expect(page.getByText('Read-only diagnostics.', { exact: false })).toBeVisible();
  for (const name of ['Start simulation', 'Start hardware test', 'Stop all tests', 'Run pulse', 'Inject soil ADC', 'Test OLED', 'Save capture']) await expect(page.getByRole('button', { name, exact: true })).toBeDisabled();
  await expect(page.getByRole('button', { name: 'Export live CSV' })).toBeEnabled();
  expect(state.writes).toEqual([]);
  state.supported = false; state.snapshot.hardware.source = 'documentedFallback'; state.snapshot.hardware.oled.available = null; state.snapshot.zones.forEach(zone => { zone.samples = []; zone.validSamples = null; zone.unclampedPercent = null; });
  await expect(page.getByText('Firmware diagnostics are unavailable.', { exact: false })).toBeVisible();
  await expect(page.getByText('OLED status unavailable', { exact: true })).toBeVisible();
  await expect(page.getByText('OLED not detected', { exact: true })).toHaveCount(0);
  await expect(page.getByText('Individual conversions, counters and calibration are unavailable', { exact: false })).toBeVisible();
});

test('rejected action, stale telemetry, reboot and offline never enable another pulse', async ({ page }) => {
  const state = await garden(page);
  await page.getByRole('button', { name: 'Start hardware test', exact: true }).click();
  state.failure = 'Main tank water is low';
  await page.getByRole('button', { name: 'Run pulse', exact: true }).click();
  await expect(page.getByRole('alert')).toContainText('Main tank water is low');
  expect(state.snapshot.zones[0].relayOn).toBe(false); state.failure = '';
  state.cached = true; state.receivedAt = new Date(Date.now() - 11000).toISOString();
  await expect(page.getByRole('button', { name: 'Run pulse', exact: true })).toBeDisabled();
  await expect(page.getByRole('button', { name: 'Stop all tests', exact: true })).toBeEnabled();
  state.device = { ...state.device, bootId: 'new-boot' }; state.sockets[0].send(JSON.stringify({ type: 'telemetry', device: state.device }));
  await expect(page.getByRole('button', { name: 'Stop all tests', exact: true })).toBeDisabled();
  state.offline = true;
  await expect(page.getByRole('button', { name: 'Start simulation', exact: true })).toBeDisabled();
});

test('editable simulation sequence completes, hardware sequence awaits execution and navigation ends lease', async ({ page }) => {
  const state = await garden(page);
  await page.getByRole('button', { name: 'Start simulation', exact: true }).click();
  await page.getByRole('textbox', { name: 'Editable sequence (JSON)' }).fill(JSON.stringify([{ command: 'debug.input.set', parameters: { input: 'soil', zoneId: 1, raw: 3000 }, waitMs: 100 }, { command: 'debug.input.clear', parameters: { input: 'all' } }]));
  await page.getByRole('button', { name: 'Run sequence', exact: true }).click();
  await expect(page.getByText('Sequence completed. Outputs stopped and injections released.')).toBeVisible();
  expect(state.snapshot.zones[0].overrideRaw).toBe(null);
  await page.getByRole('button', { name: 'End session', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Start hardware test', exact: true })).toBeEnabled();
  await page.getByRole('button', { name: 'Start hardware test', exact: true }).click();
  await page.getByRole('textbox', { name: 'Editable sequence (JSON)' }).fill(JSON.stringify([{ command: 'debug.output.pulse', parameters: { zoneId: 1, durationMs: 100 } }]));
  await page.getByRole('button', { name: 'Run sequence', exact: true }).click();
  await expect(page.getByText('Sequence completed. Outputs stopped and injections released.')).toBeVisible();
  const startIndex = state.writes.findLastIndex(write => write.path.endsWith('/debug/session'));
  expect(state.writes.slice(startIndex).some(write => write.body.command === 'debug.input.clear')).toBe(false);
  await page.getByRole('button', { name: 'Overview', exact: true }).click();
  await expect.poll(() => state.snapshot.session.mode).toBe('observe');
});

test('Stop aborts a waiting sequence and polling never overlaps slow reads', async ({ page }) => {
  const state = await garden(page);
  await page.getByRole('button', { name: 'Start simulation', exact: true }).click();
  await page.getByRole('textbox', { name: 'Editable sequence (JSON)' }).fill(JSON.stringify([{ command: 'debug.input.set', parameters: { input: 'soil', zoneId: 1, raw: 3000 }, waitMs: 60000 }, { command: 'debug.output.pulse', parameters: { zoneId: 1, durationMs: 1000 } }]));
  await page.getByRole('button', { name: 'Run sequence', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Abort sequence', exact: true })).toBeEnabled();
  await page.getByRole('button', { name: 'Stop all tests', exact: true }).click();
  await expect.poll(() => state.snapshot.zones[0].overrideRaw).toBe(null);
  expect(state.writes.some(write => write.body.command === 'debug.output.pulse')).toBe(false);
  let release!: () => void; state.getGate = new Promise<void>(resolve => { release = resolve; });
  await expect.poll(() => state.inFlight).toBe(1);
  await page.waitForTimeout(1300); expect(state.maxInFlight).toBe(1); release();
});

test('capture JSON and CSV preserve raw values and command lifecycle', async ({ page }) => {
  const state = await garden(page);
  await page.getByRole('button', { name: 'Start simulation', exact: true }).click();
  await page.getByRole('button', { name: 'Scan I²C addresses', exact: true }).click();
  await expect(page.getByRole('region', { name: 'Command trace' }).getByText('debug.i2c.scan', { exact: true }).first()).toBeVisible();
  await page.getByRole('textbox', { name: 'Capture name', exact: true }).fill('Fixture capture');
  await page.getByRole('button', { name: 'Save capture', exact: true }).click();
  await expect(page.getByText('Fixture capture', { exact: true })).toBeVisible();
  const json = page.waitForEvent('download'); await page.getByRole('button', { name: 'JSON', exact: true }).click();
  const download = await json; expect(download.suggestedFilename()).toBe('Fixture-capture.json');
  const { readFile } = await import('node:fs/promises'); const parsed = JSON.parse(await readFile((await download.path())!, 'utf8'));
  expect(parsed.snapshots[0].snapshot.zones[0].samples).toHaveLength(16);
  expect(parsed.actions.some((action: DebugAction) => action.command === 'debug.i2c.scan' && action.status === 'completed')).toBe(true);
  const csv = page.waitForEvent('download'); await page.getByRole('button', { name: 'Export live CSV', exact: true }).click();
  const content = await readFile((await (await csv).path())!, 'utf8');
  expect(content).toContain('physicalRaw,effectiveRaw,filteredRaw'); expect(content).toContain('"2100"');
  expect(state.captures).toHaveLength(1);
});

test('presets cover default startup and confirmations and clamp ADC noise at calibration edges', async ({ page }) => {
  const state = await garden(page);
  await page.getByRole('button', { name: 'Start simulation', exact: true }).click();
  await page.getByRole('button', { name: 'Dry → wet', exact: true }).click();
  const editor = page.getByRole('textbox', { name: 'Editable sequence (JSON)' });
  const dryWet = JSON.parse(await editor.inputValue()) as { parameters?: { raw: number }; waitMs?: number }[];
  // Three startup samples plus three confirmation samples at the default 5s
  // cadence must fit before the preset moves away from the dry endpoint.
  expect(dryWet[0].parameters?.raw).toBe(state.snapshot.zones[0].dryRaw);
  expect(dryWet[0].waitMs).toBeGreaterThan(6 * 5000);
  expect(dryWet[2].parameters?.raw).toBe(state.snapshot.zones[0].wetRaw);
  expect(dryWet[2].waitMs).toBeGreaterThanOrEqual(6 * 5000);
  await expect(page.getByText('Pulse and soak timers keep real time.', { exact: false })).toBeVisible();
  for (const dryRaw of [55, 4040]) {
    state.snapshot.zones[0].dryRaw = dryRaw;
    await expect.poll(async () => await page.getByRole('region', { name: 'From pin to watering decision' }).textContent()).toContain(`Dry ${dryRaw.toLocaleString('en-US')}`);
    await page.getByRole('button', { name: 'Noisy sensor', exact: true }).click();
    const noisy = JSON.parse(await editor.inputValue()) as { command: string; parameters?: { raw: number } }[];
    const inputs = noisy.filter(step => step.command === 'debug.input.set').map(step => step.parameters!.raw);
    expect(inputs).toHaveLength(5);
    expect(inputs.every(raw => Number.isInteger(raw) && raw >= 0 && raw <= 4095)).toBe(true);
    await expect(page.getByRole('button', { name: 'Run sequence', exact: true })).toBeEnabled();
  }
});

test('unconfirmed renewal remains visible after successful polls and Stop until a new confirmed session', async ({ page }) => {
  const state = await garden(page);
  await page.getByRole('button', { name: 'Start simulation', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Run pulse', exact: true })).toBeEnabled();
  state.renewUnconfirmed = true;
  const warning = page.getByRole('alert', { name: 'Test session renewal failed' });
  await expect(warning).toContainText('Lease renewal is unconfirmed', { timeout: 8000 });
  const readsAtFailure = state.reads;
  await expect.poll(() => state.reads).toBeGreaterThanOrEqual(readsAtFailure + 3);
  await expect(warning).toContainText('new test actions are disabled');
  await expect(page.getByRole('button', { name: 'Run pulse', exact: true })).toBeDisabled();
  await expect(page.getByRole('button', { name: 'Sample now', exact: true })).toBeDisabled();
  await expect(page.getByRole('button', { name: 'Stop all tests', exact: true })).toBeEnabled();
  await expect(page.getByRole('button', { name: 'End session', exact: true })).toBeEnabled();
  expect(state.snapshot.session.mode).toBe('simulation');
  expect(state.snapshot.session.leaseRemainingMs).toBeGreaterThan(10000);
  await page.getByRole('button', { name: 'Stop all tests', exact: true }).click();
  await expect(warning).toBeVisible();
  await expect(page.getByRole('button', { name: 'Run pulse', exact: true })).toBeDisabled();
  // The hook does not automatically repeat an uncertain renewal.
  await page.waitForTimeout(1500);
  expect(state.writes.filter(write => write.path.endsWith('/debug/session/renew'))).toHaveLength(1);
  state.renewUnconfirmed = false;
  await page.getByRole('button', { name: 'End session', exact: true }).click();
  await expect(warning).toHaveCount(0);
  await expect(page.getByRole('button', { name: 'Start simulation', exact: true })).toBeEnabled();
  await page.getByRole('button', { name: 'Start simulation', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Run pulse', exact: true })).toBeEnabled();
  await expect(warning).toHaveCount(0);
});
