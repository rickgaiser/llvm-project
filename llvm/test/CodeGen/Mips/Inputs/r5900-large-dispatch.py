"""Emit a computed-goto interpreter with a configurable number of targets."""

import sys


count = int(sys.argv[1])
optsize = len(sys.argv) > 2 and sys.argv[2] == "optsize"

print("define void @very_large_dispatch(ptr %destination, ptr %storage)" +
      (" #0" if optsize else "") + " {")
print("entry:")
print("  indirectbr ptr %destination, [")
for i in range(count):
    print(f"    label %bb{i}" + ("," if i + 1 < count else ""))
print("  ]")
for i in range(count):
    print(f"bb{i}:")
    print(f"  store volatile i32 {i}, ptr %storage, align 4")
    print("  br label %exit")
print("exit:")
print("  ret void")
print("}")
if optsize:
    print("attributes #0 = { optsize }")
