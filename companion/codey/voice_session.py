"""传输无关的语音会话核:从 asr_stream.handle 抽出,WS/USB 共用。
出站经 Channel(send_text/send_hello/send_focus_ack),入站 on_pcm/on_control。"""
import json


class VoiceSession:
    def __init__(self, channel, make_backend, paster, loop,
                 resolve_pid=None, resolve_status=None, focus=None):
        self.ch = channel
        self.make_backend = make_backend
        self.paster = paster
        self.loop = loop
        self._resolve_pid = resolve_pid
        self._resolve_status = resolve_status
        if focus is None:
            from codey import focus as _focus
            focus = _focus
        self.focus = focus
        self.backend = None
        self.last_sent = None
        self.cur_seq = 0
        self.target_pane = None
        self.synced = ""

    def _paste_on(self):
        from codey import envcfg as _ec
        return _ec.paste_enabled() if self.paster is None or self.paster.enabled is None else self.paster.enabled

    def _enter_on(self):
        from codey import envcfg as _ec
        return _ec.auto_enter() if self.paster is None or self.paster.auto_enter is None else self.paster.auto_enter

    async def _sync_to_pane(self, text):
        if self.target_pane is None:
            return
        text = text or ""
        i = 0
        while i < len(self.synced) and i < len(text) and self.synced[i] == text[i]:
            i += 1
        payload = "\x7f" * (len(self.synced) - i) + text[i:]
        ok = True
        if payload:
            ok = await self.loop.run_in_executor(None, self.focus.send_text_to_pane, self.target_pane, payload)
        if ok:
            self.synced = text

    async def _send(self, text, final):
        text = (text or "").strip()
        if final or text != self.last_sent:
            await self.ch.send_text(text, final, self.cur_seq)
            self.last_sent = text
            await self._sync_to_pane(text)

    async def _emit(self, events):
        final_text = None
        for ev in events:
            await self._send(ev["text"], ev["final"])
            if ev["final"] and ev["text"]:
                final_text = ev["text"]
        return final_text

    async def _close_backend(self):
        if self.backend is None:
            return
        close = getattr(self.backend, "close", None)
        if close:
            try:
                await close()
            except Exception:
                pass

    async def on_pcm(self, pcm):
        if self.backend is None:
            self.backend = self.make_backend()
            await self.backend.start()
        try:
            await self._emit(await self.backend.accept(pcm))
        except Exception as e:
            print(f"[asr] accept error: {e}", flush=True)

    async def on_control(self, data):
        t = data.get("type")
        if t == "hello":
            await self.ch.send_hello()
        elif t == "listen" and data.get("state") == "start":
            await self._close_backend()
            self.backend = self.make_backend()
            await self.backend.start()
            self.last_sent = None
            self.synced = ""
            sid = data.get("session") or ""
            self.target_pane = None
            if sid and self._resolve_pid:
                status = self._resolve_status(sid) if self._resolve_status else "waiting"
                if status == "waiting":
                    pid = self._resolve_pid(sid)
                    self.target_pane = await self.loop.run_in_executor(None, self.focus.pane_for_pid, pid)
                    print(f"[voice] target session={sid} pid={pid} pane={self.target_pane}", flush=True)
                else:
                    print(f"[voice] session={sid} status={status} 非空闲 → 不流式同步", flush=True)
            try:
                self.cur_seq = int(data.get("seq") or 0)
            except (TypeError, ValueError):
                self.cur_seq = 0
        elif t == "listen" and data.get("state") == "stop":
            if self.backend is None:
                self.backend = self.make_backend()
                await self.backend.start()
            try:
                final_text = await self._emit(await self.backend.stop())
            except Exception as e:
                print(f"[asr] stop error: {e}", flush=True)
                final_text = None
            await self._close_backend()
            self.backend = None
            if self.target_pane is not None:
                await self._sync_to_pane(final_text or "")
                pasted = False
            else:
                pasted = final_text and self._paste_on()
                if pasted:
                    try:
                        self.paster.paste(final_text)
                        if self._enter_on():
                            self.paster.enter()
                    except Exception as e:
                        print(f"[asr] paste error: {e}", flush=True)
            if final_text:
                from codey import asr_history, envcfg as _ec
                asr_history.append(final_text, engine=_ec.select_engine(), pasted=bool(pasted))
            self.target_pane = None
            self.synced = ""
        elif t == "listen" and data.get("state") == "cancel":
            if self.target_pane is not None and self.synced:
                await self.loop.run_in_executor(None, self.focus.send_text_to_pane,
                                                self.target_pane, "\x7f" * len(self.synced))
            await self._close_backend()
            self.backend = None
            self.target_pane = None
            self.synced = ""
        elif t == "submit":
            if self._paste_on() and self.paster:
                try:
                    self.paster.enter()
                except Exception:
                    pass
        elif t == "clear":
            if self._paste_on() and self.paster:
                try:
                    self.paster.clear()
                except Exception:
                    pass
        elif t == "focus":
            sid = data.get("session") or ""
            pid = self._resolve_pid(sid) if self._resolve_pid else 0
            ok, why = await self.loop.run_in_executor(None, self.focus.focus_pid, pid)
            print(f"[focus] session={sid} pid={pid} -> {ok} ({why})", flush=True)
            await self.ch.send_focus_ack(ok, why)

    async def close(self):
        await self._close_backend()
