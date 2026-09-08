import { test, expect, type Page, type WebSocketRoute } from '@playwright/test';
import type { Device, TelegramDelivery } from '../app/garden-shared';

const now = new Date('2026-09-08T08:00:00Z');
const device: Device = {
  id:'test-garden', online:true, firmwareVersion:'0.5.1', bootId:'test-boot', schemaVersion:1,
  configRevision:2, automaticWateringEnabled:true, lastSeenAt:now.toISOString(), wifiRssi:-60,
  mainTankLow:false, mainTankLastChangedAt:now.toISOString(), directTelegram:true,
  telegramDebugEnabled:true, telegramConfigured:true, telegramPendingMessages:0,
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
    sockets:[] as WebSocketRoute[], deliveries:[delivery], deliveryFetches:0,
    historyFetches:0, historyFailure:false, historyGate:Promise.resolve(),
  };
  await page.clock.install({time:new Date(now.getTime()-1000)});
  await page.clock.pauseAt(now);
  await page.route('**/api/**', async route => {
    const url = new URL(route.request().url());
    const path = url.pathname;
    if (path === '/api/me') return route.fulfill({json:{authenticated:true,authMode:'development',name:'Test gardener'}});
    if (path === '/api/devices') return route.fulfill({json:[device]});
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
    socket.send(JSON.stringify({type:'snapshot',devices:[device]}));
  });
  return state;
}

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
