import json

json_path = r"C:\Users\k1yt\.gemini\antigravity-cli\brain\8e062b9f-7b17-4f99-b763-1207f27687a5\scratch\fidelity_experiment_raw.json"
with open(json_path, "r", encoding="utf-8") as f:
    data = json.load(f)

print(f"Total Sentences: {data['metadata']['total_sentences']}")
print(f"Summary stats: {data['metadata']['summary_stats']}")
print()

for pair in data["pairs"]:
    pname = pair["pair"]
    diff_count = pair["stats"]["different"]
    total = pair["stats"]["total"]
    print(f"==================================================")
    print(f"PAIR: {pname} | Diffs: {diff_count}/{total} ({(diff_count/total)*100:.0f}%)")
    print(f"==================================================")
    for itm in pair["items"]:
        if itm["is_different"]:
            print(f"[{itm['id']}] Genre: {itm['genre']}")
            print(f"  SRC:    {itm['src_text']}")
            print(f"  Cond A: {itm['cond_a']['translation']}")
            print(f"  Cond B: {itm['cond_b']['translation']}")
            print()
