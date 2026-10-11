'use client';

import { useRef, useState } from 'react';
import { type Device, type Zone, fetchJson, supportsPausedRecovery, zoneName } from './garden-shared';

type Confirmation = { action: 'clear' | 'resume'; bootId: string; generation: number; expiresAt: number };
type Result = { zoneId: number; action: 'clear' | 'resume'; paused: boolean; status: 'confirmed' };

export default function ZoneRecovery({ device, zone, now, onSnapshot }: {
  device: Device; zone: Zone; now: number; onSnapshot: (device: Device) => boolean;
}) {
  const [confirmation, setConfirmation] = useState<Confirmation | null>(null);
  const [busy, setBusy] = useState<'sending' | 'verifying' | null>(null);
  const [error, setError] = useState('');
  const [notice, setNotice] = useState('');
  const [completedAction, setCompletedAction] = useState<'clear' | 'resume' | null>(null);
  const actionButton = useRef<HTMLButtonElement>(null);
  const confirmButton = useRef<HTMLButtonElement>(null);
  const supported = supportsPausedRecovery(device.firmwareVersion);
  const fresh = now > 0 && !!device.telemetryReceivedAt && device.telemetryBootId === device.bootId
    && now - Date.parse(device.telemetryReceivedAt) <= 30000;
  const canClear = zone.fault === 'maximum_watering_reached';
  const canResume = !!zone.paused && !zone.fault && zone.phase !== 'disabled' && device.mainTankLow === false;
  const eligible = (canClear || canResume) && supported && device.online && fresh
    && !!device.bootId && Number.isInteger(zone.recoveryGeneration);
  const unchanged = !!confirmation && confirmation.bootId === device.bootId
    && confirmation.generation === zone.recoveryGeneration
    && (confirmation.action === 'clear' ? canClear : canResume);
  const unexpired = !!confirmation && now < confirmation.expiresAt;

  function review() {
    if (!eligible || !device.bootId || zone.recoveryGeneration === undefined) return;
    setConfirmation({ action: canClear ? 'clear' : 'resume', bootId: device.bootId, generation: zone.recoveryGeneration, expiresAt: Date.now() + 60000 });
    setError(''); setNotice(''); setCompletedAction(null);
    requestAnimationFrame(() => confirmButton.current?.focus());
  }

  function cancel() {
    setConfirmation(null); setError('');
    requestAnimationFrame(() => actionButton.current?.focus());
  }

  async function submit() {
    if (!confirmation || !eligible || !unchanged || Date.now() >= confirmation.expiresAt || busy) return;
    const selected = confirmation;
    setBusy('sending'); setError(''); setNotice(''); setCompletedAction(null);
    try {
      const result = await fetchJson<Result>(`devices/${encodeURIComponent(device.id)}/zones/${zone.id}/recovery`, {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: selected.action, expectedBootId: selected.bootId, expectedRecoveryGeneration: selected.generation }),
        signal: AbortSignal.timeout(15000),
      });
      if (result.status !== 'confirmed' || result.zoneId !== zone.id || result.action !== selected.action || result.paused !== (selected.action === 'clear')) {
        throw new Error('The controller did not confirm this recovery. Refresh the channel before trying again.');
      }
      setBusy('verifying');
      for (let attempt = 0; attempt < 10; attempt++) {
        const snapshot = await fetchJson<Device>(`devices/${encodeURIComponent(device.id)}/latest`, { signal: AbortSignal.timeout(5000) });
        const current = snapshot.zones.find(item => item.id === zone.id);
        const age = snapshot.telemetryReceivedAt ? Date.now() - Date.parse(snapshot.telemetryReceivedAt) : Infinity;
        if (!snapshot.online || !Number.isFinite(age) || age < -5000 || age > 30000) {
          throw new Error('Fresh channel state is unavailable. Refresh the channel before trying again.');
        }
        if (snapshot.bootId !== selected.bootId || snapshot.telemetryBootId !== selected.bootId) {
          throw new Error('The controller restarted. Refresh the channel before trying again.');
        }
        if (current && (current.recoveryGeneration ?? 0) > selected.generation) {
          const applied = onSnapshot(snapshot);
          if (!applied || current.recoveryGeneration !== selected.generation + 1 || !!current.paused !== result.paused || current.fault) {
            throw new Error('The channel state changed during recovery. Review its current status before continuing.');
          }
          setNotice(selected.action === 'clear'
            ? 'Fault cleared. This channel stays paused until you choose Resume watering, including after a restart.'
            : current.phase === 'stabilizing' ? 'Channel resumed — checking fresh sensor readings.'
            : snapshot.mainTankLow ? 'Channel resumed. Watering is blocked while the main tank is low.'
            : !snapshot.automaticWateringEnabled ? 'Channel resumed. Automatic watering is disabled.'
            : current.relayOn ? 'Channel resumed. Watering is in progress.' : 'Channel resumed. Automatic monitoring is active.');
          setCompletedAction(selected.action);
          setConfirmation(null);
          requestAnimationFrame(() => actionButton.current?.focus());
          return;
        }
        await new Promise(resolve => setTimeout(resolve, 500));
      }
      throw new Error('Recovery was acknowledged, but the new channel state could not be confirmed. Refresh the channel before trying again.');
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : 'Recovery could not be confirmed. Refresh the channel before trying again.');
      setConfirmation(null);
    } finally { setBusy(null); }
  }

  if (!canClear && !zone.paused && !busy && !notice && !error) return null;

  return <section className="garden-recovery" aria-labelledby="recovery-heading" aria-busy={!!busy}>
    <div className="garden-detail-heading">
      <div><h3 id="recovery-heading">{canClear ? 'Watering limit reached' : zone.paused ? 'Channel paused' : 'Channel recovery'}</h3>
        <p className="garden-caption">{canClear ? 'Watering stopped before the moisture target was confirmed. Check the water supply, tubing and sensor.'
          : zone.paused ? 'Automatic and manual watering are paused for this channel. The pause is kept after a restart.' : 'The controller reports the current channel state above.'}</p></div>
      {!confirmation && (canClear || canResume) && <button ref={actionButton} type="button" className="garden-button" disabled={!eligible || !!busy} onClick={review}>{canClear ? 'Clear fault' : 'Resume watering'}</button>}
    </div>
    {!supported && <p className="garden-caption">Update the controller to firmware 0.7.0 to recover a channel here.</p>}
    {supported && (!device.online || !fresh) && <p className="garden-caption" role="status">Connect to the controller and wait for fresh readings to recover this channel.</p>}
    {zone.paused && zone.fault && !canClear && <p className="garden-caption">Resolve the sensor or emergency-stop fault before resuming this channel.</p>}
    {zone.paused && zone.phase === 'disabled' && <p className="garden-caption">Enable this channel in its configuration before resuming.</p>}
    {zone.paused && device.mainTankLow && <p className="garden-caption">Refill the main tank before resuming this channel.</p>}
    {confirmation && <div className="garden-recovery-confirmation">
      <strong>{confirmation.action === 'clear' ? 'Clear the fault and keep paused' : 'Resume watering'} for {zoneName(zone)} — Zone {zone.id}?</strong>
      <p>{confirmation.action === 'clear' ? 'The pump will stay off. Resume watering is a separate action when you are ready.' : 'The controller will check fresh sensor readings. Automatic watering may then start if enabled and the soil is dry.'}</p>
      {!unchanged && !busy && <p className="garden-field-error" role="alert">The channel state changed. Cancel and review its current status.</p>}
      {unchanged && !unexpired && !busy && <p className="garden-field-error" role="alert">This confirmation expired. Cancel and review the channel again.</p>}
      <div className="garden-detail-actions"><button ref={confirmButton} type="button" className="garden-button garden-button-primary" disabled={!eligible || !unchanged || !unexpired || !!busy} onClick={() => void submit()}>{busy === 'sending' ? 'Confirming with controller…' : busy === 'verifying' ? 'Checking channel state…' : confirmation.action === 'clear' ? 'Clear and keep paused' : 'Confirm resume'}</button>
        <button type="button" className="garden-button garden-button-quiet" disabled={!!busy} onClick={cancel}>Cancel</button></div>
    </div>}
    {error && <p className="garden-field-error" role="alert">{error}</p>}
    {notice && <p className="garden-save-notice" role="status">{completedAction === 'resume'
      ? zone.fault ? 'Channel faulted again. Review its current status before continuing.'
        : zone.paused ? 'This channel is paused again.'
        : zone.phase === 'stabilizing' ? 'Channel resumed — checking fresh sensor readings.'
        : device.mainTankLow ? 'Channel resumed. Watering is blocked while the main tank is low.'
        : !device.automaticWateringEnabled ? 'Channel resumed. Automatic watering is disabled.'
        : zone.relayOn ? 'Channel resumed. Watering is in progress.' : 'Channel resumed. Automatic monitoring is active.'
      : completedAction === 'clear' && !zone.paused ? 'This channel has resumed. Review its current status.' : notice}</p>}
  </section>;
}
