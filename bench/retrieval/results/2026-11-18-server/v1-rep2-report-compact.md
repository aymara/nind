| dataset | system | build s | build peak MB | index MB | k10 p50 ms | k10 p99 ms | k1000 p50 ms | nDCG@10 | top10 = nind | update s | update |
|---|---|---|---|---|---|---|---|---|---|---|---|
| msmarco-stop | nind | 48.66 | 3767 | 824.7 |  |  |  |  |  | 1.083 | new segment |
| msmarco-stop | lucene | 64.73 | 3828 | 651.0 |  |  |  |  |  | 3.992 | in place |
| msmarco-stop | tantivy | 83.96 | 1427 | 655.6 |  |  |  |  |  | 1.386 | in place |
| msmarco-stop | nind-8s | 17.33 | 4131 | 928.4 |  |  |  |  |  | 1.116 | new segment |
| msmarco-stop | nind-32s | 19.61 | 1736 | 1070.7 |  |  |  |  |  | 94.311 | merged 8753405 documents |
