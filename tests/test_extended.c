#define _POSIX_C_SOURCE 200809L /* fmemopen */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "loader.h"
#include "periph/nvic.h"
#include "periph/i2c.h"
#include "periph/bme280.h"
#include "periph/ssd1306.h"
#include "periph/usart.h"

static void put32(uint8_t *p, uint32_t v)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (i * 8)); }

static void test_nvic(void)
{
    bus_t *b = bus_new(); assert(b); cpu_t c;
    uint8_t image[328] = {0};
    put32(image, SRAM_BASE + SRAM_SIZE); put32(image + 4, FLASH_BASE + 257);
    put32(image + 54 * 4, FLASH_BASE + 321);
    put32(image + 19 * 4, FLASH_BASE + 321);
    const uint8_t code[] = {0,0xBF,0,0xBE}; /* NOP, BKPT */
    const uint8_t handler[] = {0xEF,0xF3,5,0x82,0,0x92,0x70,0x47}; /* MRS; STR r2,[sp]; BX LR */
    memcpy(image + 256, code, sizeof code); memcpy(image + 320, handler, sizeof handler);
    assert(bus_load_flash(b, 0, image, sizeof image) == 0); cpu_reset(&c,b);
    nvic_t *n = nvic_new(&c); assert(n && bus_add_periph(b,nvic_as_periph(n)) == 0);
    /* IRQ38: bank1 bit6. Pending while disabled remains visible, not dispatched. */
    bus_write32(b, 0xE000E204, 1u << 6); bus_write32(b, 0xE000E204, 0);
    assert(cpu_exception_pending(&c,54) && cpu_step(&c,b) == 0 && c.ipsr == 0);
    assert(bus_read32(b,0xE000E284) == (1u << 6));
    bus_write32(b,0xE000E104,1u << 6); assert(cpu_step(&c,b) == 0 && c.ipsr == 54 && c.r[2] == 54);
    assert(!cpu_exception_pending(&c,54) && bus_read32(b,0xE000E304) == (1u << 6));
    assert(bus_read32(b,c.r[SP]+24) == FLASH_BASE+258);
    assert(cpu_step(&c,b) == 0 && cpu_step(&c,b) == 0 && c.ipsr == 0 && c.r[0] == 54);
    assert(cpu_step(&c,b) == -1 && !strcmp(c.halt_reason,"bkpt #0") && c.steps == 5);
    /* W1C, enable aliases, highest IRQ and IPR byte lanes. */
    nvic_reset(n); assert(nvic_pend(n,239) && !nvic_pend(n,240));
    assert(bus_read32(b,0xE000E21C) == 0x8000); bus_write32(b,0xE000E29C,0x8000);
    assert(!cpu_exception_pending(&c,255));
    bus_write32(b,0xE000E100,8); bus_write32(b,0xE000E180,0); assert(bus_read32(b,0xE000E180)==8);
    bus_write32(b,0xE000E180,8); assert(bus_read32(b,0xE000E100)==0);
    bus_write8(b,0xE000E426,0xAB); assert(bus_read8(b,0xE000E426)==0xA0);
    /* PRIMASK retains pending; arbitration is lowest exception number. */
    cpu_reset(&c,b); nvic_reset(n); c.primask = true;
    bus_write32(b,0xE000E100,8); bus_write32(b,0xE000E104,64);
    assert(nvic_pend(n,38) && nvic_pend(n,3));
    assert(cpu_step(&c,b)==0 && c.ipsr==0); c.primask=false;
    assert(cpu_step(&c,b)==0 && c.ipsr==19 && cpu_exception_pending(&c,54));
    /* Invalid vector: pending is only acknowledged after successful entry. */
    cpu_reset(&c,b); nvic_reset(n);
    bus_write32(b,0xE000E11C,0xFFFF0000u); assert(!bus_read32(b,0xE000E11C));
    bus_write32(b,0xE000E11C,0x8000u); assert(nvic_pend(n,239));
    assert(cpu_step(&c,b)==-1 && cpu_exception_pending(&c,255) && c.steps==0 && c.ipsr==0);
    assert(!nvic_bad_access_count(n) && !bus_fault(b)); nvic_free(n); bus_free(b);
    puts("PASS NVIC: IRQ38/IPSR54, gating, coalescing, PRIMASK, return, arbitration, bank bounds");
}

typedef struct { unsigned reads, writes, starts, stops; bool ack[64]; uint8_t bytes[64]; } fake_t;
static void fake_start(void *p, bool read) { (void)read; ++((fake_t*)p)->starts; }
static bool fake_write(void *p, uint8_t v) { fake_t *f=p; f->bytes[f->writes++]=v; return v != 0xEE; }
static uint8_t fake_read(void *p, bool ack) { fake_t *f=p; f->ack[f->reads]=ack; return (uint8_t)(0x80+f->reads++); }
static void fake_stop(void *p) { ++((fake_t*)p)->stops; }
typedef struct { bus_t *bus; rcc_t *rcc; gpio_t *gpio; i2c_t *i2c; } fixture_t;
static void wr(fixture_t *f, unsigned off, unsigned value) { bus_write32(f->bus,I2C1_BASE+off,value); }
static unsigned rd(fixture_t *f, unsigned off) { return bus_read32(f->bus,I2C1_BASE+off); }
static fixture_t fixture(void)
{
    fixture_t f={0}; f.bus=bus_new(); f.rcc=rcc_new();
    f.gpio=gpio_new("GPIOB",GPIOB_BASE,f.rcc,RCC_GPIOB_ENABLE_BIT); f.i2c=i2c_new(f.rcc,f.gpio);
    assert(f.bus && f.rcc && f.gpio && f.i2c);
    assert(!bus_add_periph(f.bus,rcc_as_periph(f.rcc)) && !bus_add_periph(f.bus,gpio_as_periph(f.gpio)) && !bus_add_periph(f.bus,i2c_as_periph(f.i2c)));
    bus_write32(f.bus,RCC_BASE+RCC_AHB1ENR_OFFSET,2);
    assert(bus_read32(f.bus,GPIOB_BASE+GPIO_MODER_OFFSET)==0x280);
    assert(bus_read32(f.bus,GPIOB_BASE+GPIO_PUPDR_OFFSET)==0x100);
    assert(bus_read32(f.bus,GPIOB_BASE+GPIO_OSPEEDR_OFFSET)==0xC0);
    bus_write8(f.bus,GPIOB_BASE+GPIO_AFRH_OFFSET,0x44);
    bus_write16(f.bus,GPIOB_BASE+GPIO_AFRH_OFFSET+2,0xA532);
    assert(bus_read32(f.bus,GPIOB_BASE+GPIO_AFRH_OFFSET)==0xA5320044u);
    bus_write32(f.bus,GPIOB_BASE+GPIO_MODER_OFFSET,0xA0280);
    bus_write32(f.bus,GPIOB_BASE+GPIO_OTYPER_OFFSET,0x300);
    assert(gpio_pin_is_af(f.gpio,8,4) && gpio_pin_is_af(f.gpio,9,4));
    bus_write32(f.bus,RCC_BASE+RCC_APB1ENR_OFFSET,1u<<21);
    wr(&f,I2C_CR2,16); wr(&f,I2C_CCR,80); wr(&f,I2C_TRISE,17); wr(&f,I2C_CR1,I2C_PE);
    return f;
}
static void cleanup(fixture_t *f) { bus_free(f->bus); i2c_free(f->i2c); gpio_free(f->gpio); rcc_free(f->rcc); }
static void address(fixture_t *f, uint8_t a, unsigned cr)
{
    wr(f,I2C_CR1,cr|I2C_START); assert(rd(f,I2C_SR1)&I2C_SB);
    wr(f,I2C_DR,a); assert(rd(f,I2C_SR1)&I2C_ADDR);
}
static void clear_address(fixture_t *f) { (void)rd(f,I2C_SR1); (void)rd(f,I2C_SR2); }
static void rx(fixture_t *f, uint8_t addr, uint8_t *out, unsigned n)
{
    unsigned cr=I2C_PE | (n>1?I2C_ACK:0) | (n==2?I2C_POS:0);
    address(f,addr,cr); clear_address(f);
    if(n==1) { wr(f,I2C_CR1,I2C_PE|I2C_STOP); assert(rd(f,I2C_SR1)&I2C_RXNE); *out=(uint8_t)rd(f,I2C_DR); }
    else if(n==2) {
        wr(f,I2C_CR1,I2C_PE|I2C_POS); assert(rd(f,I2C_SR1)&I2C_BTF);
        wr(f,I2C_CR1,I2C_PE|I2C_POS|I2C_STOP); out[0]=(uint8_t)rd(f,I2C_DR); out[1]=(uint8_t)rd(f,I2C_DR);
    } else {
        unsigned i=0; while(n-i>3) { assert(rd(f,I2C_SR1)&I2C_RXNE); out[i++]=(uint8_t)rd(f,I2C_DR); }
        assert(rd(f,I2C_SR1)&I2C_BTF); wr(f,I2C_CR1,I2C_PE);
        out[i++]=(uint8_t)rd(f,I2C_DR); assert(rd(f,I2C_SR1)&I2C_BTF);
        wr(f,I2C_CR1,I2C_PE|I2C_STOP); out[i++]=(uint8_t)rd(f,I2C_DR); out[i]=(uint8_t)rd(f,I2C_DR);
    }
    assert(!(rd(f,I2C_SR2)&2)); wr(f,I2C_CR1,I2C_PE|I2C_ACK);
}
static void test_i2c(void)
{
    fixture_t f=fixture(); fake_t fake={0};
    i2c_device_t d={0x50,&fake,fake_start,fake_write,fake_read,fake_stop}; assert(!i2c_attach(f.i2c,d) && i2c_attach(f.i2c,d)==-1);
    /* SB requires the actual SR1 read: peek must not arm its clearing sequence. */
    wr(&f,I2C_CR1,I2C_PE|I2C_START); assert(i2c_peek(f.i2c,I2C_SR1)&I2C_SB);
    wr(&f,I2C_DR,0xA0); assert(i2c_peek(f.i2c,I2C_SR1)&I2C_SB);
    (void)rd(&f,I2C_SR1); (void)rd(&f,I2C_SR2); wr(&f,I2C_DR,0xA0);
    (void)rd(&f,I2C_SR2); assert(i2c_peek(f.i2c,I2C_SR1)&I2C_ADDR);
    clear_address(&f); assert(!(rd(&f,I2C_SR1)&I2C_ADDR));
    wr(&f,I2C_DR,0xD0); assert((rd(&f,I2C_SR1)&(I2C_BTF|I2C_TXE))==(I2C_BTF|I2C_TXE));
    uint8_t data[16]; rx(&f,0xA1,data,1); assert(data[0]==0x80 && !fake.ack[0]);
    assert(fake.starts==2 && fake.stops==1 && fake.writes==1 && fake.bytes[0]==0xD0);
    /* RM0390 24.6.7: SR1 read before ADDR rises still arms the next SR2. */
    wr(&f,I2C_CR1,I2C_PE|I2C_START); (void)rd(&f,I2C_SR1);
    wr(&f,I2C_DR,0xA0); assert(i2c_peek(f.i2c,I2C_SR1)&I2C_ADDR);
    (void)rd(&f,I2C_SR2); assert(!(i2c_peek(f.i2c,I2C_SR1)&I2C_ADDR));
    wr(&f,I2C_CR1,I2C_PE|I2C_STOP);
    for(unsigned n=2;n<=8;++n) {
        unsigned start=fake.reads; rx(&f,0xA1,data,n);
        assert(fake.reads==start+n);
        for(unsigned i=0;i<n;++i) assert(data[i]==(uint8_t)(0x80+start+i) && fake.ack[start+i]==(i+1<n));
    }
    address(&f,0xA0,I2C_PE); clear_address(&f); wr(&f,I2C_DR,0xEE);
    assert(rd(&f,I2C_SR1)&I2C_AF); assert(!(rd(&f,I2C_SR1)&I2C_BTF));
    wr(&f,I2C_SR1,0xFFFF); assert(rd(&f,I2C_SR1)&I2C_AF); wr(&f,I2C_SR1,~I2C_AF); assert(!(rd(&f,I2C_SR1)&I2C_AF));
    wr(&f,I2C_CR1,I2C_PE|I2C_STOP);
    wr(&f,I2C_CR1,I2C_PE|I2C_START); (void)rd(&f,I2C_SR1); wr(&f,I2C_DR,0xFE);
    assert((rd(&f,I2C_SR1)&I2C_AF) && !(rd(&f,I2C_SR1)&I2C_ADDR)); wr(&f,I2C_CR1,I2C_PE|I2C_STOP);
    bus_write32(f.bus,RCC_BASE+RCC_APB1ENR_OFFSET,0); wr(&f,I2C_DR,0);
    bus_write32(f.bus,RCC_BASE+RCC_APB1ENR_OFFSET,1u<<21);
    wr(&f,I2C_CR1,0); wr(&f,I2C_DR,0);
    bus_write32(f.bus,GPIOB_BASE+GPIO_OTYPER_OFFSET,0); wr(&f,I2C_CR1,I2C_PE|I2C_START);
    i2c_stats_t st=i2c_get_stats(f.i2c);
    assert(st.clock_gated_writes==1 && st.pe_disabled_transfers==1 && st.pin_mismatch_starts==1 && st.protocol_errors==1 && !st.bad_accesses);
    assert(!gpio_bad_access_count(f.gpio) && !bus_fault(f.bus)); cleanup(&f);
    puts("PASS I2C/GPIOB: reset/AFRH lanes, 1/2/3..8 bytes, ACK/POS/STOP, repeated START, NACK, gating, flag-read sequences");
}

static void write_sensor(fixture_t *f, unsigned reg, unsigned value)
{
    address(f,0xEC,I2C_PE); clear_address(f); wr(f,I2C_DR,reg); wr(f,I2C_DR,value); wr(f,I2C_CR1,I2C_PE|I2C_STOP);
}
static void read_sensor(fixture_t *f, unsigned reg, uint8_t *out, unsigned n)
{
    address(f,0xEC,I2C_PE); clear_address(f); wr(f,I2C_DR,reg); rx(f,0xED,out,n);
}
static int32_t signed16(unsigned v) { return v&0x8000u ? (int32_t)v-65536 : (int32_t)v; }
static unsigned le16(const uint8_t *p) { return p[0]|(unsigned)p[1]<<8; }
static uint32_t raw20(const uint8_t *p) { return (uint32_t)p[0]<<12|(uint32_t)p[1]<<4|p[2]>>4; }
static int64_t shr(int64_t n,unsigned bits) { int64_t d=INT64_C(1)<<bits; return n/d-(n<0&&n%d!=0); }
static int64_t compensate_t(const uint8_t *cal,uint32_t raw,int64_t *fine)
{
    int64_t a=shr(((int64_t)(raw>>3)-2*le16(cal))*signed16(le16(cal+2)),11);
    int64_t d=(int64_t)(raw>>4)-le16(cal);
    *fine=a+shr(shr(d*d,12)*signed16(le16(cal+4)),14);
    return shr(*fine*5+128,8);
}
static int64_t compensate_p(const uint8_t *c,uint32_t raw,int64_t fine)
{
    int64_t a=fine-128000,b=a*a*signed16(le16(c+10));
    b+=a*signed16(le16(c+8))*131072+signed16(le16(c+6))*INT64_C(34359738368);
    a=shr(a*a*signed16(le16(c+4)),8)+a*signed16(le16(c+2))*4096;
    a=shr(((INT64_C(1)<<47)+a)*le16(c),33); assert(a);
    int64_t p=(((INT64_C(1048576)-raw)*INT64_C(2147483648)-b)*3125)/a;
    return shr(p+shr(signed16(le16(c+16))*shr(p,13)*shr(p,13),25)+shr(signed16(le16(c+14))*p,19),8)+signed16(le16(c+12))*16;
}
static int64_t compensate_h(const uint8_t *c, unsigned h1,uint32_t raw,int64_t fine)
{
    int64_t h4=(int64_t)c[3]*16+(c[4]&15),h5=(int64_t)c[5]*16+(c[4]>>4);
    if(h4&2048)h4-=4096;
    if(h5&2048)h5-=4096;
    int64_t h6=c[6]<128?c[6]:(int)c[6]-256,v=fine-76800;
    int64_t a=shr(((int64_t)raw*16384-h4*1048576-h5*v)+16384,15);
    int64_t b=shr((shr(shr(v*h6,10)*(shr(v*c[2],11)+32768),10)+2097152)*signed16(le16(c))+8192,14);
    v=a*b; v-=shr(shr(shr(v,15)*shr(v,15),7)*h1,4);
    if(v<0)v=0;
    if(v>419430400)v=419430400;
    return shr(v,12);
}
static void test_bme(void)
{
    fixture_t f=fixture(); bme280_t *b=bme280_new(0x76); assert(b && !i2c_attach(f.i2c,bme280_as_device(b)));
    uint8_t id,cal[26],hc[7],raw[8]; read_sensor(&f,0xD0,&id,1); assert(id==0x60);
    read_sensor(&f,0x88,cal,26); read_sensor(&f,0xE1,hc,7);
    assert(le16(cal)==24000 && le16(cal+2)==25600 && le16(cal+6)==31250);
    write_sensor(&f,0xF2,1); write_sensor(&f,0xF4,0x27); /* T/P/H x1, normal */
    const int32_t temperatures[]={-4000,-1234,0,2500,3050,8500};
    for(unsigned i=0;i<sizeof temperatures/sizeof temperatures[0];++i) {
        assert(bme280_set_temperature(b,temperatures[i])); read_sensor(&f,0xF7,raw,8);
        int64_t fine; assert(compensate_t(cal,raw20(raw+3),&fine)==temperatures[i]);
        assert(llabs(compensate_p(cal+6,raw20(raw),fine)-(int64_t)101325*256)<=26);
        assert(compensate_h(hc,cal[25],(unsigned)raw[6]*256+raw[7],fine)==51200);
    }
    /* Measure the inverse's error over its complete centidegree input API.
     * Also reject the reserved skipped sample, even if its numeric value
     * happens to compensate to the requested temperature.
     */
    for (int32_t requested = -4000; requested <= 8500; ++requested) {
        assert(bme280_set_temperature(b, requested)); read_sensor(&f, 0xFA, raw, 3);
        int64_t fine;
        assert(raw20(raw) != 0x80000u);
        assert(compensate_t(cal, raw20(raw), &fine) == requested);
    }
    assert(!bme280_set_temperature(b,8501));
    assert(bme280_set_humidity(b,0)); assert(bme280_set_pressure(b,30000)); read_sensor(&f,0xF7,raw,8);
    int64_t fine; (void)compensate_t(cal,raw20(raw+3),&fine);
    assert(compensate_h(hc,cal[25],(unsigned)raw[6]*256+raw[7],fine)==0);
    assert(llabs(compensate_p(cal+6,raw20(raw),fine)-30000*256)<=26);
    assert(bme280_set_humidity(b,102400) && bme280_set_pressure(b,110000)); read_sensor(&f,0xF7,raw,8);
    assert(compensate_h(hc,cal[25],(unsigned)raw[6]*256+raw[7],fine)==102400);
    assert(llabs(compensate_p(cal+6,raw20(raw),fine)-110000*256)<=26);
    /* ctrl_hum takes effect only on ctrl_meas write; forced immediately sleeps. */
    write_sensor(&f,0xF2,0); read_sensor(&f,0xFD,raw,2); assert(le16(raw)!=0x80);
    write_sensor(&f,0xF4,0x21); read_sensor(&f,0xF7,raw,8);
    assert(raw20(raw)==0x80000 && ((unsigned)raw[6]*256+raw[7])==0x8000);
    assert(!(bme280_peek(b,0xF4)&3) && bme280_peek(b,0xF3)==0);
    write_sensor(&f,0xE0,0xB6); assert(bme280_peek(b,0xD0)==0x60 && bme280_peek(b,0xF4)==0);
    assert(!i2c_get_stats(f.i2c).protocol_errors && !i2c_get_stats(f.i2c).bad_accesses);
    bme280_free(b); cleanup(&f);
    puts("PASS BME280: I2C id/calibration burst, independent T/P/H compensation, 25.00/30.50, endpoints, skip, forced/reset");
}
static void test_msr(void)
{
    cpu_t c; bus_t *b=bus_new(); assert(b);
    uint8_t image[16]={0}; put32(image,SRAM_BASE+SRAM_SIZE); put32(image+4,FLASH_BASE+9);
    image[8]=0x80; image[9]=0xF3; image[10]=0x10; image[11]=0x88; /* msr primask,r0 */
    assert(!bus_load_flash(b,0,image,sizeof image));
    const uint32_t values[]={0,1,2,3,0xFFFFFFFFu};
    for(unsigned i=0;i<5;++i) {
        cpu_reset(&c,b); c.r[0]=values[i]; c.n=c.z=c.c=c.v=true;
        assert(cpu_step(&c,b)==0 && c.primask==((values[i]&1u)!=0));
        assert(c.n && c.z && c.c && c.v && c.r[PC]==FLASH_BASE+12);
    }
    bus_free(b); puts("PASS MSR PRIMASK: low bit only, flags unchanged, nested-mask restoration support");
}

static void test_programs(void)
{
    const char *files[]={"19_nvic_swpend","20_nvic_disabled","21_gpiob","22_i2c_chip_id","23_msr_primask"};
    for(unsigned i=0;i<5;++i) {
        char path[128]; snprintf(path,sizeof path,"build/progs/%s.bin",files[i]);
        bus_t *bus=bus_new(); cpu_t cpu; assert(bus && !load_bin(bus,path)); cpu_reset(&cpu,bus);
        rcc_t *rcc=rcc_new(); gpio_t *gpio=gpio_new("GPIOB",GPIOB_BASE,rcc,RCC_GPIOB_ENABLE_BIT);
        nvic_t *nvic=nvic_new(&cpu); i2c_t *i2c=i2c_new(rcc,gpio); bme280_t *bme=bme280_new(0x76);
        assert(rcc && gpio && nvic && i2c && bme);
        assert(!bus_add_periph(bus,rcc_as_periph(rcc)) && !bus_add_periph(bus,gpio_as_periph(gpio)) &&
               !bus_add_periph(bus,nvic_as_periph(nvic)) && !bus_add_periph(bus,i2c_as_periph(i2c)));
        assert(!i2c_attach(i2c,bme280_as_device(bme)));
        while(!cpu.halted && cpu.steps<100000) cpu_step(&cpu,bus);
        if(!cpu.halted || strcmp(cpu.halt_reason,"bkpt #0")) fprintf(stderr,"%s: %s at PC=%08x steps=%llu\n",files[i],cpu.halt_reason,cpu.r[PC],(unsigned long long)cpu.steps);
        assert(cpu.halted && !strcmp(cpu.halt_reason,"bkpt #0"));
        if(i==0) assert(cpu.r[0]==54 && cpu.steps==10 && !cpu.ipsr);
        if(i==1) assert(!cpu.r[7] && cpu_exception_pending(&cpu,54) && !cpu.ipsr);
        if(i==2) assert(cpu.r[2]==0x280 && cpu.r[3]==0x100 && cpu.r[4]==0xC0 && cpu.r[5]==0x44);
        if(i==3) assert(cpu.r[7]==0x60 && !i2c_get_stats(i2c).protocol_errors);
        if(i==4) assert(cpu.r[4]==1 && cpu.r[5]==0 && cpu.r[6]==1 && cpu.r[7]==0 && !cpu.primask);
        assert(!bus_fault(bus) && !bus_unmapped_count(bus));
        nvic_free(nvic); i2c_free(i2c); bme280_free(bme); gpio_free(gpio); rcc_free(rcc); bus_free(bus);
        printf("PASS program %s\n",files[i]);
    }
}

static unsigned usart_irqs;
static void count_irq(void *ctx) { (void)ctx; ++usart_irqs; }
static unsigned tx_count;
static void count_tx(void *ctx, uint8_t byte) { (void)ctx; (void)byte; ++tx_count; }

/* USART2 RX, RXNE/ORE/TXE interrupt requests and the CR1 bits they need. */
static void test_usart_rx_irq(void)
{
    rcc_t *rcc = rcc_new();
    gpio_t *gpioa = gpio_new("GPIOA", GPIOA_BASE, rcc, RCC_GPIOA_ENABLE_BIT);
    usart_t *u = usart_new(rcc, gpioa);
    bus_t *bus = bus_new();
    assert(rcc && gpioa && u && bus);
    assert(!bus_add_periph(bus, rcc_as_periph(rcc)) && !bus_add_periph(bus, gpio_as_periph(gpioa)) &&
           !bus_add_periph(bus, usart_as_periph(u)));
    usart_set_irq(u, count_irq, NULL);
    usart_set_tx_observer(u, count_tx, NULL);
    const uint32_t base = USART2_BASE;
    assert(!usart_receive(u, 'x'));                       /* APB1 clock off */
    bus_write32(bus, 0x40023840u, 1u << 17);              /* USART2EN */
    bus_write32(bus, 0x40023830u, 1u);                    /* GPIOAEN */
    bus_write32(bus, GPIOA_BASE + 0x00u, 0xA8000000u | (2u << 4));  /* PA2 AF */
    bus_write32(bus, GPIOA_BASE + 0x20u, 7u << 8);       /* PA2 AF7 */
    assert(!usart_receive(u, 'x'));                       /* UE/RE off */
    bus_write32(bus, base + USART_CR1_OFFSET, USART_CR1_UE | USART_CR1_TE | USART_CR1_RE);
    assert(!usart_get_stats(u).bad_accesses && usart_irqs == 0);
    assert(usart_receive(u, 'a') && usart_irqs == 0);    /* RXNEIE off: no request */
    assert(bus_read32(bus, base) & USART_SR_RXNE);
    assert(!usart_receive(u, 'b'));                       /* RXNE set: held back */
    assert(bus_read32(bus, base + USART_DR_OFFSET) == 'a');
    assert(!(bus_read32(bus, base) & USART_SR_RXNE));
    bus_write32(bus, base + USART_CR1_OFFSET,
                USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE);
    assert(usart_irqs == 0);                              /* nothing asserted yet */
    assert(usart_receive(u, 'b') && usart_irqs == 1);
    assert(bus_read32(bus, base + USART_DR_OFFSET) == 'b');
    /* TXEIE with TXE already set requests at once; each DR write requests again. */
    bus_write32(bus, base + USART_CR1_OFFSET,
                USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_TXEIE);
    assert(usart_irqs == 2);
    bus_write32(bus, base + USART_DR_OFFSET, 'z');
    assert(usart_irqs == 3 && tx_count == 1);
    bus_write32(bus, base + USART_CR1_OFFSET, USART_CR1_UE | USART_CR1_TE | (UINT32_C(1) << 6));
    assert(usart_get_stats(u).bad_accesses == 1);         /* TCIE is not modeled */
    bus_free(bus); usart_free(u); gpio_free(gpioa); rcc_free(rcc);
    puts("PASS USART2 RX hold-back, DR read clears RXNE, RXNE/TXE interrupt requests");
}

/* SSD1306: control bytes, horizontal window, page mode, Co = 1, PBM dump. */
static void test_ssd1306(void)
{
    ssd1306_t *o = ssd1306_new(0x3C);
    assert(o);
    i2c_device_t d = ssd1306_as_device(o);
    const uint8_t init[] = {0x00, 0xAE, 0xA8, 0x3F, 0x20, 0x00, 0xA1, 0xC8, 0x8D, 0x14, 0xAF};
    d.start(d.ctx, false);
    for (unsigned i = 0; i < sizeof init; ++i) assert(d.write(d.ctx, init[i]));
    d.stop(d.ctx);
    assert(ssd1306_display_on(o) && !ssd1306_unknown_bytes(o));
    /* Window columns 126..127, pages 2..3: five bytes wrap to the next page and back. */
    const uint8_t window[] = {0x00, 0x21, 126, 127, 0x22, 2, 3};
    d.start(d.ctx, false);
    for (unsigned i = 0; i < sizeof window; ++i) d.write(d.ctx, window[i]);
    d.stop(d.ctx);
    d.start(d.ctx, false);
    d.write(d.ctx, 0x40);
    for (uint8_t b = 1; b <= 5; ++b) d.write(d.ctx, b);
    d.stop(d.ctx);
    assert(ssd1306_gddram(o, 2, 126) == 5 && ssd1306_gddram(o, 2, 127) == 2 &&
           ssd1306_gddram(o, 3, 126) == 3 && ssd1306_gddram(o, 3, 127) == 4);
    /* Page mode with Co = 1 pairs: B5, column 0x10 (low 0, high 1). */
    const uint8_t page_mode[] = {0x80, 0x20, 0x80, 0x02, 0x80, 0xB5, 0x80, 0x00, 0x80, 0x11, 0xC0, 0xFF};
    d.start(d.ctx, false);
    for (unsigned i = 0; i < sizeof page_mode; ++i) d.write(d.ctx, page_mode[i]);
    d.stop(d.ctx);
    assert(ssd1306_gddram(o, 5, 0x10) == 0xFF && !ssd1306_unknown_bytes(o));
    d.start(d.ctx, false);
    d.write(d.ctx, 0x00); d.write(d.ctx, 0x26);           /* scroll setup: not modeled */
    d.stop(d.ctx);
    assert(ssd1306_unknown_bytes(o) == 1);
    char buffer[16384];
    FILE *f = fmemopen(buffer, sizeof buffer, "w");
    assert(f && ssd1306_write_pbm(o, f));
    fclose(f);
    const char *header = "P1\n# SSD1306 GDDRAM, display on\n128 64\n";
    assert(!strncmp(buffer, header, strlen(header)));
    /* Pixel (0x10, 40) is page 5 bit 0. */
    const char *rows = strstr(buffer, "128 64\n") + 7;
    assert(rows[40 * 129 + 0x10] == '1' && rows[40 * 129 + 0x0F] == '0');
    ssd1306_free(o);
    puts("PASS SSD1306 control bytes, horizontal window wrap, page mode, Co=1, PBM dump");
}

int main(int argc, char **argv)
{
    test_nvic(); test_i2c(); test_bme(); test_msr(); test_usart_rx_irq(); test_ssd1306();
    if(argc==2 && !strcmp(argv[1],"--programs")) test_programs();
    else assert(argc==1);
    return 0;
}
