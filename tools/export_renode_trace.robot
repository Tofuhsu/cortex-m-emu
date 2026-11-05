*** Settings ***
Library             OperatingSystem
Library             Collections

*** Variables ***
${BIN}              ${EMPTY}
${REPL}             ${EMPTY}
${OUT}              ${OUTPUT DIR}/renode.csv
${STEPS}            100

*** Test Cases ***
Export Register Snapshots
    [Documentation]    Post-step CSV, row zero follows explicit GP/NZCV reset preconditions. Run through renode-test, not plain robot.
    ...                Stop BEFORE BKPT (Renode stepping BKPT differs); no silent state normalization.
    Should Not Be Empty    ${BIN}
    Should Not Be Empty    ${REPL}
    Execute Command    mach create "trace"
    Execute Command    machine LoadPlatformDescription @${REPL}
    Execute Command    sysbus LoadBinary @${BIN} 0x08000000
    ${sp}=    Execute Command    sysbus ReadDoubleWord 0x08000000
    ${rv}=    Execute Command    sysbus ReadDoubleWord 0x08000004
    ${spint}=    Evaluate    int(str($sp).strip(), 0)
    ${rvint}=    Evaluate    int(str($rv).strip(), 0) & ~1
    Execute Command    cpu SetRegister "SP" ${spint}
    Execute Command    cpu SetRegister "PC" ${rvint}
    # Declared common test preconditions for architecturally UNKNOWN reset GP/NZCV.
    FOR    ${index}    IN RANGE    13
        Execute Command    cpu SetRegister "R${index}" 0
    END
    Execute Command    cpu SetRegister "cpsr" 0x01000000
    # LR is deliberately left at Renode reset value; use explicit comparator allowlist.
    Create File    ${OUT}    step,pc,r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10,r11,r12,r13,r14,r15,xpsr\n
    Append Snapshot    0
    ${limit}=    Evaluate    int($STEPS) + 1
    FOR    ${step}    IN RANGE    1    ${limit}
        ${pc}=    Execute Command    cpu GetRegister "PC"
        ${pcint}=    Evaluate    int(str($pc).strip(), 0)
        ${insn}=    Execute Command    sysbus ReadWord ${pcint}
        ${bkpt}=    Evaluate    int(str($insn).strip(), 0) & 0xFF00 == 0xBE00
        IF    ${bkpt}    BREAK
        Execute Command    cpu Step
        Append Snapshot    ${step}
    END

*** Keywords ***
Append Snapshot
    [Arguments]    ${step}
    @{values}=    Create List
    FOR    ${reg}    IN RANGE    16
        ${name}=    Evaluate    'R' + str($reg) if $reg < 13 else ['SP','LR','PC'][$reg-13]
        ${value}=    Execute Command    cpu GetRegister "${name}"
        ${hex}=    Evaluate    hex(int(str($value).strip(), 0))
        Append To List    ${values}    ${hex}
    END
    ${psr}=    Execute Command    cpu XProgramStatusRegister
    ${psrhex}=    Evaluate    hex(int(str($psr).strip(), 0))
    ${pc}=    Get From List    ${values}    15
    ${registers}=    Evaluate    ','.join($values)
    Append To File    ${OUT}    ${step},${pc},${registers},${psrhex}\n
