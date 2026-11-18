| dataset | system | build s | build peak MB | index MB | k10 p50 ms | k10 p99 ms | k1000 p50 ms | nDCG@10 | top10 = nind | update s | update |
|---|---|---|---|---|---|---|---|---|---|---|---|
| msmarco-stop | nind | 48.19 | 3771 | 824.7 |  |  |  |  |  | 1.071 | new segment |
| msmarco-stop | lucene | 68.84 | 3686 | 650.2 |  |  |  |  |  | 4.639 | in place |
| msmarco-stop | tantivy | 89.67 | 1438 | 656.2 |  |  |  |  |  | 1.319 | in place |
| msmarco-stop | nind-8s | 19.50 | 4126 | 928.4 |  |  |  |  |  | 1.123 | new segment |
| msmarco-stop | nind-32s | 19.53 | 1738 | 1070.7 |  |  |  |  |  | 93.055 | merged 8753405 documents |
