# temporal-slab Tools

Utilities for benchmark analysis and visualization.

## Setup

Install Python dependencies:
```bash
pip install -r requirements.txt
```

Or system packages (Debian/Ubuntu):
```bash
sudo apt install python3-matplotlib python3-numpy
```

## plot_bench.py

Generate charts from benchmark CSV output.

### Usage

```bash
# Generate charts from default location
python3 tools/plot_bench.py

# Custom input/output directories
python3 tools/plot_bench.py --input benchmarks/results --output docs/images
```

### Generated Charts

**latency_percentiles.png** - Allocation vs free latency (p50/p95/p99/p999)
- Shows tail latency behavior
- Compares alloc vs free performance
- 100ns threshold line for HFT requirements

**latency_cdf.png** - Cumulative distribution (approximated from percentiles)
- Visualizes latency distribution
- Helps identify jitter sources
- Logarithmic scale for wide range

**fragmentation.png** - Internal fragmentation by size class
- Average wasted bytes per class
- Space efficiency percentage
- Overall 88.9% efficiency target line

**summary.png** - Performance summary card
- Key metrics in one view
- Suitable for README embedding
- Text-based, easy to read

### Input Format

Expects CSV files in `benchmarks/results/` with schema:
```csv
allocator,threads,size_class,op,avg_ns,p50_ns,p95_ns,p99_ns,p999_ns
temporal-slab,1,128,alloc,73.7,70,85,212,2537
```

And:
```csv
allocator,size_class,requested,rounded,wasted,efficiency_pct
temporal-slab,64,48,64,16,75.0
```

### Dependencies

- **matplotlib** - Chart generation
- **numpy** - Statistical calculations

### Example Output

```
Found 1 CSV file(s) in benchmarks/results
Loading test.csv...
Loaded 2 latency rows, 25 fragmentation rows

Generating visualizations...
Generated: docs/images/latency_percentiles.png
Generated: docs/images/latency_cdf.png
Generated: docs/images/fragmentation.png
Generated: docs/images/summary.png

✓ All charts generated in docs/images
```

## Future Tools

**Planned additions:**
- `compare_allocators.py` - Side-by-side comparison (temporal-slab vs jemalloc/tcmalloc)
- `parse_output.py` - Convert benchmark stdout to CSV (if needed)
- `profile_sizes.py` - LD_PRELOAD malloc interceptor for size distribution analysis
