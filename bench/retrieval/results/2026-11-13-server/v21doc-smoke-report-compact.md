| dataset | system | build s | build peak MB | index MB | k10 p50 ms | k10 p99 ms | k1000 p50 ms | nDCG@10 | top10 = nind | update s | update |
|---|---|---|---|---|---|---|---|---|---|---|---|
| v21doc-stop-1M | nind | 145.39 | 7362 | 1239.7 | 0.947 | 6.399 | 27.820 |  |  | 1.652 | new segment |
| v21doc-stop-1M | lucene | 124.59 | 4193 | 809.9 | 2.404 | 13.153 | 23.709 |  | 0.980 | 3.768 | in place |
| v21doc-stop-1M | tantivy | 46.87 | 1714 | 896.3 | 7.251 | 70.021 | 14.110 |  | 0.979 | 1.867 | in place |
| v21doc-stop-1M | nind-8s | 57.19 | 8062 | 1486.6 | 1.462 | 7.990 | 21.628 |  | 1.000 | 1.368 | new segment |
| v21doc-stop-1M | nind-8t |  |  |  | 1.056 | 5.432 | 28.442 |  | 1.000 |  |  |
| v21doc-stop-1M | nind-8s-8t |  |  |  | 1.108 | 5.291 | 10.890 |  | 1.000 |  |  |
