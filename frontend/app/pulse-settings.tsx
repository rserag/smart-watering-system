'use client';

import { useRef, useState } from 'react';
import { fetchJson } from './garden-shared';

type Pulse = { zoneId: number; revision: number; pulseOnMs: number; soakMs: number; maxPulseOnMs: number };

export default function PulseSettings({ deviceId, zoneId, online }: { deviceId: string; zoneId: number; online: boolean }) {
  const [editing, setEditing] = useState(false);
  const [settings, setSettings] = useState<Pulse | null>(null);
  const [seconds, setSeconds] = useState('');
  const [busy, setBusy] = useState<'loading' | 'saving' | null>(null);
  const [error, setError] = useState('');
  const [mustReload, setMustReload] = useState(false);
  const [notice, setNotice] = useState('');
  const editButton = useRef<HTMLButtonElement>(null);
  const input = useRef<HTMLInputElement>(null);
  const path = `devices/${encodeURIComponent(deviceId)}/zones/${zoneId}/pulse`;
  const duration = Number(seconds);
  const pulseOnMs = Math.round(duration * 1000);
  const valid = seconds.trim() !== '' && Number.isFinite(duration) && duration >= 1 && !!settings && duration <= settings.maxPulseOnMs / 1000;

  function close() {
    setEditing(false); setError('');
    requestAnimationFrame(() => editButton.current?.focus());
  }

  async function load() {
    setEditing(true); setBusy('loading'); setError(''); setNotice(''); setMustReload(true);
    try {
      const result = await fetchJson<Pulse>(path);
      setSettings(result); setSeconds(String(result.pulseOnMs / 1000)); setMustReload(false);
      requestAnimationFrame(() => input.current?.focus());
    } catch (reason) { setError(reason instanceof Error ? reason.message : 'Unable to read controller settings.'); }
    finally { setBusy(null); }
  }

  async function save(event: React.FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!valid || !settings || !online || busy || mustReload) return;
    setBusy('saving'); setError(''); setNotice('');
    try {
      const result = await fetchJson<Pulse>(path, {
        method: 'PATCH', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ pulseOnMs, expectedRevision: settings.revision }),
      });
      setSettings(result);
      setNotice(`Pulse saved on controller: ${result.pulseOnMs / 1000} seconds. This setting is kept after a restart.`);
      close();
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : 'Save could not be confirmed. Reload settings before trying again.');
      setMustReload(true);
    } finally { setBusy(null); }
  }

  return <section className="garden-pulse-settings" aria-labelledby="pulse-heading">
    <div className="garden-detail-heading">
      <div><h3 id="pulse-heading">Watering pulse</h3><p className="garden-caption">How long the pump runs before pausing to let water soak in.</p></div>
      {!editing && <button ref={editButton} type="button" className="garden-button" disabled={!online} onClick={() => void load()}>Configure pulse</button>}
    </div>
    {!online && <p className="garden-caption" role="status">Connect to the controller to change its pulse.</p>}
    {editing && <form className="garden-name-editor garden-pulse-editor" onSubmit={save} onKeyDown={event => {
      if (event.key === 'Escape' && !busy) { event.preventDefault(); close(); }
    }} aria-busy={!!busy}>
      {busy === 'loading' && <p role="status">Reading controller settings…</p>}
      {settings && <>
        <label htmlFor="pulse-seconds">Pulse duration (seconds)</label>
        <div className="garden-name-fields">
          <input ref={input} id="pulse-seconds" type="number" inputMode="decimal" min="1" max={settings.maxPulseOnMs / 1000} step="0.001" required value={seconds}
            disabled={!!busy || !online || mustReload} onChange={event => setSeconds(event.target.value)} aria-invalid={!valid} aria-describedby="pulse-help pulse-impact" />
          <button type="submit" className="garden-button garden-button-primary" disabled={!!busy || !online || mustReload || !valid || pulseOnMs === settings.pulseOnMs}>{busy === 'saving' ? 'Confirming with controller…' : 'Save pulse'}</button>
        </div>
        <p id="pulse-help" className="garden-caption">1–{settings.maxPulseOnMs / 1000} seconds. Controller reported {settings.pulseOnMs / 1000} seconds, followed by a {settings.soakMs / 1000}-second soak.</p>
        <p id="pulse-impact" className="garden-pulse-impact">Saving stops any watering in progress on this controller and restarts its watering checks.</p>
      </>}
      {error && <p className="garden-field-error" role="alert">{error}</p>}
      <div className="garden-detail-actions garden-pulse-actions">
        {mustReload && !busy && <button type="button" className="garden-button" disabled={!online} onClick={() => void load()}>Reload settings</button>}
        <button type="button" className="garden-button garden-button-quiet" disabled={!!busy} onClick={close}>Cancel</button>
      </div>
    </form>}
    {notice && <p className="garden-save-notice" role="status">{notice}</p>}
  </section>;
}
