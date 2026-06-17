from codey import usb_frames as uf


def test_crc16_known_vector():
    # CRC-16/CCITT-FALSE("123456789") == 0x29B1
    assert uf.crc16(b"123456789") == 0x29B1


def test_encode_roundtrip_single_frame():
    raw = uf.encode(uf.STT, b'{"text":"hi"}')
    dec = uf.FrameDecoder()
    frames, logs = dec.feed(raw)
    assert logs == b""
    assert frames == [(uf.STT, b'{"text":"hi"}')]


def test_empty_payload():
    raw = uf.encode(uf.STATE_REQ)
    frames, logs = uf.FrameDecoder().feed(raw)
    assert frames == [(uf.STATE_REQ, b"")]
    assert logs == b""


def test_log_bytes_before_and_after_frame_are_passthrough():
    raw = b"[ws] connected\n" + uf.encode(uf.HELLO, b"v1") + b"[boot] ok\n"
    frames, logs = uf.FrameDecoder().feed(raw)
    assert frames == [(uf.HELLO, b"v1")]
    assert logs == b"[ws] connected\n[boot] ok\n"


def test_frame_split_across_two_feeds():
    raw = uf.encode(uf.PCM, b"\x01\x02\x03\x04")
    dec = uf.FrameDecoder()
    f1, l1 = dec.feed(raw[:4])
    f2, l2 = dec.feed(raw[4:])
    assert f1 == [] and f2 == [(uf.PCM, b"\x01\x02\x03\x04")]
    assert l1 == b"" and l2 == b""


def test_bad_crc_is_dropped_and_resyncs_to_next_frame():
    bad = bytearray(uf.encode(uf.STT, b"xx"))
    bad[-1] ^= 0xFF                                  # 破坏 CRC
    good = uf.encode(uf.STT, b"ok")
    frames, logs = uf.FrameDecoder().feed(bytes(bad) + good)
    assert (uf.STT, b"ok") in frames                 # 坏帧后能重同步到好帧
    assert (uf.STT, b"xx") not in frames


def test_trailing_lone_magic_first_byte_is_held_not_emitted_as_log():
    dec = uf.FrameDecoder()
    frames, logs = dec.feed(b"hello\xc0")            # 0xC0 可能是下一个魔数起点
    assert frames == []
    assert logs == b"hello"                          # 0xC0 暂留,不当日志吐出
    frames, logs = dec.feed(b"\xde" + uf.encode(uf.HELLO, b"")[2:])
    assert frames == [(uf.HELLO, b"")]


def test_oversized_len_header_resyncs_to_next_good_frame():
    bogus = uf.MAGIC + bytes([uf.STT, 0xFF, 0xFF])   # 声称 len=65535 的损坏头
    good = uf.encode(uf.PCM, b"ok")
    dec = uf.FrameDecoder()
    frames, logs = dec.feed(bogus + good)
    assert (uf.PCM, b"ok") in frames                  # 不被巨长 len 阻塞,能解出后续好帧
    assert len(dec._buf) < 64                          # 内部缓冲不被撑大(无 head-of-line 阻塞)


def test_multiple_frames_in_one_feed():
    raw = uf.encode(uf.PCM, b"a") + uf.encode(uf.PCM, b"b") + uf.encode(uf.PCM, b"c")
    frames, logs = uf.FrameDecoder().feed(raw)
    assert frames == [(uf.PCM, b"a"), (uf.PCM, b"b"), (uf.PCM, b"c")]
    assert logs == b""


def test_payload_containing_magic_bytes_decodes_as_one_frame():
    raw = uf.encode(uf.STATE, b"\xc0\xde\x01\x02")    # payload 内含魔数字节
    frames, logs = uf.FrameDecoder().feed(raw)
    assert frames == [(uf.STATE, b"\xc0\xde\x01\x02")]
    assert logs == b""


def test_large_pcm_frame_roundtrips_9600B():
    # 300ms @16k/mono/int16 = 9600B;须能编/解码往返(USB 上行 PCM 帧)
    pcm = bytes(9600)
    raw = uf.encode(uf.PCM, pcm)
    frames, logs = uf.FrameDecoder().feed(raw)
    assert frames == [(uf.PCM, pcm)] and logs == b""
