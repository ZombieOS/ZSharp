let count = 0;
export function counted() { return ++count; }
export function add(a, b) { return a + b; }
export function exact(value) { return value; }
export function array(value) { return value; }
export function record() { return ZSharp.record({ name: 'bridge', value: 42 }); }
export function callback(value) { return ZSharp.call('Tests:Bridge:Double', value); }
export function proxy(value) {
    const old = value.get('value');
    value.set('value', 21);
    return value.invoke('increase', old);
}
export function reentrant() { return ZSharp.call('Tests:Bridge:Reenter'); }
export async function promised(value) { return value + 1; }
export function never() { return new Promise(() => {}); }
export function nonfinite() { return NaN; }
export function timeout() { while (true) {} }
export class Counter {
    #secret = 'private';
    constructor(initial) { this.value = initial; }
    increase(amount) { this.value += amount; return this.value; }
    self() { return this; }
}
