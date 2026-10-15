import os, sys
DATA = os.environ.get("ESL_DATA") or (sys.argv[1] if len(sys.argv) > 1 and os.path.isdir(sys.argv[1]) else "ESL_tempo_data")
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
os.makedirs(OUT, exist_ok=True)
def d(*p): return os.path.join(DATA, *p)
