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
int main(void)
{
    bus_t *b=bus_new(); assert(b);
    const uint8_t vectors[]={0,0,2,0x20,9,0,0,8};
    assert(!bus_load_flash(b,0,vectors,sizeof vectors));
    test_add_register(b);
    assert(!bus_fault(b)); bus_free(b);
    return 0;
}
