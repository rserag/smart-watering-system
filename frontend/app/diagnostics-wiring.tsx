'use client';

import { useState } from 'react';
import type { Device } from './garden-shared';
import { formatValue, zoneLabel, type DebugSnapshot } from './diagnostics-shared';

const rightPins: [string, number | null][] = [['VIN', null], ['GND', null], ['D13', 13], ['D12', 12], ['D14', 14], ['D27', 27], ['D26', 26], ['D25', 25], ['D33', 33], ['D32', 32], ['D35', 35], ['D34', 34], ['VN', 39], ['VP', 36], ['EN', null]];
const leftPins: [string, number | null][] = [['3V3', null], ['GND', null], ['D15', 15], ['D2', 2], ['D4', 4], ['RX2', 16], ['TX2', 17], ['D5', 5], ['D18', 18], ['D19', 19], ['D21', 21], ['RX0', 3], ['TX0', 1], ['D22', 22], ['D23', 23]];

export default function DiagnosticsWiring({ device, snapshot, zoneId, onZoneChange }: { device: Device; snapshot: DebugSnapshot | null; zoneId: number; onZoneChange: (id: number) => void }) {
  const [inspection, setInspection] = useState<{ label: string; gpio: number | null } | null>(null);
  const hardware = snapshot?.hardware ?? { relayPins: [27, 26, 25, 33], sensorPins: [34, 35, 36, 39], tankPin: 23, relayActiveHigh: true, oled: { sda: 21, scl: 22, address: 60, available: false } };
  const describe = (label: string, gpio: number | null) => {
    if (gpio == null) return { role: label === 'GND' ? 'Common ground' : label === 'EN' ? 'Board enable/reset' : label === 'VIN' ? 'Board VIN supply' : '3.3 V supply', zone: null, value: 'Inspect only' };
    const relay = hardware.relayPins.indexOf(gpio), sensor = hardware.sensorPins.indexOf(gpio);
    if (relay >= 0) return { role: `${zoneLabel(device, relay + 1)} relay output`, zone: relay + 1, value: snapshot?.zones.find(z => z.id === relay + 1)?.relayOn ? 'HIGH · commanded on' : 'LOW · commanded off' };
    if (sensor >= 0) return { role: `${zoneLabel(device, sensor + 1)} soil ADC input`, zone: sensor + 1, value: `${formatValue(snapshot?.zones.find(z => z.id === sensor + 1)?.physicalRaw)} counts` };
    if (gpio === hardware.tankPin) return { role: 'Main-tank float input', zone: null, value: snapshot?.tank.physicalLow == null ? 'Unavailable' : snapshot.tank.physicalLow ? 'LOW · low water' : 'HIGH · tank ready' };
    if (gpio === hardware.oled.sda) return { role: 'OLED I²C SDA', zone: null, value: `Address 0x${hardware.oled.address.toString(16).toUpperCase().padStart(2, '0')}` };
    if (gpio === hardware.oled.scl) return { role: 'OLED I²C SCL', zone: null, value: '400 kHz bus' };
    return { role: 'Unassigned / reserved pin', zone: null, value: 'Inspect only' };
  };
  const pins = (items: [string, number | null][], side: string) => <div className={`debug-pin-header debug-pin-${side}`}>{items.map(([label, gpio], index) => {
    const info = describe(label, gpio);
    const configured = gpio != null && (hardware.relayPins.includes(gpio) || hardware.sensorPins.includes(gpio) || [hardware.tankPin, hardware.oled.sda, hardware.oled.scl].includes(gpio));
    return <button type="button" key={`${side}-${index}`} className={`${configured ? 'is-configured' : ''} ${info.zone === zoneId ? 'is-zone-selected' : ''} ${inspection?.label === label && inspection.gpio === gpio ? 'is-inspected' : ''}`} aria-label={`${label}${gpio != null ? `, GPIO ${gpio}` : ''}, ${info.role}. Inspect connection.`} onClick={() => { setInspection({ label, gpio }); if (info.zone) onZoneChange(info.zone); }}><span className="debug-pin-contact" aria-hidden="true" /><span>{label}</span><small>{gpio == null ? '—' : gpio}</small></button>;
  })}</div>;
  const inspected = inspection ? describe(inspection.label, inspection.gpio) : null;
  const zone = snapshot?.zones.find(z => z.id === zoneId);
  return <section className="debug-panel debug-wiring" aria-labelledby="debug-wiring-heading">
    <div className="debug-panel-heading"><div><p className="garden-eyebrow">Physical connections</p><h2 id="debug-wiring-heading">30-pin board & wiring</h2></div><span className="debug-tag">USB at top</span></div>
    <p className="garden-caption">Header labels follow your board photo. Connected pins follow the confirmed firmware wiring.{hardware && snapshot?.hardware.source === 'documentedFallback' ? ' Mapping from documentation; firmware has not reported it.' : ''}</p>
    <div className="debug-board" aria-label="ESP32 board, 15 header pins per side, USB-C at top and antenna at bottom">
      {pins(leftPins, 'left')}
      <div className="debug-board-body" aria-hidden="true"><div className="debug-board-usb">USB-C</div><div className="debug-board-chip"><span>ESP32</span><small>30-pin board</small></div><div className="debug-board-antenna"><i /><i /><i /><span>ANTENNA</span></div></div>
      {pins(rightPins, 'right')}
    </div>
    <div className="debug-pin-detail" aria-live="polite">{inspection && inspected ? <><strong>{inspection.label}{inspection.gpio != null ? ` / GPIO ${inspection.gpio}` : ''} · {inspected.role}</strong><span>{inspected.value}</span></> : <><strong>Select a header pin</strong><span>Highlight its channel and inspect the connection.</span></>}</div>
    <dl className="debug-wiring-values">
      <div><dt>{zoneLabel(device, zoneId)} sensor</dt><dd>GPIO {hardware.sensorPins[zoneId - 1]} · ADC input</dd></div>
      <div><dt>{zoneLabel(device, zoneId)} relay</dt><dd>GPIO {hardware.relayPins[zoneId - 1]} · {hardware.relayActiveHigh ? 'HIGH active' : 'LOW active'}</dd></div>
      <div><dt>Physical relay command</dt><dd>{zone?.relayOn ? 'On' : 'Off'}{snapshot?.session.mode === 'simulation' && <span> · virtual {zone?.virtualRelayOn ? 'on' : 'off'}</span>}</dd></div>
      <div><dt>Main-tank input</dt><dd>GPIO {hardware.tankPin} · LOW = low water</dd></div>
      <div><dt>OLED</dt><dd>SDA {hardware.oled.sda} / SCL {hardware.oled.scl} · 0x{hardware.oled.address.toString(16).toUpperCase()}</dd></div>
    </dl>
    <p className="garden-caption">Tank: 3.3 V → 5 kΩ pull-up → float signal → 1 kΩ → GPIO {hardware.tankPin}; 100 nF to GND. Float closes to GND at low water. Relay state is the GPIO command; flow and current are not measured.</p>
  </section>;
}
