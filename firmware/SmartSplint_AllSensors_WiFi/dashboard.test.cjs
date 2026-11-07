// Run with: node dashboard.test.cjs
// Exercises the actual embedded dashboard script with mocked HTTP/DOM.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, 'SmartSplint_AllSensors_WiFi.ino'), 'utf8');
const html = source.match(/R"HTML\(([\s\S]*?)\)HTML"/)[1];
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const ids = [...html.matchAll(/id="([^"]+)"/g)].map(m => m[1]);
const imu = ok => ({ok, status: ok ? 'OK' : 'Read failed', read_errors: ok ? 0 : 4,
  accel_g: ok ? [0.01, -0.02, 0.99] : [null,null,null],
  gyro_dps: ok ? [1,2,3] : [null,null,null]});
const fixture = {hostname:'smartsplint',ssid:'project',ip:'192.168.1.42',rssi:-50,
  uptime_s:65,free_heap:120000,fsr:[{raw:4050,mv:2480,saturated:true},
  {raw:123,mv:80,saturated:false}],temperature:{c:28.5,status:'OK'},imu:[imu(true),imu(true)]};
let response = fixture, offline = false, httpError = false;
const elements = Object.fromEntries(ids.map(id => [id,{textContent:'',className:''}]));
const context = vm.createContext({document:{getElementById(id){assert.ok(id in elements,id);return elements[id];}},
  AbortController, setTimeout(){return 1;}, clearTimeout(){},
  async fetch(){if(offline)throw Error('Offline');return {ok:!httpError,status:503,json:async()=>response};}});
const tick = () => new Promise(resolve => setImmediate(resolve));
(async()=>{
  vm.runInContext(script,context); await tick();
  assert.equal(elements.temperature.textContent,'28.50 °C');
  assert.match(elements.fsrState0.textContent,/Near ADC limit/);
  assert.match(elements.imu0.textContent,/X: 0.010/);
  assert.equal(elements.conn.textContent,'Connected to project');
  response={...fixture,temperature:{c:null,status:'Not found'},imu:[imu(false),imu(true)]};
  await vm.runInContext('refresh()',context);
  assert.equal(elements.temperature.textContent,'--');
  assert.equal(elements.imu0.textContent,'--');
  assert.match(elements.imu1.textContent,/Acceleration/);
  offline=true;await vm.runInContext('refresh()',context);
  for(const id of ['temperature','fsr0','fsr1','imu0','imu1'])assert.equal(elements[id].textContent,'--');
  assert.equal(elements.conn.textContent,'Device not reachable');
  offline=false;httpError=true;await vm.runInContext('refresh()',context);
  assert.equal(elements.fsr0.textContent,'--');
  httpError=false;response=fixture;await vm.runInContext('refresh()',context);
  assert.equal(elements.temperature.textContent,'28.50 °C');
  console.log('PASS: live values, saturation, missing sensors, offline/HTTP failures, recovery');
})().catch(e=>{console.error(e);process.exitCode=1;});
