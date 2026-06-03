const clampPct = (x) => Math.max(0, Math.min(100, Math.round(Number(x) || 0)));

// 按码点截断,超出加 '…'(总长含省略号 <= max)
function truncate(s, max) {
  const str = String(s || '');
  const cp = Array.from(str);
  if (cp.length <= max) return str;
  return cp.slice(0, Math.max(0, max - 1)).join('') + '…';
}

function projectName(cwd) {
  const parts = String(cwd || '').split('/').filter(Boolean);
  return parts.length ? parts[parts.length - 1] : '';
}

module.exports = { clampPct, truncate, projectName };
