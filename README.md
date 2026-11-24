# kvx

A JSON dictionary and a CSV reader for Pure Data vanilla (0.54 and newer), with a Python client.
Pd has no built-in way to keep nested, named data (settings, datasets, presets
as trees) or to exchange it with other programs. People have asked for a
native dictionary on the pd-list (Feb 2026) and the existing JSON external is
not vanilla-friendly on every CPU. kvx is plain C with no dependencies, built
for Linux, macOS (Intel and Apple Silicon) and Windows.

| Object | What it does |
| --- | --- |
| `kvx/dict` | A named dictionary of numbers, words, lists and objects (the shape of JSON). Read and write JSON files. Objects with the same name share one dictionary. Answers requests from other programs. |
| `kvx/table` | Reads CSV and TSV files for sonification. Columns are kept as numbers, so files with millions of rows stay small. Thin out with `-stride`, pick columns, step through rows, play them at a rate, or copy a column into a Pd array. |
| `pdkv` (Python) | `pip install ./python`. Read and write the dictionary from Python, over a local connection. |

## Examples

```
[set synth.freq 220(      [set notes 60 64 67(      [get synth.freq(
         \                        |                        /
                     [kvx/dict demo]
                      |       |       |
                   values   info    replies
```

```
[read patch-settings.json(          load a file next to the patch
[get synth.partials(                -> a list: 1 2 3 5
[set synth.freq 440(                change one value, keep the rest
[write patch-settings.json pretty(  save it back
```

```python
import pdkv
pd = pdkv.Client()
pd["synth.freq"] = 220
pd.set("cfg", {"a": [1, 2, 3], "ok": True})
pd.get("cfg.a")        # [1, 2, 3]
```

Open `dict-help.pd` and `table-help.pd` for every message and `python-bridge.pd`
for the Python side.

## Messages

Paths: `a.b.c` goes down into objects, `list.0` or `list[0]` picks an item of a
list. Missing parents are created (a number in the path makes a list).

| Message | |
| --- | --- |
| `set path v ...` | One value is a number or word, several make a list. |
| `setjson path text`, `json text` | JSON text into a path, or replacing everything. |
| `setnull path`, `setbool path 0/1` | The two values Pd has no atom for. |
| `get [path]`, `tojson [path] [pretty]`, `print [path]`, `bang` | Read. Plain values and flat lists come out as Pd messages, anything nested as `json <text>`. |
| `has`, `type`, `keys`, `size` | Questions about a path, answered on the middle outlet. |
| `push path v ...`, `del path`, `clear` | Append to a list, remove, empty. |
| `read file`, `write file [pretty]` | JSON files (relative to the patch). |
| `req id message ...` | Run a message and answer on the right outlet as `send reply id ...`, ready for `[netsend]`. Used by Python. |
| `jsonbuf text`, `jsonapply [path]` | JSON text in pieces, for values longer than one message can carry. Python uses them for big values. |

null comes out as the word `null`, true and false as 1 and 0 (`type` tells
them apart from numbers). Pd never frees symbols, so for big data that changes
all the time use `read` and `write` instead of passing JSON text around.

## CSV files: kvx/table

```
[read quakes.csv -stride 10 -cols time BHZ(     every 10th row, two columns
[toarray BHZ wave norm(                          column into a Pd array, scaled to -1..1
[play 20(                                        one row every 20 ms
```

| Message | |
| --- | --- |
| `read file [options]` | `-stride n` every nth row, `-maxrows n`, `-skip n` lines, `-cols a b 3` (names or numbers from 0), `-delim comma\|semicolon\|tab`, `-decimal point\|comma`, `-header 0\|1`, `-reltime 0\|1`. The file is read row by row. |
| `columns`, `rows`, `get row col`, `row n`, `stats col` | Questions and values. Rows and columns count from 0. `stats` gives min, max, mean, standard deviation and count. |
| `toarray col array [norm]` | Copy a column into a Pd array (resized to fit). `norm` scales to -1..1. |
| `bang`, `reset`, `seek n`, `play [ms]`, `stop` | Step through the rows or play them on a clock. `end` comes out of the right outlet after the last row. |
| `nan value`, `clear` | What empty cells come out as (default 0), and free the data. |

Delimiter, header and quoted cells are detected. Dates such as
`2024-03-05T10:20:30Z` become seconds, counted from the first row (a Pd float
cannot hold a whole date); the start comes out as `timeorigin col seconds`.
Text columns come out as words. Pd floats are 32-bit, so a column with huge
offsets (for example epoch seconds) loses precision: use the default relative
time.

## Install

In Pd: Help > Find externals, search `kvx`. Python: `pip install ./python`.

## Build

Needs the Pd source (for the headers) and pd-lib-builder (included):

```
git clone --depth 1 https://github.com/pure-data/pure-data.git pd
make PDINCLUDEDIR=pd/src
make test                              # JSON, paths and CSV, no Pd needed
make test-object PDINCLUDEDIR=pd/src   # the object, with a mock instead of Pd
python -m unittest discover -s python/tests
```

Every push builds Linux, macOS and Windows binaries on GitHub Actions and
makes a ready deken package (Actions tab, artifact `kvx-deken`).

The help files are generated by `tools/build_help.py`.

## License

BSD 3-Clause, see LICENSE.
