"""Run a small benchmark sweep. Usage: python example.py"""
import sortbench as sb

print("available contenders:")
for key, name, kind, ok in sb.list_contenders():
    print(f"  {key:8} {name:22} {kind:9} {'available' if ok else 'NOT INSTALLED'}")
print()

# single dataset
sb.benchmark(n=1_000_000, dtype="int32", distribution="uniform").print_table()
print()

# across data shapes (smaller n so the pure-python contender stays quick)
for dist, res in sb.sweep(300_000, dtype="int32",
                          distributions=["uniform", "duplicates", "few_distinct",
                                         "near_sorted", "sorted"]).items():
    res.print_table()
    print()
