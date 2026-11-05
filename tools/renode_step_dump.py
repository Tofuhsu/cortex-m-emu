# Runs inside the Renode monitor (IronPython 2) through `include`.
# export_renode_trace.robot prepends TRACE_STEPS and TRACE_OUT.
# Stepping here, instead of one Robot command per register, keeps a
# 20000-step firmware trace to a few seconds and avoids running out of
# local ports (Errno 49) from one XML-RPC connection per command.
cpu = self.Machine["sysbus.cpu"]
bus = self.Machine.SystemBus
out = open(TRACE_OUT, "w")
out.write("step,pc,r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10,r11,r12,r13,r14,r15,xpsr\n")


def row(n):
    regs = [int(cpu.GetRegister(i).RawValue) for i in range(16)]
    out.write("%d,0x%x,%s,0x%x\n" % (n, regs[15], ",".join("0x%x" % r for r in regs),
                                     int(cpu.XProgramStatusRegister)))


row(0)
for n in range(1, TRACE_STEPS + 1):
    # Stop BEFORE BKPT: Renode's PC after stepping BKPT differs.
    if bus.ReadWord(int(cpu.PC.RawValue)) & 0xFF00 == 0xBE00:
        break
    cpu.Step(1)
    row(n)
out.close()
