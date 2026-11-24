"""Tests for pdkv against a fake Pd that speaks the same text protocol.
This checks escaping, decoding and request/answer matching, not Pd itself."""
import json
import socket
import threading
import unittest
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import pdkv
from pdkv import decode_message, encode_atom


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class FakePd:
    """Listens like [netreceive], answers like [kvx/dict] + [netsend]."""

    def __init__(self, send_port, recv_port):
        self.recv_port = recv_port
        self.seen = []
        self.store = {}
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", send_port))
        self.srv.listen(2)
        self.back = None
        self.silent = False
        threading.Thread(target=self.run, daemon=True).start()

    def reply(self, *atoms):
        if self.back is None:
            self.back = socket.create_connection(("127.0.0.1", self.recv_port))
        text = " ".join(encode_atom(a) if not isinstance(a, str) else
                        "".join({"\\": "\\\\", " ": "\\ ", ";": "\;", ",": "\\,", "$": "\\$"}.get(c, c) for c in a)
                        for a in atoms) + ";\n"
        self.back.sendall(text.encode())

    def run(self):
        conn, _ = self.srv.accept()
        buf = ""
        while True:
            data = conn.recv(65536)
            if not data:
                return
            msgs, buf = decode_message(buf + data.decode())
            for m in msgs:
                self.seen.append(m)
                if self.silent:
                    continue
                self.handle(m)

    def handle(self, m):
        assert m[0] == "req"
        rid, sel, args = m[1], m[2], m[3:]
        if sel == "setjson":
            self.store[args[0]] = json.loads(args[1])
            self.reply("reply", rid, "ok")
        elif sel == "jsonbuf":
            self.buf = "" if not args else getattr(self, "buf", "") + "".join(args)
            self.reply("reply", rid, "ok")
        elif sel == "jsonapply":
            self.store[args[0]] = json.loads(self.buf)
            self.reply("reply", rid, "ok")
        elif sel == "get":
            if args[0] in self.store:
                self.reply("reply", rid, "json", json.dumps(self.store[args[0]], separators=(",", ":")))
            elif args[0] == "boom":
                self.reply("reply", rid, "error", "something broke")
            else:
                self.reply("reply", rid, "missing", args[0])
        elif sel == "keys":
            self.reply("reply", rid, "keys", args[0], "a b", "c;d")
        elif sel == "size":
            self.reply("reply", rid, "size", args[0], 3)
        elif sel == "type":
            self.reply("reply", rid, "type", args[0], "missing")
        elif sel == "has":
            self.reply("reply", rid, "has", args[0], 1)
        else:
            self.reply("reply", rid, "ok")


class Tests(unittest.TestCase):
    def setUp(self):
        sp, rp = free_port(), free_port()
        self.pd = FakePd(sp, rp)
        self.c = pdkv.Client(send_port=sp, recv_port=rp, timeout=2.0)

    def tearDown(self):
        self.c.close()

    def test_roundtrip_values(self):
        for v in [220, 0.1, "saw", "two words", "a;b,c$d\\e", "123", {"a": [1, 2, {"b": None}], "t": True},
                  [1, 2, 3], "ünï", 1.2345678901234567]:
            self.c["x"] = v
            self.assertEqual(self.c["x"], v)

    def test_big_value_goes_in_pieces(self):
        big = {"data": list(range(3000)), "s": "a b;c,$" * 500}
        self.c["big"] = big
        self.assertEqual(self.c["big"], big)
        sizes = [sum(len(a) for a in m) + len(m) for m in self.pd.seen]
        self.assertLess(max(sizes), 3500)
        self.assertLess(max(len(a) for m in self.pd.seen for a in m), 1000)
        self.assertGreater(len(self.pd.seen), 5)

    def test_wire_format(self):
        self.c.set("a.b", {"k": "v w"})
        req = self.pd.seen[-1]
        self.assertEqual(req[0:3], ["req", "1", "setjson"])
        self.assertEqual(req[3], "a.b")
        self.assertEqual(json.loads(req[4]), {"k": "v w"})

    def test_missing_and_default(self):
        with self.assertRaises(KeyError):
            self.c["nope"]
        self.assertEqual(self.c.get("nope", default=5), 5)

    def test_error(self):
        with self.assertRaises(pdkv.PdError) as cm:
            self.c.get("boom")
        self.assertIn("something broke", str(cm.exception))

    def test_info(self):
        self.assertEqual(self.c.keys("o"), ["a b", "c;d"])
        self.assertEqual(self.c.size("o"), 3)
        self.assertEqual(self.c.type_of("o"), "missing")
        self.assertTrue("o" in self.c)

    def test_timeout(self):
        self.pd.silent = True
        c = pdkv.Client(send_port=self.c.send_port, recv_port=free_port(), timeout=0.3)
        try:
            with self.assertRaises(TimeoutError):
                c.get("x")
        finally:
            c.close()

    def test_push_needs_list_path(self):
        with self.assertRaises(ValueError):
            self.c.push(".", 1)


class Codec(unittest.TestCase):
    def test_encode(self):
        self.assertEqual(encode_atom("a b"), "a\\ b")
        self.assertEqual(encode_atom("a;b"), "a\;b")
        self.assertEqual(encode_atom("a,b"), "a\\,b")
        self.assertEqual(encode_atom("$1"), "\\$1")
        self.assertEqual(encode_atom("123"), "\\123")        # stays text
        self.assertEqual(encode_atom("1e-05"), "\\1e-05")
        self.assertEqual(encode_atom(5), "5")
        self.assertEqual(encode_atom(0.5), "0.5")
        self.assertEqual(encode_atom(True), "1")

    def test_decode(self):
        msgs, rest = decode_message("reply 1 json {\"a\":1};\nreply 2 ok;\n")
        self.assertEqual(msgs, [["reply", "1", "json", "{\"a\":1}"], ["reply", "2", "ok"]])
        self.assertEqual(rest, "")
        msgs, rest = decode_message("reply 3 json a\\ b\;c;\n")
        self.assertEqual(msgs, [["reply", "3", "json", "a b;c"]])

    def test_decode_partial(self):
        msgs, rest = decode_message("reply 1 ok;\nreply 2 js")
        self.assertEqual(msgs, [["reply", "1", "ok"]])
        msgs2, rest2 = decode_message(rest + "on x;\n")
        self.assertEqual(msgs2, [["reply", "2", "json", "x"]])
        self.assertEqual(rest2, "")
        msgs3, rest3 = decode_message("reply 1 a\\")
        self.assertEqual(msgs3, [])
        msgs4, _ = decode_message(rest3 + " b;")
        self.assertEqual(msgs4, [["reply", "1", "a b"]])

    def test_encode_decode_symmetry(self):
        for text in ["plain", "a b", "x;y", "c,d", "$0-x", "back\\slash", "line\nbreak", '{"a":"b c"}', "ü"]:
            msgs, _ = decode_message("m " + encode_atom(text) + ";")
            self.assertEqual(msgs, [["m", text]])


if __name__ == "__main__":
    unittest.main()
