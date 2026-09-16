#include "cdj_sh7764_eth.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned char ram[8192], sent[2048];
static size_t sent_len;
static unsigned sends;
static uint32_t get(unsigned a) { return ram[a]|(uint32_t)ram[a+1]<<8|(uint32_t)ram[a+2]<<16|(uint32_t)ram[a+3]<<24; }
static void put(unsigned a,uint32_t v) { for(unsigned i=0;i<4;i++) ram[a+i]=v>>(i*8); }
static bool rd(void *p,uint32_t a,void *d,size_t n) { (void)p; if(a>sizeof ram||n>sizeof ram-a)return false; memcpy(d,ram+a,n);return true; }
static bool wr(void *p,uint32_t a,const void *d,size_t n) { (void)p; if(a>sizeof ram||n>sizeof ram-a)return false; memcpy(ram+a,d,n);return true; }
static bool dr(void *p,uint32_t a,uint32_t d[3]) { (void)p; if(a>sizeof ram-12)return false;for(unsigned i=0;i<3;i++)d[i]=get(a+4*i);return true; }
static bool dw(void *p,uint32_t a,const uint32_t *d,size_t n) { (void)p; if(a>sizeof ram||n*4>sizeof ram-a)return false;for(size_t i=0;i<n;i++)put(a+4*i,d[i]);return true; }
static bool send_frame(void *p,const uint8_t *d,size_t n) { (void)p;assert(n<=sizeof sent);memcpy(sent,d,n);sent_len=n;sends++;return true; }
static const CdjSh7764EthOps ops={rd,wr,send_frame,dr,dw};
static void write_reg(CdjSh7764Eth *s,unsigned o,uint32_t v) { assert(cdj_sh7764_eth_write(s,o,v)); }
static uint32_t read_reg(CdjSh7764Eth *s,unsigned o) { uint32_t v;assert(cdj_sh7764_eth_read(s,o,&v));return v; }
static void setup(CdjSh7764Eth *s) {
 memset(ram,0,sizeof ram); sends=0;sent_len=0;
 cdj_sh7764_eth_init(s,&ops,NULL);cdj_sh7764_eth_set_link(s,true,true);
 write_reg(s,0,0x40);write_reg(s,0x18,0x100);write_reg(s,0x20,0x200);
 write_reg(s,0x100,0x62);write_reg(s,0x58,3);write_reg(s,0x108,1518);
}
static void desc(unsigned a,uint32_t status,unsigned len,unsigned buf) {put(a,status);put(a+4,len<<16);put(a+8,buf);}
int main(void) {
 CdjSh7764Eth s;setup(&s);
 assert(read_reg(&s,0x50)==0x707);write_reg(&s,0x118,4);write_reg(&s,0x30,0x400000);
 assert(cdj_sh7764_eth_irq(&s));write_reg(&s,0x28,0x400000);assert(cdj_sh7764_eth_irq(&s));
 write_reg(&s,0x110,4);assert(!cdj_sh7764_eth_irq(&s));
 desc(0x100,0xf0000000,14,0x400);memset(ram+0x400,0xab,14);
 write_reg(&s,8,1);assert(sends==1&&sent_len==60&&get(0x100)==0x70000000);
 for(unsigned i=0;i<60;i++)assert(sent[i]==(i<14?0xab:0));
 assert(read_reg(&s,0x28)&0x200000);write_reg(&s,0x28,0x200000);assert(!(read_reg(&s,0x28)&0x200000));
 setup(&s);desc(0x100,0xa0000000,32,0x400);desc(0x110,0xd0000000,32,0x420);
 memset(ram+0x400,0x55,64);write_reg(&s,8,1);assert(sends==1&&sent_len==64&&s.tx_cursor==0x100);
 assert(!(get(0x100)&0x80000000)&&!(get(0x110)&0x80000000));
 setup(&s);uint8_t frame[96]={0};memset(frame,0xff,6);memset(frame+6,0x12,90);
 desc(0x200,0x80000000,64,0x600);desc(0x210,0xc0000000,64,0x640);write_reg(&s,0x10,1);
 assert(cdj_sh7764_eth_receive(&s,frame,96));assert(!memcmp(ram+0x600,frame,96));
 assert(get(0x200)==0x20000000&&get(0x210)==0x50000000&&get(0x214)==0x00400060);
 assert(read_reg(&s,0x28)&0x40000);assert(s.rx_cursor==0x200);
 setup(&s);frame[0]=1;desc(0x200,0xc0000000,128,0x600);write_reg(&s,0x10,1);
 assert(cdj_sh7764_eth_receive(&s,frame,96));assert(get(0x200)==0x78000080);
 setup(&s);desc(0x200,0xc0000000,128,0x600);write_reg(&s,0x38,0x80);write_reg(&s,0x10,1);
 assert(cdj_sh7764_eth_receive(&s,frame,96));assert(get(0x200)==0x70000000);
 setup(&s);write_reg(&s,0x10,1);assert(cdj_sh7764_eth_receive(&s,frame,96));
 assert(read_reg(&s,0x28)&0x20000);assert(read_reg(&s,0x10)==1);
 write_reg(&s,0,1);assert(read_reg(&s,0x18)==0x100&&read_reg(&s,0x20)==0x200&&read_reg(&s,0x40)==1);
 assert(read_reg(&s,0)==0&&read_reg(&s,0x100)==0);
 setup(&s);assert(!cdj_sh7764_eth_write(&s,0,0x50)&&s.error);
 setup(&s);desc(0x100,0xf0000000,14,0x1fff);assert(!cdj_sh7764_eth_write(&s,8,1)&&!sends);
 setup(&s);desc(0x100,0xf0000000,64,0x2000);assert(!cdj_sh7764_eth_write(&s,8,1)&&!sends);
 setup(&s);assert(!cdj_sh7764_eth_read(&s,0x120,&s.tx_cursor));
 setup(&s);cdj_sh7764_eth_set_link(&s,false,false);desc(0x100,0xf0000000,64,0x400);write_reg(&s,8,1);assert(!sends&&(get(0x100)&0x80000000));
 puts("SH7764 Ethernet core tests passed");return 0;
}
