import json
import sys

with open(r'c:\Users\k1yt\.gemini\antigravity-cli\brain\8e062b9f-7b17-4f99-b763-1207f27687a5\scratch\massive_benchmark_raw.json', 'r', encoding='utf-8') as f:
    d = json.load(f)

# Collect all divergent items
div_items = []
for p in d['pairs']:
    for it in p['items']:
        if not it['all_identical']:
            div_items.append({
                'pair': p['pair'],
                'id': it['id'],
                'type': it['type'],
                'genre': it['genre'],
                'src': it['src_text'],
                'a': it['cond_a']['translation'],
                'b': it['cond_b']['translation'],
                'c': it['cond_c']['translation'],
                'diff_ab': it['diff_ab'],
                'diff_ac': it['diff_ac'],
                'diff_bc': it['diff_bc']
            })

print(f'Total divergent items: {len(div_items)}')
print()

# Group by pair
from collections import Counter
pair_counts = Counter(x['pair'] for x in div_items)
print('Divergent items per pair:')
for pair, cnt in sorted(pair_counts.items()):
    print(f'  {pair}: {cnt}')
print()

# Type breakdown
type_counts = Counter(x['type'] for x in div_items)
print('Divergent items per type:', dict(type_counts))
print()

# Show all divergent items
for x in div_items:
    print(f"--- {x['id']} ({x['pair']}, {x['type']}, {x['genre']}) ---")
    print(f"  SRC: {x['src']}")
    print(f"  A: {x['a']}")
    print(f"  B: {x['b']}")
    print(f"  C: {x['c']}")
    print(f"  diffs: ab={x['diff_ab']}, ac={x['diff_ac']}, bc={x['diff_bc']}")
    print()
