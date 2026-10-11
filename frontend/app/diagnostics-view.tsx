'use client';

import { useEffect, useRef, useState } from 'react';
import { type Device, formatTime } from './garden-shared';
import { commandExamples, diagnosticsFetch, downloadCapture, formatValue, parseScenario, scenarioPreset, validateCommand, zoneLabel, type DebugCapture, type DebugCommand, type DebugZone } from './diagnostics-shared';
import { useDiagnostics } from './use-diagnostics';
import DiagnosticsWiring from './diagnostics-wiring';
import DiagnosticsTrace from './diagnostics-trace';

function sleep(ms: number, signal: AbortSignal) {
  return new Promise<void>((resolve, reject) => {
    if (signal.aborted) { reject(new Error('Sequence stopped.')); return; }
    const abort = () => { clearTimeout(timer); reject(new Error('Sequence stopped.')); };
    const timer = setTimeout(() => { signal.removeEventListener('abort', abort); resolve(); }, ms);
    signal.addEventListener('abort', abort, { once: true });
  });
}

function Pipeline({ zone, supported }: { zone?: DebugZone; supported: boolean }) {
  if (!zone) return <p className="debug-empty">Waiting for zone diagnostics.</p>;
  const staleDerived = !zone.sensorValid;
  return <>
    <div className="debug-pipeline">
      <article><span>01 · Physical ADC batch</span><strong>{formatValue(zone.physicalRaw)}</strong><small>16-conversion mean · counts</small><div className="debug-adc-samples" aria-label={`${zone.samples.length} physical ADC conversions`}>{zone.samples.map((sample, index) => <i key={index} style={{ height: `${Math.max(5, sample / 4095 * 100)}%` }} title={`Conversion ${index + 1}: ${sample}`} />)}</div><small>Min {formatValue(zone.minRaw)} / max {formatValue(zone.maxRaw)}</small></article>
      <article className={zone.overrideRaw != null ? 'has-override' : ''}><span>02 · Effective ADC</span><strong>{formatValue(zone.raw)}</strong><small>{zone.overrideRaw != null ? `Injected ${zone.overrideRaw} before filter & validation` : 'Physical batch mean · no injection'}</small><small>Valid {formatValue(zone.validSamples)} / invalid {formatValue(zone.invalidSamples)} samples</small></article>
      <article className={staleDerived ? 'is-stale' : ''}><span>03 · Filter & calibration</span><strong>{formatValue(zone.filteredRaw)}</strong><small>{staleDerived ? 'Last valid filtered value; current ADC is invalid' : 'Filtered ADC · counts'}</small><small>Dry {formatValue(zone.dryRaw)} / wet {formatValue(zone.wetRaw)}</small></article>
      <article className={staleDerived ? 'is-stale' : ''}><span>04 · Relative moisture</span><strong>{formatValue(zone.relativeMoisturePercent, '%')}</strong><small>{staleDerived ? 'Last valid percentage; not a fresh reading' : `Unclamped ${formatValue(zone.unclampedPercent, '%')} → 0–100%`}</small><small>Calibrated relative scale</small></article>
      <article><span>05 · Controller decision</span><strong className="debug-small-value">{zone.phase.replaceAll('_', ' ')}</strong><small>{zone.decisionReason?.replaceAll('_', ' ') || 'Detailed decision unavailable'}</small><small>Dry {formatValue(zone.dryCount)} / wet {formatValue(zone.wetCount)} counts</small></article>
      <article><span>06 · Output command</span><strong>{zone.relayOn ? 'HIGH' : 'LOW'}</strong><small>Physical relay {zone.relayOn ? 'on' : 'off'}{zone.virtualRelayOn ? ' · virtual relay on' : ''}</small><small>{formatValue(zone.pulseRemainingMs, ' ms')} pulse remaining</small></article>
    </div>
    <dl className="debug-metrics"><div><dt>Sensor validity</dt><dd>{zone.sensorValid ? 'Valid' : 'Invalid'}</dd></div><div><dt>Sample age</dt><dd>{formatValue(zone.sampleAgeMs, ' ms')}</dd></div><div><dt>Start / stop thresholds</dt><dd>{formatValue(zone.startWateringPercent, '%')} / {formatValue(zone.stopWateringPercent, '%')}</dd></div><div><dt>Pulse / soak</dt><dd>{formatValue(zone.pulseMs, ' ms')} / {formatValue(zone.soakMs, ' ms')}</dd></div><div><dt>Cycle relay time / limit</dt><dd>{formatValue(zone.wateringOnMsThisCycle, ' ms')} / {formatValue(zone.maxOnMs, ' ms')}</dd></div><div><dt>Enabled / paused</dt><dd>{zone.enabled == null ? 'Unknown' : zone.enabled ? 'Enabled' : 'Disabled'} / {zone.paused ? 'paused' : 'unpaused'}</dd></div><div><dt>Fault</dt><dd>{zone.fault ?? 'None'}</dd></div><div><dt>Phase elapsed</dt><dd>{formatValue(zone.phaseElapsedMs, ' ms')}</dd></div></dl>
    {!supported && <p className="garden-caption">Legacy telemetry provides batch mean, filter, percentage and phase only. Individual conversions, counters and calibration are unavailable until firmware 0.8.0 reports them.</p>}
  </>;
}

export default function DiagnosticsView({ device, email, zoneId, onZoneChange }: { device: Device; email?: string; zoneId: number; onZoneChange: (id: number) => void }) {
  const debug = useDiagnostics(device, email);
  const { snapshot } = debug;
  const zone = snapshot?.zones.find(item => item.id === zoneId);
  const mode = snapshot?.session.mode ?? 'observe';
  const [duration, setDuration] = useState('1000');
  const [raw, setRaw] = useState('2600');
  const [tankValue, setTankValue] = useState('low');
  const [oledPattern, setOledPattern] = useState('checkerboard');
  const [oledDuration, setOledDuration] = useState('3000');
  const [sequence, setSequence] = useState(() => JSON.stringify(scenarioPreset('drywet', zoneId), null, 2));
  const [running, setRunning] = useState(false);
  const [progress, setProgress] = useState('');
  const [localError, setLocalError] = useState('');
  const [command, setCommand] = useState<DebugCommand>('debug.sample');
  const [parameters, setParameters] = useState('{}');
  const [captureName, setCaptureName] = useState('Garden diagnostic test');
  const [captureBusy, setCaptureBusy] = useState(false);
  const [auditFilter, setAuditFilter] = useState('');
  const sequenceAbort = useRef<AbortController | null>(null);
  const latest = useRef(debug);
  useEffect(() => { latest.current = debug; }, [debug]);
  const sequenceSession = useRef<{ bootId: string | null; sessionId: string | null; mode: string } | null>(null);
  const issued = useRef(new Set<string>());
  const scenario = parseScenario(sequence, mode);
  let commandParameters: Record<string, unknown> = {};
  let commandError: string | null = null;
  try {
    const parsed = JSON.parse(parameters);
    if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) throw new Error('Parameters must be a JSON object.');
    commandParameters = parsed;
    commandError = validateCommand(command, parsed, mode);
  } catch (reason) { commandError = reason instanceof Error ? reason.message : 'Invalid JSON parameters.'; }
  const outputValid = Number.isInteger(Number(duration)) && Number(duration) >= 100 && Number(duration) <= 60000;
  const inputValid = raw.trim() !== '' && Number.isInteger(Number(raw)) && Number(raw) >= 0 && Number(raw) <= 4095;
  const oledValid = Number.isInteger(Number(oledDuration)) && Number(oledDuration) >= 100 && Number(oledDuration) <= 10000;
  const disabled = !debug.canAct || !!debug.busy || running;
  const simulation = mode === 'simulation';
  const actions = (debug.data?.actions ?? []).filter(item => !auditFilter || `${item.command} ${item.status} ${item.requestId} ${item.message ?? ''}`.toLowerCase().includes(auditFilter.toLowerCase()));

  async function perform(operation: () => Promise<unknown>) {
    setLocalError('');
    try { await operation(); } catch (reason) { setLocalError(reason instanceof Error ? reason.message : 'Unable to confirm this request.'); }
  }

  async function stopTests() {
    sequenceAbort.current?.abort(); sequenceAbort.current = null; issued.current.clear();
    setRunning(false); setProgress('Sequence stopped.');
    const current = latest.current;
    if (!current.canStop) return;
    await current.action('debug.output.stop', {}, true);
    if (current.ownSession && current.snapshot?.session.mode === 'simulation') await latest.current.action('debug.input.clear', { input: 'all' }, true);
  }

  useEffect(() => {
    if (!running) return;
    const session = sequenceSession.current;
    const failed = debug.data?.actions.find(item => issued.current.has(item.requestId) && ['rejected', 'cancelled', 'unconfirmed', 'failed'].includes(item.status));
    if (!debug.canAct || session?.bootId !== snapshot?.bootId || session?.sessionId !== snapshot?.session.sessionId || session?.mode !== mode || failed) {
      sequenceAbort.current?.abort();
      // Release injected values and pending outputs when the same session remains reachable.
      if (latest.current.canStop && latest.current.ownSession) {
        void latest.current.action('debug.output.stop', {}, true).then(() => latest.current.snapshot?.session.mode === 'simulation' ? latest.current.action('debug.input.clear', { input: 'all' }, true) : undefined).catch(() => { /* Local lease is also bounded. */ });
      }
    }
  }, [debug.canAct, debug.data?.actions, snapshot?.bootId, snapshot?.session.sessionId, mode, running]);
  useEffect(() => () => { sequenceAbort.current?.abort(); }, []);

  async function runScenario() {
    if (!debug.canAct || scenario.error || running || debug.busy) return;
    const controller = new AbortController(); sequenceAbort.current = controller;
    sequenceSession.current = { bootId: snapshot?.bootId ?? null, sessionId: snapshot?.session.sessionId ?? null, mode };
    issued.current.clear(); setRunning(true); setLocalError('');
    try {
      for (let index = 0; index < scenario.steps.length; index++) {
        if (controller.signal.aborted || !latest.current.canAct) throw new Error('Sequence stopped because controller/session state changed.');
        const step = scenario.steps[index];
        setProgress(`Step ${index + 1} of ${scenario.steps.length}: ${step.command}`);
        if (step.command !== 'wait') {
          const reply = await latest.current.action(step.command, step.parameters ?? {});
          issued.current.add(reply.requestId);
          if (step.command === 'debug.output.pulse' || step.command === 'debug.oled.test') {
            const deadline = Date.now() + Number(step.parameters?.durationMs ?? 0) + 8000;
            for (;;) {
              if (controller.signal.aborted) throw new Error('Sequence stopped.');
              const terminal = latest.current.data?.actions.find(item => item.requestId === reply.requestId && ['completed', 'cancelled', 'rejected', 'failed', 'unconfirmed'].includes(item.status));
              if (terminal) { if (terminal.status !== 'completed') throw new Error(terminal.message || `Action ${terminal.status}.`); break; }
              if (Date.now() > deadline) throw new Error('Timed action completion is unconfirmed. Sequence stopped; inspect the trace.');
              await sleep(250, controller.signal);
            }
          }
        }
        if (step.waitMs) await sleep(step.waitMs, controller.signal);
      }
      await latest.current.action('debug.output.stop', {}, true);
      if (latest.current.snapshot?.session.mode === 'simulation') await latest.current.action('debug.input.clear', { input: 'all' }, true);
      setProgress('Sequence completed. Outputs stopped and injections released.');
    } catch (reason) {
      setProgress('Sequence stopped.');
      if (!controller.signal.aborted) {
        setLocalError(reason instanceof Error ? reason.message : 'Sequence failed.');
        try { await stopTests(); } catch { /* The trace retains unconfirmed outcomes. */ }
      }
    } finally { if (sequenceAbort.current === controller) sequenceAbort.current = null; setRunning(false); issued.current.clear(); }
  }

  const localCapture = (): DebugCapture => ({ id: 'live', name: captureName || 'Live diagnostics', createdAt: new Date().toISOString(), sampleCount: debug.trace.length, snapshots: debug.trace, actions: debug.data?.actions ?? [] });
  async function saveCapture() {
    if (!debug.operator || !captureName.trim() || captureBusy) return;
    setCaptureBusy(true); setLocalError('');
    try { await diagnosticsFetch(device.id, 'debug/captures', { name: captureName.trim() }); await debug.refresh(); } catch (reason) { setLocalError(reason instanceof Error ? reason.message : 'Unable to save capture.'); } finally { setCaptureBusy(false); }
  }
  async function exportSaved(id: string, format: 'json' | 'csv') {
    setCaptureBusy(true);
    try { downloadCapture(await diagnosticsFetch<DebugCapture>(device.id, `debug/captures/${encodeURIComponent(id)}`), format); } catch (reason) { setLocalError(reason instanceof Error ? reason.message : 'Unable to export capture.'); } finally { setCaptureBusy(false); }
  }

  return <div className="debug-dashboard">
    <section className={`debug-control-bar ${simulation ? 'is-simulation' : mode === 'hardware' ? 'is-hardware' : ''}`} aria-label="Diagnostics session">
      <div className="debug-session-summary"><span className="debug-mode">{simulation ? 'Simulation' : mode === 'hardware' ? 'Hardware test' : 'Observe'}</span><strong>{device.id}</strong><span className={`debug-freshness ${debug.fresh && debug.sameBoot ? '' : 'is-stale'}`}>{debug.fresh && debug.sameBoot ? `${Math.max(0, Math.round(debug.age / 1000))}s ago` : 'Stale / previous boot'}</span></div>
      <div className="debug-session-meta"><span>{snapshot?.session.automationHeld ? 'Automation held' : 'Normal automation'}</span><span>{mode === 'observe' ? 'No test session' : `${Math.ceil(debug.leaseRemaining / 1000)}s lease · ${snapshot?.session.owner ?? 'unknown owner'}`}</span></div>
      <div className="debug-session-actions"><button className="garden-button" disabled={!debug.canStart || !!debug.busy || running} onClick={() => void perform(() => debug.start('simulation'))}>Start simulation</button><button className="garden-button" disabled={!debug.canStart || !!debug.busy || running} onClick={() => void perform(() => debug.start('hardware'))}>Start hardware test</button><button className="garden-button debug-stop" disabled={!debug.canStop} onClick={() => void perform(stopTests)}>Stop all tests</button>{debug.ownSession && <button className="garden-button garden-button-quiet" disabled={!debug.canStop} onClick={() => void perform(async () => { try { await stopTests(); } finally { await latest.current.end(); } })}>End session</button>}</div>
    </section>
    <div className="debug-context"><span>Firmware <strong>{device.firmwareVersion ?? 'Unknown'}</strong></span><span>Boot <strong>{snapshot?.bootId ?? 'Unavailable'}</strong></span><span>Sequence <strong>{snapshot?.sequence ?? 'Unavailable'}</strong></span><span>Config <strong>{snapshot?.configRevision ?? device.configRevision}</strong></span><span>Uptime <strong>{formatValue(snapshot?.uptimeMs == null ? null : snapshot.uptimeMs / 1000, ' s')}</strong></span><span>Received <strong>{formatTime(debug.data?.receivedAt ?? null)}</strong></span></div>
    {!debug.data && <p className="garden-caption" role="status">Loading controller diagnostics…</p>}
    {debug.data && !debug.supported && <p className="debug-banner" role="status">Firmware diagnostics are unavailable. Showing documented wiring and existing telemetry. Update the controller to firmware 0.8.0 to test hardware and inspect the full pipeline.</p>}
    {debug.data && !debug.operator && <p className="debug-banner">Read-only diagnostics. Only the configured operator can start sessions, control I/O or create captures.</p>}
    {simulation && <p className="debug-banner debug-simulation-banner">SIMULATION · physical relays stay LOW. Injected inputs affect virtual decisions only. Physical readings remain visible beside effective readings.</p>}
    {mode === 'hardware' && <p className="debug-banner">Hardware pulses bypass soil eligibility, enabled state, pause and fault gates. The real tank cutoff, timed cutoff and one-relay limit remain active. Faults and pauses are preserved.</p>}
    {snapshot?.session.automationHeld && mode === 'observe' && <div className="debug-resume"><span>Automation remains held after testing, including across controller restarts.</span><button className="garden-button" disabled={!debug.canStart || !!debug.busy} onClick={() => void perform(() => debug.action('debug.automation.resume', {}))}>Resume normal automation</button></div>}
    {(localError || debug.error) && <div className="garden-error" role="alert"><span>{localError || debug.error}</span><button className="garden-button" onClick={() => void debug.refresh()}>Refresh diagnostics</button></div>}
    {debug.notice && <p className="garden-notice" role="status">{debug.notice}</p>}
    <div className="debug-zone-selector" role="group" aria-label="Diagnostic zone">{(snapshot?.zones.length ? snapshot.zones : device.zones).map(item => <button className={item.id === zoneId ? 'is-selected' : ''} key={item.id} aria-pressed={item.id === zoneId} onClick={() => onZoneChange(item.id)}><strong>{zoneLabel(device, item.id)}</strong><small>Zone {item.id} · {snapshot?.hardware.sensorPins[item.id - 1] ?? [34, 35, 36, 39][item.id - 1]} ADC / {snapshot?.hardware.relayPins[item.id - 1] ?? [27, 26, 25, 33][item.id - 1]} relay</small></button>)}</div>
    <section className="debug-panel" aria-labelledby="debug-pipeline-heading"><div className="debug-panel-heading"><div><p className="garden-eyebrow">{zoneLabel(device, zoneId)} · one correlated observation</p><h2 id="debug-pipeline-heading">From pin to watering decision</h2></div><button className="garden-button" disabled={disabled} onClick={() => void perform(() => debug.action('debug.sample', {}))}>Sample now</button></div><Pipeline zone={zone} supported={debug.supported} />{snapshot?.timings && <p className="garden-caption">Controller loop {formatValue(snapshot.timings.loopDurationUs, ' µs')} · peak controller loop {formatValue(snapshot.timings.maxLoopDurationUs, ' µs')} · ADC sampling {formatValue(snapshot.timings.sampleDurationUs, ' µs')}</p>}</section>
    <div className="debug-main-grid"><DiagnosticsWiring device={device} snapshot={snapshot ?? null} zoneId={zoneId} onZoneChange={onZoneChange} /><div className="debug-side-panels"><DiagnosticsTrace samples={debug.trace} zoneId={zoneId} />
      <section className="debug-panel" aria-labelledby="debug-input-heading"><div className="debug-panel-heading"><div><p className="garden-eyebrow">Real and effective inputs</p><h2 id="debug-input-heading">Input simulation</h2></div><span className="debug-tag">Simulation only</span></div><dl className="debug-metrics debug-input-values"><div><dt>Physical soil ADC</dt><dd>{formatValue(zone?.physicalRaw)}</dd></div><div><dt>Effective soil ADC</dt><dd>{formatValue(zone?.raw)}{zone?.overrideRaw != null && <span className="debug-injection"> Injected</span>}</dd></div><div><dt>Physical tank</dt><dd>{snapshot?.tank.physicalLow == null ? 'Unavailable' : snapshot.tank.physicalLow ? 'Low / LOW' : 'Ready / HIGH'}</dd></div><div><dt>Effective tank / debounced</dt><dd>{snapshot?.tank.effectiveLow == null ? 'Unavailable' : snapshot.tank.effectiveLow ? 'Low' : 'Ready'} / {snapshot?.tank.low == null ? 'Unavailable' : snapshot.tank.low ? 'low' : 'ready'}{snapshot?.tank.overrideLow != null && <span className="debug-injection"> Injected</span>}</dd></div><div><dt>Pending tank / debounce left</dt><dd>{snapshot?.tank.pendingLow == null ? 'Unavailable' : snapshot.tank.pendingLow ? 'Low' : 'Ready'} / {formatValue(snapshot?.tank.debounceRemainingMs, ' ms')}</dd></div></dl>
        <form className="debug-control-form" onSubmit={event => { event.preventDefault(); if (inputValid) void perform(() => debug.action('debug.input.set', { input: 'soil', zoneId, raw: Number(raw) })); }}><label>Soil ADC override<input type="number" min="0" max="4095" step="1" value={raw} onChange={event => setRaw(event.target.value)} disabled={disabled || !simulation} /></label><button className="garden-button" disabled={disabled || !simulation || !inputValid}>Inject soil ADC</button><button type="button" className="garden-button garden-button-quiet" disabled={!debug.canStop || !debug.ownSession || !simulation || running} onClick={() => void perform(() => debug.action('debug.input.clear', { input: 'soil', zoneId }))}>Release soil</button></form>
        <div className="debug-control-form"><label>Tank input override<select value={tankValue} onChange={event => setTankValue(event.target.value)} disabled={disabled || !simulation}><option value="low">Low / switch closed</option><option value="ready">Ready / switch open</option></select></label><button className="garden-button" disabled={disabled || !simulation} onClick={() => void perform(() => debug.action('debug.input.set', { input: 'tank', low: tankValue === 'low' }))}>Inject tank state</button><button className="garden-button garden-button-quiet" disabled={!debug.canStop || !debug.ownSession || !simulation || running} onClick={() => void perform(() => debug.action('debug.input.clear', { input: 'tank' }))}>Release tank</button></div><p className="garden-caption">Overrides enter before ADC filtering/validation and tank debounce. They change software inputs; they do not drive voltage onto input pins.</p>
      </section>
    </div></div>
    <div className="debug-two-columns"><section className="debug-panel" aria-labelledby="debug-output-heading"><div className="debug-panel-heading"><div><p className="garden-eyebrow">Bounded output testing</p><h2 id="debug-output-heading">{simulation ? 'Virtual relay pulse' : 'Relay pulse'}</h2></div><span className="debug-tag">GPIO {snapshot?.hardware.relayPins[zoneId - 1] ?? [27, 26, 25, 33][zoneId - 1]}</span></div><p className="garden-caption">{zoneLabel(device, zoneId)} · 100–60000 ms. {simulation ? 'Virtual output only; the physical relay remains LOW.' : 'Physical output command; water flow and pump current are not measured.'}</p><form className="debug-control-form" onSubmit={event => { event.preventDefault(); if (outputValid) void perform(() => debug.action('debug.output.pulse', { zoneId, durationMs: Number(duration) })); }}><label>Relay duration (ms)<input type="number" min="100" max="60000" step="100" value={duration} disabled={disabled} onChange={event => setDuration(event.target.value)} /></label><button className="garden-button garden-button-primary" disabled={disabled || !outputValid || (!simulation && snapshot?.tank.physicalLow !== false)}>Run pulse</button><button type="button" className="garden-button" disabled={!debug.canStop} onClick={() => void perform(async () => { sequenceAbort.current?.abort(); await debug.action('debug.output.stop', { zoneId }, true); if (debug.ownSession && simulation) await latest.current.action('debug.input.clear', { input: 'all' }, true); })}>Stop selected relay</button></form><dl className="debug-metrics"><div><dt>Physical output</dt><dd>{zone?.relayOn ? 'HIGH · on' : 'LOW · off'}</dd></div><div><dt>Virtual output</dt><dd>{zone?.virtualRelayOn ? 'On' : 'Off'}</dd></div><div><dt>Reported pulse remaining</dt><dd>{formatValue(zone?.pulseRemainingMs, ' ms')}</dd></div><div><dt>Real tank cutoff</dt><dd>{snapshot?.tank.physicalLow == null ? 'Unknown' : snapshot.tank.physicalLow ? 'Blocking physical pulses' : 'Ready'}</dd></div></dl></section>
      <section className="debug-panel" aria-labelledby="debug-display-heading"><div className="debug-panel-heading"><div><p className="garden-eyebrow">Display and bus</p><h2 id="debug-display-heading">OLED & I²C</h2></div><span className="debug-tag">{snapshot?.hardware.oled.available === true ? 'OLED detected' : snapshot?.hardware.oled.available === false ? 'OLED not detected' : 'OLED status unavailable'}</span></div><div className="debug-control-form"><label>OLED pattern<select value={oledPattern} disabled={disabled} onChange={event => setOledPattern(event.target.value)}><option value="checkerboard">Checkerboard</option><option value="text">Text</option><option value="off">Off</option></select></label><label>OLED duration (ms)<input type="number" min="100" max="10000" step="100" value={oledDuration} disabled={disabled} onChange={event => setOledDuration(event.target.value)} /></label><button className="garden-button" disabled={disabled || !oledValid || !snapshot?.hardware.oled.available} onClick={() => void perform(() => debug.action('debug.oled.test', { pattern: oledPattern, durationMs: Number(oledDuration) }))}>Test OLED</button></div><button className="garden-button" disabled={disabled} onClick={() => void perform(() => debug.action('debug.i2c.scan', {}))}>Scan I²C addresses</button><p className="garden-caption">Bounded scan on SDA {snapshot?.hardware.oled.sda ?? 21} / SCL {snapshot?.hardware.oled.scl ?? 22}. Results appear in the command trace. Display returns to normal after the test or session end.</p></section></div>
    <section className="debug-panel" aria-labelledby="debug-scenario-heading"><div className="debug-panel-heading"><div><p className="garden-eyebrow">Repeatable test sequences</p><h2 id="debug-scenario-heading">Scenarios</h2></div><span className="debug-tag">Bound to current boot & session</span></div><p className="garden-caption">Dry → wet holds each endpoint for 35 seconds to cover default startup/confirmation samples and filter settling. Pulse and soak timers keep real time. Existing enabled, pause, fault and tank settings still govern virtual automatic watering; the sequence stops outputs on completion.</p><div className="debug-scenario-presets" role="group" aria-label="Scenario presets">{[['drywet', 'Dry → wet'], ['noisy', 'Noisy sensor'], ['invalid', 'Invalid sensor'], ['tank', 'Tank low → ready'], ['pulse', 'Pulse / pause']].map(([value, label]) => <button className="garden-button" key={value} disabled={running || (value !== 'pulse' && !simulation)} onClick={() => { setSequence(JSON.stringify(scenarioPreset(value, zoneId, zone), null, 2)); setProgress(''); }}>{label}</button>)}</div><label className="debug-json-editor">Editable sequence (JSON)<textarea rows={8} value={sequence} onChange={event => setSequence(event.target.value)} disabled={running} spellCheck={false} /></label><div className="debug-scenario-footer"><span className="garden-caption">{scenario.error || `${scenario.steps.length} validated steps. Timed actions wait for completion; finish releases injections and stops outputs.`}</span><button className="garden-button garden-button-primary" disabled={disabled || !!scenario.error} onClick={() => void runScenario()}>Run sequence</button><button className="garden-button" disabled={!running || !debug.canStop} onClick={() => void perform(stopTests)}>Abort sequence</button></div>{progress && <p className="garden-notice" role="status">{progress}</p>}</section>
    <details className="debug-panel debug-console"><summary><span><strong>Advanced command console</strong><small>Supported commands with validated JSON parameters</small></span></summary><form onSubmit={event => { event.preventDefault(); if (!commandError) void perform(() => debug.action(command, commandParameters)); }}><label>Command<select value={command} onChange={event => { const next = event.target.value as DebugCommand; setCommand(next); setParameters(JSON.stringify({ ...commandExamples[next], ...('zoneId' in commandExamples[next] ? { zoneId } : {}) }, null, 2)); }}>{Object.keys(commandExamples).map(item => <option value={item} key={item}>{item}</option>)}</select></label><label className="debug-json-editor">Command parameters (JSON)<textarea rows={4} value={parameters} onChange={event => setParameters(event.target.value)} spellCheck={false} /></label><div className="debug-scenario-footer"><span className="garden-caption">{commandError || 'Parameters validated. Boot, expiry and session are bound by the server.'}</span><button className="garden-button" disabled={!!commandError || !!debug.busy || running || (command === 'debug.automation.resume' ? !debug.canStart || !snapshot?.session.automationHeld : ['debug.output.stop', 'debug.input.clear'].includes(command) ? !debug.canStop : !debug.canAct)}>Send command</button></div></form></details>
    <section className="debug-panel" aria-labelledby="debug-audit-heading"><div className="debug-panel-heading"><div><p className="garden-eyebrow">Acceptance → execution → completion</p><h2 id="debug-audit-heading">Command trace</h2></div><label className="debug-audit-filter">Filter trace<input value={auditFilter} onChange={event => setAuditFilter(event.target.value)} placeholder="Command, status or request ID" /></label></div><p className="garden-caption">An accepted ACK confirms the request. Started and terminal controller events report actual execution. Unconfirmed actions require inspecting current state before another request.</p><div className="debug-audit-list">{actions.length ? actions.slice(0, 100).map(item => <details key={`${item.id}-${item.recordType ?? ''}-${item.status}`}><summary><time dateTime={item.updatedAt ?? item.createdAt}>{formatTime(item.updatedAt ?? item.createdAt)}</time><strong>{item.command}</strong><span className={`debug-action-status is-${item.status}`}>{item.status.replaceAll('_', ' ')}</span></summary><p>{item.message || 'No additional message.'}</p><small>Request {item.requestId} · {item.recordType ?? 'command'} · Boot {item.bootId ?? 'Unavailable'}</small><pre>{JSON.stringify({ parameters: item.parameters, result: item.result, sessionId: item.sessionId }, null, 2)}</pre></details>) : <p className="debug-empty">No matching commands or controller events.</p>}</div></section>
    <section className="debug-panel" aria-labelledby="debug-capture-heading"><div className="debug-panel-heading"><div><p className="garden-eyebrow">Correlated snapshots and commands</p><h2 id="debug-capture-heading">Captures & export</h2></div><span className="debug-tag">{debug.trace.length} live snapshots</span></div><div className="debug-control-form"><label>Capture name<input value={captureName} maxLength={80} onChange={event => setCaptureName(event.target.value)} /></label><button className="garden-button" disabled={!debug.operator || !debug.trace.length || !captureName.trim() || captureBusy} onClick={() => void saveCapture()}>Save capture</button><button className="garden-button" disabled={!debug.trace.length} onClick={() => downloadCapture(localCapture(), 'json')}>Export live JSON</button><button className="garden-button" disabled={!debug.trace.length} onClick={() => downloadCapture(localCapture(), 'csv')}>Export live CSV</button></div><p className="garden-caption">Live trace keeps up to 600 snapshots per controller boot. Saved captures retain the backend ring buffer for 7 days; command audit is retained for 30 days.</p><div className="debug-captures">{debug.data?.captures?.length ? debug.data.captures.map(capture => <div key={capture.id}><span><strong>{capture.name}</strong><small>{formatTime(capture.createdAt)} · {capture.sampleCount} snapshots</small></span><button className="garden-button garden-button-quiet" disabled={captureBusy} onClick={() => void exportSaved(capture.id, 'json')}>JSON</button><button className="garden-button garden-button-quiet" disabled={captureBusy} onClick={() => void exportSaved(capture.id, 'csv')}>CSV</button></div>) : <p className="debug-empty">No saved captures.</p>}</div></section>
  </div>;
}
