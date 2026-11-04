# Week 3: exceptions, SysTick, NVIC, USART

Goal: `test_svc` passes, then a SysTick interrupt fires and returns, then the
firmware prints over USART2 to the terminal.

## Read first

- ARMv7-M Architecture Reference Manual (Arm DDI 0403E):
  - B1.4: registers, especially xPSR (APSR, IPSR, EPSR) and the two stack pointers
  - B1.5: the exception model. Exception numbers, priorities, then the two
    pseudocode functions that matter most: exception entry (`ExceptionEntry`,
    `PushStack`) and exception return (`ExceptionReturn`, the EXC_RETURN table)
  - B3.3: SysTick (SYST_CSR, SYST_RVR, SYST_CVR, SYST_CALIB)
  - B3.4: NVIC (ISER, ICER, ISPR, ICPR, IPR)
  - B5.2: MRS, MSR, CPS (CPSIE/CPSID)
- RM0390 (STM32F446): USART chapter (SR, DR, BRR, CR1), RCC_APB1ENR, GPIOx_AFRL

## Core ideas

1. **Exception entry is not an instruction.** Between two instructions the
   hardware pushes 8 words (r0-r3, r12, LR, return address, xPSR), sets LR to an
   EXC_RETURN value, sets IPSR to the exception number and jumps to the handler
   read from the vector table. `test_svc` does not count entry as a step.
2. **The frame order is fixed.** Lowest address first: r0, r1, r2, r3, r12, LR,
   return address, xPSR. The handler sees SP pointing at the stacked r0.
3. **EXC_RETURN is a magic address.** Loading 0xFFFFFFF1/F9/FD into PC in Handler
   mode (with BX, POP or LDR) means "return from exception", not "jump there".
   0xFFFFFFF9 = return to Thread mode, use the main stack. Your `write_pc_thumb`
   is where this check belongs.
4. **Stack alignment.** On the Cortex-M4, entry aligns SP down to 8 bytes and
   records that in bit 9 of the stacked xPSR. Return undoes it.
5. **Which return address.** SVC stacks the address of the *next* instruction.
   An interrupt that arrives between two instructions stacks the address of the
   instruction that has not run yet.
6. **SysTick in an instruction-accurate model.** Real SysTick counts processor
   clock cycles. Here it can only count executed instructions. That is a model
   choice: say so in the README Limits, and never turn the count into time.

## Order that works well

1. Add IPSR to `cpu_t`; MRS (read IPSR, and later PRIMASK) → first instruction of
   the SVC handler
2. SVC → exception entry: push the frame, LR = 0xFFFFFFF9, IPSR = 11, PC = vector 11
3. Exception return in `write_pc_thumb` → `test_svc`
4. SysTick model (`src/periph/systick.c`): CSR/RVR/CVR; when CVR reaches 0 with
   TICKINT set, it raises exception 15. `cpu_step` takes a pending exception
   before fetching the next instruction
5. Firmware: replace `delay()` with a SysTick tick counter
6. USART2 model: writes to DR go to stdout; SR reads with TXE and TC set.
   Needs RCC_APB1ENR and GPIO AFRL in the existing models
7. Firmware: print a boot line. Renode's UART test already expects
   `sensor-node boot`, so the same binary can be checked in both

Steps 1-3 are CPU core. Steps 4 and 6 are new peripheral models.

## Check yourself

- Why does the handler in `12_svc.s` write 42 to `[sp, #0]` instead of to r0?
- What would go wrong if exception entry did not save r0-r3 and r12 but did save
  r4-r11? (Hint: the AAPCS calling convention.)
- An interrupt arrives while `push {r4, lr}` is executing. Does it wait or
  interrupt the push? Which answer does an instruction-accurate model give?
- Why is 0xFFFFFFF9 safe to use as a magic value? Could real code ever branch there?
