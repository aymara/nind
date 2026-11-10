| dataset | system | build s | build peak MB | index MB | k10 p50 ms | k10 p99 ms | k1000 p50 ms | nDCG@10 | top10 = nind | update s | update |
|---|---|---|---|---|---|---|---|---|---|---|---|
| msmarco-stop | nind | 102.16 | 7702 | 824.7 |  |  |  |  |  | 4.402 | new segment |
| msmarco-stop | lucene | 64.73 | 3828 | 651.0 |  |  |  |  |  | 3.992 | in place |
| msmarco-stop | tantivy | 83.96 | 1427 | 655.6 |  |  |  |  |  | 1.386 | in place |
| msmarco-stop | nind-8s | 36.62 | 9519 | 928.4 |  |  |  |  |  | 4.403 | new segment |
| msmarco-stop | nind-32s | 29.01 | 3514 | 1070.7 |  |  |  |  |  | 98.117 | merged 8753405 documents |
