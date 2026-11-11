import time, statistics, sys
from nind.nind_engine import NindEngine
def rbytes():
    return next(int(l.split()[1]) for l in open("/proc/self/io") if l.startswith("read_bytes"))
qs=[l.rstrip("\n").split("\t",1)[1] for l in open("data/v2-stop/queries.tsv") if l.strip()]
t0=rbytes(); e=NindEngine(sys.argv[1], search_threads=int(sys.argv[2])); t1=rbytes()
for q in qs[:500]: e.search(q, top_k=10, k1=1.2, b=0.75)
t2=rbytes(); ts=[]
for q in qs[:2000]:
    s=time.perf_counter(); e.search(q, top_k=10, k1=1.2, b=0.75); ts.append(time.perf_counter()-s)
t3=rbytes()
print(sys.argv[1].split("/")[-2], "open %.2f GB, warm-up %.2f GB, measured %.2f GB, %.1f MB/query, p50 %.2f ms" % ((t1-t0)/1e9,(t2-t1)/1e9,(t3-t2)/1e9,(t3-t2)/2000/1e6,1000*statistics.median(ts)))
