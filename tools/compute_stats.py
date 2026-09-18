"""Compute statistics for the 30-pair 3-condition massive benchmark."""
import json
from collections import defaultdict

with open(r'c:\Users\k1yt\.gemini\antigravity-cli\brain\8e062b9f-7b17-4f99-b763-1207f27687a5\scratch\massive_benchmark_raw.json', 'r', encoding='utf-8') as f:
    d = json.load(f)

# Language family classification
FAMILY = {
    'KO': 'EastAsian', 'JA': 'EastAsian', 'ZH': 'EastAsian',
    'EN': 'Germanic',
    'DE': 'Germanic', 'NL': 'Germanic',
    'ES': 'Romance', 'PT': 'Romance', 'FR': 'Romance', 'IT': 'Romance',
    'RU': 'Slavic', 'PL': 'Slavic',
    'HI': 'IndoAryan', 'BN': 'IndoAryan', 'UR': 'IndoAryan',
    'AR': 'Semitic',
    'TR': 'Turkic',
    'TH': 'KraDai', 'VI': 'Austroasiatic', 'ID': 'Austronesian',
    'FA': 'Iranian',
}

# Group by source language family
src_family_stats = defaultdict(lambda: {'total': 0, 'div_ab': 0, 'div_ac': 0, 'div_bc': 0, 'all_ident': 0})
tgt_family_stats = defaultdict(lambda: {'total': 0, 'div_ab': 0, 'div_ac': 0, 'div_bc': 0, 'all_ident': 0})

for p in d['pairs']:
    src = p['src_code']
    tgt = p['tgt_code']
    src_f = FAMILY.get(src, 'Other')
    tgt_f = FAMILY.get(tgt, 'Other')

    s = p['stats']
    src_family_stats[src_f]['total'] += s['total_items']
    src_family_stats[src_f]['div_ab'] += s['diff_ab']
    src_family_stats[src_f]['div_ac'] += s['diff_ac']
    src_family_stats[src_f]['div_bc'] += s['diff_bc']
    src_family_stats[src_f]['all_ident'] += s['all_identical']

    tgt_family_stats[tgt_f]['total'] += s['total_items']
    tgt_family_stats[tgt_f]['div_ab'] += s['diff_ab']
    tgt_family_stats[tgt_f]['div_ac'] += s['diff_ac']
    tgt_family_stats[tgt_f]['div_bc'] += s['diff_bc']
    tgt_family_stats[tgt_f]['all_ident'] += s['all_identical']

print('=== SOURCE LANGUAGE FAMILY SENSITIVITY ===')
print(f"{'Family':<15} {'Total':>6} {'div_AB':>8} {'div_AC':>8} {'div_BC':>8} {'Ident':>6} {'AB%':>7} {'AC%':>7} {'BC%':>7}")
for f, s in sorted(src_family_stats.items(), key=lambda x: -x[1]['div_ac']/max(x[1]['total'],1)):
    t = s['total']
    print(f"{f:<15} {t:>6} {s['div_ab']:>8} {s['div_ac']:>8} {s['div_bc']:>8} {s['all_ident']:>6} "
          f"{s['div_ab']/t*100:>6.1f}% {s['div_ac']/t*100:>6.1f}% {s['div_bc']/t*100:>6.1f}%")

print()
print('=== TARGET LANGUAGE FAMILY SENSITIVITY ===')
print(f"{'Family':<15} {'Total':>6} {'div_AB':>8} {'div_AC':>8} {'div_BC':>8} {'Ident':>6} {'AB%':>7} {'AC%':>7} {'BC%':>7}")
for f, s in sorted(tgt_family_stats.items(), key=lambda x: -x[1]['div_ac']/max(x[1]['total'],1)):
    t = s['total']
    print(f"{f:<15} {t:>6} {s['div_ab']:>8} {s['div_ac']:>8} {s['div_bc']:>8} {s['all_ident']:>6} "
          f"{s['div_ab']/t*100:>6.1f}% {s['div_ac']/t*100:>6.1f}% {s['div_bc']/t*100:>6.1f}%")

# Per-pair breakdown
print()
print('=== PER-PAIR DIVERGENCE (sorted by A_vs_C sensitivity) ===')
pairs_sorted = sorted(d['pairs'], key=lambda p: -p['stats']['diff_ac'])
print(f"{'Pair':<10} {'Total':>5} {'AB':>4} {'AC':>4} {'BC':>4} {'Ident':>5} {'AB%':>6} {'AC%':>6} {'BC%':>6}")
for p in pairs_sorted:
    s = p['stats']
    t = s['total_items']
    print(f"{p['pair']:<10} {t:>5} {s['diff_ab']:>4} {s['diff_ac']:>4} {s['diff_bc']:>4} {s['all_identical']:>5} "
          f"{s['diff_ab']/t*100:>5.1f}% {s['diff_ac']/t*100:>5.1f}% {s['diff_bc']/t*100:>5.1f}%")

# Divergence type analysis
print()
print('=== DIVERGENCE TYPE BREAKDOWN ===')
sent_total = sum(1 for p in d['pairs'] for i in p['items'] if i['type']=='sentence')
para_total = sum(1 for p in d['pairs'] for i in p['items'] if i['type']=='paragraph')
print(f'Sentences: {sent_total}, Paragraphs: {para_total}')
print()
print('By-type divergence:')
for typ in ['sentence', 'paragraph']:
    items = [i for p in d['pairs'] for i in p['items'] if i['type']==typ]
    n = len(items)
    div_ab = sum(1 for i in items if i['diff_ab'])
    div_ac = sum(1 for i in items if i['diff_ac'])
    div_bc = sum(1 for i in items if i['diff_bc'])
    ident = sum(1 for i in items if i['all_identical'])
    print(f'  {typ}: total={n}, AB={div_ab} ({div_ab/n*100:.1f}%), AC={div_ac} ({div_ac/n*100:.1f}%), BC={div_bc} ({div_bc/n*100:.1f}%), identical={ident} ({ident/n*100:.1f}%)')
