#!/usr/bin/env python3

import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

fig, ax = plt.subplots(figsize=(10, 8))
ax.axis('off')

# Title
title_text = "temporal-slab Performance Summary"
title_underline = "=" * len(title_text)
ax.text(0.5, 0.95, title_text, fontsize=20, fontweight='bold', 
        ha='center', va='top', family='monospace')
ax.text(0.5, 0.93, title_underline, fontsize=16, 
        ha='center', va='top', family='monospace')

# Thesis statement
thesis = ("temporal-slab eliminates allocator-induced latency spikes and RSS drift\n"
          "in churn-heavy, fixed-size workloads by aligning allocation with lifetime phases.")
ax.text(0.5, 0.89, thesis, fontsize=11, ha='center', va='top', 
        family='sans-serif', style='italic', color='#2c3e50')

# Allocation Latency
latency_y = 0.81
ax.text(0.1, latency_y, "Allocation Latency:", fontsize=14, fontweight='bold', 
        va='top', family='monospace')
ax.text(0.12, latency_y - 0.04, "• p50:    30 ns     (median)", fontsize=12, 
        va='top', family='monospace')
ax.text(0.12, latency_y - 0.08, "• p99:    374 ns    (3.3× better than malloc)", fontsize=12, 
        va='top', family='monospace', color='#27ae60', fontweight='bold')
ax.text(0.12, latency_y - 0.12, "• p99.9:  1137 ns   (4.7× better than malloc)", fontsize=12, 
        va='top', family='monospace', color='#27ae60', fontweight='bold')
ax.text(0.12, latency_y - 0.16, "• Variance: 19.6× (vs malloc 110.8×)", fontsize=12, 
        va='top', family='monospace')

# RSS Stability
rss_y = 0.60
ax.text(0.1, rss_y, "RSS Stability:", fontsize=14, fontweight='bold', 
        va='top', family='monospace')
ax.text(0.12, rss_y - 0.04, "• Steady-state churn: 0% growth (100 cycles)", fontsize=12, 
        va='top', family='monospace', color='#27ae60', fontweight='bold')
ax.text(0.12, rss_y - 0.08, "• Long-term churn:    2.4% growth (1000 cycles)", fontsize=12, 
        va='top', family='monospace')
ax.text(0.12, rss_y - 0.12, "• Baseline RSS:       +37% vs malloc (explicit trade-off)", fontsize=12, 
        va='top', family='monospace')

# Epoch-Scoped Reclamation
reclaim_y = 0.43
ax.text(0.1, reclaim_y, "Epoch-Scoped RSS Reclamation:", fontsize=14, fontweight='bold', 
        va='top', family='monospace')
ax.text(0.12, reclaim_y - 0.04, "• API: epoch_close() defines lifetime boundaries", fontsize=12, 
        va='top', family='monospace')
ax.text(0.12, reclaim_y - 0.08, "• Mechanism: madvise(MADV_DONTNEED) on empty slabs", fontsize=12, 
        va='top', family='monospace')
ax.text(0.12, reclaim_y - 0.12, "• Result: 19.15 MiB reclaimable, 100% slab reuse, 0 new mmap calls", fontsize=11, 
        va='top', family='monospace', color='#27ae60', fontweight='bold')

# Memory Efficiency
mem_y = 0.26
ax.text(0.1, mem_y, "Memory Efficiency (Normalized):", fontsize=14, fontweight='bold', 
        va='top', family='monospace')
ax.text(0.12, mem_y - 0.04, "• Average: 88.9% (11.1% internal fragmentation)", fontsize=12, 
        va='top', family='monospace')
ax.text(0.12, mem_y - 0.08, "• Waste:   Comparable to malloc (15-25%)", fontsize=12, 
        va='top', family='monospace')

# Key Properties
props_y = 0.18
ax.text(0.1, props_y, "Key Properties:", fontsize=14, fontweight='bold', 
        va='top', family='monospace')
ax.text(0.12, props_y - 0.04, "✓ O(1) deterministic class selection", fontsize=12, 
        va='top', family='monospace')
ax.text(0.12, props_y - 0.08, "✓ Lock-free allocation fast path", fontsize=12, 
        va='top', family='monospace')
ax.text(0.12, props_y - 0.12, "✓ Safe handle validation (no crashes)", fontsize=12, 
        va='top', family='monospace')
ax.text(0.12, props_y - 0.16, "✓ Application-controlled reclamation", fontsize=12, 
        va='top', family='monospace')

# Target Workloads
target_y = -0.02
ax.text(0.1, target_y, "Target Workloads:", fontsize=14, fontweight='bold', 
        va='top', family='monospace')
ax.text(0.12, target_y - 0.04, "• Request-scoped allocation (web servers, RPC)", fontsize=11, 
        va='top', family='monospace')
ax.text(0.12, target_y - 0.07, "• Frame-based systems (games, simulations)", fontsize=11, 
        va='top', family='monospace')
ax.text(0.12, target_y - 0.10, "• Cache metadata, session stores, connection tracking", fontsize=11, 
        va='top', family='monospace')
ax.text(0.12, target_y - 0.13, "• Fixed-size, churn-heavy allocation patterns", fontsize=11, 
        va='top', family='monospace')

# Add background box
rect = mpatches.FancyBboxPatch((0.05, -0.02), 0.9, 0.99,
                                boxstyle="round,pad=0.02",
                                edgecolor='#7f8c8d',
                                facecolor='#fdfcf8',
                                linewidth=2)
ax.add_patch(rect)

plt.tight_layout()
plt.savefig('docs/images/summary.png', dpi=150, bbox_inches='tight', facecolor='white')
print("Generated: docs/images/summary.png")
