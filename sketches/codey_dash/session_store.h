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
  long    tokIn, tokOut, tokCacheR, tokCacheW;   // token 四件套(与 tokTotal 同 long)
  int     compactions;                            // 上下文压缩次数
  char    summary[64];                            // 会话摘要(首条提示截断)
  int     turn;
  int     added, modified;
  int     subagents;
  int     ports[MAX_PORTS];
  int     nports;
  long    startedAt;      // epoch seconds
  long    memKb;          // 进程树常驻内存 KB(0=未知)
};

struct Prov {
  const char* name;       // static label ("Claude" / "Codex")
  uint32_t    color;
  int  sessUsed, weekUsed;        // account usage % (weekUsed feeds the edge arc)
  long sessReset, weekReset;      // reset epochs (parsed; not currently rendered)
  int  activeCount, dirtyRepos;
  long tokPerMin;
  bool limited;                   // provider 配额已满(限流)
  Sess sess[MAX_SESS];
  int  nsess;
};
