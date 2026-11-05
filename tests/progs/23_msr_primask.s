@ Preserve an outer disabled critical section, then explicitly restore zero.
@ ARM ARM B5.2.3. No flag changes. Matches Renode 1.17 single-step.
.include "vectors.inc"
_start:
    cpsid i
    mrs r4, primask
    movs r0, #0
    msr primask, r0
    mrs r5, primask
    msr primask, r4
    mrs r6, primask
    msr primask, r0
    mrs r7, primask
    bkpt #0
