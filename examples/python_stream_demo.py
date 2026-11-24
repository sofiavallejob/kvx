"""Plays sensors.jsonl into Pd live, for 10-live-stream.pd.

Open the patch, turn sound on, click 1, then run:
    pip install ./python        (once, from the kvx folder)
    python examples/python_stream_demo.py
Stop with Ctrl-C.
"""
import json
import os
import time

import pdkv

here = os.path.dirname(os.path.abspath(__file__))
rows = []
with open(os.path.join(here, "sensors.jsonl")) as f:
    for line in f:
        d = json.loads(line)
        rows.append([d["ax"], d["light"], d["activity"]])

pd = pdkv.Client()
print("sending", len(rows), "rows at 50 per second, Ctrl-C to stop")
try:
    while True:
        for i in range(0, len(rows), 5):       # five rows every 100 ms
            pd.push_rows(rows[i:i + 5])
            time.sleep(0.1)
except KeyboardInterrupt:
    pass
finally:
    pd.close()
