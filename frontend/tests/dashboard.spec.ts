import { test, expect, type Page, type WebSocketRoute } from '@playwright/test';
import type { Device, TelegramDelivery } from '../app/garden-shared';

const now = new Date('2026-09-08T08:00:00Z');
const device: Device = {
  id:'test-garden', online:true, firmwareVersion:'0.6.0', bootId:'test-boot', schemaVersion:1,
  configRevision:2, automaticWateringEnabled:true, lastSeenAt:now.toISOString(), wifiRssi:-60,
  mainTankLow:false, mainTankLastChangedAt:now.toISOString(), directTelegram:true,
  telegramDebugEnabled:true, telegramConfigured:true, telegramPendingMessages:0,
  telegramHourlySilent:false,
  telegramLastSendSucceeded:true, telegramWorkerRunning:true, telegramTimeReady:true,
  telegramLastFailureStage:null,
  zones:[{id:1, name:'Tomatoes', raw:2000, filteredRaw:2000, moisturePercent:37,
    sensorValid:true, phase:'monitoring', relayOn:false, wateringOnMsThisCycle:0,
    fault:null, lastWateredAt:null, thresholds:{startWateringPercent:27,stopWateringPercent:62}}],
};
const delivery: TelegramDelivery = {
  deviceId:device.id, eventId:'report-1', requestId:null, kind:'hourly_debug', status:'sending',
  updateSequence:2, attempt:1, pendingCount:1, httpStatus:null, errorStage:null,
  telegramErrorCode:null, telegramMessageId:null, updatedAt:now.toISOString(), sentAt:null,
};

async function garden(page: Page) {
  const state = {
    authenticated:true,
    device:{...device},
    hourlyWrites:[] as {enabled:boolean}[], hourlyError:'', hourlyGate:Promise.resolve(),
    sockets:[] as WebSocketRoute[], deliveries:[delivery], deliveryFetches:0,
    historyFetches:0, historyFailure:false, historyGate:Promise.resolve(),
    pulse:{zoneId:1,revision:2,pulseOnMs:8000,soakMs:90000,maxPulseOnMs:45000},
    pulseWrites:[] as {pulseOnMs:number;expectedRevision:number}[], pulseError:'', pulseReadError:'', pulseGate:Promise.resolve(),
    recoveryWrites:[] as {action:'clear'|'resume';expectedBootId:string;expectedRecoveryGeneration:number}[],
    recoveryError:'', recoveryGate:Promise.resolve(), recoveryReportGate:Promise.resolve(), recoveryBroadcast:true,
    recoveryNext:null as Device | null,
  };
  await page.clock.install({time:new Date(now.getTime()-1000)});
  await page.clock.pauseAt(now);
  await page.route('**/api/**', async route => {
    const url = new URL(route.request().url());
    const path = url.pathname;
    if (path === '/api/me') return route.fulfill({json:{authenticated:state.authenticated,authMode:'development',name:'Test gardener'}});
    if (path === '/api/devices') return route.fulfill({json:[state.device]});
    if (path.endsWith('/latest')) {
      await state.recoveryReportGate;
      if (state.recoveryNext) { state.device=state.recoveryNext; state.recoveryNext=null; }
      return route.fulfill({json:state.device});
    }
    if (path.endsWith('/zones/1/recovery')) {
      expect(route.request().method()).toBe('POST');
      const body = route.request().postDataJSON();
      state.recoveryWrites.push(body);
      await state.recoveryGate;
      if (state.recoveryError) return route.fulfill({status:409,json:{detail:state.recoveryError}});
      const zone = state.device.zones[0];
      if (body.expectedBootId !== state.device.bootId || body.expectedRecoveryGeneration !== zone.recoveryGeneration) {
        return route.fulfill({status:409,json:{detail:'Channel state changed. Review the current fault before trying again.'}});
      }
      const next={...state.device,zones:[{...zone,paused:body.action==='clear',fault:null,
        phase:body.action==='clear'?'paused':'stabilizing',sensorValid:body.action==='clear',relayOn:false,
        recoveryGeneration:(zone.recoveryGeneration??0)+1,wateringOnMsThisCycle:0}]};
      if (state.recoveryBroadcast) {
        state.device=next;
        state.sockets.at(-1)!.send(JSON.stringify({type:'telemetry',device:state.device}));
      } else { state.recoveryNext=next; }
      return route.fulfill({json:{zoneId:1,action:body.action,paused:body.action==='clear',status:'confirmed'}});
    }
    if (path.endsWith('/telegram/hourly-silent')) {
      expect(route.request().method()).toBe('PATCH');
      const body = route.request().postDataJSON();
      state.hourlyWrites.push(body);
      await state.hourlyGate;
      if (state.hourlyError) return route.fulfill({status:409,json:{detail:state.hourlyError}});
      state.device={...state.device,telegramHourlySilent:body.enabled};
      state.sockets.at(-1)!.send(JSON.stringify({type:'telemetry',device:state.device}));
      return route.fulfill({json:{enabled:body.enabled}});
    }
    if (path.endsWith('/zones/1/pulse')) {
      if (route.request().method() === 'GET' && state.pulseReadError) return route.fulfill({status:503,json:{detail:state.pulseReadError}});
      if (route.request().method() === 'PATCH') {
        const body = route.request().postDataJSON();
        state.pulseWrites.push(body);
        await state.pulseGate;
        if (state.pulseError) return route.fulfill({status:409,json:{detail:state.pulseError}});
        state.pulse = {...state.pulse,pulseOnMs:body.pulseOnMs,revision:state.pulse.revision+1};
      }
      return route.fulfill({json:state.pulse});
    }
    if (path.endsWith('/telegram/deliveries')) {
      state.deliveryFetches++;
      return route.fulfill({json:state.deliveries});
    }
    if (path.endsWith('/history.csv')) {
      expect(url.searchParams.get('zone_id')).toBe('1');
      expect(Date.parse(url.searchParams.get('to')!) - Date.parse(url.searchParams.get('from')!)).toBe(7*86400000);
      return route.fulfill({contentType:'text/csv',headers:{'Content-Disposition':'attachment; filename="watering-history.csv"'},
        body:'row,zone_id\n'+Array.from({length:100005},(_,index)=>`${index},1\n`).join('')});
    }
    if (path.endsWith('/history')) {
      state.historyFetches++;
      await state.historyGate;
      if (state.historyFailure) return route.fulfill({status:503,json:{detail:'Temporary history outage'}});
      const bucket = Number(url.searchParams.get('bucket_seconds'))*1000;
      const start = Math.floor(Date.parse(url.searchParams.get('from')!)/bucket)*bucket;
      const end = Date.parse(url.searchParams.get('to')!);
      return route.fulfill({json:{points:Array.from({length:Math.floor((end-start)/bucket)+1},(_,index)=>({
        timestamp:new Date(start+index*bucket).toISOString(), average:37,minimum:36,maximum:38,samples:60,
      }))}});
    }
    if (path.endsWith('/events')) return route.fulfill({json:[]});
    // Unexpected commands fail the test and never reach a real controller.
    throw new Error(`Unexpected API request: ${route.request().method()} ${path}`);
  });
  await page.routeWebSocket('**/ws/dashboard', socket => {
    state.sockets.push(socket);
    socket.send(JSON.stringify({type:'snapshot',devices:[state.device]}));
  });
  return state;
}

test('silent hourly setting saves both modes and waits for device confirmation', async ({page}) => {
  const state = await garden(page);
  let release!: () => void;
  state.hourlyGate = new Promise<void>(resolve => {release=resolve;});
  await page.goto('/?view=system');
  const toggle = page.getByRole('switch',{name:'Silent hourly updates'});
  await expect(toggle).toHaveAttribute('aria-checked','false');
  await toggle.click();
  await expect(toggle).toBeDisabled();
  await expect(toggle).toHaveAttribute('aria-checked','false');
  expect(state.hourlyWrites).toEqual([{enabled:true}]);
  release();
  await expect(toggle).toBeEnabled();
  await expect(toggle).toHaveAttribute('aria-checked','true');
  await expect(page.getByText('Hourly notification setting saved on the controller.')).toBeVisible();
  await expect(page.getByRole('switch',{name:'Telegram debug messages'})).toHaveAttribute('aria-checked','true');
  await toggle.click();
  await expect(toggle).toHaveAttribute('aria-checked','false');
  expect(state.hourlyWrites).toEqual([{enabled:true},{enabled:false}]);
});

function faultedDevice(): Device {
  return {...device,firmwareVersion:'0.7.0',telemetryReceivedAt:now.toISOString(),telemetryBootId:device.bootId,
    zones:[{...device.zones[0],fault:'maximum_watering_reached',phase:'fault',wateringOnMsThisCycle:60000,paused:false,recoveryGeneration:12}]};
}

test('clearing keeps the channel paused until a separately confirmed resume', async ({page}) => {
  const state=await garden(page);
  state.device=faultedDevice();
  let release!:()=>void;
  state.recoveryGate=new Promise<void>(resolve=>{release=resolve;});
  await page.goto('/');
  await page.getByRole('button',{name:'Clear fault',exact:true}).click();
  await expect(page.getByText('The pump will stay off. Resume watering is a separate action when you are ready.')).toBeVisible();
  await page.screenshot({path:`test-results/clear-paused-confirmation-${test.info().project.name}.png`,fullPage:true});
  await page.getByRole('button',{name:'Clear and keep paused',exact:true}).click();
  await expect(page.getByRole('button',{name:'Confirming with controller…'})).toBeDisabled();
  await expect(page.getByText(/Fault cleared\. This channel stays paused/)).toHaveCount(0);
  expect(state.recoveryWrites).toEqual([{action:'clear',expectedBootId:'test-boot',expectedRecoveryGeneration:12}]);
  release();
  await expect(page.getByText(/Fault cleared\. This channel stays paused/)).toBeVisible();
  await expect(page.getByRole('button',{name:/Tomatoes, Zone 1,.*Paused/})).toBeVisible();
  expect(state.device.zones[0].relayOn).toBe(false);
  await expect(page.getByRole('button',{name:'Resume watering',exact:true})).toBeVisible();
  await page.screenshot({path:`test-results/channel-paused-${test.info().project.name}.png`,fullPage:true});
  await page.getByRole('button',{name:'Resume watering',exact:true}).click();
  await expect(page.getByText(/The controller will check fresh sensor readings/)).toBeVisible();
  await page.getByRole('button',{name:'Confirm resume',exact:true}).click();
  await expect(page.getByText('Channel resumed — checking fresh sensor readings.')).toBeVisible();
  expect(state.recoveryWrites.at(-1)).toEqual({action:'resume',expectedBootId:'test-boot',expectedRecoveryGeneration:13});
  state.device={...state.device,zones:[{...state.device.zones[0],phase:'monitoring',sensorValid:true}]};
  state.sockets[0].send(JSON.stringify({type:'telemetry',device:state.device}));
  await expect(page.getByText('Channel resumed. Automatic monitoring is active.')).toBeVisible();
  const dimensions=await page.evaluate(()=>({width:document.documentElement.clientWidth,scroll:document.documentElement.scrollWidth}));
  expect(dimensions.scroll).toBeLessThanOrEqual(dimensions.width);
  await page.screenshot({path:`test-results/channel-recovery-${test.info().project.name}.png`,fullPage:true});
});

test('recovery waits for new telemetry as well as acknowledgment', async ({page}) => {
  const state=await garden(page);
  state.device=faultedDevice(); state.recoveryBroadcast=false;
  let release!:()=>void;
  state.recoveryReportGate=new Promise<void>(resolve=>{release=resolve;});
  await page.goto('/');
  await page.getByRole('button',{name:'Clear fault',exact:true}).click();
  await page.getByRole('button',{name:'Clear and keep paused',exact:true}).click();
  await expect(page.getByRole('button',{name:'Checking channel state…'})).toBeDisabled();
  await expect(page.getByText(/Fault cleared\. This channel stays paused/)).toHaveCount(0);
  await expect(page.getByRole('button',{name:'Resume watering',exact:true})).toHaveCount(0);
  release();
  await expect(page.getByText(/Fault cleared\. This channel stays paused/)).toBeVisible();
});

test('recovery confirmation becomes unusable when a new fault replaces it', async ({page}) => {
  const state=await garden(page); state.device=faultedDevice();
  await page.goto('/');
  await page.getByRole('button',{name:'Clear fault',exact:true}).click();
  state.device={...state.device,zones:[{...state.device.zones[0],recoveryGeneration:14}]};
  state.sockets[0].send(JSON.stringify({type:'telemetry',device:state.device}));
  await expect(page.getByRole('button',{name:'Clear and keep paused',exact:true})).toBeDisabled();
  await expect(page.getByRole('alert')).toHaveText('The channel state changed. Cancel and review its current status.');
  await page.getByRole('button',{name:'Cancel',exact:true}).click();
  expect(state.recoveryWrites).toEqual([]);
});

test('recovery failure keeps the fault visible and offers no automatic resume', async ({page}) => {
  const state=await garden(page); state.device=faultedDevice();
  state.recoveryError='Recovery could not be confirmed. Refresh the channel before trying again.';
  await page.goto('/');
  await page.getByRole('button',{name:'Clear fault',exact:true}).click();
  await page.getByRole('button',{name:'Clear and keep paused',exact:true}).click();
  await expect(page.getByRole('alert')).toHaveText(state.recoveryError);
  await expect(page.getByText('maximum_watering_reached',{exact:true})).toBeVisible();
  await expect(page.getByRole('button',{name:'Resume watering',exact:true})).toHaveCount(0);
});

test('recovery controls require supported firmware and fresh telemetry from this boot', async ({page}) => {
  const state=await garden(page); state.device={...faultedDevice(),firmwareVersion:'0.6.0'};
  await page.goto('/');
  const button=page.getByRole('button',{name:'Clear fault',exact:true});
  await expect(button).toBeDisabled();
  await expect(page.getByText('Update the controller to firmware 0.7.0 to recover a channel here.')).toBeVisible();
  for (const changed of [
    {...faultedDevice(),online:false},
    {...faultedDevice(),telemetryBootId:'previous-boot'},
    {...faultedDevice(),telemetryReceivedAt:new Date(now.getTime()-31000).toISOString()},
  ]) {
    state.device=changed;
    state.sockets[0].send(JSON.stringify({type:'telemetry',device:state.device}));
    await expect(button).toBeDisabled();
  }
  expect(state.recoveryWrites).toEqual([]);
});

test('saved pause survives dashboard reload and tank low blocks resume', async ({page}) => {
  const state=await garden(page);
  state.device={...faultedDevice(),mainTankLow:true,zones:[{...faultedDevice().zones[0],fault:null,paused:true,phase:'paused',recoveryGeneration:13}]};
  await page.goto('/');
  await expect(page.getByRole('heading',{name:'Channel paused',exact:true})).toBeVisible();
  await expect(page.getByText('Refill the main tank before resuming this channel.')).toBeVisible();
  await page.reload();
  await expect(page.getByRole('heading',{name:'Channel paused',exact:true})).toBeVisible();
  state.device={...state.device,mainTankLow:false};
  state.sockets.at(-1)!.send(JSON.stringify({type:'telemetry',device:state.device}));
  await expect(page.getByRole('button',{name:'Resume watering',exact:true})).toBeEnabled();
  expect(state.recoveryWrites).toEqual([]);
});

test('recovery confirmation expires and a later generation cannot verify an earlier action', async ({page}) => {
  const state=await garden(page); state.device=faultedDevice();
  await page.goto('/');
  await page.getByRole('button',{name:'Clear fault',exact:true}).click();
  await page.clock.runFor(65000);
  state.device={...state.device,telemetryReceivedAt:new Date(now.getTime()+65000).toISOString()};
  state.sockets[0].send(JSON.stringify({type:'telemetry',device:state.device}));
  await expect(page.getByRole('button',{name:'Clear and keep paused',exact:true})).toBeDisabled();
  await expect(page.getByRole('alert')).toHaveText('This confirmation expired. Cancel and review the channel again.');
  await page.getByRole('button',{name:'Cancel',exact:true}).click();
  expect(state.recoveryWrites).toEqual([]);
  await page.getByRole('button',{name:'Clear fault',exact:true}).click();
  state.recoveryBroadcast=false;
  let release!:()=>void;
  state.recoveryReportGate=new Promise<void>(resolve=>{release=resolve;});
  await page.getByRole('button',{name:'Clear and keep paused',exact:true}).click();
  await expect(page.getByRole('button',{name:'Checking channel state…'})).toBeDisabled();
  state.recoveryNext={...state.recoveryNext!,zones:[{...state.recoveryNext!.zones[0],recoveryGeneration:14}]};
  release();
  await expect(page.getByRole('alert')).toHaveText('The channel state changed during recovery. Review its current status before continuing.');
  await expect(page.getByText(/Fault cleared\. This channel stays paused/)).toHaveCount(0);
});

test('delayed recovery verification cannot overwrite a newer Telegram resume', async ({page}) => {
  const state=await garden(page); state.device={...faultedDevice(),telemetryId:100};
  let release!:()=>void;
  const gate=new Promise<void>(resolve=>{release=resolve;});
  await page.route('**/api/devices/test-garden/latest', async route => {
    const captured=state.device;
    await gate;
    return route.fulfill({json:captured});
  });
  await page.goto('/');
  await page.getByRole('button',{name:'Clear fault',exact:true}).click();
  await page.getByRole('button',{name:'Clear and keep paused',exact:true}).click();
  await expect(page.getByRole('button',{name:'Checking channel state…'})).toBeDisabled();
  state.device={...state.device,telemetryId:102,zones:[{...state.device.zones[0],paused:false,phase:'stabilizing',recoveryGeneration:14}]};
  state.sockets[0].send(JSON.stringify({type:'telemetry',device:state.device}));
  release();
  await expect(page.getByRole('alert')).toHaveText('The channel state changed during recovery. Review its current status before continuing.');
  await expect(page.getByRole('heading',{name:'Channel paused',exact:true})).toHaveCount(0);
  await expect(page.getByText(/Fault cleared\. This channel stays paused/)).toHaveCount(0);
  await expect(page.getByRole('button',{name:'Resume watering',exact:true})).toHaveCount(0);
});

test('failed silent setting save retains reported state and allows retry', async ({page}) => {
  const state = await garden(page);
  state.hourlyError='Could not persist setting';
  await page.goto('/?view=system');
  const toggle=page.getByRole('switch',{name:'Silent hourly updates'});
  await toggle.click();
  await expect(page.getByRole('alert')).toHaveText('Could not persist setting');
  await expect(toggle).toHaveAttribute('aria-checked','false');
  await expect(toggle).toBeEnabled();
  state.hourlyError='';
  await toggle.click();
  await expect(toggle).toHaveAttribute('aria-checked','true');
  await expect(page.getByRole('alert')).toBeHidden();
});

test('Telegram setting changes appear live and survive page reload', async ({page}) => {
  const state = await garden(page);
  await page.goto('/?view=system');
  const toggle=page.getByRole('switch',{name:'Silent hourly updates'});
  await expect(toggle).toHaveAttribute('aria-checked','false');
  state.device={...state.device,telegramHourlySilent:true};
  state.sockets[0].send(JSON.stringify({type:'telemetry',device:state.device}));
  await expect(toggle).toHaveAttribute('aria-checked','true');
  expect(state.hourlyWrites).toEqual([]);
  await page.reload();
  await expect(toggle).toHaveAttribute('aria-checked','true');
});

test('silent hourly control is disabled for older firmware and offline devices', async ({page}) => {
  const state = await garden(page);
  state.device={...state.device,firmwareVersion:'0.5.1'};
  await page.goto('/?view=system');
  const toggle=page.getByRole('switch',{name:'Silent hourly updates'});
  await expect(toggle).toBeDisabled();
  await expect(page.getByText('Update the controller to firmware 0.6.0 to use this setting.')).toBeVisible();
  state.device={...state.device,firmwareVersion:'0.6.0',online:false};
  state.sockets[0].send(JSON.stringify({type:'device.status',device:state.device}));
  await expect(toggle).toBeDisabled();
  expect(state.hourlyWrites).toEqual([]);
});

test('logout returns to sign-in and stays signed out after reload', async ({page}) => {
  const state = await garden(page);
  await page.route('**/auth/logout', route => {
    expect(route.request().method()).toBe('POST');
    state.authenticated = false;
    return route.fulfill({status:204});
  });
  await page.goto('/?view=system');
  await page.getByRole('button', {name:'Log out', exact:true}).click();
  await expect(page.getByRole('link', {name:'Continue to local preview'})).toBeVisible();
  await expect(page).toHaveURL('/');
  await expect(page.getByRole('navigation')).toBeHidden();
  await page.reload();
  await expect(page.getByRole('link', {name:'Continue to local preview'})).toBeVisible();
});

test('logout disables duplicate requests and allows retry after failure', async ({page}) => {
  const state = await garden(page);
  let release!: () => void;
  const gate = new Promise<void>(resolve => {release=resolve;});
  let requests = 0;
  await page.route('**/auth/logout', async route => {
    requests++;
    if (requests === 1) {
      await gate;
      return route.fulfill({status:503});
    }
    state.authenticated = false;
    return route.fulfill({status:204});
  });
  await page.goto('/');
  await page.getByRole('button', {name:'Log out', exact:true}).click();
  await expect(page.getByRole('button', {name:'Logging out…'})).toBeDisabled();
  expect(requests).toBe(1);
  release();
  await expect(page.getByRole('alert')).toHaveText('Could not log out. Please try again.');
  await expect(page.getByRole('navigation')).toBeVisible();
  await page.getByRole('button', {name:'Log out', exact:true}).click();
  await expect(page.getByRole('link', {name:'Continue to local preview'})).toBeVisible();
  expect(requests).toBe(2);
});

test('refresh keeps the selected timestamp, chart and keyboard focus', async ({page}) => {
  const state = await garden(page);
  await page.goto('/?view=history');
  const slider = page.getByRole('slider');
  await expect(slider).toBeVisible();
  await slider.focus();
  await slider.press('Home');
  for (let i=0;i<10;i++) await slider.press('ArrowRight');
  const reading = await slider.getAttribute('aria-valuetext');
  let release!: () => void;
  state.historyGate = new Promise<void>(resolve => {release=resolve;});
  await page.clock.fastForward(360000);
  await expect(page.getByText('Updating readings…')).toBeVisible();
  await expect(slider).toBeFocused();
  await expect(slider).toHaveAttribute('aria-valuetext',reading!);
  release();
  await expect(page.getByText('Updating readings…')).toBeHidden();
  await expect(slider).toHaveAttribute('aria-valuetext',reading!);
  await expect(slider).toBeFocused();
});

test('failed refresh retains readings and retry recovers', async ({page}) => {
  const state = await garden(page);
  await page.goto('/?view=history');
  await expect(page.getByRole('slider')).toBeVisible();
  state.historyFailure=true;
  await page.clock.fastForward(60000);
  await expect(page.getByText('Could not refresh. Showing the previous readings.')).toBeVisible();
  await expect(page.getByRole('slider')).toBeVisible();
  state.historyFailure=false;
  await page.clock.fastForward(1000);
  await page.getByRole('button',{name:'Try again',exact:true}).click();
  await expect(page.getByText('Could not refresh. Showing the previous readings.')).toBeHidden();
  await expect(page.getByRole('slider')).toBeVisible();
});

test('history filters survive navigation, reload and back', async ({page}) => {
  await garden(page);
  await page.goto('/?view=history');
  await page.getByRole('combobox',{name:'Period',exact:true}).selectOption('7d');
  await page.getByRole('combobox',{name:'Metric',exact:true}).selectOption('raw');
  await page.getByRole('button',{name:'System',exact:true}).click();
  await page.getByRole('button',{name:'History',exact:true}).click();
  await expect(page.getByRole('combobox',{name:'Period',exact:true})).toHaveValue('7d');
  await expect(page.getByRole('combobox',{name:'Metric',exact:true})).toHaveValue('raw');
  await page.reload();
  await expect(page.getByRole('combobox',{name:'Metric',exact:true})).toHaveValue('raw');
  await page.getByRole('combobox',{name:'Period',exact:true}).selectOption('30d');
  await page.goBack();
  await expect(page.getByRole('combobox',{name:'Period',exact:true})).toHaveValue('7d');
});

test('reconnect refetches deliveries and ignores older live reports', async ({page}) => {
  const state = await garden(page);
  await page.goto('/?view=system');
  await expect(page.getByText('Latest: sending')).toBeVisible();
  state.sockets[0].send(JSON.stringify({type:'telegram.delivery',delivery}));
  state.deliveries=[{...delivery,updateSequence:3,status:'sent',sentAt:now.toISOString(),telegramMessageId:123}];
  await state.sockets[0].close({code:1012,reason:'Test reconnect'});
  await expect(page.getByText('Reconnecting',{exact:true})).toBeVisible();
  await page.clock.fastForward(2000);
  await expect(page.getByText('Latest: sent')).toBeVisible();
  state.sockets.at(-1)!.send(JSON.stringify({type:'telegram.delivery',delivery}));
  await page.getByRole('button',{name:'Overview',exact:true}).click();
  await page.getByRole('button',{name:'System',exact:true}).click();
  await expect(page.getByText('Latest: sent')).toBeVisible();
  await expect(page.getByText('Message 123')).toBeVisible();
  expect(state.deliveryFetches).toBeGreaterThanOrEqual(2);
});

test('CSV download retains every supplied row and the selected range', async ({page}) => {
  await garden(page);
  await page.goto('/?view=history&period=7d');
  await expect(page.getByRole('slider')).toBeVisible();
  const downloadEvent=page.waitForEvent('download');
  await page.getByRole('link',{name:'Export CSV'}).click();
  const download=await downloadEvent;
  expect(await download.failure()).toBeNull();
  const stream=await download.createReadStream();
  const chunks:Buffer[]=[];
  for await (const chunk of stream!) chunks.push(Buffer.from(chunk));
  const lines=Buffer.concat(chunks).toString().trim().split('\n');
  expect(lines).toHaveLength(100006);
  expect(lines.at(-1)).toBe('100004,1');
});

test('shows actual thresholds and last report on every viewport', async ({page}) => {
  await garden(page);
  await page.goto('/?view=history');
  await expect(page.getByText('Start at 27% or lower')).toBeVisible();
  await expect(page.getByText('Stop at 62%')).toBeVisible();
  await expect(page.getByText(/Last report/)).toBeVisible();
  await expect(page.getByText('30% reference')).toHaveCount(0);
  const dimensions=await page.evaluate(()=>({width:document.documentElement.clientWidth,scroll:document.documentElement.scrollWidth}));
  expect(dimensions.scroll).toBeLessThanOrEqual(dimensions.width);
});

test('pulse editor reads the device and confirms saving only after its response', async ({page}) => {
  const state = await garden(page);
  await page.goto('/');
  await page.getByRole('button',{name:'Configure pulse'}).click();
  const input = page.getByRole('spinbutton',{name:'Pulse duration (seconds)'});
  await expect(input).toHaveValue('8');
  await page.clock.runFor(20);
  await expect(input).toBeFocused();
  await expect(page.getByText('Saving stops any watering in progress on this controller and restarts its watering checks.')).toBeVisible();
  await expect(page.getByRole('button',{name:'Save pulse',exact:true})).toBeDisabled();
  await input.fill('46');
  await expect(page.getByRole('button',{name:'Save pulse',exact:true})).toBeDisabled();
  await input.fill('12.5');
  let release!: () => void;
  state.pulseGate = new Promise<void>(resolve => {release=resolve;});
  await page.getByRole('button',{name:'Save pulse',exact:true}).click();
  await expect(page.getByRole('button',{name:'Confirming with controller…'})).toBeDisabled();
  await expect(page.getByText(/Pulse saved on controller/)).toHaveCount(0);
  release();
  await expect(page.getByText('Pulse saved on controller: 12.5 seconds. This setting is kept after a restart.')).toBeVisible();
  expect(state.pulseWrites).toEqual([{pulseOnMs:12500,expectedRevision:2}]);
  await page.clock.runFor(20);
  await expect(page.getByRole('button',{name:'Configure pulse'})).toBeFocused();
  await page.getByRole('button',{name:'Configure pulse'}).click();
  await expect(input).toHaveValue('12.5');
  const dimensions=await page.evaluate(()=>({width:document.documentElement.clientWidth,scroll:document.documentElement.scrollWidth}));
  expect(dimensions.scroll).toBeLessThanOrEqual(dimensions.width);
  await page.screenshot({path:`test-results/pulse-editor-${test.info().project.name}.png`,fullPage:true});
});

test('pulse rejection preserves draft and requires current settings before retry', async ({page}) => {
  const state = await garden(page);
  await page.goto('/');
  await page.getByRole('button',{name:'Configure pulse'}).click();
  const input = page.getByRole('spinbutton',{name:'Pulse duration (seconds)'});
  await input.fill('10');
  state.pulseError='Settings changed since you opened this editor. Reload settings before saving.';
  await page.getByRole('button',{name:'Save pulse',exact:true}).click();
  await expect(page.getByRole('alert')).toHaveText(state.pulseError);
  await expect(input).toHaveValue('10');
  await expect(page.getByRole('button',{name:'Save pulse',exact:true})).toBeDisabled();
  await expect(page.getByText(/Pulse saved on controller/)).toHaveCount(0);
  state.pulse={...state.pulse,revision:3,pulseOnMs:9000};
  state.pulseError='';
  await page.getByRole('button',{name:'Reload settings'}).click();
  await expect(input).toHaveValue('9');
  await input.fill('11');
  await page.getByRole('button',{name:'Save pulse',exact:true}).click();
  await expect(page.getByText(/Pulse saved on controller: 11 seconds/)).toBeVisible();
  expect(state.pulseWrites.at(-1)).toEqual({pulseOnMs:11000,expectedRevision:3});
});

test('failed pulse reads offer retry without assuming a default', async ({page}) => {
  const state = await garden(page);
  state.pulseReadError='Controller is unavailable. Reconnect and reload settings.';
  await page.goto('/');
  await page.getByRole('button',{name:'Configure pulse'}).click();
  await expect(page.getByRole('alert')).toHaveText(state.pulseReadError);
  await expect(page.getByRole('spinbutton',{name:'Pulse duration (seconds)'})).toHaveCount(0);
  state.pulseReadError='';
  await page.getByRole('button',{name:'Reload settings'}).click();
  await expect(page.getByRole('spinbutton',{name:'Pulse duration (seconds)'})).toHaveValue('8');
  await page.getByRole('button',{name:'Cancel',exact:true}).click();
  expect(state.pulseWrites).toEqual([]);
});

test('pulse editing is blocked when the controller disconnects and cancelling sends no change', async ({page}) => {
  const state = await garden(page);
  await page.goto('/');
  await page.getByRole('button',{name:'Configure pulse'}).click();
  const input = page.getByRole('spinbutton',{name:'Pulse duration (seconds)'});
  await input.fill('10');
  state.sockets[0].send(JSON.stringify({type:'device.status',device:{...device,online:false}}));
  await expect(page.getByRole('button',{name:'Save pulse',exact:true})).toBeDisabled();
  await expect(page.getByText('Connect to the controller to change its pulse.')).toBeVisible();
  await page.getByRole('button',{name:'Cancel',exact:true}).click();
  await expect(page.getByRole('button',{name:'Configure pulse'})).toBeDisabled();
  expect(state.pulseWrites).toEqual([]);
});
