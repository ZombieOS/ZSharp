export function greeting(name) {
  return `Hello from JavaScript, ${name}!`;
}

export function answer() {
  return 42;
}

export function available() {
  return true;
}

function decorate(value) {
  return `[${value}]`;
}

export function firstDecorated(values) {
  return decorate(values[0]);
}

export function textValues() {
  return ["first", "日本語", "third"];
}

export function numberValues() {
  return [7, 11, 42];
}

export function thirdNumber(values) {
  return values[2];
}

export function emptyValue() {
  return null;
}

export function unicode() {
  return "• — “Hello” ‘Hello’ café 日本語 안녕하세요 😀 ❤️";
}

export function unicodeRoundTrip() {
  return unicode().includes("日本語") && unicode().includes("😀");
}

// The bridge must not rewrite export-looking text in comments or strings.
export function exportText() {
  return "the word export stays intact";
}

export async function asynchronous(name) {
  const value = await Promise.resolve(name);
  return `Async ${value}`;
}

export default function defaultGreeting() {
  return "Default export works";
}

export function failClearly() {
  throw new Error("intentional JavaScript regression failure");
}
