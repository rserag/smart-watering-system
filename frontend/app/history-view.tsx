'use client';

import { useEffect, useId, useRef, useState, useMemo } from 'react';
import { type Device, type Zone, type HistoryPoint, type WateringEvent, fetchJson, formatDuration, formatTime, historyRange, zoneName } from './garden-shared';

const metrics: Record<string, {label: string; unit: string}> = {
  moisturePercent: {label:'Soil moisture', unit:'%'},
  filteredRaw: {label:'Filtered sensor', unit:'counts'},
  raw: {label:'Raw sensor', unit:'counts'},
  wateringOnMs: {label:'Cycle watering time', unit:'ms'},
};

function HistoryChart({points, events, range, metric, thresholds}: {points: HistoryPoint[]; events: WateringEvent[]; range: ReturnType<typeof historyRange>; metric: string; thresholds: Zone['thresholds']}) {
  const container = useRef<HTMLDivElement>(null);
  const [width, setWidth] = useState(640);
  const [focused, setFocused] = useState<string | null>(null);
  const clipId = useId();
  useEffect(() => {
    const node = container.current;
    if (!node) return;
    const observer = new ResizeObserver(entries => setWidth(Math.max(200, entries[0].contentRect.width)));
    observer.observe(node);
    return () => observer.disconnect();
  }, []);
  const height = 280, left = metric === 'wateringOnMs' ? 66 : 48, right = 14, top = 24, bottom = 48;
  const values = points.flatMap(point => [point.minimum, point.maximum, point.average]);
  const low = Math.min(...values), high = Math.max(...values);
  const padding = Math.max((high - low) * 0.1, 1);
  const min = metric === 'moisturePercent' ? Math.min(0, low) : Math.max(0, low - padding);
  const max = metric === 'moisturePercent' ? Math.max(100, high) : high + padding;
  const x = (date: string | number) => left + ((typeof date === 'number' ? date : Date.parse(date)) - range.from.getTime()) / (range.to.getTime() - range.from.getTime()) * (width - left - right);
  const y = (value: number) => top + (max - value) / Math.max(max - min, 1) * (height - top - bottom);
  const line = points.map((point, index) => `${index ? 'L' : 'M'}${x(point.timestamp)},${y(point.average)}`).join(' ');
  const band = points.map((point, index) => `${index ? 'L' : 'M'}${x(point.timestamp)},${y(point.maximum)}`).join(' ') + ' ' + [...points].reverse().map(point => `L${x(point.timestamp)},${y(point.minimum)}`).join(' ') + ' Z';
  const pointIndex = focused === null ? points.length - 1 : points.reduce((closest, candidate, index) =>
    Math.abs(Date.parse(candidate.timestamp) - Date.parse(focused)) < Math.abs(Date.parse(points[closest].timestamp) - Date.parse(focused)) ? index : closest, 0);
  const point = points[pointIndex];
  const tickCount = width < 500 ? 3 : 5;
  const unit = metrics[metric].unit;
  const number = (value: number) => new Intl.NumberFormat(undefined, {maximumFractionDigits:1, notation:Math.abs(value) >= 10000 ? 'compact' : 'standard'}).format(value);

  return <div ref={container} className="garden-chart">
    <svg viewBox={`0 0 ${width} ${height}`} width="100%" height={height} role="img" aria-label={`${metrics[metric].label} from ${formatTime(range.from.toISOString())} to ${formatTime(range.to.toISOString())}. Use the reading selector below for exact values.`} onPointerMove={event => {
      const bounds = event.currentTarget.getBoundingClientRect();
      const pointer = (event.clientX - bounds.left) * width / bounds.width;
      let closest = 0;
      points.forEach((candidate, index) => { if (Math.abs(x(candidate.timestamp) - pointer) < Math.abs(x(points[closest].timestamp) - pointer)) closest = index; });
      setFocused(points[closest].timestamp);
    }}>
      <defs><clipPath id={clipId}><rect x={left} y={top} width={width-left-right} height={height-top-bottom} /></clipPath></defs>
      <text x={left} y={14} className="garden-axis-unit">{unit === '%' ? 'Moisture (%)' : unit === 'ms' ? 'Time (ms)' : 'Sensor counts'}</text>
      {[0, .25, .5, .75, 1].map(tick => <g key={tick}><line className="garden-chart-grid" x1={left} x2={width-right} y1={y(min+(max-min)*tick)} y2={y(min+(max-min)*tick)} /><text x={left-9} y={y(min+(max-min)*tick)+4} textAnchor="end">{number(min+(max-min)*tick)}</text></g>)}
      <g clipPath={`url(#${clipId})`}>
        <path className="garden-history-range" d={band} />
        {events.map(event => <rect className="garden-event-band" key={event.id} x={x(event.startedAt)} y={top} width={Math.max(3,x(event.endedAt ?? range.to.toISOString())-x(event.startedAt))} height={height-top-bottom}><title>{formatTime(event.startedAt)} · Watering · {formatDuration(event.durationMs)}</title></rect>)}
        {metric === 'moisturePercent' && thresholds && <>
          <line className="garden-threshold" x1={left} x2={width-right} y1={y(thresholds.startWateringPercent)} y2={y(thresholds.startWateringPercent)} />
          <line className="garden-threshold garden-stop-threshold" x1={left} x2={width-right} y1={y(thresholds.stopWateringPercent)} y2={y(thresholds.stopWateringPercent)} />
        </>}
        <path className="garden-history-line" d={line} />
        <line className="garden-chart-guide" x1={x(point.timestamp)} x2={x(point.timestamp)} y1={top} y2={height-bottom} />
        <circle className="garden-chart-point" cx={x(point.timestamp)} cy={y(point.average)} r={4} />
      </g>
      {Array.from({length:tickCount},(_,index) => {
        const date = range.from.getTime()+(range.to.getTime()-range.from.getTime())*index/(tickCount-1);
        return <text key={index} x={x(date)} y={height-bottom+24} textAnchor={index === 0 ? 'start' : index === tickCount-1 ? 'end' : 'middle'}>{new Intl.DateTimeFormat(undefined, range.bucket === 300 ? {hour:'2-digit',minute:'2-digit'} : {month:'short',day:'numeric'}).format(new Date(date))}</text>;
      })}
    </svg>
    <div className="garden-chart-legend"><span><i className="garden-legend-line" />Average</span><span><i className="garden-legend-band" />Minimum–maximum</span>{events.length > 0 && <span><i className="garden-legend-event" />Watering</span>}{metric === 'moisturePercent' && thresholds && <><span><i className="garden-legend-reference" />Start below {thresholds.startWateringPercent}%</span><span><i className="garden-legend-reference garden-stop-reference" />Stop at {thresholds.stopWateringPercent}%</span></>}</div>
    {metric === 'moisturePercent' && <p className="garden-caption garden-threshold-note">{thresholds ? 'Current controller thresholds; historical settings may differ.' : 'Controller thresholds are not available yet.'}</p>}
    <label className="garden-reading-label" htmlFor={`${clipId}-reading`}>Inspect a reading <span>{formatTime(point.timestamp)} · <strong>{number(point.average)} {unit}</strong></span></label>
    <input className="garden-reading-slider" id={`${clipId}-reading`} type="range" min={0} max={Math.max(0,points.length-1)} value={pointIndex} onChange={event => setFocused(points[Number(event.target.value)].timestamp)} aria-valuetext={`${formatTime(point.timestamp)}, average ${number(point.average)} ${unit}, minimum ${number(point.minimum)}, maximum ${number(point.maximum)}`} />
  </div>;
}

export default function HistoryView({device, zoneId, onZoneChange, period, metric, onFilterChange}: {
  device:Device; zoneId:number; onZoneChange:(zone:number)=>void; period:string; metric:string;
  onFilterChange:(filters:{period?:string; metric?:string})=>void;
}) {
  const [refreshAt,setRefreshAt] = useState(() => Date.now());
  const range = useMemo(() => historyRange(period, new Date(refreshAt)), [period,refreshAt]);
  const [result,setResult] = useState<{key:string; range:ReturnType<typeof historyRange>; points:HistoryPoint[]; events:WateringEvent[]} | null>(null);
  const [failure,setFailure] = useState<{key:string; message:string} | null>(null);
  const key = `${device.id}/${zoneId}/${metric}/${period}`;
  useEffect(() => { const timer = setInterval(() => setRefreshAt(Date.now()),60000); return () => clearInterval(timer); },[period]);
  useEffect(() => {
    const controller = new AbortController();
    const query = new URLSearchParams({from:range.from.toISOString(), to:range.to.toISOString(), zone_id:String(zoneId), metric, bucket_seconds:String(range.bucket)});
    const eventQuery = new URLSearchParams({from:range.from.toISOString(), to:range.to.toISOString(), zone_id:String(zoneId)});
    Promise.all([
      fetchJson<{points:HistoryPoint[]}>(`devices/${encodeURIComponent(device.id)}/history?${query}`,{signal:controller.signal}),
      fetchJson<WateringEvent[]>(`devices/${encodeURIComponent(device.id)}/events?${eventQuery}`,{signal:controller.signal}),
    ]).then(([history,events]) => { if (!controller.signal.aborted) { setResult({key,range,points:history.points,events}); setFailure(null); } }).catch(reason => { if (!controller.signal.aborted) setFailure({key,message:reason.message}); });
    return () => controller.abort();
  },[device.id,zoneId,metric,range,key]);
  const data = result?.key === key ? result : null;
  const error = failure?.key === key ? failure.message : '';
  const selectedZone = device.zones.find(zone => zone.id === zoneId);
  const displayedRange = data?.range ?? range;
  const refreshing = !!data && data.range.to.getTime() !== range.to.getTime() && !error;
  const exportQuery = new URLSearchParams({from:displayedRange.from.toISOString(),to:displayedRange.to.toISOString(),zone_id:String(zoneId)});
  return <>
    <div className="garden-history-toolbar"><label>Zone<select value={zoneId} onChange={event => onZoneChange(Number(event.target.value))}>{device.zones.map(zone => <option value={zone.id} key={zone.id}>{zoneName(zone)}{zone.name ? ` · Zone ${zone.id}` : ''}</option>)}</select></label><label>Metric<select value={metric} onChange={event => onFilterChange({metric:event.target.value})}>{Object.entries(metrics).map(([value,item]) => <option key={value} value={value}>{item.label}</option>)}</select></label><label>Period<select value={period} onChange={event => {onFilterChange({period:event.target.value});setRefreshAt(Date.now());}}><option value="24h">Last 24 hours</option><option value="7d">Last 7 days</option><option value="30d">Last 30 days</option></select></label><a className="garden-button garden-export" href={`/api/devices/${encodeURIComponent(device.id)}/history.csv?${exportQuery}`}>Export CSV</a></div>
    <section className="garden-chart-panel" aria-label="Zone history"><div className="garden-chart-heading"><div><p className="garden-eyebrow">{zoneName(selectedZone ?? {id:zoneId})}</p><h2>{metrics[metric].label}</h2></div><span className="garden-caption">{formatTime(displayedRange.from.toISOString())} – {formatTime(displayedRange.to.toISOString())}</span></div>
      {refreshing && <p className="garden-caption" role="status">Updating readings…</p>}
      {error && data && <div className="garden-error" role="alert"><span>Could not refresh. Showing the previous readings.</span><button className="garden-button" onClick={() => setRefreshAt(Date.now())}>Try again</button></div>}
      {error && !data ? <div className="garden-chart-empty" role="alert"><p>{error}</p><button className="garden-button" onClick={() => setRefreshAt(Date.now())}>Try again</button></div> : !data ? <div className="garden-chart-empty" role="status">Loading readings…</div> : data.points.length ? <HistoryChart key={key} points={data.points} events={data.events} range={displayedRange} metric={metric} thresholds={selectedZone?.thresholds} /> : <div className="garden-chart-empty">No readings in this period.</div>}
    </section>
    <section className="garden-events" aria-labelledby="watering-events-heading"><div className="garden-section-heading"><h2 id="watering-events-heading">Watering events</h2><span className="garden-caption">{zoneName(selectedZone ?? {id:zoneId})}</span></div>{data ? data.events.length ? <ul>{data.events.map(event => <li key={event.id}><time dateTime={event.startedAt}>{formatTime(event.startedAt)}</time><span><strong>{zoneName(device.zones.find(zone => zone.id === event.zoneId) ?? {id:event.zoneId})}</strong><small>{event.source} watering · {event.status}</small></span><strong>{formatDuration(event.durationMs)}</strong></li>)}</ul> : <p className="garden-events-empty">No watering events in this period.</p> : <p className="garden-events-empty">{error ? 'Watering events are unavailable.' : 'Loading watering events…'}</p>}</section>
  </>;
}
