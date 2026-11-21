| dataset | system | build s | build peak MB | index MB | k10 p50 ms | k10 p99 ms | k1000 p50 ms | nDCG@10 | top10 = nind | update s | update |
|---|---|---|---|---|---|---|---|---|---|---|---|
| msmarco-stop | nind | 55.70 | 3774 | 768.3 |  |  |  |  |  | 1.103 | new segment |
| msmarco-stop | lucene | 64.73 | 3828 | 651.0 |  |  |  |  |  | 3.992 | in place |
| msmarco-stop | tantivy | 83.96 | 1427 | 655.6 |  |  |  |  |  | 1.386 | in place |
| msmarco-stop | nind-8s | 21.93 | 3934 | 842.8 |  |  |  |  |  | 1.120 | new segment |
| msmarco-stop | nind-32s | 19.08 | 1706 | 945.1 |  |  |  |  |  | 108.574 | merged 8753405 documents |
