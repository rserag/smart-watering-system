'use client';

import { useCallback, useEffect, useMemo, useState } from 'react';

type Me = { authenticated: boolean; authMode: 'development' | 'google'; email?: string; name?: string };
type Zone = {
  id: number; raw: number; filteredRaw: number; moisturePercent: number; sensorValid: boolean;
  phase: string; relayOn: boolean; wateringOnMsThisCycle: number; fault: string | null; lastWateredAt: string | null;
};
type Device = {
  id: string; online: boolean; firmwareVersion: string | null; bootId: string | null; schemaVersion: number;
  configRevision: number; automaticWateringEnabled: boolean; lastSeenAt: string; wifiRssi: number | null;
  mainTankLow: boolean | null; mainTankLastChangedAt: string | null; zones: Zone[];
  directTelegram: boolean; telegramDebugEnabled: boolean; telegramConfigured: boolean;
  telegramPendingMessages: number; telegramLastSendSucceeded: boolean;
  telegramWorkerRunning: boolean; telegramTimeReady: boolean; telegramLastFailureStage: string | null;
};
type HistoryPoint = { timestamp: string; average: number; minimum: number; maximum: number; samples: number };
type WateringEvent = { id: number; zoneId: number; startedAt: string; endedAt: string | null; durationMs: number | null; source: string; status: string };
type TelegramDelivery = {
  eventId: string; deviceId: string; requestId: string | null; kind: string; status: string;
  updateSequence: number; attempt: number; pendingCount: number; httpStatus: number | null;
  errorStage: string | null; telegramErrorCode: number | null; telegramMessageId: number | null;
  updatedAt: string; sentAt: string | null;
};

function telegramKind(value: string) {
  return ({ manual_debug: 'Manual debug', hourly_debug: 'Hourly debug', pump_started: 'Pump started', tank_low: 'Tank low', tank_restored: 'Tank restored' } as Record<string, string>)[value] ?? value;
}

function formatTime(value: string | null) {
  if (!value) return '—';
  return new Intl.DateTimeFormat(undefined, { month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' }).format(new Date(value));
}

function formatDuration(value: number | null) {
  if (value === null) return 'In progress';
  const totalSeconds = Math.round(value / 1000);
  return `${Math.floor(totalSeconds / 60)}m ${totalSeconds % 60}s`;
}

function historyRange(period: string) {
  const to = new Date();
  const hours = period === '7d' ? 24 * 7 : period === '30d' ? 24 * 30 : 24;
  return { from: new Date(to.getTime() - hours * 3600_000), to, bucket: period === '30d' ? 7200 : period === '7d' ? 1800 : 300 };
}

function HistoryChart({ points, events, range, metric }: { points: HistoryPoint[]; events: WateringEvent[]; range: ReturnType<typeof historyRange>; metric: string }) {
  const chart = useMemo(() => {
    const width = 800, height = 260, left = 52, right = 16, top = 18, bottom = 34;
    const values = points.map((point) => point.average);
    const extent = values.length ? [Math.min(...values), Math.max(...values)] : [0, 100];
    const baseMin = metric === 'moisturePercent' ? Math.min(0, extent[0]) : extent[0];
    const baseMax = metric === 'moisturePercent' ? Math.max(100, extent[1]) : extent[1];
    const padding = Math.max((baseMax - baseMin) * 0.08, 1);
    const min = baseMin - (metric === 'moisturePercent' ? 0 : padding);
    const max = baseMax + (metric === 'moisturePercent' ? 0 : padding);
    const x = (time: number) => left + ((time - range.from.getTime()) / (range.to.getTime() - range.from.getTime())) * (width - left - right);
    const y = (value: number) => top + ((max - value) / Math.max(max - min, 1)) * (height - top - bottom);
    const path = points.map((point, index) => `${index ? 'L' : 'M'} ${x(new Date(point.timestamp).getTime()).toFixed(1)} ${y(point.average).toFixed(1)}`).join(' ');
    return { width, height, left, right, top, bottom, min, max, x, y, path };
  }, [points, range, metric]);

  if (!points.length) return <div className="garden-chart-empty">No readings in this period yet.</div>;
  const ticks = [0, 0.25, 0.5, 0.75, 1];
  return (
    <svg className="garden-history-chart" viewBox={`0 0 ${chart.width} ${chart.height}`} role="img" aria-label="Historical watering sensor chart">
      {ticks.map((tick) => {
        const y = chart.top + tick * (chart.height - chart.top - chart.bottom);
        const value = chart.max - tick * (chart.max - chart.min);
        return <g key={tick}><line x1={chart.left} x2={chart.width - chart.right} y1={y} y2={y} /><text x={8} y={y + 4}>{Math.round(value)}</text></g>;
      })}
      {events.map((event) => {
        const start = chart.x(new Date(event.startedAt).getTime());
        const end = chart.x(new Date(event.endedAt ?? range.to).getTime());
        return <rect className="garden-event-band" key={event.id} x={start} y={chart.top} width={Math.max(end - start, 3)} height={chart.height - chart.top - chart.bottom} />;
      })}
      {metric === 'moisturePercent' && <line className="garden-threshold" x1={chart.left} x2={chart.width - chart.right} y1={chart.y(30)} y2={chart.y(30)} />}
      <path className="garden-history-line" d={chart.path} />
      <text className="garden-axis-label" x={chart.left} y={chart.height - 8}>{range.from.toLocaleDateString()}</text>
      <text className="garden-axis-label" textAnchor="end" x={chart.width - chart.right} y={chart.height - 8}>{range.to.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}</text>
    </svg>
  );
}

export default function Dashboard() {
  const [me, setMe] = useState<Me | null>(null);
  const [devices, setDevices] = useState<Device[]>([]);
  const [selectedId, setSelectedId] = useState('');
  const [view, setView] = useState<'live' | 'history'>('live');
  const [period, setPeriod] = useState('24h');
  const [zoneId, setZoneId] = useState(1);
  const [metric, setMetric] = useState('moisturePercent');
  const [history, setHistory] = useState<HistoryPoint[]>([]);
  const [events, setEvents] = useState<WateringEvent[]>([]);
  const [error, setError] = useState('');
  const [loadingHistory, setLoadingHistory] = useState(false);
  const [savingTelegram, setSavingTelegram] = useState(false);
  const [sendingTelegramDebug, setSendingTelegramDebug] = useState(false);
  const [telegramDeliveries, setTelegramDeliveries] = useState<TelegramDelivery[]>([]);

  const fetchJson = useCallback(async <T,>(path: string, init?: RequestInit): Promise<T> => {
    const response = await fetch(path, { ...init, credentials: 'include' });
    if (!response.ok) {
      let detail = '';
      try { detail = String(((await response.json()) as { detail?: unknown }).detail ?? ''); } catch { /* Response was not JSON. */ }
      throw new Error(response.status === 401 ? 'Authentication required' : detail || `Request failed (${response.status})`);
    }
    return response.json() as Promise<T>;
  }, []);

  const loadDevices = useCallback(async () => {
    const data = await fetchJson<Device[]>('/api/devices');
    setDevices(data);
    setSelectedId((current) => current || data[0]?.id || '');
  }, [fetchJson]);

  useEffect(() => {
    fetchJson<Me>('/api/me').then((identity) => {
      setMe(identity);
      if (identity.authenticated) return loadDevices();
    }).catch(() => setError('The dashboard cannot reach the local server. Trust its local CA and confirm the backend is running.'));
  }, [fetchJson, loadDevices]);

  useEffect(() => {
    if (!me?.authenticated) return;
    const websocketUrl = new URL(window.location.origin);
    websocketUrl.protocol = websocketUrl.protocol === 'https:' ? 'wss:' : 'ws:';
    websocketUrl.pathname = '/ws/dashboard';
    const socket = new WebSocket(websocketUrl);
    socket.onmessage = (event) => {
      const message = JSON.parse(event.data);
      if (message.type === 'snapshot') {
        setDevices(message.devices);
        setSelectedId((current) => current || message.devices[0]?.id || '');
      } else if (message.type === 'telegram.delivery' && message.delivery) {
        setTelegramDeliveries((current) => {
          const next = current.filter((delivery) => delivery.eventId !== message.delivery.eventId);
          return [message.delivery, ...next].sort((a, b) => new Date(b.updatedAt).getTime() - new Date(a.updatedAt).getTime()).slice(0, 50);
        });
      } else if (message.device) {
        setDevices((current) => {
          const next = current.filter((device) => device.id !== message.device.id);
          return [...next, message.device].sort((a, b) => a.id.localeCompare(b.id));
        });
      }
    };
    socket.onerror = () => setError('Live updates are temporarily unavailable; historical data remains accessible.');
    return () => socket.close();
  }, [me?.authenticated]);

  const range = useMemo(() => historyRange(period), [period]);
  const selected = devices.find((device) => device.id === selectedId) ?? devices[0];
  const historyDeviceId = selected?.id;

  const setTelegramDebug = useCallback(async (enabled: boolean) => {
    if (!selected) return;
    setSavingTelegram(true);
    try {
      await fetchJson<{ commandId: string; status: string }>(`/api/devices/${encodeURIComponent(selected.id)}/commands`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ command: 'telegram.debug.set', parameters: { enabled } }),
      });
      setDevices((current) => current.map((device) => device.id === selected.id ? { ...device, telegramDebugEnabled: enabled } : device));
      setError('');
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : 'Unable to update Telegram debug');
    } finally {
      setSavingTelegram(false);
    }
  }, [selected, fetchJson]);

  const sendTelegramDebug = useCallback(async () => {
    if (!selected) return;
    setSendingTelegramDebug(true);
    try {
      await fetchJson<{ commandId: string; status: string }>(`/api/devices/${encodeURIComponent(selected.id)}/telegram/debug`, { method: 'POST' });
      setError('');
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : 'Unable to send Telegram debug report');
    } finally {
      setSendingTelegramDebug(false);
    }
  }, [selected, fetchJson]);

  useEffect(() => {
    if (!me?.authenticated || !historyDeviceId) return;
    let active = true;
    fetchJson<TelegramDelivery[]>(`/api/devices/${encodeURIComponent(historyDeviceId)}/telegram/deliveries?limit=50`)
      .then((data) => { if (active) setTelegramDeliveries(data); })
      .catch((reason) => { if (active) setError(reason instanceof Error ? reason.message : 'Unable to load Telegram history'); });
    return () => { active = false; };
  }, [me?.authenticated, historyDeviceId, fetchJson]);

  useEffect(() => {
    if (view !== 'history' || !historyDeviceId) return;
    let active = true;
    async function loadHistory() {
      setLoadingHistory(true);
      const query = new URLSearchParams({ from: range.from.toISOString(), to: range.to.toISOString(), zone_id: String(zoneId), metric, bucket_seconds: String(range.bucket) });
      const eventQuery = new URLSearchParams({ from: range.from.toISOString(), to: range.to.toISOString(), zone_id: String(zoneId) });
      try {
        const [historyResult, eventResult] = await Promise.all([
          fetchJson<{ points: HistoryPoint[] }>(`/api/devices/${encodeURIComponent(historyDeviceId)}/history?${query}`),
          fetchJson<WateringEvent[]>(`/api/devices/${encodeURIComponent(historyDeviceId)}/events?${eventQuery}`),
        ]);
        if (active) { setHistory(historyResult.points); setEvents(eventResult); setError(''); }
      } catch (reason) {
        if (active) setError(reason instanceof Error ? reason.message : 'Unable to load history');
      } finally {
        if (active) setLoadingHistory(false);
      }
    }
    void loadHistory();
    return () => { active = false; };
  }, [view, historyDeviceId, zoneId, metric, period, range, fetchJson]);

  if (me === null) return <main className="garden-auth"><div className="garden-auth-mark" /><h1>Garden Watering</h1><p>Connecting to the local control plane…</p></main>;
  if (!me.authenticated) return (
    <main className="garden-auth"><div className="garden-auth-mark" /><p className="garden-eyebrow">Private dashboard</p><h1>Garden Watering</h1><p>Review live conditions, watering activity, and historical sensor data.</p>
      <button onClick={() => { window.location.href = '/auth/google/login'; }}><span className="garden-google">G</span>{me.authMode === 'google' ? 'Continue with Google' : 'Continue to local preview'}</button>
    </main>
  );

  const zones = selected?.zones ?? [];
  const displayZones = Array.from({ length: 4 }, (_, index) => zones.find((zone) => zone.id === index + 1));
  const exportQuery = new URLSearchParams({ from: range.from.toISOString(), to: range.to.toISOString(), zone_id: String(zoneId) });
  const selectedTelegramDeliveries = telegramDeliveries.filter((delivery) => delivery.deviceId === selected?.id);
  const lastTelegramDelivery = selectedTelegramDeliveries[0];
  const lastTelegramSuccess = selectedTelegramDeliveries.find((delivery) => delivery.status === 'sent');

  return (
    <main className="garden-app">
      <aside className="garden-sidebar">
        <div className="garden-brand"><span className="garden-brand-mark" /><span>Garden Watering</span></div>
        <nav className="garden-nav"><button className={view === 'live' ? 'garden-nav-active' : ''} onClick={() => setView('live')}>Live overview</button><button className={view === 'history' ? 'garden-nav-active' : ''} onClick={() => setView('history')}>History</button></nav>
        <div className="garden-profile"><span className="garden-profile-avatar">{me.name?.split(' ').map((part) => part[0]).slice(0, 2).join('') || 'U'}</span><span><strong>{me.name}</strong><small>{me.email}</small></span></div>
      </aside>

      <section className="garden-content">
        {error && <div className="garden-error" role="status">{error}<button onClick={() => setError('')} aria-label="Dismiss">×</button></div>}
        {!selected ? (
          <div className="garden-empty"><span className="garden-empty-pulse" /><h1>Waiting for the controller</h1><p>The dashboard is ready. Configure the ESP32 to connect to <code>/ws/device</code>; its first telemetry message will appear here automatically.</p></div>
        ) : view === 'live' ? (
          <>
            <header className="garden-page-header"><div><p className="garden-eyebrow">Watering system</p><h1>Garden status</h1><p>Configuration revision {selected.configRevision} · Firmware {selected.firmwareVersion}</p></div><div className={selected.online ? 'garden-live' : 'garden-live garden-offline'}><span />{selected.online ? 'Live' : `Last seen ${formatTime(selected.lastSeenAt)}`}</div></header>
            {selected.mainTankLow && <div className="garden-tank-alert" role="alert"><strong>Main tank water is low</strong><span>All watering is stopped until the tank is refilled.</span></div>}
            <div className="garden-stats"><article><p>Controller</p><strong>{selected.online ? 'Online' : 'Offline'}</strong><small>{selected.id}</small></article><article className={selected.mainTankLow ? 'garden-stat-danger' : ''}><p>Main tank</p><strong>{selected.mainTankLow === null ? 'Waiting' : selected.mainTankLow ? 'Low' : 'Ready'}</strong><small>{selected.mainTankLow === null ? 'No level reading yet' : `Changed ${formatTime(selected.mainTankLastChangedAt)}`}</small></article><article><p>Automatic watering</p><strong>{selected.automaticWateringEnabled ? 'Enabled' : 'Disabled'}</strong><small>{selected.mainTankLow ? 'Blocked by tank safety' : 'Local safety remains active'}</small></article><article><p>Wi-Fi signal</p><strong>{selected.wifiRssi === null ? '—' : `${selected.wifiRssi} dBm`}</strong><small>{selected.wifiRssi !== null && selected.wifiRssi > -67 ? 'Good connection' : 'Check signal quality'}</small></article></div>
            <section className="garden-notifications" aria-labelledby="telegram-heading">
              <div className="garden-telegram-heading"><div><p className="garden-eyebrow">Notifications</p><h2 id="telegram-heading">Telegram</h2><p>The ESP32 sends directly. Hourly and pump messages follow the debug toggle; low-tank alerts are always enabled.</p></div><div className="garden-telegram-actions"><div className="garden-telegram-control"><button type="button" role="switch" aria-checked={selected.telegramDebugEnabled} className={selected.telegramDebugEnabled ? 'garden-switch garden-switch-on' : 'garden-switch'} disabled={!selected.online || !selected.directTelegram || savingTelegram} onClick={() => void setTelegramDebug(!selected.telegramDebugEnabled)}><span /></button><strong>{savingTelegram ? 'Saving…' : selected.telegramDebugEnabled ? 'Debug on' : 'Debug off'}</strong></div><button className="garden-debug-send" disabled={!selected.online || !selected.directTelegram || !selected.telegramConfigured || !selected.telegramWorkerRunning || sendingTelegramDebug} onClick={() => void sendTelegramDebug()}>{sendingTelegramDebug ? 'Queuing…' : 'Send debug now'}</button></div></div>
              <dl className="garden-telegram-health"><div><dt>Controller</dt><dd>{!selected.directTelegram ? 'Firmware update required' : selected.online ? 'Online' : 'Offline'}</dd></div><div><dt>Bot</dt><dd>{selected.telegramConfigured ? 'Configured' : 'Missing credentials'}</dd></div><div><dt>Sender</dt><dd>{selected.telegramWorkerRunning ? 'Running' : 'Stopped'}</dd></div><div><dt>Secure clock</dt><dd>{selected.telegramTimeReady ? 'Ready' : 'Synchronizing'}</dd></div><div><dt>Queue</dt><dd>{selected.telegramPendingMessages} pending</dd></div><div><dt>Last success</dt><dd>{lastTelegramSuccess ? formatTime(lastTelegramSuccess.sentAt) : 'None yet'}</dd></div><div><dt>Last issue</dt><dd>{selected.telegramLastFailureStage ?? 'None reported'}</dd></div></dl>
              <div className="garden-telegram-history"><div className="garden-telegram-history-head"><strong>Recent interactions</strong><span>{lastTelegramDelivery ? `Latest: ${lastTelegramDelivery.status}` : 'No reports from the controller yet'}</span></div>{selectedTelegramDeliveries.length ? selectedTelegramDeliveries.slice(0, 8).map((delivery) => <div className="garden-telegram-row" key={delivery.eventId}><span>{formatTime(delivery.updatedAt)}</span><strong>{telegramKind(delivery.kind)}</strong><span className={`garden-delivery-status garden-delivery-${delivery.status}`}>{delivery.status.replace('_', ' ')}</span><span>{delivery.attempt} attempt{delivery.attempt === 1 ? '' : 's'}</span><small>{delivery.status === 'sent' ? `Message ${delivery.telegramMessageId ?? 'sent'}` : delivery.errorStage ? `${delivery.errorStage}${delivery.telegramErrorCode ? ` (${delivery.telegramErrorCode})` : ''}` : delivery.httpStatus ? `HTTP ${delivery.httpStatus}` : 'Waiting on controller'}</small></div>) : <div className="garden-telegram-empty">Use “Send debug now” to test the complete ESP32-to-Telegram path.</div>}</div>
            </section>
            <div className="garden-section-heading"><div><h2>Zones</h2><p>Live conditions and controller state</p></div></div>
            <div className="garden-zones">{displayZones.map((zone, index) => (
              <article className="garden-zone" key={index}><div className="garden-zone-title"><h3>Zone {index + 1}{index === 0 ? ' · Tomatoes' : ''}</h3><span className={zone?.relayOn ? 'garden-status-watering' : zone ? 'garden-status-on' : 'garden-status-off'}>{zone?.relayOn ? 'Watering' : zone?.phase ?? 'No data'}</span></div><div className="garden-zone-body"><div className={zone ? 'garden-moisture garden-moisture-active' : 'garden-moisture'} style={zone ? { background: `conic-gradient(#50b56a 0 ${Math.max(0, Math.min(100, zone.moisturePercent))}%, #eef1ec ${Math.max(0, Math.min(100, zone.moisturePercent))}% 100%)` } : undefined}><strong>{zone ? `${Math.round(zone.moisturePercent)}%` : '—'}</strong><small>moisture</small></div><dl><div><dt>Sensor</dt><dd>{zone ? (zone.sensorValid ? 'Healthy' : 'Invalid') : 'Waiting'}</dd></div><div><dt>Relay</dt><dd>{zone?.relayOn ? 'On' : 'Off'}</dd></div><div><dt>Last watered</dt><dd>{formatTime(zone?.lastWateredAt ?? null)}</dd></div><div><dt>Fault</dt><dd>{zone?.fault ?? 'None'}</dd></div></dl></div></article>
            ))}</div>
          </>
        ) : (
          <>
            <header className="garden-page-header"><div><p className="garden-eyebrow">Analysis</p><h1>Historical data</h1><p>Explore sensor trends and completed watering events</p></div></header>
            <div className="garden-history-toolbar"><label>Device<select value={selected.id} onChange={(event) => setSelectedId(event.target.value)}>{devices.map((device) => <option key={device.id} value={device.id}>{device.id}</option>)}</select></label><label>Zone<select value={zoneId} onChange={(event) => setZoneId(Number(event.target.value))}>{[1, 2, 3, 4].map((id) => <option key={id} value={id}>Zone {id}</option>)}</select></label><label>Metric<select value={metric} onChange={(event) => setMetric(event.target.value)}><option value="moisturePercent">Moisture percentage</option><option value="filteredRaw">Filtered sensor</option><option value="raw">Raw sensor</option><option value="wateringOnMs">Cycle watering time</option></select></label><label>Period<select value={period} onChange={(event) => setPeriod(event.target.value)}><option value="24h">Last 24 hours</option><option value="7d">Last 7 days</option><option value="30d">Last 30 days</option></select></label><button className="garden-export" onClick={() => { window.location.href = `/api/devices/${encodeURIComponent(selected.id)}/history.csv?${exportQuery}`; }}>Export CSV</button></div>
            <section className="garden-chart-panel"><div className="garden-chart-heading"><h2>{metric === 'moisturePercent' ? 'Moisture trend' : 'Sensor trend'}</h2><span>{loadingHistory ? 'Loading…' : `${history.length} time buckets`}</span></div><HistoryChart points={history} events={events} range={range} metric={metric} /></section>
            <section className="garden-events"><div className="garden-event-header"><span>Time</span><span>Event</span><span>Duration</span></div>{events.length ? events.map((event) => <div className="garden-event-row" key={event.id}><span>{formatTime(event.startedAt)}</span><span>Zone {event.zoneId} · {event.source} watering · {event.status}</span><strong>{formatDuration(event.durationMs)}</strong></div>) : <div className="garden-events-empty">No watering events in this period.</div>}</section>
          </>
        )}
      </section>
    </main>
  );
}
