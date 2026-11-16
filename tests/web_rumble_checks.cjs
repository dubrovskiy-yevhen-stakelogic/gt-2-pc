// Check the browser rumble bridge the web build compiles in (input_system_sdl.cpp).
// Usage: node tests/web_rumble_checks.cjs
const fs = require('node:fs');
const assert = require('node:assert/strict');
const source = fs.readFileSync(__dirname + '/../src/platform/input/input_system_sdl.cpp', 'utf8');
const match = source.match(/EM_JS\(int, WebRumble, \(const char\* name, int low, int high, int ms\), \{([\s\S]*?)\n\}\);/);
assert.ok(match, 'WebRumble not found in input_system_sdl.cpp');
const WebRumble = new Function('function WebRumble(name, low, high, ms) {' + match[1] + '}\nreturn WebRumble;')();

let pads = [], calls = [];
Object.defineProperty(globalThis, 'navigator', {value: {getGamepads: () => pads}});
globalThis.UTF8ToString = text => text;
const pad = (id, actuator = true) => ({id, vibrationActuator: actuator
    ? {playEffect: (type, params) => { calls.push({id, type, params}); return Promise.resolve('complete'); }} : null});

assert.equal(WebRumble('pad A', 0, 0, 150), 0, 'a missing pad must not rumble');

pads = [pad('pad A')];
assert.equal(WebRumble('pad A', 65535, 32768, 150), 1);
assert.deepEqual(calls.map(c => [c.id, c.type]), [['pad A', 'dual-rumble']]);
assert.deepEqual(calls[0].params, {startDelay: 0, duration: 150, strongMagnitude: 1, weakMagnitude: 32768 / 65535});

calls = [];
pads = [pad('pad A', false), pad('pad B')];
assert.equal(WebRumble('pad A', 65535, 0, 150), 1, 'a pad without an actuator falls back to one with');
assert.deepEqual(calls.map(c => c.id), ['pad B']);

calls = [];
pads = [pad('pad A', false)];
assert.equal(WebRumble('pad A', 65535, 0, 150), 0, 'no actuator anywhere stays silent');

calls = [];
pads = [pad('pad A')];
assert.equal(WebRumble('pad A', 0, 0, 150), 1, 'zero magnitudes are still sent to stop the motors');
assert.deepEqual([calls[0].params.strongMagnitude, calls[0].params.weakMagnitude], [0, 0]);

console.log('web rumble checks: ok');
