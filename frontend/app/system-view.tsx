'use client';

import { useCallback, useEffect, useState } from 'react';
import { type Device, type TelegramDelivery, fetchJson, formatTime, supportsManualTelegramDebug, telegramKind, telegramIssue } from './garden-shared';

export default function SystemView({ selected, deliveries }: { selected: Device; deliveries: TelegramDelivery[] }) {
  const [savingTelegram, setSavingTelegram] = useState(false);
  const [sendingTelegramDebug, setSendingTelegramDebug] = useState(false);
  const [error, setError] = useState('');
  const [notice, setNotice] = useState('');
  const [initialDeliveries, setInitialDeliveries] = useState<TelegramDelivery[]>([]);
  useEffect(() => {
    let active = true;
    fetchJson<TelegramDelivery[]>(`devices/${encodeURIComponent(selected.id)}/telegram/deliveries?limit=50`).then(data => { if (active) setInitialDeliveries(data); }).catch(reason => { if (active) setError(reason.message); });
    return () => { active = false; };
  }, [selected.id]);
  const combined = new Map(initialDeliveries.map(delivery => [delivery.eventId, delivery]));
  deliveries.forEach(delivery => combined.set(delivery.eventId, delivery));
  const selectedTelegramDeliveries = [...combined.values()].filter(delivery => delivery.deviceId === selected.id).sort((a, b) => new Date(b.updatedAt).getTime() - new Date(a.updatedAt).getTime());
  const lastTelegramDelivery = selectedTelegramDeliveries[0];
  const lastTelegramSuccess = selectedTelegramDeliveries.find(delivery => delivery.status === 'sent');
  const manualTelegramSupported = supportsManualTelegramDebug(selected.firmwareVersion);
  const setTelegramDebug = useCallback(async (enabled: boolean) => {
    setSavingTelegram(true); setError(''); setNotice('');
    try {
      await fetchJson(`devices/${encodeURIComponent(selected.id)}/commands`, {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({command:'telegram.debug.set', parameters:{enabled}})});
      setNotice('Change sent. Waiting for the controller to report its setting.');
    } catch (reason) { setError(reason instanceof Error ? reason.message : 'Unable to update Telegram debug.'); } finally { setSavingTelegram(false); }
  }, [selected.id]);
  const sendTelegramDebug = useCallback(async () => {
    setSendingTelegramDebug(true); setError(''); setNotice('');
    try {
      await fetchJson(`devices/${encodeURIComponent(selected.id)}/telegram/debug`, {method:'POST'});
      setNotice('Report requested. Delivery progress appears below.');
    } catch (reason) { setError(reason instanceof Error ? reason.message : 'Unable to request the report.'); } finally { setSendingTelegramDebug(false); }
  }, [selected.id]);
  return <>
    <div className="garden-system-stats"><article><span>Controller</span><strong>{selected.id}</strong><small>Firmware {selected.firmwareVersion ?? 'Unknown'} · Configuration {selected.configRevision}</small></article><article><span>Wi-Fi signal</span><strong>{selected.wifiRssi === null ? 'No reading' : `${selected.wifiRssi} dBm`}</strong><small>Last seen {formatTime(selected.lastSeenAt)}</small></article><article><span>Main tank</span><strong>{selected.mainTankLow === null ? 'No reading' : selected.mainTankLow ? 'Low' : 'Ready'}</strong><small>Changed {formatTime(selected.mainTankLastChangedAt)}</small></article></div>
    {error && <p className="garden-error" role="alert">{error}</p>}
    {notice && <p className="garden-notice" role="status">{notice}</p>}
            <section className="garden-notifications" aria-labelledby="telegram-heading">
              <div className="garden-telegram-heading"><div><p className="garden-eyebrow">Notifications</p><h2 id="telegram-heading">Telegram</h2><p>The ESP32 sends directly. Hourly and pump messages follow the debug toggle; low-tank alerts are always enabled.</p></div><div className="garden-telegram-actions"><div className="garden-telegram-control"><button type="button" role="switch" aria-label="Telegram debug messages" aria-checked={selected.telegramDebugEnabled} className={selected.telegramDebugEnabled ? 'garden-switch garden-switch-on' : 'garden-switch'} disabled={!selected.online || !selected.directTelegram || savingTelegram} onClick={() => void setTelegramDebug(!selected.telegramDebugEnabled)}><span /></button><strong>{savingTelegram ? 'Saving…' : selected.telegramDebugEnabled ? 'Debug on' : 'Debug off'}</strong></div><button className="garden-debug-send" disabled={!manualTelegramSupported || !selected.online || !selected.directTelegram || !selected.telegramConfigured || !selected.telegramWorkerRunning || sendingTelegramDebug} onClick={() => void sendTelegramDebug()}>{!manualTelegramSupported ? 'Update firmware to test' : sendingTelegramDebug ? 'Queuing…' : 'Send debug now'}</button></div></div>
              <dl className="garden-telegram-health"><div><dt>Controller</dt><dd>{!selected.directTelegram ? 'Firmware update required' : selected.online ? 'Online' : 'Offline'}</dd></div><div><dt>Bot</dt><dd>{selected.telegramConfigured ? 'Configured' : 'Missing credentials'}</dd></div><div><dt>Sender</dt><dd>{selected.telegramWorkerRunning ? 'Running' : 'Stopped'}</dd></div><div><dt>Secure clock</dt><dd>{selected.telegramTimeReady ? 'Ready' : 'Synchronizing'}</dd></div><div><dt>Queue</dt><dd>{selected.telegramPendingMessages} pending</dd></div><div><dt>Last success</dt><dd>{lastTelegramSuccess ? formatTime(lastTelegramSuccess.sentAt) : 'None yet'}</dd></div><div><dt>Last issue</dt><dd>{selected.telegramLastFailureStage ?? 'None reported'}</dd></div></dl>
              <div className="garden-telegram-history"><div className="garden-telegram-history-head"><strong>Recent interactions</strong><span>{lastTelegramDelivery ? `Latest: ${lastTelegramDelivery.status}` : 'No reports from the controller yet'}</span></div>{selectedTelegramDeliveries.length ? selectedTelegramDeliveries.slice(0, 8).map((delivery) => <div className="garden-telegram-row" key={delivery.eventId}><span>{formatTime(delivery.updatedAt)}</span><strong>{telegramKind(delivery.kind)}</strong><span className={`garden-delivery-status garden-delivery-${delivery.status}`}>{delivery.status.replace('_', ' ')}</span><span>{delivery.attempt} attempt{delivery.attempt === 1 ? '' : 's'}</span><small>{delivery.status === 'sent' ? `Message ${delivery.telegramMessageId ?? 'sent'}` : delivery.errorStage ? `${telegramIssue(delivery.errorStage)}${delivery.telegramErrorCode ? ` (${delivery.telegramErrorCode})` : ''}` : delivery.httpStatus ? `HTTP ${delivery.httpStatus}` : 'Waiting on controller'}</small></div>) : <div className="garden-telegram-empty">Use “Send debug now” to test the complete ESP32-to-Telegram path.</div>}</div>
            </section>
  </>;
}
