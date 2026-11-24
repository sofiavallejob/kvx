"""Drives the sound in 06-python-live.pd from Python.

Start the patch, click connect, turn sound on and click 1 (listen), then run:
    pip install ./python        (once, from the kvx folder)
    python examples/python_demo.py
"""
import csv
import os
import time

import pdkv

here = os.path.dirname(os.path.abspath(__file__))

# one value of the seismogram every 60 rows, so about 200 notes
values = []
with open(os.path.join(here, "quakes.csv"), newline="") as f:
    for i, row in enumerate(csv.DictReader(f)):
        if i % 60 == 0:
            values.append(float(row["ch1"]))


def mtof(note):
    return 440.0 * 2 ** ((note - 69) / 12.0)


with pdkv.Client() as pd:
    pd["synth.amp"] = 0.2
    pd["cfg"] = {"source": "quakes.csv", "notes": len(values), "tags": ["demo", "python"]}
    print("stored:", pd["cfg"])                     # comes back as Python data
    print("keys:", pd.keys("cfg"))
    for v in values:
        pd["synth.freq"] = mtof(60 + 12 * v)         # the patch reads this
        time.sleep(0.05)
    pd["synth.amp"] = 0.0
    print("done")
