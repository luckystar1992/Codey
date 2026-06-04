// sketches/codey_dash/session_store.h — fixed-capacity session / provider model.
#pragma once
#include <stdint.h>

static const int MAX_SESS  = 12;   // per provider (overflow shown as "+N more")
static const int MAX_PORTS = 6;

struct Sess {
  char    id[40];
  char    name[40];
  char    model[24];      // raw model name (display via modelShort())
  char    branch[40];
  char    task[64];       // current_task
  char    effort[10];
  uint8_t status;         // SessStatus
  int     ctxPct;
  long    ctxTok, ctxWin, tokTotal;
  int     turn;
  int     added, modified;
  int     subagents;
  int     ports[MAX_PORTS];
  int     nports;
  long    startedAt;      // epoch seconds
};

struct Prov {
  const char* name;       // static label ("Claude" / "Codex")
  uint32_t    color;
  int  sessUsed, weekUsed;        // account usage % (weekUsed feeds the edge arc)
  long sessReset, weekReset;      // reset epochs (parsed; not currently rendered)
  int  activeCount, dirtyRepos;
  long tokPerMin;
  Sess sess[MAX_SESS];
  int  nsess;
};
