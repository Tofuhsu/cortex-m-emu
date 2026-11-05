/* Direct opcode unit tests are additional to GNU-assembled programs 24 onward.
 * They do not stand in for clean GNU assembly or a real Renode comparison.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cpu.h"

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

int main(void)
{
    bus_t *b=bus_new(); assert(b);
    const uint8_t vectors[]={0,0,2,0x20,9,0,0,8};
    assert(!bus_load_flash(b,0,vectors,sizeof vectors));
    test_add_register(b);
    test_data_processing(b);
    test_immediate_shift(b);
    test_uxtb(b);
    assert(!bus_fault(b)); bus_free(b);
    return 0;
}
