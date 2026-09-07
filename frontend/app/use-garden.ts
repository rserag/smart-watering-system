'use client';

import { useCallback, useEffect, useState } from 'react';
import { type Device, type Me, type TelegramDelivery, fetchJson } from './garden-shared';

export function useGarden() {
  const [me, setMe] = useState<Me | null>(null);
  const [devices, setDevices] = useState<Device[]>([]);
  const [deliveries, setDeliveries] = useState<TelegramDelivery[]>([]);
  const [error, setError] = useState('');
  const [loading, setLoading] = useState(true);
  const [connection, setConnection] = useState<'connecting' | 'live' | 'reconnecting'>('connecting');
  const [now, setNow] = useState(0);

  const load = useCallback(() => fetchJson<Me>('me').then(async identity => {
      setMe(identity);
      if (identity.authenticated) setDevices(await fetchJson<Device[]>('devices'));
      setError('');
    }).catch(reason => {
      setError(reason instanceof Error ? reason.message : 'The garden is temporarily unavailable.');
    }).finally(() => setLoading(false)), []);

  useEffect(() => { void load(); }, [load]);
  useEffect(() => {
    const timer = window.setInterval(() => setNow(Date.now()), 15000);
    return () => window.clearInterval(timer);
  }, []);

  const updateZoneName = useCallback((deviceId: string, zoneId: number, name: string | null) => {
    setDevices(current => current.map(device => device.id === deviceId
      ? { ...device, zones: device.zones.map(zone => zone.id === zoneId ? { ...zone, name } : zone) }
      : device));
  }, []);

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
            setDevices(message.devices);
            setError('');
            setConnection('live');
            setNow(Date.now());
            attempts = 0;
          } else if (message.type === 'zone.updated') {
            updateZoneName(message.deviceId, message.zone.id, message.zone.name);
          } else if (message.type === 'telegram.delivery' && message.delivery) {
            setDeliveries(current => [message.delivery, ...current.filter(item => item.eventId !== message.delivery.eventId)].slice(0, 100));
          } else if (message.device) {
            setDevices(current => {
              const previous = current.find(item => item.id === message.device.id);
              const device: Device = {
                ...message.device,
                // A telemetry snapshot may have started before a name was saved.
                // Labels change through zone.updated or a reconnect snapshot.
                zones: message.device.zones.map((zone: Device['zones'][number]) => {
                  const known = previous?.zones.find(item => item.id === zone.id);
                  return known ? { ...zone, name: known.name } : zone;
                }),
              };
              return [...current.filter(item => item.id !== device.id), device].sort((a, b) => a.id.localeCompare(b.id));
            });
            setNow(Date.now());
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
  }, [me?.authenticated, updateZoneName]);

  const reload = () => { setLoading(true); setError(''); return load(); };
  return { me, devices, deliveries, error, loading, connection, now, reload, updateZoneName };
}
