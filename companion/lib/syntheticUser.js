const CMD_PREFIX = /^\s*<(command-name|command-message|command-args|bash-input|bash-stdout|bash-stderr|local-command-[a-z]+)>/;

function textOf(content) {
  if (typeof content === 'string') return content;
  if (Array.isArray(content)) {
    const t = content.find((b) => b && b.type === 'text');
    return t ? String(t.text || '') : '';
  }
  return '';
}

function isSyntheticUserMessage(entry) {
  if (!entry || entry.type !== 'user') return false;
  if (entry.isMeta === true) return true;
  const content = entry.message && entry.message.content;
  if (Array.isArray(content) && content.length > 0 && content.every((b) => b && b.type === 'tool_result')) return true;
  if (CMD_PREFIX.test(textOf(content))) return true;
  return false;
}

module.exports = { isSyntheticUserMessage, textOf };
