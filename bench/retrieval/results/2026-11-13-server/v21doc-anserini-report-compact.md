| dataset | system | build s | build peak MB | index MB | k10 p50 ms | k10 p99 ms | k1000 p50 ms | nDCG@10 | top10 = nind | update s | update |
|---|---|---|---|---|---|---|---|---|---|---|---|
| v21doc-stop-trec | nind |  |  |  | 10.030 | 63.760 | 31.005 | 0.3526 |  |  |  |
| v21doc-stop-trec | lucene |  |  |  | 5.016 | 36.979 | 38.929 | 0.3514 | 0.975 |  |  |
| v21doc-stop-trec | tantivy |  |  |  |  |  |  |  |  |  |  |
| v21doc-stop-trec | nind-8s |  |  |  | 8.443 | 58.866 | 40.553 | 0.3526 | 1.000 |  |  |
| v21doc-stop-trec | nind-32s |  |  |  | 10.700 | 70.983 | 56.683 | 0.3526 | 1.000 |  |  |
| v21doc-trec | nind |  |  |  | 15.372 | 92.608 | 45.017 | 0.3522 |  |  |  |
| v21doc-trec | lucene |  |  |  | 7.117 | 54.868 | 51.051 | 0.3515 | 0.968 |  |  |
| v21doc-trec | tantivy |  |  |  |  |  |  |  |  |  |  |
| v21doc-trec | nind-8s |  |  |  | 11.005 | 72.764 | 55.521 | 0.3522 | 1.000 |  |  |
