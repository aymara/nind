| dataset | system | build s | build peak MB | index MB | k10 p50 ms | k10 p99 ms | k1000 p50 ms | nDCG@10 | top10 = nind | update s | update |
|---|---|---|---|---|---|---|---|---|---|---|---|
| fiqa | nind | 1.31 | 209 | 15.1 | 1.642 | 3.296 | 2.220 | 0.2365 |  | 3.125 | in place |
| fiqa | nind+local | 2.07 | 211 | 51.5 | 1.636 | 3.392 | 2.153 | 0.2365 | 1.000 | 3.193 | in place |
| fiqa | lucene | 2.94 | 561 | 9.3 | 1.033 | 2.916 | 11.312 | 0.2375 | 0.959 | 1.063 | in place |
| fiqa | lucene+pos | 3.93 | 1254 | 47.0 | 0.971 | 2.543 | 11.484 | 0.2375 | 0.959 | 1.070 | in place |
| fiqa | bm25s | 3.16 | 288 | 39.6 | 0.330 | 1.955 | 0.670 | 0.2365 | 1.000 | 2.923 | full rebuild |
| fiqa | tantivy | 0.52 | 103 | 10.5 | 0.736 | 1.263 | 2.140 | 0.2375 | 0.959 | 0.606 | in place |
| fiqa-stop | nind | 1.05 | 181 | 13.3 | 0.443 | 1.383 | 0.947 | 0.2379 |  | 2.550 | in place |
| fiqa-stop | nind+local | 1.66 | 185 | 40.6 | 0.440 | 1.305 | 1.016 | 0.2379 | 1.000 | 2.777 | in place |
| fiqa-stop | lucene | 2.80 | 503 | 8.5 | 0.803 | 1.925 | 10.542 | 0.2406 | 0.964 | 0.995 | in place |
| fiqa-stop | lucene+pos | 3.69 | 1130 | 38.4 | 0.854 | 2.193 | 10.631 | 0.2406 | 0.964 | 1.156 | in place |
| fiqa-stop | bm25s | 2.50 | 222 | 32.7 | 0.248 | 2.709 | 0.535 | 0.2379 | 1.000 | 2.256 | full rebuild |
| fiqa-stop | tantivy | 0.64 | 90 | 9.2 | 0.308 | 0.677 | 1.514 | 0.2406 | 0.965 | 1.224 | in place |
| msmarco | nind | 84.35 | 8765 | 1053.6 | 132.917 | 388.855 | 140.297 | 0.2263 |  | 437.697 | in place |
| msmarco | nind+local | 291.54 | 8971 | 3812.4 |  |  |  |  |  | 340.490 | in place |
| msmarco | lucene | 52.83 | 3438 | 709.1 | 2.398 | 16.883 | 20.312 | 0.2276 | 0.974 | 3.466 | in place |
| msmarco | lucene+pos | 106.31 | 5459 | 3058.3 | 2.407 | 19.941 | 22.112 | 0.2276 | 0.974 | 4.327 | in place |
| msmarco | bm25s | 413.24 | 18153 | 2852.8 | 49.721 | 429.079 | 47.450 | 0.2263 | 0.998 | 340.787 | full rebuild |
| msmarco | tantivy | 55.09 | 1481 | 739.3 | 41.108 | 108.156 | 46.218 | 0.2270 | 0.973 | 0.850 | in place |
| msmarco-stop | nind | 58.24 | 6535 | 899.6 | 14.531 | 108.985 | 15.855 | 0.2280 |  | 237.125 | in place |
| msmarco-stop | nind+local | 146.68 | 6763 | 2992.3 |  |  |  |  |  | 240.543 | in place |
| msmarco-stop | lucene | 36.82 | 3439 | 646.9 | 1.621 | 13.052 | 15.953 | 0.2270 | 0.987 | 2.156 | in place |
| msmarco-stop | lucene+pos | 75.01 | 5638 | 2525.4 | 1.541 | 11.462 | 15.397 | 0.2270 | 0.987 | 3.377 | in place |
| msmarco-stop | bm25s | 246.53 | 14003 | 2236.8 | 393.569 | 507.768 | 349.030 | 0.2280 | 0.997 | 251.560 | full rebuild |
| msmarco-stop | tantivy | 49.67 | 1447 | 658.0 | 5.709 | 29.599 | 9.391 | 0.2273 | 0.986 | 0.656 | in place |
| msmarco-stop-1M | nind | 6.33 | 942 | 109.7 | 1.195 | 7.299 | 1.979 |  |  | 19.916 | in place |
| msmarco-stop-1M | nind+local | 12.66 | 973 | 334.0 |  |  |  |  |  | 21.331 | in place |
| msmarco-stop-1M | lucene | 6.39 | 1077 | 76.5 | 0.804 | 3.528 | 11.744 |  | 0.984 | 3.121 | in place |
| msmarco-stop-1M | lucene+pos | 11.97 | 4127 | 286.9 | 0.802 | 3.425 | 11.292 |  | 0.984 | 1.338 | in place |
| msmarco-stop-1M | bm25s | 23.67 | 1643 | 254.7 | 27.273 | 39.485 | 27.085 |  | 0.989 | 23.228 | full rebuild |
| msmarco-stop-1M | tantivy | 6.89 | 381 | 75.5 | 0.761 | 3.848 | 4.121 |  | 0.974 | 0.917 | in place |
| msmarco-stop-2M | nind | 12.05 | 1682 | 212.3 | 2.893 | 17.355 | 3.710 |  |  | 43.069 | in place |
| msmarco-stop-2M | nind+local | 29.06 | 1714 | 669.3 |  |  |  |  |  | 48.713 | in place |
| msmarco-stop-2M | lucene | 10.55 | 1290 | 149.7 | 0.978 | 5.091 | 13.133 |  | 0.983 | 1.559 | in place |
| msmarco-stop-2M | lucene+pos | 21.17 | 4414 | 572.3 | 0.938 | 5.010 | 12.269 |  | 0.983 | 1.794 | in place |
| msmarco-stop-2M | bm25s | 53.50 | 3196 | 506.3 | 60.253 | 86.745 | 59.834 |  | 0.994 | 49.128 | full rebuild |
| msmarco-stop-2M | tantivy | 13.61 | 646 | 148.2 | 1.501 | 7.376 | 5.492 |  | 0.985 | 0.822 | in place |
| msmarco-stop-4M | nind | 28.00 | 3120 | 415.8 | 7.232 | 44.214 | 7.529 |  |  | 98.486 | in place |
| msmarco-stop-4M | nind+local | 60.58 | 3213 | 1346.1 |  |  |  |  |  | 100.259 | in place |
| msmarco-stop-4M | lucene | 19.55 | 2174 | 299.8 | 1.190 | 6.587 | 13.912 |  | 0.983 | 2.155 | in place |
| msmarco-stop-4M | lucene+pos | 38.39 | 4598 | 1151.6 | 1.181 | 6.665 | 14.372 |  | 0.983 | 2.959 | in place |
| msmarco-stop-4M | bm25s | 112.34 | 6367 | 1012.7 | 147.101 | 202.162 | 147.447 |  | 0.998 | 104.742 | full rebuild |
| msmarco-stop-4M | tantivy | 26.51 | 1166 | 295.9 | 2.680 | 13.543 | 6.101 |  | 0.984 | 0.569 | in place |
| nfcorpus | nind | 0.37 | 74 | 2.3 | 0.021 | 0.150 | 0.104 | 0.3067 |  | 0.290 | in place |
| nfcorpus | nind+local | 0.48 | 74 | 6.0 | 0.022 | 0.248 | 0.105 | 0.3067 | 1.000 | 0.316 | in place |
| nfcorpus | lucene | 2.08 | 308 | 1.4 | 0.526 | 2.869 | 3.470 | 0.3080 | 0.988 | 0.789 | in place |
| nfcorpus | lucene+pos | 2.54 | 622 | 5.8 | 0.487 | 2.572 | 3.494 | 0.3080 | 0.988 | 0.903 | in place |
| nfcorpus | bm25s | 0.32 | 74 | 4.4 | 0.068 | 0.138 | 0.268 | 0.3067 | 1.000 | 0.335 | full rebuild |
| nfcorpus | tantivy | 0.91 | 71 | 1.1 | 0.023 | 0.088 | 0.381 | 0.3078 | 0.988 | 0.788 | in place |
| nfcorpus-stop | nind | 0.41 | 71 | 2.2 | 0.018 | 0.071 | 0.068 | 0.3079 |  | 0.265 | in place |
| nfcorpus-stop | nind+local | 0.49 | 71 | 5.1 | 0.018 | 0.071 | 0.053 | 0.3079 | 1.000 | 0.295 | in place |
| nfcorpus-stop | lucene | 2.32 | 304 | 1.4 | 0.561 | 2.593 | 1.645 | 0.3080 | 0.989 | 0.842 | in place |
| nfcorpus-stop | lucene+pos | 2.57 | 599 | 5.0 | 0.474 | 1.906 | 1.609 | 0.3080 | 0.989 | 0.873 | in place |
| nfcorpus-stop | bm25s | 0.25 | 67 | 3.9 | 0.092 | 0.170 | 0.262 | 0.3079 | 1.000 | 0.247 | full rebuild |
| nfcorpus-stop | tantivy | 0.94 | 65 | 1.0 | 0.018 | 0.045 | 0.144 | 0.3075 | 0.990 | 0.853 | in place |
| scifact | nind | 0.43 | 79 | 3.1 | 0.191 | 0.440 | 0.537 | 0.6615 |  | 0.338 | in place |
| scifact | nind+local | 0.52 | 83 | 8.1 | 0.180 | 0.419 | 0.544 | 0.6615 | 1.000 | 0.361 | in place |
| scifact | lucene | 2.10 | 317 | 1.9 | 0.830 | 2.617 | 8.857 | 0.6611 | 0.981 | 0.778 | in place |
| scifact | lucene+pos | 2.63 | 632 | 7.9 | 0.813 | 3.117 | 8.671 | 0.6611 | 0.981 | 0.884 | in place |
| scifact | bm25s | 0.43 | 82 | 5.8 | 0.073 | 0.179 | 0.313 | 0.6615 | 1.000 | 0.427 | full rebuild |
| scifact | tantivy | 0.84 | 74 | 1.5 | 0.098 | 0.195 | 1.051 | 0.6611 | 0.981 | 0.803 | in place |
| scifact-stop | nind | 0.44 | 78 | 2.9 | 0.067 | 0.181 | 0.369 | 0.6645 |  | 0.304 | in place |
| scifact-stop | nind+local | 0.49 | 86 | 6.8 | 0.068 | 0.197 | 0.374 | 0.6645 | 1.000 | 0.329 | in place |
| scifact-stop | lucene | 2.21 | 327 | 1.8 | 0.646 | 1.935 | 8.860 | 0.6639 | 0.982 | 0.756 | in place |
| scifact-stop | lucene+pos | 2.65 | 646 | 6.8 | 0.683 | 1.716 | 8.679 | 0.6639 | 0.982 | 0.863 | in place |
| scifact-stop | bm25s | 0.35 | 77 | 5.2 | 0.081 | 0.299 | 0.316 | 0.6645 | 1.000 | 0.337 | full rebuild |
| scifact-stop | tantivy | 0.96 | 73 | 1.4 | 0.053 | 0.118 | 1.011 | 0.6639 | 0.982 | 0.870 | in place |
