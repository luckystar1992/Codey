function deriveStatus({ hasActiveDescendant, pendingTool, modelGenerating, done } = {}) {
  if (done) return 'done';
  if (hasActiveDescendant || pendingTool) return 'executing';
  if (modelGenerating) return 'thinking';
  return 'waiting';
}
module.exports = { deriveStatus };
