#!/bin/bash
# Codey statusline wrapper.
#
# Claude Code delivers the REAL rate-limit usage (the same numbers shown in /status and the
# claude-hud HUD) to the statusline command on stdin as JSON: rate_limits.five_hour /
# .seven_day { used_percentage, resets_at }. We tee that JSON to ~/.claude/codey-usage.json
# so the Codey Companion can serve the watch the EXACT live percentages.
#
# The capture is best-effort and backgrounded: it can never add latency to, or break, the
# normal statusline render (which is still claude-hud, invoked exactly as before).

input=$(cat)

# --- best-effort capture of the real usage (backgrounded; failures are silent) ---
(
  printf '%s' "$input" | /opt/homebrew/bin/bun -e '
    try {
      const fs = require("fs");
      const j = JSON.parse(fs.readFileSync(0, "utf8"));
      const r = (j && j.rate_limits) || {};
      const num = v => (typeof v === "number" && isFinite(v)) ? v : null;
      const out = {
        ts: Math.floor(Date.now() / 1000),
        model: (j.model && j.model.display_name) || null,
        five_hour: { used_percentage: num(r.five_hour && r.five_hour.used_percentage), resets_at: num(r.five_hour && r.five_hour.resets_at) },
        seven_day: { used_percentage: num(r.seven_day && r.seven_day.used_percentage), resets_at: num(r.seven_day && r.seven_day.resets_at) }
      };
      const p = process.env.HOME + "/.claude/codey-usage.json";
      const tmp = p + "." + process.pid + ".tmp";   // per-process temp -> no rename races
      fs.writeFileSync(tmp, JSON.stringify(out));
      fs.renameSync(tmp, p);
    } catch (e) {}
  ' 2>/dev/null
) &

# --- render the real statusline (claude-hud), unchanged; fall back if it's not installed ---
plugin_dir=$(ls -d "${CLAUDE_CONFIG_DIR:-$HOME/.claude}"/plugins/cache/claude-hud/claude-hud/*/ 2>/dev/null | awk -F/ '{ print $(NF-1) "\t" $0 }' | sort -t. -k1,1n -k2,2n -k3,3n -k4,4n | tail -1 | cut -f2-)
if [ -n "$plugin_dir" ] && [ -f "${plugin_dir}src/index.ts" ]; then
  printf '%s' "$input" | "/opt/homebrew/bin/bun" --env-file /dev/null "${plugin_dir}src/index.ts"
else
  # claude-hud unavailable -> never invoke bun with an empty path; emit a minimal line instead
  printf '%s' "$input" | /opt/homebrew/bin/bun -e 'try{const fs=require("fs");const j=JSON.parse(fs.readFileSync(0,"utf8"));process.stdout.write((j.model&&j.model.display_name)||"Claude");}catch(e){process.stdout.write("");}'
fi
