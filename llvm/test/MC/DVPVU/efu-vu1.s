# RUN: llvm-mc -triple=dvpvu -mcpu=vu1 -show-encoding %s | FileCheck %s
# Test EFU (Elementary Function Unit) instructions - VU1 only

# Elementary Sine: P = sin(vs.x)
esin        p, vf1x
# CHECK: esin p, vf1x

# Elementary Cosine: P = cos(vs.y)
ecos        p, vf2y
# CHECK: ecos p, vf2y

# Elementary Exponent: P = exp(vs.z)
eexp        p, vf3z
# CHECK: eexp p, vf3z

# Elementary Logarithm: P = log(vs.w)
elog        p, vf4w
# CHECK: elog p, vf4w

# Elementary Square Root: P = sqrt(vs.x)
esqrt       p, vf5x
# CHECK: esqrt p, vf5x

# Elementary Reciprocal Square Root: P = 1/sqrt(vs.y)
ersqrt      p, vf6y
# CHECK: ersqrt p, vf6y

# Elementary Arc Tangent: P = atan(vs.z)
eatan       p, vf7z
# CHECK: eatan p, vf7z

# Wait for P register
waitp
# CHECK: waitp
