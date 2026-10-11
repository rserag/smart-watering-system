'use client';

import { useCallback, useEffect, useRef, useState } from 'react';
import { type Device, type Me, type TelegramDelivery, fetchJson, mergeDeliveries } from './garden-shared';

export function useGarden() {
  const [me, setMe] = useState<Me | null>(null);
  const [devices, setDevices] = useState<Device[]>([]);
  const observedDevices = useRef<Device[]>([]);
  const [deliveries, setDeliveries] = useState<TelegramDelivery[]>([]);
  const [error, setError] = useState('');
  const [loading, setLoading] = useState(true);
  const [connection, setConnection] = useState<'connecting' | 'live' | 'reconnecting'>('connecting');
  const [now, setNow] = useState(0);
  const [snapshotVersion, setSnapshotVersion] = useState(0);
  const replaceDevices = useCallback((incoming: Device[]) => {
    observedDevices.current = incoming;
    setDevices(incoming);
  }, []);

  const load = useCallback(() => fetchJson<Me>('me').then(async identity => {
      setMe(identity);
      if (identity.authenticated) replaceDevices(await fetchJson<Device[]>('devices'));
      setError('');
    }).catch(reason => {
      setError(reason instanceof Error ? reason.message : 'The garden is temporarily unavailable.');
    }).finally(() => setLoading(false)), [replaceDevices]);

  useEffect(() => { void load(); }, [load]);
  useEffect(() => {
    const timer = window.setInterval(() => setNow(Date.now()), 15000);
    return () => window.clearInterval(timer);
  }, []);

  const updateZoneName = useCallback((deviceId: string, zoneId: number, name: string | null) => {
    replaceDevices(observedDevices.current.map(device => device.id === deviceId
      ? { ...device, zones: device.zones.map(zone => zone.id === zoneId ? { ...zone, name } : zone) }
      : device));
  }, [replaceDevices]);

  const updateDevice = useCallback((incoming: Device, requireCurrentBoot = false): boolean => {
    const current = observedDevices.current;
    const previous = current.find(item => item.id === incoming.id);
    // Database insertion IDs order telemetry even if server clocks move.
    // A delayed verification read must not overwrite newer live state.
    const olderTelemetry = previous?.telemetryId != null && incoming.telemetryId != null && incoming.telemetryId < previous.telemetryId;
    const olderRecovery = previous?.bootId === incoming.bootId && previous?.zones.some(zone => {
      const next = incoming.zones.find(item => item.id === zone.id);
      return next && (next.recoveryGeneration ?? 0) < (zone.recoveryGeneration ?? 0);
    });
    if (previous && ((requireCurrentBoot && previous.bootId !== incoming.bootId) || olderTelemetry || olderRecovery)) return false;
    const device = { ...incoming, zones: incoming.zones.map(zone => {
      const known = previous?.zones.find(item => item.id === zone.id);
      return known ? { ...zone, name: known.name } : zone;
    }) };
    replaceDevices([...current.filter(item => item.id !== device.id), device].sort((a, b) => a.id.localeCompare(b.id)));
    setNow(Date.now());
    return true;
  }, [replaceDevices]);
  const updateRecoverySnapshot = useCallback((incoming: Device) => updateDevice(incoming, true), [updateDevice]);

  useEffect(() => {
    if (!me?.authenticated) return;
    let disposed = false;
    let socket: WebSocket | undefined;
    let retry: ReturnType<typeof setTimeout> | undefined;
    let attempts = 0;
    const connect = () => {
      if (disposed) return;
      const url = new URL('/ws/dashboard', window.location.origin);
      url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
      socket = new WebSocket(url);
      socket.onmessage = event => {
        if (disposed) return;
        try {
          const message = JSON.parse(event.data);
          if (message.type === 'snapshot') {
            replaceDevices(message.devices);
            setSnapshotVersion(version => version + 1);
            setError('');
            setConnection('live');
            setNow(Date.now());
            attempts = 0;
          } else if (message.type === 'zone.updated') {
            updateZoneName(message.deviceId, message.zone.id, message.zone.name);
          } else if (message.type === 'telegram.delivery' && message.delivery) {
            setDeliveries(current => mergeDeliveries(current, [message.delivery]).slice(0, 100));
          } else if (message.device) {
            updateDevice(message.device);
          }
        } catch { setError('An update could not be read. Reconnecting to the garden…'); socket?.close(); }
      };
      socket.onerror = () => socket?.close();
      socket.onclose = () => {
        if (disposed) return;
        setConnection('reconnecting');
        retry = setTimeout(async () => {
          try {
            const identity = await fetchJson<Me>('me');
            if (disposed) return;
            if (!identity.authenticated) { setMe(identity); return; }
          } catch { /* Retry the live connection after a temporary network failure. */ }
          connect();
        }, Math.min(1000 * 2 ** attempts++, 30000));
      };
    };
    connect();
    return () => { disposed = true; clearTimeout(retry); socket?.close(); };
  }, [me?.authenticated, updateZoneName, updateDevice, replaceDevices]);

  const reload = () => { setLoading(true); setError(''); return load(); };
  return { me, devices, deliveries, error, loading, connection, now, snapshotVersion, reload, updateZoneName, updateRecoverySnapshot };
}
