# nind vs other indexers

## v21doc-stop-trec

10960555 documents (avgdl 1190.8), 215 queries, 109605 documents added by the update; k1=0.9 b=0.4, 8 threads

| system | build s | build peak MB | build largest process MB | index MB | N | avgdl | open s | k10 p50 ms | k10 p99 ms | k10 QPS | k100 p50 ms | k1000 p50 ms | search peak MB | nDCG@10 | R@100 | RR@10 | top10 = nind | update s | added docs | update | update peak MB | update = build |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| nind |  |  |  |  | 10960555 | 1190.77 | 0.064 | 5.722 | 39.422 | 116 | 10.775 | 26.189 | 698 | 0.3526 | 0.2543 | 0.6667 |  |  |  |  |  |  |
| lucene |  |  |  |  | 10960555 | 1190.77 | 0.365 | 5.016 | 36.979 | 137 | 10.514 | 38.929 | 2808 | 0.3514 | 0.2522 | 0.6631 | 0.975 |  |  |  |  |  |
| tantivy |  |  |  |  |  |  |  |  |  |  |  |  | 6 |  |  |  |  |  |  |  |  |  |
| nind-8s |  |  |  |  | 10960555 | 1190.77 | 0.056 | 6.988 | 50.396 | 100 | 13.934 | 36.165 | 702 | 0.3526 | 0.2543 | 0.6667 | 1.000 |  |  |  |  |  |
| nind-32s |  |  |  |  | 10960555 | 1190.77 | 0.058 | 9.339 | 61.507 | 77 | 19.537 | 53.251 | 710 | 0.3526 | 0.2543 | 0.6667 | 1.000 |  |  |  |  |  |

- **tantivy**: failed, see work/v21doc-anserini/v21doc-stop-trec/tantivy/search.log

## v21doc-trec

10960555 documents (avgdl 1572.4), 215 queries, 109605 documents added by the update; k1=0.9 b=0.4, 8 threads

| system | build s | build peak MB | build largest process MB | index MB | N | avgdl | open s | k10 p50 ms | k10 p99 ms | k10 QPS | k100 p50 ms | k1000 p50 ms | search peak MB | nDCG@10 | R@100 | RR@10 | top10 = nind | update s | added docs | update | update peak MB | update = build |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| nind |  |  |  |  | 10960555 | 1572.40 | 0.062 | 7.015 | 40.859 | 102 | 13.380 | 33.802 | 763 | 0.3522 | 0.2546 | 0.6657 |  |  |  |  |  |  |
| lucene |  |  |  |  | 10960555 | 1572.40 | 0.310 | 7.117 | 54.868 | 94 | 14.942 | 51.051 | 3111 | 0.3515 | 0.2526 | 0.6648 | 0.968 |  |  |  |  |  |
| tantivy |  |  |  |  |  |  |  |  |  |  |  |  | 6 |  |  |  |  |  |  |  |  |  |
| nind-8s |  |  |  |  | 10960555 | 1572.40 | 0.057 | 8.385 | 52.159 | 86 | 17.944 | 48.403 | 767 | 0.3522 | 0.2546 | 0.6657 | 1.000 |  |  |  |  |  |

- **tantivy**: failed, see work/v21doc-anserini/v21doc-trec/tantivy/search.log

