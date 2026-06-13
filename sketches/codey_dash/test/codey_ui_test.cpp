// sketches/codey_dash/test/codey_ui_test.cpp
#include <cassert>
#include <cstring>
#include <cstdio>
#include "../codey_ui.h"

static const char* S(char* b, size_t n, void(*f)(const char*, char*, size_t), const char* in) {
  f(in, b, n); return b;
}

int main() {
  char b[64];

  // ---- statusFromStr / statusWord / statusRank ----
  assert(statusFromStr("executing") == ST_EXECUTING);
  assert(statusFromStr("thinking")  == ST_THINKING);
  assert(statusFromStr("waiting")   == ST_WAITING);
  assert(statusFromStr("done")      == ST_DONE);
  assert(statusFromStr(nullptr)     == ST_WAITING);
  assert(!strcmp(statusWord(ST_EXECUTING), "EXECUTING"));
  assert(!strcmp(statusWord(ST_DONE), "DONE"));
  assert(statusRank(ST_WAITING) == 0 && statusRank(ST_EXECUTING) == 1 && statusRank(ST_THINKING) == 2 && statusRank(ST_DONE) == 3);

  // ---- cpLen: UTF-8 码点计数 ----
  assert(cpLen("abc") == 3);
  assert(cpLen("项目名称") == 4);
  assert(cpLen("") == 0);

  // ---- truncCp: 按码点截断,超长加 …(U+2026) ----
  truncCp("abcdef", 4, b, sizeof(b)); assert(!strcmp(b, "abc\xE2\x80\xA6"));
  truncCp("短", 4, b, sizeof(b));      assert(!strcmp(b, "短"));
  truncCp("项目名称很长", 4, b, sizeof(b)); assert(!strcmp(b, "项目名\xE2\x80\xA6"));
  truncCp("abc", 3, b, sizeof(b));     assert(!strcmp(b, "abc"));

  // ---- composeMetaLine: 行2点分串,按列开关取段 ----
  {
    DispCfg d = dispDefault();                 // 全开
    char line[96];
    composeMetaLine(&d, "Opus4.8", 71, "229M", 567, "59M", line, sizeof(line));
    assert(!strcmp(line, "Opus4.8 \xC2\xB7 71% \xC2\xB7 229M \xC2\xB7 t567 \xC2\xB7 59M"));

    d.col[DC_MODEL] = false; d.col[DC_MEMORY] = false;   // 关 model + memory
    composeMetaLine(&d, "Opus4.8", 71, "229M", 567, "59M", line, sizeof(line));
    assert(!strcmp(line, "71% \xC2\xB7 229M \xC2\xB7 t567"));

    DispCfg e = dispDefault();
    for (int i = 0; i < DISP_NCOL; i++) e.col[i] = false; // 全关 -> 空串
    composeMetaLine(&e, "Opus4.8", 71, "229M", 567, "59M", line, sizeof(line));
    assert(line[0] == 0);

    // 单段:仅 turn 开 -> 无分隔符
    DispCfg f = dispDefault();
    for (int i = 0; i < DISP_NCOL; i++) f.col[i] = false;
    f.col[DC_TURN] = true;
    composeMetaLine(&f, "Opus4.8", 71, "229M", 567, "59M", line, sizeof(line));
    assert(!strcmp(line, "t567"));

    // 截断:小缓冲必须 NUL 结尾且不溢出
    char tiny[10];
    composeMetaLine(&d, "Opus4.8", 71, "229M", 567, "59M", tiny, sizeof(tiny));
    assert(strlen(tiny) < sizeof(tiny));
  }

  // ---- fmtK ----
  fmtK(950, b, sizeof(b));     assert(!strcmp(b, "950"));
  fmtK(1500, b, sizeof(b));    assert(!strcmp(b, "1.5k"));
  fmtK(94000, b, sizeof(b));   assert(!strcmp(b, "94.0k"));   // 与 sim k() 一致(<100k 保留 1 位小数)
  fmtK(1234567, b, sizeof(b)); assert(!strcmp(b, "1235k"));
  fmtK(0, b, sizeof(b));       assert(!strcmp(b, "0"));

  // ---- fmtTokens: K千 / W万 / B十亿 ----
  fmtTokens(500, b, sizeof(b));        assert(!strcmp(b, "500"));
  fmtTokens(1500, b, sizeof(b));       assert(!strcmp(b, "1.5K"));
  fmtTokens(12345, b, sizeof(b));      assert(!strcmp(b, "1.2W"));
  fmtTokens(1234567, b, sizeof(b));    assert(!strcmp(b, "123W"));
  fmtTokens(123456789, b, sizeof(b));  assert(!strcmp(b, "1.23M"));     // 亿
  fmtTokens(356700000, b, sizeof(b));  assert(!strcmp(b, "3.57M"));
  fmtTokens(1000000000L, b, sizeof(b)); assert(!strcmp(b, "1.00B"));
  fmtTokens(2345678901L, b, sizeof(b)); assert(!strcmp(b, "2.35B"));

  // ---- fmtMem: KB -> M/G ----
  fmtMem(0, b, sizeof(b));        assert(!strcmp(b, "-"));
  fmtMem(512, b, sizeof(b));      assert(!strcmp(b, "512K"));
  fmtMem(131072, b, sizeof(b));   assert(!strcmp(b, "128M"));
  fmtMem(2097152, b, sizeof(b));  assert(!strcmp(b, "2.0G"));

  // ---- fmtElapsed ----
  fmtElapsed(40, b, sizeof(b));        assert(!strcmp(b, "0m"));
  fmtElapsed(40*60, b, sizeof(b));     assert(!strcmp(b, "40m"));
  fmtElapsed(3*3600+36*60, b, sizeof(b)); assert(!strcmp(b, "3h36m"));

  // ---- modelShort ----
  modelShort("claude-opus-4-8", b, sizeof(b));            assert(!strcmp(b, "Opus 4.8"));
  modelShort("claude-sonnet-4-6", b, sizeof(b));          assert(!strcmp(b, "Son 4.6"));
  modelShort("claude-haiku-4-5-20251001", b, sizeof(b));  assert(!strcmp(b, "Haiku 4.5"));
  modelShort("gpt-5.1-codex", b, sizeof(b));              assert(!strcmp(b, "GPT-5.1"));
  modelShort("gpt-5.1-codex-mini", b, sizeof(b));         assert(!strcmp(b, "GPT-5.1"));
  modelShort("", b, sizeof(b));                            assert(!strcmp(b, ""));
  modelShort("Opus 4.8", b, sizeof(b));                    assert(!strcmp(b, "Opus 4.8"));
  // 越界回归:超长数字串不得溢出内部 ver[12](v<VMAX 保护,版本截断到 11 位)
  modelShort("claude-opus-99999999999999999999", b, sizeof(b)); assert(!strcmp(b, "Opus 99999999999"));

  // ---- parseWssHost: 从 wss://host/... / ws://host:port/... 抽 host ----
  parseWssHost("wss://ab12.ngrok-free.app/x", b, sizeof(b)); assert(!strcmp(b, "ab12.ngrok-free.app"));
  parseWssHost("wss://h.example.com", b, sizeof(b));         assert(!strcmp(b, "h.example.com"));
  parseWssHost("ws://1.2.3.4:8788/", b, sizeof(b));          assert(!strcmp(b, "1.2.3.4"));
  parseWssHost("", b, sizeof(b));                            assert(!strcmp(b, ""));
  char tiny[5];
  parseWssHost("wss://abcdefgh/x", tiny, sizeof(tiny));
  assert(!strcmp(tiny, "abcd"));   // truncated to n-1, NUL-terminated

  // ---- dispDefault: 缺省全开 ----
  {
    DispCfg d = dispDefault();
    for (int i = 0; i < DISP_NCOL; i++)  assert(d.col[i]);
    for (int i = 0; i < DISP_NPROV; i++) assert(d.prov[i]);
  }

  // ---- layoutColumns: 动态列布局 ----
  {
    int idx[DISP_NCOL], xs[DISP_NCOL];

    // 全开 → 复现原始 6 列起点 46,112,204,242,300,358(默认行为不变的回归锁)
    bool all6[DISP_NCOL] = { true, true, true, true, true, true };
    int n = layoutColumns(all6, idx, xs);
    assert(n == 6);
    int wantIdx6[6] = { DC_STATUS, DC_MODEL, DC_CTX, DC_TOKENS, DC_MEMORY, DC_TURN };
    int wantX6[6]   = { 46, 112, 204, 242, 300, 358 };
    for (int k = 0; k < 6; k++) { assert(idx[k] == wantIdx6[k]); assert(xs[k] == wantX6[k]); }

    // memory+turn 关 → 4 列重新铺满并居中(块仍以 224 为中线)
    bool noMemTurn[DISP_NCOL] = { true, true, true, true, false, false };
    n = layoutColumns(noMemTurn, idx, xs);
    assert(n == 4);
    int wantIdx4[4] = { DC_STATUS, DC_MODEL, DC_CTX, DC_TOKENS };
    int wantX4[4]   = { 97, 163, 255, 293 };   // start 224-(66+92+38+58)/2=97,逐列累加槽宽
    for (int k = 0; k < 4; k++) { assert(idx[k] == wantIdx4[k]); assert(xs[k] == wantX4[k]); }
    // 居中对称:块中线 = (首槽起点 + 末槽末端)/2 ≈ 224
    assert((xs[0] + (xs[3] + DISP_COL_W[DC_TOKENS])) / 2 == 224);

    // 仅 status+tokens → 2 列居中(start 224-(66+58)/2=162)
    bool stTok[DISP_NCOL] = { true, false, false, true, false, false };
    n = layoutColumns(stTok, idx, xs);
    assert(n == 2);
    assert(idx[0] == DC_STATUS && idx[1] == DC_TOKENS);
    assert(xs[0] == 162 && xs[1] == 228);   // 162 + 66 = 228

    // 仅 1 列(turn)→ 居中;0 列 → n==0 不崩
    bool turnOnly[DISP_NCOL] = { false, false, false, false, false, true };
    n = layoutColumns(turnOnly, idx, xs);
    assert(n == 1 && idx[0] == DC_TURN && xs[0] == 224 - DISP_COL_W[DC_TURN] / 2);
    bool none[DISP_NCOL] = { false, false, false, false, false, false };
    n = layoutColumns(none, idx, xs);
    assert(n == 0);
  }

  (void)S;
  printf("codey_ui tests: ALL PASS\n");
  return 0;
}
