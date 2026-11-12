| dataset | system | build s | build peak MB | index MB | k10 p50 ms | k10 p99 ms | k1000 p50 ms | nDCG@10 | top10 = nind | update s | update |
|---|---|---|---|---|---|---|---|---|---|---|---|
| msmarco-stop | nind | 60.68 | 4122 | 824.7 |  |  |  |  |  | 1.068 | new segment |
| msmarco-stop | lucene | 64.73 | 3828 | 651.0 |  |  |  |  |  | 3.992 | in place |
| msmarco-stop | tantivy | 83.96 | 1427 | 655.6 |  |  |  |  |  | 1.386 | in place |
| msmarco-stop | nind-8s | 19.44 | 5620 | 928.4 |  |  |  |  |  | 1.116 | new segment |
| msmarco-stop | nind-32s | 19.74 | 2536 | 1070.7 |  |  |  |  |  | 94.060 | merged 8753405 documents |
