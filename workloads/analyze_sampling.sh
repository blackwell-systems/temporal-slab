#!/bin/bash
#
# analyze_sampling.sh - Post-process sampling data to build truth tables
#
# Purpose: Convert raw per-thread sampling output into percentile distributions
#          for tail latency attribution (CPU work vs scheduler interference)
#
# Usage:
#   ./contention_sampling_test 8 2>&1 | tee output.txt
#   ./analyze_sampling.sh output.txt
#
# Outputs:
#   1. Percentile table (p50/p90/p95/p99/p999/max)
#   2. Truth table for tail attribution
#   3. Repair timing analysis
#
# Environment: WSL2 Ubuntu 22.04, requires bc for arithmetic
# Date: Feb 10 2026

set -e

if [ $# -lt 1 ]; then
    echo "Usage: $0 <log_file>"
    echo ""
    echo "Example:"
    echo "  ./contention_sampling_test 8 2>&1 | tee output.txt"
    echo "  ./analyze_sampling.sh output.txt"
    exit 1
fi

LOG_FILE="$1"
if [ ! -f "$LOG_FILE" ]; then
    echo "Error: File not found: $LOG_FILE"
    exit 1
fi

echo "=== Sampling Data Analysis ==="
echo "Date: $(date '+%Y-%m-%d %H:%M:%S')"
echo "Log file: $LOG_FILE"
echo ""

# Extract per-thread statistics
echo "=== Per-Thread Summary ==="
grep -E "^\[Thread [0-9]+\] Samples:" "$LOG_FILE" | while read line; do
    echo "$line"
done

echo ""
echo "=== Aggregate Percentiles (Estimated) ==="
echo "Note: True percentiles require sample-level data (not yet exported)"
echo ""

# Extract average values across all threads
AVG_WALL=$(grep "Avg: wall=" "$LOG_FILE" | grep -v "repair" | awk -F'wall=' '{print $2}' | awk '{print $1}' | awk '{sum+=$1; count++} END {if(count>0) print int(sum/count); else print 0}')
AVG_CPU=$(grep "Avg: cpu=" "$LOG_FILE" | grep -v "repair" | awk -F'cpu=' '{print $2}' | awk '{print $1}' | awk '{sum+=$1; count++} END {if(count>0) print int(sum/count); else print 0}')
AVG_WAIT=$(grep "Avg: wait=" "$LOG_FILE" | grep -v "repair" | awk -F'wait=' '{print $2}' | awk '{print $1}' | awk '{sum+=$1; count++} END {if(count>0) print int(sum/count); else print 0}')

MAX_WALL=$(grep "Max: wall=" "$LOG_FILE" | grep -v "repair" | awk -F'wall=' '{print $2}' | awk '{print $1}' | sort -n | tail -1)
MAX_CPU=$(grep "Max: cpu=" "$LOG_FILE" | grep -v "repair" | awk -F'cpu=' '{print $2}' | awk '{print $1}' | sort -n | tail -1)
MAX_WAIT=$(grep "Max: wait=" "$LOG_FILE" | grep -v "repair" | awk -F'wait=' '{print $2}' | awk '{print $1}' | sort -n | tail -1)

echo "Metric     | Avg (ns) | Max (ns) |"
echo "-----------|----------|----------|"
echo "Wall time  | $AVG_WALL    | $MAX_WALL   |"
echo "CPU time   | $AVG_CPU     | $MAX_CPU    |"
echo "Wait time  | $AVG_WAIT    | $MAX_WAIT   |"

if [ "$AVG_CPU" -gt 0 ]; then
    RATIO=$(echo "scale=2; $AVG_WALL / $AVG_CPU" | bc)
    echo ""
    echo "Wall/CPU ratio: ${RATIO}x"
fi

echo ""
echo "=== Truth Table for Tail Attribution ==="
echo ""

# Classify based on wait_ns vs cpu_ns relationship
if [ "$AVG_WAIT" -gt "$AVG_CPU" ]; then
    echo "Dominant factor: SCHEDULER INTERFERENCE"
    echo "  wait_ns > cpu_ns: Preemption dominates tail latency"
    echo "  Recommendation: Run on native Linux for accurate allocator measurements"
elif [ "$AVG_CPU" -gt 1000 ]; then
    echo "Dominant factor: ALLOCATOR CONTENTION"
    echo "  cpu_ns > 1µs: Lock/CAS contention visible in CPU time"
    echo "  Recommendation: Reduce thread count or increase temporal locality"
else
    echo "Dominant factor: FAST PATH"
    echo "  cpu_ns < 1µs: Allocation mostly lock-free, minimal contention"
    echo "  Status: Healthy"
fi

echo ""

# Analyze repairs if present
REPAIR_COUNT=$(grep "Total repairs observed:" "$LOG_FILE" | awk '{print $NF}')
if [ ! -z "$REPAIR_COUNT" ] && [ "$REPAIR_COUNT" -gt 0 ]; then
    echo "=== Repair Timing Analysis ==="
    
    AVG_REPAIR_WALL=$(grep "REPAIRS DETECTED" -A 1 "$LOG_FILE" | grep "Avg: wall=" | awk -F'wall=' '{print $2}' | awk '{print $1}' | awk '{sum+=$1; count++} END {if(count>0) print int(sum/count); else print 0}')
    AVG_REPAIR_CPU=$(grep "REPAIRS DETECTED" -A 1 "$LOG_FILE" | grep "Avg: cpu=" | awk -F'cpu=' '{print $2}' | awk '{print $1}' | awk '{sum+=$1; count++} END {if(count>0) print int(sum/count); else print 0}')
    AVG_REPAIR_WAIT=$(grep "REPAIRS DETECTED" -A 1 "$LOG_FILE" | grep "Avg: wait=" | awk -F'wait=' '{print $2}' | awk '{print $1}' | awk '{sum+=$1; count++} END {if(count>0) print int(sum/count); else print 0}')
    
    MAX_REPAIR_WALL=$(grep "REPAIRS DETECTED" -A 2 "$LOG_FILE" | grep "Max: wall=" | awk -F'wall=' '{print $2}' | awk '{print $1}' | sort -n | tail -1)
    MAX_REPAIR_CPU=$(grep "REPAIRS DETECTED" -A 2 "$LOG_FILE" | grep "Max: cpu=" | awk -F'cpu=' '{print $2}' | awk '{print $1}' | sort -n | tail -1)
    MAX_REPAIR_WAIT=$(grep "REPAIRS DETECTED" -A 2 "$LOG_FILE" | grep "Max: wait=" | awk -F'wait=' '{print $2}' | awk '{print $1}' | sort -n | tail -1)
    
    echo "Repairs detected: $REPAIR_COUNT"
    echo ""
    echo "Metric     | Avg (ns) | Max (ns) |"
    echo "-----------|----------|----------|"
    echo "Wall time  | $AVG_REPAIR_WALL | $MAX_REPAIR_WALL |"
    echo "CPU time   | $AVG_REPAIR_CPU  | $MAX_REPAIR_CPU  |"
    echo "Wait time  | $AVG_REPAIR_WAIT | $MAX_REPAIR_WAIT |"
    
    if [ "$AVG_REPAIR_CPU" -gt 0 ]; then
        REPAIR_RATIO=$(echo "scale=2; $AVG_REPAIR_WALL / $AVG_REPAIR_CPU" | bc)
        echo ""
        echo "Repair wall/CPU ratio: ${REPAIR_RATIO}x"
        
        if [ "$AVG_REPAIR_WAIT" -gt "$AVG_REPAIR_CPU" ]; then
            echo "  Interpretation: Repairs blocked by scheduler (WSL2/VM)"
        elif [ "$AVG_REPAIR_CPU" -gt 5000 ]; then
            echo "  Interpretation: Repairs are CPU-intensive (list ops/contention)"
        else
            echo "  Interpretation: Repairs are fast, no impact on tail latency"
        fi
    fi
    
    # Repair rate
    TOTAL_ALLOCS=$(grep "Total allocations:" "$LOG_FILE" | tail -1 | awk '{print $NF}')
    if [ ! -z "$TOTAL_ALLOCS" ] && [ "$TOTAL_ALLOCS" -gt 0 ]; then
        REPAIR_RATE=$(echo "scale=6; $REPAIR_COUNT * 100 / $TOTAL_ALLOCS" | bc)
        ALLOCS_PER_REPAIR=$(echo "scale=0; $TOTAL_ALLOCS / $REPAIR_COUNT" | bc)
        echo ""
        echo "Repair rate: ${REPAIR_RATE}% (1 per ${ALLOCS_PER_REPAIR} allocations)"
    fi
fi

echo ""
echo "=== Environment ==="
grep -E "^(Date|Environment|Threads|Pattern):" "$LOG_FILE" || echo "(No environment info found)"

echo ""
echo "=== Analysis Complete ==="
echo ""
echo "Limitations of current analysis:"
echo "  - Percentiles are estimated from per-thread averages (not true sample-level)"
echo "  - Need to export raw sample arrays for accurate p99/p999 computation"
echo ""
echo "Future enhancement: Export ThreadStats samples to CSV for precise percentile calculation"
