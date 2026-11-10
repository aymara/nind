| dataset | system | build s | build peak MB | index MB | k10 p50 ms | k10 p99 ms | k1000 p50 ms | nDCG@10 | top10 = nind | update s | update |
|---|---|---|---|---|---|---|---|---|---|---|---|
| msmarco-stop | nind | 100.20 | 7703 | 824.7 |  |  |  |  |  | 4.378 | new segment |
| msmarco-stop | lucene | 68.84 | 3686 | 650.2 |  |  |  |  |  | 4.639 | in place |
| msmarco-stop | tantivy | 89.67 | 1438 | 656.2 |  |  |  |  |  | 1.319 | in place |
| msmarco-stop | nind-8s | 36.84 | 9525 | 928.4 |  |  |  |  |  | 4.386 | new segment |
| msmarco-stop | nind-32s | 31.67 | 3385 | 1070.7 |  |  |  |  |  | 99.054 | merged 8753405 documents |
