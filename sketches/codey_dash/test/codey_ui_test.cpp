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
  assert(statusRank(ST_EXECUTING) == 0 && statusRank(ST_THINKING) == 1 && statusRank(ST_WAITING) == 2 && statusRank(ST_DONE) == 2);

  // ---- cpLen: UTF-8 码点计数 ----
  assert(cpLen("abc") == 3);
  assert(cpLen("项目名称") == 4);
  assert(cpLen("") == 0);

  // ---- truncCp: 按码点截断,超长加 …(U+2026) ----
  truncCp("abcdef", 4, b, sizeof(b)); assert(!strcmp(b, "abc\xE2\x80\xA6"));
  truncCp("短", 4, b, sizeof(b));      assert(!strcmp(b, "短"));
  truncCp("项目名称很长", 4, b, sizeof(b)); assert(!strcmp(b, "项目名\xE2\x80\xA6"));
  truncCp("abc", 3, b, sizeof(b));     assert(!strcmp(b, "abc"));

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
  fmtTokens(1000000000L, b, sizeof(b)); assert(!strcmp(b, "1.00B"));
  fmtTokens(2345678901L, b, sizeof(b)); assert(!strcmp(b, "2.35B"));

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

  (void)S;
  printf("codey_ui tests: ALL PASS\n");
  return 0;
}
