const fs = require('node:fs');
const assert = require('node:assert/strict');
const source = fs.readFileSync(__dirname + '/../src/platform/input/input_system_sdl.cpp', 'utf8');
function bridge(name, args) {
    const match = source.match(new RegExp('EM_JS\\([^,]+, ' + name + ', \\([^)]*\\), \\{([\\s\\S]*?)\\n\\}\\);'));
    assert.ok(match, name + ' not found');
    return new Function(...args, match[1]);
}
const init = bridge('WebInitGamepads', []), quit = bridge('WebQuitGamepads', []);
const handle = bridge('WebGamepadHandle', ['deviceIndex']);
const rumble = bridge('WebRumble', ['handle', 'low', 'high', 'ms']);
let pads = [], calls = [];
const listeners = new Map();
globalThis.Module = {};
globalThis.window = {
    addEventListener: (name, fn) => listeners.set(name, fn),
    removeEventListener: (name, fn) => { assert.equal(listeners.get(name), fn); listeners.delete(name); }
};
Object.defineProperty(globalThis, 'navigator', {value: {getGamepads: () => pads}});
const pad = (index, actuator = true) => ({index, id: 'identical controller', vibrationActuator: actuator
    ? {playEffect: (type, params) => { calls.push({index, type, params}); return Promise.resolve('complete'); }} : null});
function disconnect(index) { const old = pads[index]; pads[index] = null; listeners.get('gamepaddisconnected')({gamepad: old}); }
function connect(p) { pads[p.index] = p; listeners.get('gamepadconnected')({gamepad: p}); }
// Browser holes are not SDL device indices. Include pads without rumble in enumeration.
pads = [null, pad(1, false), null, pad(3), pad(4)];
init();
const silent = handle(0), first = handle(1), second = handle(2);
assert.equal(rumble(silent, 65535, 0, 150), 0);
assert.equal(rumble(9999, 65535, 0, 150), 0);
assert.equal(rumble(first, 0, 0, -1), 1);
assert.equal(calls.length, 0, 'capability lookup must not play an effect');
assert.equal(rumble(first, 65535, 32768, 150), 1);
assert.equal(rumble(second, 0, 0, 150), 1);
assert.deepEqual(calls.map(c => c.index), [3, 4], 'idle second pad must not cancel first pad');
assert.deepEqual(calls[0].params, {startDelay: 0, duration: 150, strongMagnitude: 1, weakMagnitude: 32768 / 65535});
assert.deepEqual(calls[1].params, {startDelay: 0, duration: 150, strongMagnitude: 0, weakMagnitude: 0});
// A lower reused browser index is appended to SDL's list, and old handles remain dead.
disconnect(3); connect(pad(3));
assert.equal(handle(1), second);
const replacement = handle(2);
assert.notEqual(replacement, first);
assert.equal(rumble(first, 0, 0, 150), 0);
assert.equal(rumble(replacement, 10, 20, 150), 1);
assert.equal(calls.at(-1).index, 3);
pads[3].vibrationActuator.playEffect = () => { throw new Error('unsupported effect'); };
assert.equal(rumble(replacement, 10, 20, 150), 0);
pads[3].vibrationActuator.playEffect = () => Promise.reject(new Error('denied'));
assert.equal(rumble(replacement, 10, 20, 150), 1);
quit(); assert.equal(listeners.size, 0);
assert.equal(rumble(second, 10, 20, 150), 0);
delete navigator.getGamepads;
init(); assert.equal(handle(0), 0); quit();
setImmediate(() => console.log('web rumble checks: ok (identity, holes, hotplug, stops, unsupported actuators)'));
