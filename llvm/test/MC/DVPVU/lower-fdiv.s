# RUN: llvm-mc -triple=dvpvu -show-encoding %s | FileCheck %s
# Test Division and Square Root (Lower) instructions

# Division: Q = vs.x / vt.y
div         q, vf1x, vf2y
# CHECK: div q, vf1x, vf2y

# Square Root: Q = sqrt(vt.z)
sqrt        q, vf3z
# CHECK: sqrt q, vf3z

# Reciprocal Square Root: Q = vs.w / sqrt(vt.x)
rsqrt       q, vf4w, vf5x
# CHECK: rsqrt q, vf4w, vf5x

# Wait for Q register
waitq
# CHECK: waitq
