# RUN: llvm-mc -triple=mips64el -mcpu=r5900 -show-encoding %s | FileCheck %s
# RUN: llvm-mc -triple=mips64el -mcpu=r5900 -target-abi=n32 -show-encoding %s \
# RUN:   | FileCheck %s

# The MMI shifts by an immediate ($sa in their assembly strings, not the SA
# register of QFSRV).

	psllh	$5, $4, 2
# CHECK: psllh $5, $4, 2 # encoding: [0xb4,0x28,0x04,0x70]
	psrlh	$6, $1, 8
# CHECK: psrlh $6, $1, 8 # encoding: [0x36,0x32,0x01,0x70]
	psrah	$7, $2, 15
# CHECK: psrah $7, $2, 15 # encoding: [0xf7,0x3b,0x02,0x70]
	psllw	$5, $4, 31
# CHECK: psllw $5, $4, 31 # encoding: [0xfc,0x2f,0x04,0x70]
	psrlw	$6, $1, 16
# CHECK: psrlw $6, $1, 16 # encoding: [0x3e,0x34,0x01,0x70]
	psraw	$7, $2, 1
# CHECK: psraw $7, $2, 1 # encoding: [0x7f,0x38,0x02,0x70]
