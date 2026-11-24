"""pdkv - talk to a [kvx/dict] in Pure Data from Python.

    import pdkv
    pd = pdkv.Client()                 # Pd: [netreceive 9001] -> [kvx/dict name] -> [netsend]
    pd["synth.freq"] = 220
    pd["synth.freq"]                   # 220
    pd.set("cfg", {"a": [1, 2, 3], "ok": True})
    pd.get("cfg.a")                    # [1, 2, 3]

Plain Python, no dependencies, any Python 3.8+. It speaks Pd's text protocol
(FUDI) over two local TCP connections: requests go to Pd's [netreceive], and
Pd's [netsend] connects back to this client to deliver the answers.
"""
import json
import re
import socket
import threading
import itertools
import queue

__all__ = ["Client", "PdError", "encode_atom", "decode_message"]
__version__ = "0.2.0"


class PdError(Exception):
    """Pd answered with an error."""


_ESCAPES = {"\\": "\\\\", " ": "\\ ", ";": "\\;", ",": "\\,", "$": "\\$",
            "\n": "\\\n", "\t": "\\\t", "\r": "\\\r"}
_ATOM = 900     # Pd cuts a word at 1000 characters
_PIECE = 1800   # characters per message: two words (escaping can double them, Pd takes ~4000 bytes)
_NUMBER = re.compile(r"^[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?$")


def encode_atom(value):
    """One FUDI atom. Numbers go as numbers; text that merely looks like a number is
    escaped so Pd keeps it as text."""
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        return repr(value)
    text = "".join(_ESCAPES.get(c, c) for c in str(value))
    if _NUMBER.match(str(value)):
        text = "\\" + text
    return text


def decode_message(data):
    """Split FUDI text into messages (lists of strings) with the escapes removed.
    Returns (messages, rest); rest is an unfinished tail to prepend to the next chunk."""
    messages, atoms, cur = [], [], []
    has_cur = False
    last_end = 0
    i, n = 0, len(data)
    while i < n:
        c = data[i]
        if c == "\\":
            if i + 1 >= n:
                break               # a backslash at the very end: wait for more text
            cur.append(data[i + 1])
            has_cur = True
            i += 2
            continue
        if c in " \t\r\n":
            if has_cur:
                atoms.append("".join(cur))
                cur, has_cur = [], False
        elif c == ";":
            if has_cur:
                atoms.append("".join(cur))
                cur, has_cur = [], False
            if atoms:
                messages.append(atoms)
            atoms = []
            last_end = i + 1
        else:
            cur.append(c)
            has_cur = True
        i += 1
    rest = data[last_end:]
    return messages, (rest if rest.strip() else "")


class Client:
    """A connection to one [kvx/dict] in Pd.

    Patch:  [netreceive 9001] -> [kvx/dict name] , third outlet -> [netsend]
            and send [connect localhost 9002( to the [netsend].
    """

    def __init__(self, host="127.0.0.1", send_port=9001, recv_port=9002, timeout=5.0):
        self.host, self.send_port, self.recv_port = host, send_port, recv_port
        self.timeout = timeout
        self._ids = itertools.count(1)
        self._waiting = {}
        self._lock = threading.Lock()
        self._out = None
        self._closed = False
        self._server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._server.bind(("127.0.0.1" if host in ("127.0.0.1", "localhost") else "", recv_port))
        self._server.listen(4)
        threading.Thread(target=self._accept_loop, daemon=True).start()

    # ------------------------------------------------------------ plumbing
    def _accept_loop(self):
        while not self._closed:
            try:
                conn, _ = self._server.accept()
            except OSError:
                return
            threading.Thread(target=self._read_loop, args=(conn,), daemon=True).start()

    def _read_loop(self, conn):
        buf = ""
        pending = b""
        with conn:
            while not self._closed:
                try:
                    chunk = conn.recv(65536)
                except OSError:
                    return
                if not chunk:
                    return
                pending += chunk
                try:
                    text = pending.decode("utf-8")
                    pending = b""
                except UnicodeDecodeError:
                    continue            # a multi-byte character was cut in half
                messages, buf = decode_message(buf + text)
                for atoms in messages:
                    self._dispatch(atoms)

    def _dispatch(self, atoms):
        if len(atoms) < 3 or atoms[0] != "reply":
            return
        with self._lock:
            q = self._waiting.get(atoms[1])
        if q is not None:
            q.put((atoms[2], atoms[3:]))

    def _connect(self):
        if self._out is None:
            self._out = socket.create_connection((self.host, self.send_port), timeout=self.timeout)
        return self._out

    def _request(self, selector, *args):
        rid = str(next(self._ids))
        q = queue.Queue()
        with self._lock:
            self._waiting[rid] = q
        line = " ".join(["req", rid, selector] + [encode_atom(a) for a in args]) + ";\n"
        try:
            try:
                self._connect().sendall(line.encode("utf-8"))
            except OSError:
                self._out = None            # Pd may have been restarted: try once more
                self._connect().sendall(line.encode("utf-8"))
            try:
                kind, atoms = q.get(timeout=self.timeout)
            except queue.Empty:
                raise TimeoutError(
                    "no answer from Pd. Is [netreceive %d] connected to [kvx/dict], and is the "
                    "[netsend] connected to localhost %d?" % (self.send_port, self.recv_port)) from None
        finally:
            with self._lock:
                self._waiting.pop(rid, None)
        if kind == "error":
            raise PdError(" ".join(atoms))
        return kind, atoms

    def close(self):
        self._closed = True
        for s in (self._out, self._server):
            try:
                if s:
                    s.close()
            except OSError:
                pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # ------------------------------------------------------------ data
    def get(self, path=".", default=KeyError):
        """The value at path. Raises KeyError if missing, unless a default is given."""
        kind, atoms = self._request("get", path)
        if kind == "missing":
            if default is KeyError:
                raise KeyError(path)
            return default
        return json.loads("".join(atoms))   # long JSON arrives as several words

    def _send_json(self, selector, path, text):
        # Pd's network input holds about 4 kB per message: send big values in pieces
        if len(text) <= _ATOM:
            if selector == "setjson":
                self._request(selector, path, text)
            else:
                self._request(selector, text)
            return
        self._request("jsonbuf")
        for i in range(0, len(text), _PIECE):
            piece = text[i:i + _PIECE]
            self._request("jsonbuf", *[piece[j:j + _ATOM] for j in range(0, len(piece), _ATOM)])
        self._request("jsonapply", path)

    def set(self, path, value):
        """Set path to any JSON-able Python value (dicts, lists, numbers, strings, bool, None)."""
        self._send_json("setjson", path, json.dumps(value, separators=(",", ":"), ensure_ascii=True))

    def delete(self, path):
        """Remove path. Returns False if it was not there."""
        kind, _ = self._request("del", path)
        return kind != "missing"

    def has(self, path):
        _, atoms = self._request("has", path)
        return atoms[-1] == "1"

    def type_of(self, path="."):
        """'object', 'array', 'string', 'number', 'bool', 'null' or 'missing'."""
        _, atoms = self._request("type", path)
        return atoms[-1]

    def keys(self, path="."):
        kind, atoms = self._request("keys", path)
        if kind == "missing":
            raise KeyError(path)
        return atoms[1:]

    def size(self, path="."):
        kind, atoms = self._request("size", path)
        if kind == "missing":
            raise KeyError(path)
        return int(float(atoms[-1]))

    def push(self, path, value):
        """Append value to the list at path, creating the list if needed."""
        if path in (".", ""):
            raise ValueError("push needs a path to a list, not the root")
        n = 0 if self.type_of(path) == "missing" else self.size(path)
        self.set("%s.%d" % (path, n), value)

    def clear(self):
        self._request("clear")

    def dump(self):
        """The whole dictionary as Python objects."""
        return self.get(".")

    def load(self, obj):
        """Replace the whole dictionary."""
        self._send_json("json", ".", json.dumps(obj, separators=(",", ":"), ensure_ascii=True))

    def read(self, filename):
        """Load a JSON file (a relative name is relative to the Pd patch)."""
        self._request("read", filename)

    def write(self, filename, pretty=False):
        """Save as a JSON file on the Pd side."""
        self._request("write", filename, *(["pretty"] if pretty else []))

    # ------------------------------------------------------------ dict style
    def __getitem__(self, path):
        return self.get(path)

    def __setitem__(self, path, value):
        self.set(path, value)

    def __delitem__(self, path):
        if not self.delete(path):
            raise KeyError(path)

    def __contains__(self, path):
        return self.has(path)
