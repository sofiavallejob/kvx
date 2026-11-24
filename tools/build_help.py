"""Generates help/dict-help.pd, help/table-help.pd and help/python-bridge.pd for kvx."""
import os, sys, math
sys.path.insert(0, os.path.dirname(__file__))
from hcore import H

C = "\\,"   # a comma inside a Pd text or message


def para(h, x, y, text, chars=76):
    """A wrapped text block. Returns the y just below it."""
    h.text(x, y, text, chars)
    return y + 16 * math.ceil(len(text) / (chars - 8)) + 22


# =========================================================== dict-help
h = H("kvx/dict", 1900)
h.text(20, 70, f"A dictionary for nested data: numbers{C} words{C} lists and objects{C} the same shape as JSON. Objects with the same name share one dictionary{C} anywhere in Pd. Left outlet: values. Middle outlet: answers about the data. Right outlet: replies to other programs.", 72)

# ---- 1
h.section(150, "1 - store and read values")
m = [h.msg(40, 190, "set synth.freq 220"),
     h.msg(40, 226, "set synth.name saw"),
     h.msg(40, 262, "set notes 60 64 67")]
g = [h.msg(40, 320, "get synth.freq"),
     h.msg(40, 356, "get notes"),
     h.msg(40, 392, "get synth")]
d = h.obj(340, 290, "kvx/dict demo")
pv = h.obj(340, 360, "print value")
pi = h.obj(480, 360, "print info")
for i in m + g:
    h.c(i, 0, d)
h.c(d, 0, pv); h.c(d, 1, pi)
h.text(340, 190, "A path like synth.freq goes down into objects. One value is stored as a number or a word. Several values are stored as a list.", 46)
h.text(340, 410, "A number or word comes out as itself. A flat list comes out as a list. Anything nested comes out as [json <text>( .", 46)

# ---- 2
h.section(500, "2 - paths and nesting")
m2 = [h.msg(40, 540, "set a.b.c 1"),
      h.msg(40, 576, "set a.b.d hello"),
      h.msg(40, 612, "set list.0 10")]
g2 = [h.msg(40, 670, "get a"),
      h.msg(40, 706, "get a.b.c"),
      h.msg(40, 742, "del a.b.d")]
d2 = h.obj(340, 650, "kvx/dict demo")
pv2 = h.obj(340, 720, "print value")
pi2 = h.obj(480, 720, "print info")
for i in m2 + g2:
    h.c(i, 0, d2)
h.c(d2, 0, pv2); h.c(d2, 1, pi2)
h.text(340, 540, "list.0 and list[0] are the first item of a list. Missing parents are created. A number in the path makes a list{}{}, a name makes an object.".replace("{}{}", C), 46)
h.text(340, 780, "Setting inside a plain value replaces it with a new container. Deleting from a list shifts the rest down.", 46)

# ---- 3
h.section(860, "3 - JSON files")
m3 = [h.msg(40, 900, "read kvx-demo.json"),
      h.msg(40, 936, "print"),
      h.msg(40, 972, "tojson pretty"),
      h.msg(40, 1020, "keys"),
      h.msg(40, 1056, "keys synth"),
      h.msg(40, 1092, "size synth.partials"),
      h.msg(40, 1128, "type title"),
      h.msg(40, 1164, "has synth.name"),
      h.msg(40, 1212, "write kvx-out.json pretty"),
      h.msg(40, 1248, "clear")]
d3 = h.obj(340, 1020, "kvx/dict demo")
pv3 = h.obj(340, 1100, "print value")
pi3 = h.obj(480, 1100, "print info")
for i in m3:
    h.c(i, 0, d3)
h.c(d3, 0, pv3); h.c(d3, 1, pi3)
h.text(340, 900, "read loads a JSON file next to this patch (or a full path) and replaces the dictionary. print shows it in the Pd window. tojson gives it back as one [json <text>( message.", 46)
h.text(340, 1140, "keys{}size{}type{}has answer on the middle outlet.".replace("{}", " " + C + " ", 3), 46)
h.text(340, 1214, "write saves the dictionary (next to the patch unless you give a full path). clear empties it.", 46)

# ---- 4
h.section(1330, "4 - other programs")
h.text(40, 1370, f"[req <id> <message>( runs any message and answers on the right outlet as [send reply <id> ...( {C} ready to connect to [netsend]. Python uses this: see python-bridge.pd.", 72)
r = [h.msg(40, 1440, "req 7 get synth.freq"),
     h.msg(40, 1476, "req 8 keys synth"),
     h.msg(40, 1512, "req 9 set x.y 5")]
d4 = h.obj(340, 1476, "kvx/dict demo")
pr = h.obj(340, 1546, "print reply")
for i in r:
    h.c(i, 0, d4)
h.c(d4, 2, pr)

# ---- reference
h.section(1620, "reference")
y = 1650
y = para(h, 40, y, "set path v ...   one value is a number or word, several make a list.   setjson path text   JSON text into a path.   json text   replace everything.   setnull path   setbool path 0/1")
y = para(h, 40, y, "get [path]   tojson [path] [pretty]   print [path]   bang (whole dictionary as json)   has path   type path   keys [path]   size [path]")
y = para(h, 40, y, "push path v ...   append to a list (made if missing). Several values are appended as one nested list.   del path   clear   read file   write file [pretty]   req id message ...")
y = para(h, 40, y, "types: object array string number bool null missing.  null comes out as the word null{} true and false as 1 and 0 (ask type to tell them from numbers). A list of one item comes out as a plain value.".replace("{}", C))
y = para(h, 40, y, "JSON text cannot go in a message box (commas and quotes are special there). Use read{} a symbol{} or Python.".replace("{}", C))
y = para(h, 40, y, "Pd never frees symbols. For big data that changes a lot{} use read and write instead of passing JSON text around.".replace("{}", C))
y = para(h, 40, y, "jsonbuf text   jsonapply [path]   JSON text in pieces{} for values longer than one message can carry. Python uses them for big values.".replace("{}", C))
y = para(h, 40, y, "Arguments: an optional name. [kvx/dict \\$0-data] keeps the dictionary private to one patch or abstraction. No name{} no sharing.".replace("{}", C))
h.lines[0] = f"#N canvas 60 40 780 {y + 30} 12;"
h.save("dict-help.pd")


# =========================================================== table-help
h = H("kvx/table", 1900)
h.text(20, 70, f"Reads CSV and TSV files into Pd for sonification. Columns are kept as numbers{C} so big files stay small. Delimiter{C} header and dates are detected. Left outlet: data. Right outlet: information about the data.", 72)

# ---- 1
h.section(150, "1 - read a file")
m = [h.msg(40, 190, "read kvx-demo.csv"),
     h.msg(40, 226, "columns"),
     h.msg(40, 262, "rows"),
     h.msg(40, 298, "get 3 temp"),
     h.msg(40, 334, "row 3"),
     h.msg(40, 370, "stats temp")]
t1 = h.obj(340, 300, "kvx/table")
pv = h.obj(340, 380, "print data")
pi = h.obj(480, 380, "print info")
for i in m:
    h.c(i, 0, t1)
h.c(t1, 0, pv); h.c(t1, 1, pi)
h.text(340, 190, f"read loads a CSV or TSV next to this patch (or a full path). Every column becomes a list of numbers{C} text columns stay words.", 46)
h.text(340, 430, f"Columns are named by the header and counted from 0{C} rows too. Empty cells come out as 0 (change it with [nan -1( ). row gives a whole row as a list{C} for example into [unpack f f f s].", 46)

# ---- 2
h.section(540, "2 - big files and dates")
m2 = [h.msg(40, 580, "read kvx-demo.csv -stride 2"),
      h.msg(40, 616, "read kvx-demo.csv -cols time temp"),
      h.msg(40, 652, "read kvx-demo.csv -maxrows 10"),
      h.msg(40, 688, "read kvx-demo.csv -skip 20"),
      h.msg(40, 724, "rows")]
t2 = h.obj(340, 720, "kvx/table")
pi2 = h.obj(340, 790, "print info")
for i in m2:
    h.c(i, 0, t2)
h.c(t2, 1, pi2)
h.text(340, 580, f"-stride 10 keeps every 10th row{C} -cols picks columns by name or number{C} -maxrows stops early{C} -skip drops lines at the top. The file is read row by row and never held as text{C} so a file with millions of rows is fine. Use stride to thin out a long recording.", 46)
h.text(340, 840, f"Dates like 2024-03-05T10:20:30Z become seconds. They count from the first row (a Pd float cannot hold a whole date){C} and the start comes out as [timeorigin( on the right outlet. -reltime 0 keeps the full value.", 46)

# ---- 3
h.section(960, "3 - into a Pd array")
m3 = [h.msg(40, 1000, "read kvx-demo.csv"),
      h.msg(40, 1036, "toarray temp wave"),
      h.msg(40, 1072, "toarray temp wave norm")]
t3 = h.obj(340, 1180, "kvx/table")
for i in m3:
    h.c(i, 0, t3)
h.lines.append("#N canvas 0 0 450 300 (subpatch) 0;")
h.lines.append("#X array wave 60 float 3;")
h.lines.append("#X coords 0 1 59 -1 200 100 1 0 0;")
h.lines.append("#X restore 340 1000 graph;")
h.n += 1
h.text(40, 1130, f"toarray copies a column into a Pd array and resizes the array to fit. norm scales it to -1 .. 1{C} ready for [tabread4~] or [tabplay~].", 40)

# ---- 4
h.section(1290, "4 - step through the rows or play them")
m4 = [h.msg(40, 1330, "reset"),
      h.msg(40, 1366, "bang"),
      h.msg(40, 1402, "seek 30"),
      h.msg(40, 1438, "play 100"),
      h.msg(40, 1474, "stop")]
t4 = h.obj(340, 1440, "kvx/table")
pv4 = h.obj(340, 1510, "print data")
pi4 = h.obj(480, 1510, "print info")
for i in m4:
    h.c(i, 0, t4)
h.c(t4, 0, pv4); h.c(t4, 1, pi4)
h.text(340, 1330, f"bang outputs the next row and moves on. After the last row [end( comes out of the right outlet. play 100 outputs one row every 100 ms{C} from where it is. seek jumps to a row and reset goes back to the start.", 46)

# ---- reference
h.section(1600, "reference")
y = 1630
y = para(h, 40, y, "read file [-stride n] [-maxrows n] [-skip n] [-cols a b 3] [-delim comma|semicolon|tab] [-decimal point|comma] [-header 0|1] [-reltime 0|1]")
y = para(h, 40, y, f"columns   rows   get row col   row n   stats col   toarray col array [norm]   nan value   bang   reset   seek n   play [ms]   stop   clear")
y = para(h, 40, y, f"Right outlet: loaded rows cols{C} columns ...{C} rows n{C} stats col min max mean std count{C} toarray name n{C} timeorigin col seconds{C} end{C} error text.")
y = para(h, 40, y, f"Files with a decimal comma (1{C}5) need -decimal comma and usually -delim semicolon. Without a header{C} columns are called col1{C} col2 ... Quoted cells and Windows line ends are fine. Lines starting with # are ignored.")
y = para(h, 40, y, f"The first row is a header if any cell in it is text. Force it with -header 1 or -header 0.")
h.lines[0] = f"#N canvas 60 40 780 {y + 30} 12;"
h.save("table-help.pd")

# =========================================================== python-bridge
h = H("kvx - Python bridge", 640)
h.text(20, 70, f"Lets Python read and write the dictionary over a local network connection. Start Python first (import pdkv{C} pd = pdkv.Client()){C} then click connect.", 72)

h.section(150, "the patch")
nr = h.obj(40, 200, "netreceive 9001")
dd = h.obj(40, 270, "kvx/dict shared")
ns = h.obj(40, 360, "netsend")
cn = h.msg(300, 300, "connect localhost 9002")
dc = h.msg(300, 340, "disconnect")
st = h.num(40, 410, 4)
pi = h.obj(190, 360, "print kvx")
h.c(nr, 0, dd)
h.c(dd, 2, ns)
h.c(dd, 1, pi)
h.c(cn, 0, ns); h.c(dc, 0, ns)
h.c(ns, 0, st)
h.text(300, 200, "Python sends requests to port 9001. The answers leave the right outlet of the dictionary and go back to Python on port 9002. The number shows whether the connection is up.", 46)

h.section(480, "in Python")
h.text(40, 520, "import pdkv   |   pd = pdkv.Client()   |   pd[\"synth.freq\"] = 220   |   pd[\"synth.freq\"]   |   pd.set(\"cfg.a\"{} [1{} 2{} 3])   |   pd.get(\"cfg.a\")".replace("{}", C), 76)
h.text(40, 580, "pip install ./python   (from the kvx folder).  Only your own computer can connect.", 76)
h.save("python-bridge.pd")
print("help written")
