# Paper

`main.tex` is the Phase 6 manuscript draft (`docs/ROADMAP.md`). Build with:

```
make
```

Requires `pdflatex` and `bibtex` (TeX Live; the `pgfplots`, `booktabs`, `siunitx`, and `natbib`
packages). `make clean` removes build artifacts (`main.pdf` is left in place — commit it alongside
`main.tex` so the paper is readable without a LaTeX toolchain).

`data/*.dat` holds the exact same-host benchmark numbers the figures and tables in `main.tex` plot
— sourced from the runs documented in `docs/PHASE5_RESULTS.md`. To regenerate `postfix_*.dat` from
a fresh benchmark run:

```
./benchmarks/numaring_benchmarks --benchmark_filter=BM_QueueSuite \
  --benchmark_out=results.json --benchmark_out_format=json
```

then extract `items_per_second`/`p50_ns`/`p95_ns`/`p99_ns`/`p999_ns` per implementation and thread
count into the same column layout.
