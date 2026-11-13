# nind vs other indexers

## v21doc-stop-trec

10960555 documents (avgdl 1190.8), 215 queries, 109605 documents added by the update; k1=0.9 b=0.4, 8 threads

| system | build s | build peak MB | build largest process MB | index MB | N | avgdl | open s | k10 p50 ms | k10 p99 ms | k10 QPS | k100 p50 ms | k1000 p50 ms | search peak MB | nDCG@10 | R@100 | RR@10 | top10 = nind | update s | added docs | update | update peak MB | update = build |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| nind |  |  |  |  | 10960555 | 1190.77 | 0.066 | 10.030 | 63.760 | 73 | 15.865 | 31.005 | 748 | 0.3526 | 0.2543 | 0.6667 |  |  |  |  |  |  |
| lucene |  |  |  |  | 10960555 | 1190.77 | 0.365 | 5.016 | 36.979 | 137 | 10.514 | 38.929 | 2808 | 0.3514 | 0.2522 | 0.6631 | 0.975 |  |  |  |  |  |
| tantivy |  |  |  |  |  |  |  |  |  |  |  |  | 6 |  |  |  |  |  |  |  |  |  |
| nind-8s |  |  |  |  | 10960555 | 1190.77 | 0.056 | 8.443 | 58.866 | 85 | 15.873 | 40.553 | 994 | 0.3526 | 0.2543 | 0.6667 | 1.000 |  |  |  |  |  |
| nind-32s |  |  |  |  | 10960555 | 1190.77 | 0.055 | 10.700 | 70.983 | 69 | 21.545 | 56.683 | 1005 | 0.3526 | 0.2543 | 0.6667 | 1.000 |  |  |  |  |  |

- **tantivy**: failed, see work/v21doc-anserini/v21doc-stop-trec/tantivy/search.log

## v21doc-trec

10960555 documents (avgdl 1572.4), 215 queries, 109605 documents added by the update; k1=0.9 b=0.4, 8 threads

| system | build s | build peak MB | build largest process MB | index MB | N | avgdl | open s | k10 p50 ms | k10 p99 ms | k10 QPS | k100 p50 ms | k1000 p50 ms | search peak MB | nDCG@10 | R@100 | RR@10 | top10 = nind | update s | added docs | update | update peak MB | update = build |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| nind |  |  |  |  | 10960555 | 1572.40 | 0.126 | 15.372 | 92.608 | 49 | 22.782 | 45.017 | 755 | 0.3522 | 0.2546 | 0.6657 |  |  |  |  |  |  |
| lucene |  |  |  |  | 10960555 | 1572.40 | 0.310 | 7.117 | 54.868 | 94 | 14.942 | 51.051 | 3111 | 0.3515 | 0.2526 | 0.6648 | 0.968 |  |  |  |  |  |
| tantivy |  |  |  |  |  |  |  |  |  |  |  |  | 6 |  |  |  |  |  |  |  |  |  |
| nind-8s |  |  |  |  | 10960555 | 1572.40 | 0.056 | 11.005 | 72.764 | 68 | 20.843 | 55.521 | 1149 | 0.3522 | 0.2546 | 0.6657 | 1.000 |  |  |  |  |  |

- **tantivy**: failed, see work/v21doc-anserini/v21doc-trec/tantivy/search.log

