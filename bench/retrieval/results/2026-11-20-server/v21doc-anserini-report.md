# nind vs other indexers

## v21doc-stop-trec

10960555 documents (avgdl 1190.8), 215 queries, 109605 documents added by the update; k1=0.9 b=0.4, 8 threads

| system | build s | build peak MB | build largest process MB | index MB | N | avgdl | open s | k10 p50 ms | k10 p99 ms | k10 QPS | k100 p50 ms | k1000 p50 ms | search peak MB | nDCG@10 | R@100 | RR@10 | top10 = nind | update s | added docs | update | update peak MB | update = build |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| nind |  |  |  |  | 10960555 | 1190.77 | 0.064 | 5.863 | 39.662 | 116 | 10.735 | 26.588 | 699 | 0.3526 | 0.2543 | 0.6667 |  |  |  |  |  |  |
| lucene |  |  |  |  | 10960555 | 1190.77 | 0.365 | 5.016 | 36.979 | 137 | 10.514 | 38.929 | 2808 | 0.3514 | 0.2522 | 0.6631 | 0.975 |  |  |  |  |  |
| tantivy |  |  |  |  |  |  |  |  |  |  |  |  | 6 |  |  |  |  |  |  |  |  |  |
| nind-8s |  |  |  |  | 10960555 | 1190.77 | 0.054 | 6.064 | 47.469 | 111 | 12.497 | 32.991 | 701 | 0.3526 | 0.2543 | 0.6667 | 1.000 |  |  |  |  |  |
| nind-32s |  |  |  |  | 10960555 | 1190.77 | 0.057 | 8.318 | 58.448 | 88 | 17.451 | 49.792 | 709 | 0.3526 | 0.2543 | 0.6667 | 1.000 |  |  |  |  |  |

- **tantivy**: failed, see work/v21doc-anserini/v21doc-stop-trec/tantivy/search.log

## v21doc-trec

10960555 documents (avgdl 1572.4), 215 queries, 109605 documents added by the update; k1=0.9 b=0.4, 8 threads

| system | build s | build peak MB | build largest process MB | index MB | N | avgdl | open s | k10 p50 ms | k10 p99 ms | k10 QPS | k100 p50 ms | k1000 p50 ms | search peak MB | nDCG@10 | R@100 | RR@10 | top10 = nind | update s | added docs | update | update peak MB | update = build |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| nind |  |  |  |  | 10960555 | 1572.40 | 0.071 | 7.102 | 40.918 | 103 | 13.057 | 30.388 | 763 | 0.3522 | 0.2546 | 0.6657 |  |  |  |  |  |  |
| lucene |  |  |  |  | 10960555 | 1572.40 | 0.310 | 7.117 | 54.868 | 94 | 14.942 | 51.051 | 3111 | 0.3515 | 0.2526 | 0.6648 | 0.968 |  |  |  |  |  |
| tantivy |  |  |  |  |  |  |  |  |  |  |  |  | 6 |  |  |  |  |  |  |  |  |  |
| nind-8s |  |  |  |  | 10960555 | 1572.40 | 0.055 | 7.008 | 48.722 | 99 | 14.829 | 44.176 | 766 | 0.3522 | 0.2546 | 0.6657 | 1.000 |  |  |  |  |  |

- **tantivy**: failed, see work/v21doc-anserini/v21doc-trec/tantivy/search.log

