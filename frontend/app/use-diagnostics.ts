'use client';

import { useCallback, useEffect, useRef, useState } from 'react';
import type { Device } from './garden-shared';
import { DIAGNOSTICS_OPERATOR, diagnosticsFetch, type DebugCommand, type DebugReply, type DiagnosticsResponse, type TraceSample } from './diagnostics-shared';

export function useDiagnostics(device: Device, email?: string) {
  const [data, setData] = useState<DiagnosticsResponse | null>(null);
  const [trace, setTrace] = useState<TraceSample[]>([]);
  const [error, setError] = useState('');
  const [notice, setNotice] = useState('');
  const [busy, setBusy] = useState('');
  const [now, setNow] = useState(() => Date.now());
  const [leaseFailed, setLeaseFailed] = useState(false);
  const current = useRef({ device, email, data, leaseFailed });
  useEffect(() => { current.current = { device, email, data, leaseFailed }; }, [device, email, data, leaseFailed]);
  const mounted = useRef(false);
  const loading = useRef<Promise<void> | null>(null);
  const owned = useRef<{ sessionId: string; bootId: string } | null>(null);
  const operation = useRef(false);

  const consume = useCallback((incoming: DiagnosticsResponse) => {
    if (!mounted.current) return;
    const previous = current.current.data;
    const older = previous?.snapshot && incoming.snapshot && previous.snapshot.bootId === incoming.snapshot.bootId
      && previous.snapshot.sequence != null && incoming.snapshot.sequence != null && incoming.snapshot.sequence < previous.snapshot.sequence;
    const next = older ? { ...incoming, snapshot: previous.snapshot, receivedAt: previous.receivedAt } : incoming;
    current.current.data = next;
    setData(next);
    const session = next.snapshot?.session;
    if (next.canControl && current.current.email === DIAGNOSTICS_OPERATOR && session?.owner === DIAGNOSTICS_OPERATOR && session.sessionId && next.snapshot?.bootId) {
      owned.current = { sessionId: session.sessionId, bootId: next.snapshot.bootId };
    } else if (session?.mode === 'observe') owned.current = null;
    if (next.snapshot && next.receivedAt && !older) {
      const sample: TraceSample = { snapshot: next.snapshot, receivedAt: next.receivedAt };
      setTrace(previousTrace => {
        const last = previousTrace.at(-1);
        if (last?.snapshot.bootId === sample.snapshot.bootId && last.snapshot.sequence === sample.snapshot.sequence && last.receivedAt === sample.receivedAt) return previousTrace;
        const sameBoot = last?.snapshot.bootId === sample.snapshot.bootId ? previousTrace : [];
        return [...sameBoot, sample].slice(-600);
      });
    }
  }, []);

  const refresh = useCallback(async (signal?: AbortSignal) => {
    if (loading.current) return loading.current;
    const request = (async () => {
      try {
        const next = await diagnosticsFetch<DiagnosticsResponse>(device.id, 'diagnostics', undefined, signal);
        consume(next);
        if (mounted.current) setError('');
      } catch (reason) {
        if (mounted.current && !signal?.aborted) setError(reason instanceof Error ? reason.message : 'Unable to read diagnostics.');
      }
    })();
    loading.current = request;
    try { await request; } finally { if (loading.current === request) loading.current = null; }
  }, [device.id, consume]);

  const endOnExit = useCallback(() => {
    const session = owned.current;
    owned.current = null;
    if (session && current.current.device.online) {
      void diagnosticsFetch(device.id, 'debug/session/end', { sessionId: session.sessionId, expectedBootId: session.bootId }, undefined, true).catch(() => { /* The controller's local lease also expires. */ });
    }
  }, [device.id]);

  useEffect(() => {
    mounted.current = true;
    const controller = new AbortController();
    let timer: ReturnType<typeof setTimeout>;
    let disposed = false;
    const poll = async () => {
      if (disposed) return;
      if (!document.hidden) await refresh(controller.signal);
      if (!disposed) timer = setTimeout(poll, 1000);
    };
    const visibility = () => { if (document.hidden) endOnExit(); };
    document.addEventListener('visibilitychange', visibility);
    window.addEventListener('pagehide', endOnExit);
    void poll();
    return () => {
      disposed = true; mounted.current = false; controller.abort(); clearTimeout(timer);
      document.removeEventListener('visibilitychange', visibility); window.removeEventListener('pagehide', endOnExit);
      endOnExit();
    };
  }, [refresh, endOnExit]);

  useEffect(() => {
    const clock = setInterval(() => setNow(Date.now()), 250);
    let renewing = false;
    const renew = setInterval(async () => {
      const state = current.current;
      const session = owned.current;
      if (renewing || document.hidden || !session || !state.device.online || state.device.bootId !== session.bootId || state.email !== DIAGNOSTICS_OPERATOR || !state.data?.canControl || state.leaseFailed) return;
      renewing = true;
      try {
        await diagnosticsFetch<DebugReply>(device.id, 'debug/session/renew', { sessionId: session.sessionId, expectedBootId: session.bootId }, AbortSignal.timeout(8000));
        if (mounted.current) await refresh();
      } catch (reason) {
        if (mounted.current) { setLeaseFailed(true); setError(`Lease renewal is unconfirmed; controls are disabled. The controller stops when its local lease expires. ${reason instanceof Error ? reason.message : 'Reconnect and review the session.'}`); }
      } finally { renewing = false; }
    }, 5000);
    return () => { clearInterval(clock); clearInterval(renew); };
  }, [device.id, refresh]);

  const snapshot = data?.snapshot;
  const age = data?.receivedAt ? now - Date.parse(data.receivedAt) : Infinity;
  const sameBoot = !!snapshot?.bootId && snapshot.bootId === device.bootId;
  const supported = !!data?.supported;
  const operator = email === DIAGNOSTICS_OPERATOR && !!data?.canControl;
  const online = device.online && data?.online !== false;
  const fresh = age >= -5000 && age <= 10000 && data?.stale !== true;
  const elapsed = Math.max(0, Number.isFinite(age) ? age : 0);
  const leaseRemaining = Math.max(0, (snapshot?.session.leaseRemainingMs ?? 0) - elapsed);
  const ownSession = !!snapshot?.session.sessionId && snapshot.session.owner === email && snapshot.session.mode !== 'observe';
  const canStart = operator && supported && online && sameBoot && fresh && snapshot?.session.mode === 'observe';
  const canAct = operator && supported && online && sameBoot && fresh && ownSession && leaseRemaining > 0 && !leaseFailed;
  const canStop = operator && supported && online && sameBoot;

  const request = useCallback(async (label: string, suffix: string, body: Record<string, unknown>, bypassBusy = false) => {
    if (operation.current && !bypassBusy) throw new Error('Another request is still being confirmed.');
    if (!bypassBusy) operation.current = true;
    if (mounted.current) { setBusy(label); setError(''); setNotice(''); }
    try {
      const reply = await diagnosticsFetch<DebugReply>(device.id, suffix, body, AbortSignal.timeout(15000));
      if (reply.status !== 'accepted') throw new Error(reply.message || 'The request is unconfirmed. Review current state before repeating it.');
      if (reply.sessionId && suffix === 'debug/session' && typeof body.expectedBootId === 'string') {
        owned.current = { sessionId: reply.sessionId, bootId: body.expectedBootId };
        if (!mounted.current) endOnExit();
      }
      if (mounted.current) {
        if (suffix === 'debug/session') setLeaseFailed(false);
        if (suffix === 'debug/session/end') owned.current = null;
        setNotice(`${reply.command}: accepted by the controller. Execution and completion appear in the trace.`);
        await refresh();
      }
      return reply;
    } catch (reason) {
      if (mounted.current) setError(reason instanceof Error ? reason.message : 'The controller could not confirm this action.');
      throw reason;
    } finally { if (!bypassBusy) operation.current = false; if (mounted.current) setBusy(''); }
  }, [device.id, endOnExit, refresh]);

  const start = async (mode: 'hardware' | 'simulation') => {
    if (!canStart) throw new Error('Connect to supported firmware and wait for fresh state before starting a session.');
    return request('Starting session', 'debug/session', { mode, expectedBootId: device.bootId });
  };
  const action = async (command: DebugCommand, parameters: Record<string, unknown>, bypassBusy = false) => {
    const stop = command === 'debug.output.stop' || command === 'debug.input.clear';
    const resume = command === 'debug.automation.resume';
    if (resume ? !canStart || !snapshot?.session.automationHeld : stop ? !canStop : !canAct) throw new Error('This command requires current controller state and a matching test session.');
    return request(command, 'debug/actions', { command, expectedBootId: device.bootId, ...(snapshot?.session.sessionId ? { sessionId: snapshot.session.sessionId } : {}), parameters }, bypassBusy);
  };
  const end = async () => {
    if (!canStop || !ownSession || !snapshot?.session.sessionId) throw new Error('The current session cannot be ended from this dashboard.');
    return request('Ending session', 'debug/session/end', { sessionId: snapshot.session.sessionId, expectedBootId: device.bootId }, true);
  };
  return { data, snapshot, trace, error, notice, busy, now, age, fresh, sameBoot, supported, operator, canStart, canAct, canStop, ownSession, leaseRemaining, leaseFailed, start, action, end, refresh };
}
