export type Me = { authenticated: boolean; authMode: 'development' | 'google'; email?: string; name?: string };
export type Zone = {
  id: number; name?: string | null; raw: number; filteredRaw: number; moisturePercent: number; sensorValid: boolean;
  phase: string; relayOn: boolean; wateringOnMsThisCycle: number; fault: string | null; lastWateredAt: string | null;
};
export type Device = {
  id: string; online: boolean; firmwareVersion: string | null; bootId: string | null; schemaVersion: number;
  configRevision: number; automaticWateringEnabled: boolean; lastSeenAt: string; wifiRssi: number | null;
  mainTankLow: boolean | null; mainTankLastChangedAt: string | null; zones: Zone[];
  directTelegram: boolean; telegramDebugEnabled: boolean; telegramConfigured: boolean;
  telegramPendingMessages: number; telegramLastSendSucceeded: boolean;
  telegramWorkerRunning: boolean; telegramTimeReady: boolean; telegramLastFailureStage: string | null;
};
export type HistoryPoint = { timestamp: string; average: number; minimum: number; maximum: number; samples: number };
export type WateringEvent = { id: number; zoneId: number; startedAt: string; endedAt: string | null; durationMs: number | null; source: string; status: string };
export type TelegramDelivery = {
  eventId: string; deviceId: string; requestId: string | null; kind: string; status: string;
  updateSequence: number; attempt: number; pendingCount: number; httpStatus: number | null;
  errorStage: string | null; telegramErrorCode: number | null; telegramMessageId: number | null;
  updatedAt: string; sentAt: string | null;
};

export function telegramKind(value: string) {
  return ({ manual_debug: 'Manual debug', hourly_debug: 'Hourly debug', pump_started: 'Pump started', tank_low: 'Tank low', tank_restored: 'Tank restored' } as Record<string, string>)[value] ?? value;
}

export function telegramIssue(value: string) {
  return ({ controller_restart: 'Controller restarted' } as Record<string, string>)[value] ?? value;
}

export function supportsManualTelegramDebug(value: string | null) {
  if (!value) return false;
  const parts = value.split('.').map(Number);
  if (parts.some((part) => !Number.isInteger(part) || part < 0)) return false;
  const [major = 0, minor = 0, patch = 0] = parts;
  return major > 0 || minor > 5 || (minor === 5 && patch >= 1);
}

export function formatTime(value: string | null) {
  if (!value) return '—';
  return new Intl.DateTimeFormat(undefined, { month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' }).format(new Date(value));
}

export function formatDuration(value: number | null) {
  if (value === null) return 'In progress';
  const totalSeconds = Math.round(value / 1000);
  return `${Math.floor(totalSeconds / 60)}m ${totalSeconds % 60}s`;
}

export function historyRange(period: string) {
  const to = new Date();
  const hours = period === '7d' ? 24 * 7 : period === '30d' ? 24 * 30 : 24;
  return { from: new Date(to.getTime() - hours * 3600_000), to, bucket: period === '30d' ? 7200 : period === '7d' ? 1800 : 300 };
}


export function zoneName(zone: Pick<Zone, 'id' | 'name'>) {
  return zone.name?.trim() || `Zone ${zone.id}`;
}

export async function fetchJson<T>(path: string, init?: RequestInit): Promise<T> {
  const response = await fetch(path, { ...init, credentials: 'include' });
  if (!response.ok) {
    let detail: unknown;
    try { detail = ((await response.json()) as { detail?: unknown }).detail; } catch { /* Use the status below. */ }
    throw new Error(response.status === 401 ? 'Your session has expired. Please sign in again.' : typeof detail === 'string' ? detail : `Unable to complete the request (${response.status}). Please try again.`);
  }
  return response.json() as Promise<T>;
}
