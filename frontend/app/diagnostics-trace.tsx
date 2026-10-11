'use client';

import { useId, useState } from 'react';
import { formatValue, type TraceSample } from './diagnostics-shared';

export default function DiagnosticsTrace({ samples, zoneId }: { samples: TraceSample[]; zoneId: number }) {
  const [frozen, setFrozen] = useState<TraceSample[] | null>(null);
  const [selected, setSelected] = useState<number | null>(null);
  const id = useId();
  const trace = (frozen ?? samples).slice(-300);
  const points = trace.map(sample => ({ ...sample, zone: sample.snapshot.zones.find(zone => zone.id === zoneId) })).filter(point => !!point.zone);
  const width = 650, height = 200, left = 48, right = 42, top = 15, bottom = 34;
  const from = points.length ? Date.parse(points[0].receivedAt) : 0;
  const to = points.length ? Math.max(from + 1000, Date.parse(points.at(-1)!.receivedAt)) : 1000;
  const x = (time: string) => left + (Date.parse(time) - from) / (to - from) * (width - left - right);
  const y = (value: number) => top + (1 - Math.max(0, Math.min(4095, value)) / 4095) * (height - top - bottom);
  const line = (key: 'physicalRaw' | 'raw' | 'filteredRaw' | 'relativeMoisturePercent') => {
    let penDown = false;
    return points.map(point => {
      const value = point.zone?.[key];
      const valid = value != null && Number.isFinite(value) && (key === 'physicalRaw' || key === 'raw' || point.zone?.sensorValid);
      if (!valid) { penDown = false; return ''; }
      const segment = `${penDown ? 'L' : 'M'}${x(point.receivedAt)},${y(key === 'relativeMoisturePercent' ? value * 4095 / 100 : value)}`;
      penDown = true; return segment;
    }).join(' ');
  };
  const index = selected == null ? points.length - 1 : Math.min(selected, points.length - 1);
  const point = points[index];
  const time = (value: string) => new Intl.DateTimeFormat(undefined, { hour: '2-digit', minute: '2-digit', second: '2-digit' }).format(new Date(value));
  return <section className="debug-panel debug-trace" aria-labelledby="debug-trace-heading">
    <div className="debug-panel-heading"><div><p className="garden-eyebrow">Unbucketed live data</p><h2 id="debug-trace-heading">Signal trace</h2></div><button className="garden-button garden-button-quiet" onClick={() => { setFrozen(frozen ? null : samples.slice()); setSelected(null); }}>{frozen ? 'Resume live chart' : 'Freeze chart'}</button></div>
    <p className="garden-caption">Last {points.length} correlated snapshots, up to 300. ADC counts use the left scale; moisture uses the right. Invalid derived readings leave a gap.{frozen ? ' Only the chart is frozen; controller state and controls stay live.' : ''}</p>
    {points.length ? <>
      <svg viewBox={`0 0 ${width} ${height}`} className="debug-trace-chart" role="img" aria-label="Live physical ADC, effective ADC, filtered ADC and relative moisture traces" preserveAspectRatio="xMidYMid meet">
        {[0, 1024, 2048, 3072, 4095].map(value => <g key={value}><line className="garden-chart-grid" x1={left} x2={width - right} y1={y(value)} y2={y(value)} /><text x={left - 8} y={y(value) + 4} textAnchor="end">{value}</text><text x={width - right + 8} y={y(value) + 4}>{Math.round(value / 4095 * 100)}%</text></g>)}
        <path className="debug-line-physical" d={line('physicalRaw')} /><path className="debug-line-effective" d={line('raw')} /><path className="debug-line-filtered" d={line('filteredRaw')} /><path className="debug-line-moisture" d={line('relativeMoisturePercent')} />
        {points.length === 1 && (['physicalRaw', 'raw', 'filteredRaw', 'relativeMoisturePercent'] as const).map((key, series) => {
          const value = points[0].zone?.[key];
          if (value == null || !Number.isFinite(value) || (series >= 2 && !points[0].zone?.sensorValid)) return null;
          return <circle key={key} cx={x(points[0].receivedAt)} cy={y(key === 'relativeMoisturePercent' ? value * 4095 / 100 : value)} r={3} fill={['#779588', '#3464ad', '#285f37', '#b57a28'][series]} />;
        })}
        {point && <line className="garden-chart-guide" x1={x(point.receivedAt)} x2={x(point.receivedAt)} y1={top} y2={height - bottom} />}
        <text x={left} y={height - 8}>{time(points[0].receivedAt)}</text><text x={width - right} y={height - 8} textAnchor="end">{time(points.at(-1)!.receivedAt)}</text>
      </svg>
      <div className="debug-trace-legend"><span><i className="debug-line-physical" />Physical ADC</span><span><i className="debug-line-effective" />Effective ADC</span><span><i className="debug-line-filtered" />Filtered</span><span><i className="debug-line-moisture" />Moisture %</span></div>
      <label className="debug-reading" htmlFor={`${id}-sample`}>Inspect snapshot <span>{point ? `${time(point.receivedAt)} · sequence ${point.snapshot.sequence ?? 'unavailable'}` : 'Unavailable'}</span></label>
      <input id={`${id}-sample`} type="range" min={0} max={Math.max(0, points.length - 1)} value={Math.max(0, index)} onChange={event => setSelected(Number(event.target.value))} aria-valuetext={point ? `Sequence ${point.snapshot.sequence}, physical ${formatValue(point.zone?.physicalRaw)}, effective ${formatValue(point.zone?.raw)}, filtered ${formatValue(point.zone?.filteredRaw)}, moisture ${formatValue(point.zone?.relativeMoisturePercent, '%')}` : 'Unavailable'} />
      {point && <div className="debug-inspected-values"><span>Physical <strong>{formatValue(point.zone?.physicalRaw)}</strong></span><span>Effective <strong>{formatValue(point.zone?.raw)}</strong></span><span>Filtered <strong>{point.zone?.sensorValid ? formatValue(point.zone.filteredRaw) : 'Last valid value'}</strong></span><span>Moisture <strong>{point.zone?.sensorValid ? formatValue(point.zone.relativeMoisturePercent, '%') : 'Last valid value'}</strong></span></div>}
    </> : <p className="debug-empty">Waiting for the first correlated snapshot.</p>}
  </section>;
}
