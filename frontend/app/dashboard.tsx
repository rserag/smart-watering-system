'use client';

import { useEffect, useRef, useState } from 'react';
import { type Zone, formatTime, zoneName } from './garden-shared';
import { useGarden } from './use-garden';
import ZoneDetails from './zone-details';
import SystemView from './system-view';
import HistoryView from './history-view';

type View = 'live' | 'history' | 'system';
type Navigation = { view: View; zone: number; device: string };

function readNavigation(): Navigation {
  const query = new URLSearchParams(window.location.search);
  const view = query.get('view');
  const zone = Number(query.get('zone') ?? 1);
  return { view: view === 'history' || view === 'system' ? view : 'live', zone: Number.isInteger(zone) && zone > 0 && zone <= 16 ? zone : 1, device: query.get('device') ?? '' };
}

function zoneStatus(zone: Zone) {
  if (zone.fault) return { label: 'Fault', tone: 'danger' };
  if (!zone.sensorValid) return { label: 'Sensor issue', tone: 'warning' };
  if (zone.relayOn) return { label: 'Watering', tone: 'watering' };
  if (zone.phase === 'disabled') return { label: 'Disabled', tone: 'neutral' };
  return { label: zone.phase === 'monitoring' || zone.phase === 'idle' ? 'Monitoring' : zone.phase.replaceAll('_', ' '), tone: 'ready' };
}

export default function Dashboard() {
  const garden = useGarden();
  const [navigation, setNavigation] = useState<Navigation>({ view: 'live', zone: 1, device: '' });
  const detailsRef = useRef<HTMLDivElement>(null);
  useEffect(() => {
    const read = () => setNavigation(readNavigation());
    read(); window.addEventListener('popstate', read);
    return () => window.removeEventListener('popstate', read);
  }, []);
  const navigate = (change: Partial<Navigation>) => {
    const next = { ...navigation, ...change };
    const url = new URL(window.location.href);
    url.searchParams.set('view', next.view);
    url.searchParams.set('zone', String(next.zone));
    if (next.device) url.searchParams.set('device', next.device);
    window.history.pushState(null, '', url);
    setNavigation(next);
  };

  if (!garden.me) return <main className="garden-auth"><div className="garden-auth-mark" /><h1>Garden Watering</h1>
    {garden.error ? <><p role="alert">{garden.error}</p><button className="garden-button" onClick={() => void garden.reload()} disabled={garden.loading}>{garden.loading ? 'Connecting…' : 'Try again'}</button></> : <p role="status">Connecting to your garden…</p>}
  </main>;
  if (!garden.me.authenticated) return <main className="garden-auth"><div className="garden-auth-mark" /><p className="garden-eyebrow">Private dashboard</p><h1>Garden Watering</h1><p>Your zones, watering activity, and system status in one place.</p><a className="garden-button garden-button-primary" href="/auth/google/login">{garden.me.authMode === 'google' ? 'Continue with Google' : 'Continue to local preview'}</a></main>;

  const selected = garden.devices.find(device => device.id === navigation.device) ?? garden.devices[0];
  const zones = [...(selected?.zones ?? [])].sort((a, b) => a.id - b.id);
  const activeZone = zones.find(zone => zone.id === navigation.zone) ?? zones[0];
  const stale = !!selected && garden.now - new Date(selected.lastSeenAt).getTime() > 120000;
  const live = garden.connection === 'live' && selected?.online && !stale;
  const connectionLabel = garden.connection === 'reconnecting' ? 'Reconnecting' : garden.connection === 'connecting' ? 'Connecting' : !selected?.online ? 'Controller offline' : stale ? 'Readings delayed' : 'Live';
  const faultCount = zones.filter(zone => zone.fault || !zone.sensorValid).length;
  const disabledCount = zones.filter(zone => zone.phase === 'disabled').length;

  return <div className="garden-app">
    <aside className="garden-sidebar">
      <a href="?view=live" className="garden-brand" onClick={event => { event.preventDefault(); navigate({view:'live'}); }}><span className="garden-brand-mark" /><span>Garden Watering</span></a>
      <nav className="garden-nav" aria-label="Main navigation">{([['live', 'Overview'], ['history', 'History'], ['system', 'System']] as const).map(([view, label]) => <button key={view} type="button" aria-current={navigation.view === view ? 'page' : undefined} onClick={() => navigate({view})}><span className="garden-nav-mark" aria-hidden="true">{view === 'live' ? '◫' : view === 'history' ? '↗' : '⌘'}</span>{label}</button>)}</nav>
      <div className="garden-profile"><span className="garden-profile-avatar">{garden.me.name?.split(' ').map(part => part[0]).slice(0,2).join('') || 'U'}</span><span><strong>{garden.me.name}</strong><small>{garden.me.email}</small></span></div>
    </aside>
    <main className="garden-content">
      <header className="garden-page-header"><div><p className="garden-eyebrow">{navigation.view === 'live' ? 'Garden overview' : navigation.view === 'history' ? 'Watering activity' : 'System'}</p><h1>{navigation.view === 'live' ? 'Your garden' : navigation.view === 'history' ? 'History' : 'Controller & notifications'}</h1></div>
        <div className="garden-header-status"><span className={`garden-live ${live ? '' : 'garden-offline'}`} role="status"><span />{connectionLabel}</span>{selected && <span className="garden-caption">Last report {formatTime(selected.lastSeenAt)}</span>}</div>
      </header>
      {garden.error && <div className="garden-error" role="alert"><span>{garden.error}</span><button className="garden-button" onClick={() => void garden.reload()} disabled={garden.loading}>Retry</button></div>}
      {garden.devices.length > 1 && <label className="garden-device-picker">Controller<select value={selected?.id ?? ''} onChange={event => navigate({device:event.target.value, zone:1})}>{garden.devices.map(device => <option value={device.id} key={device.id}>{device.id}</option>)}</select></label>}
      {!selected ? <section className="garden-empty"><h2>{garden.loading ? 'Loading your garden…' : 'Waiting for the controller'}</h2><p>{garden.loading ? 'Fetching the latest readings.' : 'Your zones will appear when the controller sends its first readings.'}</p></section> : <>
        {!live && <p className="garden-connection-note" role="status">{garden.connection !== 'live' ? 'Restoring live updates. Showing the last available readings.' : 'Showing the last reported state. New readings will appear automatically.'}</p>}
        {selected.mainTankLow && <div className="garden-tank-alert" role="alert"><strong>Main tank water is low</strong><span>Watering is blocked until the tank is refilled.</span></div>}
        {navigation.view === 'live' ? <>
          <div className="garden-health-strip" aria-label="System status"><span><i className={`garden-status-dot ${live ? '' : 'is-neutral'}`} aria-hidden="true" />Controller {selected.online ? 'online' : 'offline'}</span><span>Main tank <strong>{selected.mainTankLow === null ? 'Awaiting reading' : selected.mainTankLow ? 'Low' : 'Ready'}</strong></span><span>Automatic watering <strong>{selected.automaticWateringEnabled ? 'Enabled' : 'Disabled'}</strong></span></div>
          <div className="garden-section-heading"><h2>Your zones</h2><p>{zones.length} zones{disabledCount ? ` · ${disabledCount} disabled` : ''}{faultCount ? ` · ${faultCount} need attention` : ''}</p></div>
          {zones.length ? <div className="garden-zones">{zones.map(zone => {
            const status = zoneStatus(zone);
            const hasReading = zone.sensorValid && Number.isFinite(zone.moisturePercent);
            const moisture = Math.max(0, Math.min(100, zone.moisturePercent));
            return <button type="button" key={zone.id} className={`garden-zone garden-zone-${status.tone}`} aria-pressed={activeZone?.id === zone.id} aria-label={`${zoneName(zone)}, Zone ${zone.id}, ${hasReading ? `${Math.round(zone.moisturePercent)} percent moisture` : 'no valid moisture reading'}, ${status.label}. Show details.`} onClick={() => {
              navigate({zone:zone.id});
              if (window.matchMedia('(max-width: 760px)').matches) requestAnimationFrame(() => detailsRef.current?.scrollIntoView({behavior:window.matchMedia('(prefers-reduced-motion: reduce)').matches ? 'instant' : 'smooth', block:'nearest'}));
            }}>
              <span className="garden-zone-title"><span><strong>{zoneName(zone)}</strong>{zone.name && <small>Zone {zone.id}</small>}</span><span className={`garden-status garden-status-${status.tone}`}>{status.label}</span></span>
              <span className="garden-zone-reading"><span className="garden-moisture-value">{hasReading ? Math.round(zone.moisturePercent) : '—'}{hasReading && <small>%</small>}</span><span className="garden-caption">Soil moisture</span></span>
              <span className="garden-moisture-track" aria-hidden="true"><span style={{width:hasReading ? `${moisture}%` : '0%'}} /></span>
              <span className="garden-zone-foot">{zone.lastWateredAt ? `Watered ${formatTime(zone.lastWateredAt)}` : 'No recorded watering'}<span aria-hidden="true">↗</span></span>
            </button>;
          })}</div> : <section className="garden-empty"><h2>No zone readings yet</h2><p>The controller is connected. Waiting for its first zone update.</p></section>}
          {activeZone && <div ref={detailsRef}><ZoneDetails key={`${selected.id}-${activeZone.id}`} deviceId={selected.id} zone={activeZone} onSaved={garden.updateZoneName} onHistory={() => navigate({view:'history', zone:activeZone.id})} /></div>}
        </> : navigation.view === 'history' ? <HistoryView device={selected} zoneId={activeZone?.id ?? navigation.zone} onZoneChange={zone => navigate({zone})} /> : <SystemView key={selected.id} selected={{...selected, online:!!live}} deliveries={garden.deliveries} />}
      </>}
    </main>
  </div>;
}
