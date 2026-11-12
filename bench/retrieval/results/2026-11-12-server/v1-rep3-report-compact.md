| dataset | system | build s | build peak MB | index MB | k10 p50 ms | k10 p99 ms | k1000 p50 ms | nDCG@10 | top10 = nind | update s | update |
|---|---|---|---|---|---|---|---|---|---|---|---|
| msmarco-stop | nind | 60.41 | 4168 | 824.7 |  |  |  |  |  | 1.038 | new segment |
| msmarco-stop | lucene | 68.84 | 3686 | 650.2 |  |  |  |  |  | 4.639 | in place |
| msmarco-stop | tantivy | 89.67 | 1438 | 656.2 |  |  |  |  |  | 1.319 | in place |
| msmarco-stop | nind-8s | 19.41 | 5620 | 928.4 |  |  |  |  |  | 1.127 | new segment |
| msmarco-stop | nind-32s | 19.76 | 2440 | 1070.7 |  |  |  |  |  | 93.216 | merged 8753405 documents |
