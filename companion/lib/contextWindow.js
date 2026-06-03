function contextWindowFor(model, maxObservedTokens) {
  const m = String(model || '').toLowerCase();
  if (m.includes('[1m]') || Number(maxObservedTokens) > 200000) return 1000000;
  return 200000;
}
module.exports = { contextWindowFor };
