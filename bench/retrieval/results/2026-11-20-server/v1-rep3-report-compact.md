| dataset | system | build s | build peak MB | index MB | k10 p50 ms | k10 p99 ms | k1000 p50 ms | nDCG@10 | top10 = nind | update s | update |
|---|---|---|---|---|---|---|---|---|---|---|---|
| msmarco-stop | nind | 57.60 | 3767 | 768.3 |  |  |  |  |  | 1.091 | new segment |
| msmarco-stop | lucene | 68.84 | 3686 | 650.2 |  |  |  |  |  | 4.639 | in place |
| msmarco-stop | tantivy | 89.67 | 1438 | 656.2 |  |  |  |  |  | 1.319 | in place |
| msmarco-stop | nind-8s | 21.14 | 3941 | 842.8 |  |  |  |  |  | 1.134 | new segment |
| msmarco-stop | nind-32s | 19.21 | 1691 | 945.1 |  |  |  |  |  | 108.223 | merged 8753405 documents |
