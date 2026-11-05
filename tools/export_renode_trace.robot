*** Settings ***
Library             OperatingSystem

*** Variables ***
${BIN}              ${EMPTY}
${REPL}             ${EMPTY}
# Optional C# peripheral model the platform file needs (loaded with include).
${MODEL}            ${EMPTY}
# Optional monitor command run after the platform is loaded.
${AFTER_LOAD}       ${EMPTY}
${OUT}              ${OUTPUT DIR}/renode.csv
${STEPS}            100

*** Test Cases ***
Export Register Snapshots
    [Documentation]    Post-step CSV; row zero follows explicit GP/NZCV reset preconditions. Run through renode-test, not plain robot.
    ...                Stops BEFORE BKPT (Renode stepping BKPT differs); no silent state normalization.
    Should Not Be Empty    ${BIN}
    Should Not Be Empty    ${REPL}
    Run Keyword If    '${MODEL}' != ''    Execute Command    include @${MODEL}
    Execute Command    mach create "trace"
    Execute Command    machine LoadPlatformDescription @${REPL}
    Run Keyword If    '${AFTER_LOAD}' != ''    Execute Command    ${AFTER_LOAD}
    Execute Command    sysbus LoadBinary @${BIN} 0x08000000
    # Without VTOR at flash and an explicit start, the first Step resets from
    # address 0 (SP = 0) and enters HardFault before the first instruction.
    Execute Command    cpu VectorTableOffset 0x08000000
    Execute Command    cpu ExecutionMode SingleStep
    Execute Command    start
    # Declared common test preconditions for architecturally UNKNOWN reset GP/NZCV.
    FOR    ${index}    IN RANGE    13
        Execute Command    cpu SetRegister "R${index}" 0
    END
    Execute Command    cpu SetRegister "cpsr" 0x01000000
    # LR is deliberately left at Renode's reset value; use the explicit comparator allowlist.
    ${body}=    Get File    ${CURDIR}/renode_step_dump.py
    ${steps}=    Evaluate    int($STEPS)
    Create File    ${OUT}.py    TRACE_STEPS = ${steps}\nTRACE_OUT = r"${OUT}"\n${body}
    Execute Command    include @${OUT}.py
    Remove File    ${OUT}.py
