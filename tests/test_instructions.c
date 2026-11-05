/* Direct opcode unit tests are additional to GNU-assembled programs 24 onward.
 * They do not stand in for clean GNU assembly or a real Renode comparison.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cpu.h"
#include "loader.h"

static void initialize(cpu_t *c, bus_t *b, unsigned flags)
{
    cpu_reset(c,b);
    for(unsigned r=0;r<13;++r)c->r[r]=0x12340000u+r*4u;
    c->r[LR]=0x11223345u;
    c->n=(flags&8u)!=0; c->z=(flags&4u)!=0; c->c=(flags&2u)!=0; c->v=(flags&1u)!=0;
}
static unsigned flags_of(const cpu_t *c)
{ return (unsigned)c->n*8u+(unsigned)c->z*4u+(unsigned)c->c*2u+(unsigned)c->v; }
static int execute(cpu_t *c,bus_t *b,uint16_t hw1,uint16_t hw2)
{
    uint8_t code[]={(uint8_t)hw1,(uint8_t)(hw1>>8),(uint8_t)hw2,(uint8_t)(hw2>>8)};
    assert(!bus_load_flash(b,8,code,sizeof code));
    return cpu_step(c,b);
}
static void unchanged_except(const cpu_t *c,const cpu_t *before,unsigned dest)
{
    for(unsigned r=0;r<15;++r)if(r!=dest)assert(c->r[r]==before->r[r]);
    assert(c->steps==1 && c->ipsr==before->ipsr && c->primask==before->primask);
}
static void test_add_register(bus_t *b)
{
    cpu_t c; unsigned count=0;
    for(unsigned rd=0;rd<16;++rd)for(unsigned rm=0;rm<16;++rm)for(unsigned f=0;f<16;++f) {
        initialize(&c,b,f); cpu_t before=c;
        uint32_t left=rd==PC?FLASH_BASE+12u:c.r[rd];
        uint32_t right=rm==PC?FLASH_BASE+12u:c.r[rm];
        uint32_t result=left+right;
        bool valid=!(rd==PC&&rm==PC) && !(rd==SP&&(result&3u));
        uint16_t instruction=(uint16_t)(0x4400u|((rd&8u)<<4)|(rm<<3)|(rd&7u));
        assert(execute(&c,b,instruction,0xBE00)==(valid?0:-1));
        assert(flags_of(&c)==f);
        if(valid) {
            assert(c.r[rd]==(rd==PC?(result&~1u):result));
            if(rd!=PC)assert(c.r[PC]==FLASH_BASE+10u);
        } else assert(c.r[PC]==FLASH_BASE+8u && strstr(c.halt_reason,"UNPREDICTABLE"));
        unchanged_except(&c,&before,valid?rd:16u); ++count;
    }
    initialize(&c,b,15); c.r[2]=5; c.r[3]=7;
    assert(execute(&c,b,0x441A,0xBE00)==0 && c.r[2]==12 && flags_of(&c)==15);
    initialize(&c,b,15); c.r[2]=0xFFFFFFFFu; c.r[3]=1;
    assert(execute(&c,b,0x441A,0xBE00)==0 && c.r[2]==0 && flags_of(&c)==15);
    printf("PASS I1 ADD register: %u register/flag combinations, exact opcode, wraparound, PC/SP rules\n",count);
}
static const uint32_t edge_values[] = {0,1,0xFFFFFFFFu,0x80000000u,0x7FFFFFFFu,0xA5A55A5Au};

static unsigned nz_flags(uint32_t value)
{ return (value>>31)*8u+(value==0?4u:0u); }

static void test_data_processing(bus_t *b)
{
    const unsigned ops[]={0,1,9,15}; cpu_t c; unsigned count=0;
    for(unsigned opi=0;opi<4;++opi)for(unsigned rd=0;rd<8;++rd)for(unsigned rm=0;rm<8;++rm)
    for(unsigned x=0;x<6;++x)for(unsigned y=0;y<6;++y)for(unsigned f=0;f<16;++f) {
        initialize(&c,b,f); c.r[rd]=edge_values[x]; c.r[rm]=edge_values[y]; cpu_t before=c;
        unsigned op=ops[opi]; uint32_t left=c.r[rd],right=c.r[rm],result;
        if(op==0)result=left&right; else if(op==1)result=left^right;
        else if(op==9)result=0u-right; else result=~right;
        unsigned expected=nz_flags(result) | (op==9 ? (right==0?2u:0u)+(right==0x80000000u?1u:0u) : f&3u);
        assert(execute(&c,b,(uint16_t)(0x4000u|(op<<6)|(rm<<3)|rd),0xBE00)==0);
        assert(c.r[rd]==result && flags_of(&c)==expected && c.r[PC]==FLASH_BASE+10u);
        unchanged_except(&c,&before,rd); ++count;
    }
    const struct {uint16_t insn;uint32_t input, expected;unsigned flags;} examples[]={
        {0x4013,0xF0,0xF0,3}, {0x4053,0xF0,0,7},
        {0x425B,0,0,6}, {0x425B,0x80000000u,0x80000000u,9},
        {0x43DB,0xFFFFFFFFu,0,7}
    };
    for(unsigned i=0;i<sizeof examples/sizeof examples[0];++i) {
        initialize(&c,b,3); c.r[2]=0xF0; c.r[3]=examples[i].input;
        assert(execute(&c,b,examples[i].insn,0xBE00)==0);
        assert(c.r[3]==examples[i].expected && flags_of(&c)==examples[i].flags);
    }
    printf("PASS I2 data processing: %u operand/register/flag cases and exact objdump vectors\n",count);
}

/* Independent one-bit-at-a-time shift oracle: no C shift by 32. */
static uint32_t oracle_shift(uint32_t value,unsigned count,bool right,bool *carry)
{
    for(unsigned i=0;i<count;++i) {
        *carry=right?(value&1u)!=0:(value&0x80000000u)!=0;
        value=right?value/2u:value*2u;
    }
    return value;
}

static void test_immediate_shift(bus_t *b)
{
    cpu_t c; unsigned cases=0;
    for(unsigned right=0;right<2;++right)for(unsigned count=0;count<32;++count)
    for(unsigned rd=0;rd<8;++rd)for(unsigned rm=0;rm<8;++rm)
    for(unsigned v=0;v<6;++v)for(unsigned f=0;f<16;++f) {
        initialize(&c,b,f); c.r[rm]=edge_values[v]; cpu_t before=c;
        bool carry=c.c; unsigned amount=right&&count==0?32u:count;
        uint32_t expected=oracle_shift(c.r[rm],amount,right!=0,&carry);
        assert(execute(&c,b,(uint16_t)((right?0x0800u:0)|(count<<6)|(rm<<3)|rd),0xBE00)==0);
        assert(c.r[rd]==expected && flags_of(&c)==(nz_flags(expected)|(carry?2u:0u)|(f&1u)));
        unchanged_except(&c,&before,rd); ++cases;
    }
    initialize(&c,b,3); c.r[3]=0x01800001;
    assert(execute(&c,b,0x021B,0xBE00)==0 && c.r[3]==0x80000100u && c.c && c.v);
    initialize(&c,b,3); c.r[2]=0x80000008;
    assert(execute(&c,b,0x0912,0xBE00)==0 && c.r[2]==0x08000000 && c.c && c.v);
    printf("PASS I3 immediate shifts: %u cases including LSL0, LSR32 and carry edges\n",cases);
}

static void test_uxtb(bus_t *b)
{
    cpu_t c; unsigned count=0;
    for(unsigned rd=0;rd<8;++rd)for(unsigned rm=0;rm<8;++rm)for(unsigned v=0;v<6;++v)for(unsigned f=0;f<16;++f) {
        initialize(&c,b,f); c.r[rm]=edge_values[v]; cpu_t before=c;
        assert(execute(&c,b,(uint16_t)(0xB2C0u|(rm<<3)|rd),0xBE00)==0);
        assert(c.r[rd]==(edge_values[v]&255u) && flags_of(&c)==f);
        unchanged_except(&c,&before,rd); ++count;
    }
    initialize(&c,b,15); c.r[3]=0xABCD1280;
    assert(execute(&c,b,0xB2DB,0xBE00)==0 && c.r[3]==0x80 && flags_of(&c)==15);
    /* Nearby extension operations were not accidentally accepted. */
    const uint16_t unsupported[]={0xB200,0xB240,0xB280};
    for(unsigned i=0;i<3;++i) { initialize(&c,b,15); assert(execute(&c,b,unsupported[i],0)==-1); }
    printf("PASS I4 UXTB: %u register/value/flag combinations and extension decode boundaries\n",count);
}

static unsigned oracle_arithmetic(uint32_t x,uint32_t imm,unsigned kind,uint32_t *result)
{
    bool carry,overflow;
    if(kind==0) {
        *result=x+imm; carry=((uint64_t)x+imm)>UINT32_MAX;
        overflow=((~(x^imm)&(x^*result))>>31)!=0;
    } else {
        uint32_t left=kind==1?x:imm,right=kind==1?imm:x;
        *result=left-right; carry=left>=right;
        overflow=(((left^right)&(left^*result))>>31)!=0;
    }
    return nz_flags(*result)+(carry?2u:0u)+(overflow?1u:0u);
}

static void test_modified_arithmetic(bus_t *b)
{
    const struct {uint16_t hw1,hw2;uint32_t imm;unsigned rn,rd,kind;bool s,write;} cases[]={
        {0xF107,0x0308,8,7,3,0,false,true},
        {0xF5A3,0x3380,65536,3,3,1,false,true},
        {0xF1C3,0x4200,0x80000000u,3,2,2,false,true},
        {0xF46F,0x6180,1024,PC,1,3,false,true},
        {0xF5B3,0x4F00,32768,3,PC,1,true,false},
        {0xF513,0x4F00,32768,3,PC,0,true,false},
        {0xF117,0x0308,8,7,3,0,true,true},
        {0xF5B3,0x3380,65536,3,3,1,true,true},
        {0xF1D3,0x4200,0x80000000u,3,2,2,true,true},
    };
    cpu_t c; unsigned count=0;
    for(unsigned i=0;i<sizeof cases/sizeof cases[0];++i)for(unsigned v=0;v<6;++v)for(unsigned f=0;f<16;++f) {
        initialize(&c,b,f);
        if(cases[i].rn!=PC)c.r[cases[i].rn]=edge_values[v];
        cpu_t before=c; uint32_t expected;
        unsigned arithmetic_flags=0;
        if(cases[i].kind==3)expected=~cases[i].imm;
        else arithmetic_flags=oracle_arithmetic(c.r[cases[i].rn],cases[i].imm,cases[i].kind,&expected);
        assert(execute(&c,b,cases[i].hw1,cases[i].hw2)==0);
        if(cases[i].write)assert(c.r[cases[i].rd]==expected);
        unchanged_except(&c,&before,cases[i].write?cases[i].rd:16u);
        assert(flags_of(&c)==(cases[i].s?arithmetic_flags:f) && c.r[PC]==FLASH_BASE+12u); ++count;
    }
    /* These require arithmetic C=1 although ThumbExpandImm's rotated carry=0. */
    initialize(&c,b,0); c.r[3]=32768;
    assert(execute(&c,b,0xF5B3,0x4F00)==0 && flags_of(&c)==6 && c.r[3]==32768);
    initialize(&c,b,0); c.r[3]=0xFFFF8000u;
    assert(execute(&c,b,0xF513,0x4F00)==0 && flags_of(&c)==6 && c.r[3]==0xFFFF8000u);
    initialize(&c,b,0); c.r[3]=0x7FFF8000u;
    assert(execute(&c,b,0xF513,0x4F00)==0 && flags_of(&c)==9);
    /* ADD/SUB SP aliases, compare-SP alias, and alignment restrictions. */
    initialize(&c,b,15); uint32_t sp=c.r[SP];
    assert(execute(&c,b,0xF10D,0x0D08)==0 && c.r[SP]==sp+8 && flags_of(&c)==15);
    initialize(&c,b,15);
    assert(execute(&c,b,0xF1AD,0x0D08)==0 && c.r[SP]==sp-8 && flags_of(&c)==15);
    initialize(&c,b,0);
    assert(execute(&c,b,0xF1BD,0x0F08)==0 && c.r[SP]==sp && flags_of(&c)==2);
    initialize(&c,b,15);
    assert(execute(&c,b,0xF10D,0x0D01)==-1 && c.r[SP]==sp && flags_of(&c)==15);
    const uint16_t unsupported[][2]={
        {0xF107,0x0D08}, /* Rd=SP without SP source */
        {0xF10F,0x0308}, /* PC source on ADD */
        {0xF010,0x0101}, /* logical S=1 remains deliberately unsupported */
        {0xF063,0x0101}, /* ORN is not MVN */
        {0xF207,0x0308}, /* ADDW plain imm12, not modified immediate */
        {0xF2A7,0x0308}, /* SUBW plain imm12 */
        {0xF107,0x1300}, /* repeated pattern with zero imm8 */
    };
    for(unsigned i=0;i<sizeof unsupported/sizeof unsupported[0];++i) {
        initialize(&c,b,15); cpu_t before=c;
        assert(execute(&c,b,unsupported[i][0],unsupported[i][1])==-1);
        assert(flags_of(&c)==15 && c.r[PC]==FLASH_BASE+8u); unchanged_except(&c,&before,16);
    }
    printf("PASS I5 modified arithmetic: %u edge/flag cases, exact vectors, CMP/CMN aliases, arithmetic C and SP rules\n",count);
}

static bool oracle_condition(unsigned cond,unsigned flags)
{
    bool n=(flags&8u)!=0,z=(flags&4u)!=0,c=(flags&2u)!=0,v=(flags&1u)!=0;
    const bool pass[]={z,!z,c,!c,n,!n,v,!v,c&&!z,!c||z,n==v,n!=v,!z&&(n==v),z||(n!=v)};
    assert(cond<14); return pass[cond];
}
static void test_wide_branch(bus_t *b)
{
    const uint32_t displacements[]={0,2,0x7FE,0x40000,0x80000,0xFFFFE,0x100000,0x180000,0x1FFFFE};
    cpu_t c; unsigned count=0;
    for(unsigned cond=0;cond<14;++cond)for(unsigned f=0;f<16;++f)
    for(unsigned i=0;i<sizeof displacements/sizeof displacements[0];++i) {
        uint32_t raw=displacements[i]; initialize(&c,b,f); cpu_t before=c;
        uint16_t first=(uint16_t)(0xF000u|((raw>>20)<<10)|(cond<<6)|((raw>>12)&63u));
        uint16_t second=(uint16_t)(0x8000u|(((raw>>18)&1u)<<13)|(((raw>>19)&1u)<<11)|((raw>>1)&0x7FFu));
        int64_t displacement=(int64_t)raw-((raw&0x100000u)?INT64_C(2097152):0);
        uint32_t expected=FLASH_BASE+12u;
        if(oracle_condition(cond,f))expected+=(uint32_t)displacement;
        assert(execute(&c,b,first,second)==0 && c.r[PC]==expected && flags_of(&c)==f);
        unchanged_except(&c,&before,16); ++count;
    }
    /* Exact objdump address, not just a reconstructed encoding. */
    initialize(&c,b,0); c.r[PC]=0x0800028Au;
    const uint8_t branch[]={0x40,0xF0,0x18,0x81};
    assert(!bus_load_flash(b,0x28A,branch,sizeof branch));
    assert(cpu_step(&c,b)==0 && c.r[PC]==0x080004BEu);
    const uint16_t reserved[]={0xF380,0xF3C0};
    for(unsigned i=0;i<2;++i) { initialize(&c,b,0); assert(execute(&c,b,reserved[i],0x8000)==-1); }
    initialize(&c,b,15); assert(execute(&c,b,0xF3EF,0x8205)==0 && c.r[2]==0 && flags_of(&c)==15);
    initialize(&c,b,15); c.r[0]=1; assert(execute(&c,b,0xF380,0x8810)==0 && c.primask && flags_of(&c)==15);
    printf("PASS I6 wide conditional branch: %u condition/flag/S/J cases; exact target; MRS/MSR separation\n",count);
}

static void test_register_lsr(bus_t *b)
{
    const unsigned regs[][3]={{2,3,3},{14,8,9},{3,3,3},{2,2,1},{4,2,4}}; /* Rn,Rd,Rm */
    const uint32_t amounts[]={0,1,31,32,33,255,256,257,0xFFFFFFFFu};
    cpu_t c; unsigned count=0;
    for(unsigned t=0;t<5;++t)for(unsigned sf=0;sf<2;++sf)for(unsigned v=0;v<6;++v)
    for(unsigned i=0;i<sizeof amounts/sizeof amounts[0];++i)for(unsigned f=0;f<16;++f) {
        unsigned rn=regs[t][0],rd=regs[t][1],rm=regs[t][2]; initialize(&c,b,f);
        c.r[rn]=edge_values[v]; c.r[rm]=amounts[i]; cpu_t before=c; bool carry=c.c;
        uint32_t result=oracle_shift(c.r[rn],c.r[rm]&255u,true,&carry);
        assert(execute(&c,b,(uint16_t)(0xFA20u|(sf<<4)|rn),(uint16_t)(0xF000u|(rd<<8)|rm))==0);
        assert(c.r[rd]==result && flags_of(&c)==(sf?(nz_flags(result)|(carry?2u:0u)|(f&1u)):f));
        unchanged_except(&c,&before,rd); ++count;
    }
    initialize(&c,b,15); c.r[2]=0x80000000u; c.r[3]=31;
    assert(execute(&c,b,0xFA22,0xF303)==0 && c.r[3]==1 && flags_of(&c)==15);
    const uint16_t unsupported[][2]={{0xFA02,0xF303},{0xFA42,0xF303},{0xFA62,0xF303},
                                    {0xFA2D,0xF303},{0xFA22,0xFD03},{0xFA22,0xF30F}};
    for(unsigned i=0;i<sizeof unsupported/sizeof unsupported[0];++i) {
        initialize(&c,b,15); assert(execute(&c,b,unsupported[i][0],unsupported[i][1])==-1);
    }
    printf("PASS I7 LSR register: %u cases, low-byte amounts 0/31/32/33/255/256, S flag and aliases\n",count);
}

static void test_multiply_divide(bus_t *b)
{
    const unsigned regs[][3]={{2,3,3},{14,8,9},{3,3,3},{2,2,1},{4,2,4}};
    cpu_t c; unsigned count=0;
    for(unsigned div=0;div<2;++div)for(unsigned t=0;t<5;++t)
    for(unsigned x=0;x<6;++x)for(unsigned y=0;y<6;++y)for(unsigned f=0;f<16;++f) {
        unsigned rn=regs[t][0],rd=regs[t][1],rm=regs[t][2];initialize(&c,b,f);
        c.r[rn]=edge_values[x]; c.r[rm]=edge_values[y]; cpu_t before=c;
        uint32_t left=c.r[rn],right=c.r[rm];
        uint32_t result=div?(right?left/right:0):(uint32_t)((uint64_t)left*right);
        assert(execute(&c,b,(uint16_t)((div?0xFBB0u:0xFB00u)|rn),
                       (uint16_t)((div?0xF0F0u:0xF000u)|(rd<<8)|rm))==0);
        assert(c.r[rd]==result && flags_of(&c)==f); unchanged_except(&c,&before,rd); ++count;
    }
    initialize(&c,b,15); c.r[2]=0xFFFFFFFFu; c.r[3]=3;
    assert(execute(&c,b,0xFBB2,0xF3F3)==0 && c.r[3]==0x55555555u && flags_of(&c)==15);
    initialize(&c,b,15); c.r[2]=0xFFFFFFFEu; c.r[3]=3;
    assert(execute(&c,b,0xFB02,0xF303)==0 && c.r[3]==0xFFFFFFFAu && flags_of(&c)==15);
    const uint16_t unsupported[][2]={{0xFB02,0x1303},{0xFB92,0xF3F3},
        {0xFB0D,0xF303},{0xFB02,0xFD03},{0xFB02,0xF30F},
        {0xFBBD,0xF3F3},{0xFBB2,0xFDF3},{0xFBB2,0xF3FF}};
    for(unsigned i=0;i<sizeof unsupported/sizeof unsupported[0];++i) {
        initialize(&c,b,15); assert(execute(&c,b,unsupported[i][0],unsupported[i][1])==-1);
    }
    printf("PASS I8/I9 multiply/divide: %u cases; unsigned truncation, zero divisor, aliases, unchanged NZCV and decode bounds\n",count);
}

static void write_word(bus_t *b,unsigned offset,uint32_t value)
{
    uint8_t data[]={(uint8_t)value,(uint8_t)(value>>8),(uint8_t)(value>>16),(uint8_t)(value>>24)};
    assert(!bus_load_flash(b,offset,data,sizeof data));
}
static void test_literal_t2(bus_t *b)
{
    cpu_t c; unsigned count=0; write_word(b,0x88,0x11223344);
    for(unsigned neg=0;neg<2;++neg)for(unsigned rt=0;rt<16;++rt)for(unsigned f=0;f<16;++f) {
        initialize(&c,b,f); cpu_t before=c;
        uint32_t expected=neg?SRAM_BASE+SRAM_SIZE:0x11223344u;
        int status=execute(&c,b,neg?0xF85F:0xF8DF,(uint16_t)((rt<<12)|(neg?12u:124u)));
        assert(status==(rt==PC?-1:0));
        if(rt!=PC)assert(c.r[rt]==expected && c.r[PC]==FLASH_BASE+12u);
        else assert(c.r[PC]==FLASH_BASE+8u);
        assert(flags_of(&c)==f); unchanged_except(&c,&before,rt==PC?16u:rt); ++count;
    }
    /* Exact GNU-as failure vector from program19 at halfword-aligned 0x106. */
    initialize(&c,b,15); c.r[PC]=FLASH_BASE+0x106u;
    write_word(b,0x106,0x007CF8DF); write_word(b,0x184,0xE000E204u);
    assert(cpu_step(&c,b)==0 && c.r[0]==0xE000E204u && c.r[PC]==FLASH_BASE+0x10Au && flags_of(&c)==15);
    write_word(b,0x88,3); initialize(&c,b,15); uint32_t sp=c.r[SP];
    assert(execute(&c,b,0xF8DF,0xD07C)==-1 && c.r[SP]==sp && flags_of(&c)==15);
    bus_t *fault=bus_new(); assert(fault);
    const uint8_t vectors[]={0,0,2,0x20,9,0,0,8}; assert(!bus_load_flash(fault,0,vectors,sizeof vectors));
    initialize(&c,fault,15); uint32_t before=c.r[0];
    assert(execute(&c,fault,0xF85F,0x0FFF)==-1 && bus_fault(fault));
    assert(c.r[0]==before && c.r[PC]==FLASH_BASE+8u && flags_of(&c)==15);
    bus_free(fault);
    printf("PASS I10 literal T2: %u U/register/flag cases, exact GNU vector, Align PC, SP and fault preservation\n",count);
}

static void check_register(const cpu_t *cpu, unsigned reg, uint32_t expected)
{
    if (cpu->r[reg] != expected) {
        fprintf(stderr, "r%u: got %08x, expected %08x\n", reg, cpu->r[reg], expected);
        assert(cpu->r[reg] == expected);
    }
}

/* These files must be assembled by the Makefile's GNU ARM as/ld/objcopy
 * pipeline. The host-only target deliberately does not call this runner.
 */
static void test_programs(void)
{
    const char *names[] = {
        "24_add_register", "25_data_processing", "26_immediate_shifts", "27_uxtb",
        "28_modified_arithmetic", "29_wide_conditional_branch", "30_register_lsr",
        "31_multiply", "32_unsigned_divide", "33_wide_literal"
    };
    const unsigned expected_flags[] = {6, 9, 2, 6, 2, 6, 8, 6, 6, 4};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i) {
        char path[160]; snprintf(path, sizeof path, "build/progs/%s.bin", names[i]);
        bus_t *bus = bus_new(); cpu_t cpu;
        assert(bus && !load_bin(bus, path)); cpu_reset(&cpu, bus);
        while (!cpu.halted && cpu.steps < 1000) cpu_step(&cpu, bus);
        if (!cpu.halted || strcmp(cpu.halt_reason, "bkpt #0")) {
            fprintf(stderr, "%s: unexpected halt at %08x after %llu steps: %s\n",
                    names[i], cpu.r[PC], (unsigned long long)cpu.steps, cpu.halt_reason);
            assert(0);
        }
        assert(!bus_fault(bus) && flags_of(&cpu) == expected_flags[i]);
        check_register(&cpu, SP, SRAM_BASE + SRAM_SIZE);
        switch (i) {
        case 0:
            check_register(&cpu, 2, 12); check_register(&cpu, 8, 19);
            check_register(&cpu, 4, SRAM_BASE + SRAM_SIZE + 7);
            check_register(&cpu, 5, SRAM_BASE + SRAM_SIZE + 4); break;
        case 1: check_register(&cpu, 3, 0x80000000); check_register(&cpu, 2, 0xFFFFFFFF); break;
        case 2: check_register(&cpu, 2, 0x08000000); check_register(&cpu, 3, 0x80000100); break;
        case 3: check_register(&cpu, 2, 0); check_register(&cpu, 3, 0x80); break;
        case 4:
            check_register(&cpu, 1, 0xFFFFFBFF); check_register(&cpu, 2, 0x8000FFE8);
            check_register(&cpu, 3, 0x7FFF8000); check_register(&cpu, 7, 16); break;
        case 5: check_register(&cpu, 0, 1); check_register(&cpu, 7, 0); break;
        case 6:
            check_register(&cpu, 2, 0x80000001); check_register(&cpu, 3, 0x80000001);
            check_register(&cpu, 4, 256); check_register(&cpu, 5, 0);
            check_register(&cpu, 6, 0); check_register(&cpu, 7, 0x80000001); break;
        case 7:
            check_register(&cpu, 2, 4); check_register(&cpu, 3, 0xFFFFFFFA);
            check_register(&cpu, 8, 36); break;
        case 8:
            check_register(&cpu, 2, 1); check_register(&cpu, 3, 0x55555555);
            check_register(&cpu, 5, 0); break;
        case 9:
            check_register(&cpu, 0, 0x12345678); check_register(&cpu, 1, 0xABCDEF00);
            check_register(&cpu, 2, 0); break;
        }
        printf("PASS assembled program %s (%llu instructions)\n", names[i],
               (unsigned long long)cpu.steps);
        bus_free(bus);
    }
}

int main(int argc, char **argv)
{
    bus_t *b=bus_new(); assert(b);
    const uint8_t vectors[]={0,0,2,0x20,9,0,0,8};
    assert(!bus_load_flash(b,0,vectors,sizeof vectors));
    test_add_register(b);
    test_data_processing(b);
    test_immediate_shift(b);
    test_uxtb(b);
    test_modified_arithmetic(b);
    test_wide_branch(b);
    test_register_lsr(b);
    test_multiply_divide(b);
    test_literal_t2(b);
    assert(!bus_fault(b)); bus_free(b);
    if (argc == 2 && !strcmp(argv[1], "--programs")) test_programs();
    else assert(argc == 1);
    return 0;
}
