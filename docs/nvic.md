# NVIC milestone decisions

[PM0214 4.3](https://www.st.com/resource/en/programming_manual/DM00046982-.pdf), ARM ARM B3.4, and RM0390's vector table are the references. ISER starts at E000E100, ICER E000E180, ISPR E000E200, ICPR E000E280, IABR E000E300 and IPR E000E400. USART2 is IRQ38, hence exception54 and vector offset D8.

CPU pending state is eight uint32 words indexed by exception number. It accepts SysTick15 and external16..255; invalid numbers are rejected before indexing. This gives the architectural capacity of 240 external IRQs, not a claim that every vector exists on F446. Requests coalesce; failed entry preserves pending.

NVIC owns enable/IPR state, borrows the CPU, and installs an enable-query callback. Its pending-register views use the CPU bitset directly, avoiding two copies that can diverge. Peripheral sources can call `nvic_pend(nvic, irq)` without knowing CPU fields. Install after `cpu_reset`; `nvic_reset` clears external enables/pending and reinstalls the query. Free NVIC before its borrowed CPU expires.

ISER/ISPR are write-one-to-set; ICER/ICPR write-one-to-clear. Zero writes have no effect. IABR reflects the active IPSR and ignores writes. IPR supports byte/halfword/word lanes and retains four priority bits. These priority values do NOT yet arbitrate: the lowest pending enabled exception number wins. No nesting, priority grouping, tail-chaining, late arrival, STIR or interrupt latency is modeled. Handler-mode pending is retained until return to Thread mode.

PRIMASK gates entry only; it does not block setting pending, peripheral counting, or frame restoration. The first handler instruction executes within the dispatching CPU step. MSR PRIMASK was added for firmware I2C critical windows; only bit0 is writable, NZCV unchanged, SP/PC operands rejected. Unprivileged execution remains unimplemented. USART TX interrupts remain optional/deferred.

Programs 19 and 20 exercise software pending and the disabled case; 23 exercises preserving an outer PRIMASK. They still require clean GNU assembly and Renode comparison. Host tests additionally check coalescing, bank bounds, priority-policy selection, active state, stacked return PC, and exception return.
