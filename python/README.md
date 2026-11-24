# pdkv

Python client for `[kvx/dict]`, the JSON dictionary for Pure Data. No
dependencies, Python 3.8 or newer, works the same on macOS (Intel and Apple
Silicon), Linux and Windows.

```
pip install ./python        # from the kvx repo
```

```python
import pdkv

pd = pdkv.Client()                  # request port 9001, answer port 9002
pd["synth.freq"] = 220
pd["synth.freq"]                    # 220
pd.set("cfg", {"a": [1, 2, 3], "ok": True})
pd.get("cfg.a")                     # [1, 2, 3]
pd.keys("cfg")                      # ['a', 'ok']
pd.push("log", {"t": 1})            # append to a list
pd.write("state.json", pretty=True)
del pd["cfg"]
```

In Pd, open `python-bridge.pd` from the kvx help folder (Help > Find externals,
install `kvx`, then open the patch). Start Python first, then click `connect`
in the patch.
