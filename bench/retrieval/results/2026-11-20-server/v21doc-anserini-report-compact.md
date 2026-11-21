| dataset | system | build s | build peak MB | index MB | k10 p50 ms | k10 p99 ms | k1000 p50 ms | nDCG@10 | top10 = nind | update s | update |
|---|---|---|---|---|---|---|---|---|---|---|---|
| v21doc-stop-trec | nind |  |  |  | 5.863 | 39.662 | 26.588 | 0.3526 |  |  |  |
| v21doc-stop-trec | lucene |  |  |  | 5.016 | 36.979 | 38.929 | 0.3514 | 0.975 |  |  |
| v21doc-stop-trec | tantivy |  |  |  |  |  |  |  |  |  |  |
| v21doc-stop-trec | nind-8s |  |  |  | 6.064 | 47.469 | 32.991 | 0.3526 | 1.000 |  |  |
| v21doc-stop-trec | nind-32s |  |  |  | 8.318 | 58.448 | 49.792 | 0.3526 | 1.000 |  |  |
| v21doc-trec | nind |  |  |  | 7.102 | 40.918 | 30.388 | 0.3522 |  |  |  |
| v21doc-trec | lucene |  |  |  | 7.117 | 54.868 | 51.051 | 0.3515 | 0.968 |  |  |
| v21doc-trec | tantivy |  |  |  |  |  |  |  |  |  |  |
| v21doc-trec | nind-8s |  |  |  | 7.008 | 48.722 | 44.176 | 0.3522 | 1.000 |  |  |
