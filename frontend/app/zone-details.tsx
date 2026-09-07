'use client';

import { useRef, useState } from 'react';
import { type Zone, fetchJson, formatTime, zoneName } from './garden-shared';

export default function ZoneDetails({ deviceId, zone, onSaved, onHistory }: {
  deviceId: string; zone: Zone;
  onSaved: (deviceId: string, zoneId: number, name: string | null) => void;
  onHistory: () => void;
}) {
  const [editing, setEditing] = useState(false);
  const [draft, setDraft] = useState('');
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState('');
  const [notice, setNotice] = useState('');
  const editButton = useRef<HTMLButtonElement>(null);
  const nameInput = useRef<HTMLInputElement>(null);
  const closeEditor = () => { setEditing(false); setError(''); requestAnimationFrame(() => editButton.current?.focus()); };

  async function saveName(event: React.FormEvent<HTMLFormElement>) {
    event.preventDefault();
    setSaving(true); setError('');
    try {
      const result = await fetchJson<{ id: number; name: string | null }>(`devices/${encodeURIComponent(deviceId)}/zones/${zone.id}`, {
        method: 'PATCH', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ name: draft.trim() }),
      });
      onSaved(deviceId, result.id, result.name);
      setNotice(result.name ? `Zone name saved: ${result.name}.` : `Name reset to Zone ${zone.id}.`);
      closeEditor();
    } catch (reason) { setError(reason instanceof Error ? reason.message : 'Unable to save the name. Please try again.'); }
    finally { setSaving(false); }
  }

  return <section className="garden-zone-details" aria-labelledby="zone-detail-heading">
    <div className="garden-detail-heading">
      <div><p className="garden-eyebrow">Zone {zone.id} details</p><h2 id="zone-detail-heading">{zoneName(zone)}</h2></div>
      <div className="garden-detail-actions">
        <button className="garden-button garden-button-quiet" ref={editButton} type="button" aria-expanded={editing} aria-controls="zone-name-editor" onClick={() => {
          setDraft(zone.name ?? ''); setEditing(true); setNotice(''); setError('');
          requestAnimationFrame(() => nameInput.current?.focus());
        }}>Edit name</button>
        <button className="garden-button" type="button" onClick={onHistory}>View history <span aria-hidden="true">↗</span></button>
      </div>
    </div>
    {editing && <form id="zone-name-editor" className="garden-name-editor" onSubmit={saveName} onKeyDown={event => { if (event.key === 'Escape' && !saving) { event.preventDefault(); closeEditor(); } }}>
      <label htmlFor="zone-name">Zone name</label>
      <div className="garden-name-fields"><input ref={nameInput} id="zone-name" value={draft} onChange={event => setDraft(event.target.value)} maxLength={60} disabled={saving} aria-describedby={error ? 'zone-name-error' : 'zone-name-help'} aria-invalid={!!error} placeholder={`Zone ${zone.id}`} autoComplete="off" />
        <button type="submit" className="garden-button garden-button-primary" disabled={saving}>{saving ? 'Saving…' : 'Save name'}</button>
        <button type="button" className="garden-button garden-button-quiet" disabled={saving} onClick={closeEditor}>Cancel</button>
      </div>
      <p id="zone-name-help" className="garden-caption">Up to 60 characters. Leave blank to use “Zone {zone.id}”.</p>
      {error && <p id="zone-name-error" className="garden-field-error" role="alert">{error}</p>}
    </form>}
    {notice && <p className="garden-save-notice" role="status">{notice}</p>}
    <dl className="garden-detail-values">
      <div><dt>Sensor</dt><dd>{zone.sensorValid ? 'Healthy' : 'Invalid reading'}</dd></div>
      <div><dt>Pump relay</dt><dd>{zone.relayOn ? 'On' : 'Off'}</dd></div>
      <div><dt>Last watered</dt><dd>{zone.lastWateredAt ? formatTime(zone.lastWateredAt) : 'No recorded watering'}</dd></div>
      <div><dt>Fault</dt><dd>{zone.fault ?? 'None'}</dd></div>
    </dl>
  </section>;
}
