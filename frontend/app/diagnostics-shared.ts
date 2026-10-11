import type { Device } from './garden-shared';

export const DIAGNOSTICS_OPERATOR = 'sahakyan.raphayel@gmail.com';
export type DebugMode = 'observe' | 'hardware' | 'simulation';
export type DebugSession = { mode: DebugMode; sessionId: string | null; owner: string | null; leaseRemainingMs: number; automationHeld: boolean };
export type DebugZone = {
  id: number; physicalRaw: number | null; raw: number | null; samples: number[]; minRaw: number | null; maxRaw: number | null;
  filteredRaw: number | null; relativeMoisturePercent: number | null; unclampedPercent: number | null; sensorValid: boolean;
  validSamples: number | null; invalidSamples: number | null; dryCount: number | null; wetCount: number | null; sampleAgeMs: number | null;
  phase: string; fault: string | null; enabled: boolean | null; paused: boolean; relayOn: boolean; virtualRelayOn: boolean;
  decisionReason: string | null; phaseElapsedMs: number | null; pulseRemainingMs: number | null; wateringOnMsThisCycle: number;
  overrideRaw: number | null; dryRaw: number | null; wetRaw: number | null; startWateringPercent: number | null;
  stopWateringPercent: number | null; pulseMs: number | null; soakMs: number | null; maxOnMs: number | null;
};
export type DebugSnapshot = {
  type: string; schemaVersion: number; deviceId: string; bootId: string | null; sequence: number | null; uptimeMs: number | null;
  configRevision: number; session: DebugSession;
  timings?: { loopDurationUs: number; maxLoopDurationUs: number; sampleDurationUs: number };
  hardware: { board: string; source?: string; relayPins: number[]; sensorPins: number[]; tankPin: number; relayActiveHigh: boolean; oled: { sda: number; scl: number; address: number; available: boolean } };
  tank: { physicalLow: boolean | null; effectiveLow: boolean | null; low: boolean | null; pendingLow: boolean | null; debounceRemainingMs: number | null; overrideLow: boolean | null };
  zones: DebugZone[];
};
export type DebugAction = {
  id: number | string; requestId: string; command: string; status: string; message?: string | null;
  createdAt: string; updatedAt?: string; recordType?: string; parameters?: Record<string, unknown>; result?: Record<string, unknown> | null;
  bootId?: string; sessionId?: string | null;
};
export type CaptureMetadata = { id: string; name: string; createdAt: string; sampleCount: number };
export type TraceSample = { snapshot: DebugSnapshot; receivedAt: string };
export type DebugCapture = CaptureMetadata & { snapshots: TraceSample[]; actions: DebugAction[] };
export type DiagnosticsResponse = { canControl: boolean; supported: boolean; online?: boolean; stale?: boolean; snapshot: DebugSnapshot | null; receivedAt: string | null; actions: DebugAction[]; captures: CaptureMetadata[] };
export type DebugReply = { requestId: string; command: string; status: 'accepted' | 'unconfirmed'; message?: string; sessionId?: string; snapshot?: DebugSnapshot; receivedAt?: string };
export type DebugCommand = 'debug.output.pulse' | 'debug.output.stop' | 'debug.input.set' | 'debug.input.clear' | 'debug.sample' | 'debug.oled.test' | 'debug.i2c.scan' | 'debug.automation.resume';
export type ScenarioStep = { command: DebugCommand | 'wait'; parameters?: Record<string, unknown>; waitMs?: number };

export const commandExamples: Record<DebugCommand, Record<string, unknown>> = {
  'debug.output.pulse': { zoneId: 1, durationMs: 1000 },
  'debug.output.stop': {},
  'debug.input.set': { input: 'soil', zoneId: 1, raw: 2600 },
  'debug.input.clear': { input: 'all' },
  'debug.sample': {},
  'debug.oled.test': { pattern: 'checkerboard', durationMs: 3000 },
  'debug.i2c.scan': {},
  'debug.automation.resume': {},
};

export function validateCommand(command: DebugCommand, parameters: Record<string, unknown>, mode: DebugMode): string | null {
  const integer = (value: unknown, min: number, max: number) => typeof value === 'number' && Number.isInteger(value) && value >= min && value <= max;
  const zone = () => integer(parameters.zoneId, 1, 4);
  let allowed: string[] = [];
  let error: string | null = null;
  switch (command) {
    case 'debug.output.pulse': allowed = ['zoneId', 'durationMs']; if (!zone() || !integer(parameters.durationMs, 100, 60000)) error = 'Choose zone 1–4 and an integer duration of 100–60000 ms.'; break;
    case 'debug.output.stop': allowed = ['zoneId']; if (parameters.zoneId !== undefined && !zone()) error = 'Zone must be 1–4, or omit it to stop all outputs.'; break;
    case 'debug.input.set':
      allowed = parameters.input === 'soil' ? ['input', 'zoneId', 'raw'] : ['input', 'low'];
      if (mode !== 'simulation') error = 'Input injection requires a simulation session.';
      else if (parameters.input === 'soil' ? !zone() || !integer(parameters.raw, 0, 4095) : parameters.input !== 'tank' || typeof parameters.low !== 'boolean') error = 'Use soil with zone 1–4 and raw 0–4095, or tank with a boolean low value.';
      break;
    case 'debug.input.clear': allowed = ['input', 'zoneId']; if (mode !== 'simulation') error = 'Releasing injected inputs requires a simulation session.'; else if (!['soil', 'tank', 'all'].includes(String(parameters.input)) || (parameters.zoneId !== undefined && (parameters.input !== 'soil' || !zone()))) error = 'Choose soil, tank, or all; an optional zone 1–4 is only valid for soil.'; break;
    case 'debug.oled.test': allowed = ['pattern', 'durationMs']; if (!['checkerboard', 'text', 'off'].includes(String(parameters.pattern)) || !integer(parameters.durationMs, 100, 10000)) error = 'Choose checkerboard, text, or off and an integer duration of 100–10000 ms.'; break;
    case 'debug.sample': case 'debug.i2c.scan': case 'debug.automation.resume': break;
    default: return 'This command is not supported.';
  }
  if (Object.keys(parameters).some(key => !allowed.includes(key))) return 'Remove unsupported parameters from this command.';
  return error;
}

export function parseScenario(text: string, mode: DebugMode): { steps: ScenarioStep[]; error: string | null } {
  try {
    const steps = JSON.parse(text);
    if (!Array.isArray(steps) || !steps.length || steps.length > 60) throw new Error('Use a JSON array with 1–60 steps.');
    let duration = 0;
    for (const step of steps) {
      if (!step || typeof step !== 'object' || Array.isArray(step) || Object.keys(step).some(key => !['command', 'parameters', 'waitMs'].includes(key))) throw new Error('Each step needs command, optional parameters and optional waitMs.');
      if (step.waitMs !== undefined && (!Number.isInteger(step.waitMs) || step.waitMs < 0 || step.waitMs > 60000)) throw new Error('Each waitMs must be 0–60000 ms.');
      duration += step.waitMs ?? 0;
      if (step.command === 'wait') { if (!step.waitMs || step.parameters !== undefined) throw new Error('A wait step needs waitMs and no parameters.'); continue; }
      if (!(step.command in commandExamples) || step.command === 'debug.automation.resume') throw new Error('Choose a supported test command; resume automation separately.');
      const parameters = step.parameters ?? {};
      if (!parameters || typeof parameters !== 'object' || Array.isArray(parameters)) throw new Error('Parameters must be a JSON object.');
      const error = validateCommand(step.command, parameters, mode);
      if (error) throw new Error(error);
    }
    if (duration > 600000) throw new Error('A sequence may wait for at most 10 minutes in total.');
    return { steps, error: null };
  } catch (reason) { return { steps: [], error: reason instanceof Error ? reason.message : 'Invalid sequence.' }; }
}

export function scenarioPreset(preset: string, zoneId: number, zone?: DebugZone): ScenarioStep[] {
  const dry = zone?.dryRaw ?? 2600, wet = zone?.wetRaw ?? 1250;
  const set = (raw: number, waitMs = 5500): ScenarioStep => ({ command: 'debug.input.set', parameters: { input: 'soil', zoneId, raw: Math.max(0, Math.min(4095, Math.round(raw))) }, waitMs });
  const clear: ScenarioStep = { command: 'debug.input.clear', parameters: { input: 'all' } };
  switch (preset) {
    case 'noisy': return [set(dry), set(dry - 150), set(dry + 150), set(dry - 90), set(dry), clear];
    case 'invalid': return [set(0, 16000), set(dry, 16000), clear];
    case 'tank': return [{ command: 'debug.input.set', parameters: { input: 'tank', low: true }, waitMs: 1000 }, { command: 'debug.input.set', parameters: { input: 'tank', low: false }, waitMs: 2500 }, clear];
    case 'pulse': return [{ command: 'debug.output.pulse', parameters: { zoneId, durationMs: 1000 }, waitMs: 1200 }, { command: 'wait', waitMs: 3000 }, { command: 'debug.output.pulse', parameters: { zoneId, durationMs: 1000 }, waitMs: 1200 }];
    // At the default 5-second cadence, 35 seconds covers three startup samples,
    // three dry confirmations and a sample margin; wet readings also need time
    // to settle through the filter. Output and soak timers remain real time.
    default: return [set(dry, 35000), set(Math.round((dry + wet) / 2), 5500), set(wet, 35000), clear];
  }
}

export async function diagnosticsFetch<T>(deviceId: string, suffix: string, body?: unknown, signal?: AbortSignal, keepalive = false): Promise<T> {
  const response = await fetch(`/api/devices/${encodeURIComponent(deviceId)}/${suffix}`, { credentials: 'include', method: body === undefined ? 'GET' : 'POST', ...(body === undefined ? {} : { headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) }), signal, keepalive });
  const data = await response.json() as { detail?: string | { message?: string; status?: string } };
  if (!response.ok) {
    const detail = data.detail;
    const message = typeof detail === 'string' ? detail : detail?.message;
    throw new Error(response.status === 401 ? 'Your session has expired. Please sign in again.' : `${message ?? `Request failed (${response.status}).`}${typeof detail !== 'string' && detail?.status === 'unconfirmed' ? ' Execution is unconfirmed. Inspect current state and the command trace before sending another action.' : ''}`);
  }
  return data as T;
}

export function formatValue(value: number | null | undefined, unit = '') { return value == null || !Number.isFinite(value) ? 'Unavailable' : `${new Intl.NumberFormat(undefined, { maximumFractionDigits: 1 }).format(value)}${unit}`; }
export function zoneLabel(device: Device, id: number) { return device.zones.find(zone => zone.id === id)?.name || `Zone ${id}`; }

export function downloadCapture(capture: DebugCapture, format: 'json' | 'csv') {
  let content: string;
  if (format === 'json') content = JSON.stringify(capture, null, 2);
  else {
    const columns = ['receivedAt', 'bootId', 'sequence', 'configRevision', 'mode', 'zoneId', 'physicalRaw', 'effectiveRaw', 'filteredRaw', 'relativeMoisturePercent', 'sensorValid', 'overrideRaw', 'phase', 'decisionReason', 'relayOn', 'virtualRelayOn', 'physicalTankLow', 'effectiveTankLow'];
    const cell = (value: unknown) => `"${String(value ?? '').replaceAll('"', '""')}"`;
    content = [columns.join(','), ...capture.snapshots.flatMap(({ snapshot: s, receivedAt }) => s.zones.map(z => [receivedAt, s.bootId, s.sequence, s.configRevision, s.session.mode, z.id, z.physicalRaw, z.raw, z.filteredRaw, z.relativeMoisturePercent, z.sensorValid, z.overrideRaw, z.phase, z.decisionReason, z.relayOn, z.virtualRelayOn, s.tank.physicalLow, s.tank.effectiveLow].map(cell).join(',')))].join('\n');
  }
  const url = URL.createObjectURL(new Blob([content], { type: format === 'json' ? 'application/json' : 'text/csv;charset=utf-8' }));
  const anchor = document.createElement('a'); anchor.href = url; anchor.download = `${capture.name.replace(/[^a-zA-Z0-9_-]/g, '-').slice(0, 70) || 'diagnostics'}.${format}`; anchor.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
