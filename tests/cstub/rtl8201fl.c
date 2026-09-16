#include "cdj_rtl8201fl.h"
#include <assert.h>
#include <string.h>
static unsigned bit(CdjRtl8201fl *s, bool drive, unsigned b)
{
    unsigned v=(drive ? 2 : 0) | (b ? 4 : 0);
    assert(cdj_rtl8201fl_pir_write(s,v));
    unsigned low=(cdj_rtl8201fl_pir_read(s)>>3)&1;
    assert(cdj_rtl8201fl_pir_write(s,v|1));
    unsigned high=(cdj_rtl8201fl_pir_read(s)>>3)&1;
    assert(low==high); /* Stock firmware samples before the rising edge. */
    return high;
}
static void send(CdjRtl8201fl *s, unsigned v, unsigned n)
{ while(n--) bit(s,true,(v>>n)&1); }
static void begin(CdjRtl8201fl *s,unsigned op,unsigned addr,unsigned reg,bool pre)
{
    if(pre) send(s,0xffffffff,32);
    send(s,1,2); send(s,op,2); send(s,addr,5); send(s,reg,5);
}
static unsigned rd(CdjRtl8201fl *s,unsigned addr,unsigned reg,bool pre)
{
    begin(s,2,addr,reg,pre);
    assert(bit(s,false,1)==1);
    assert(bit(s,false,1)==(addr==1 ? 0u : 1u));
    unsigned v=0; for(unsigned i=0;i<16;i++) v=(v<<1)|bit(s,false,1);
    bit(s,false,1); return v;
}
static void wr(CdjRtl8201fl *s,unsigned addr,unsigned reg,unsigned v,bool pre)
{ begin(s,1,addr,reg,pre); send(s,2,2); send(s,v,16); bit(s,false,1); }
static unsigned direct(CdjRtl8201fl *s,unsigned reg)
{ uint16_t v; assert(cdj_rtl8201fl_read(s,reg,&v)); return v; }
int main(void)
{
    CdjRtl8201fl s, before;
    cdj_rtl8201fl_reset(&s,0);
    assert(rd(&s,1,2,true)==0x001c);
    assert(rd(&s,1,3,false)==0xc816);
    assert(rd(&s,2,31,false)==0xffff);
    wr(&s,2,0,0xffff,false); /* No responding device; no mutation. */
    assert(rd(&s,1,0,false)==0x3100);
    assert(!cdj_rtl8201fl_link(&s));
    assert(cdj_rtl8201fl_advance(&s,500000000));
    assert(!cdj_rtl8201fl_link(&s));
    assert(cdj_rtl8201fl_set_peer(&s,true,0x101,s.now));
    uint64_t deadline=s.deadline;
    assert(cdj_rtl8201fl_advance(&s,deadline-1));
    assert(!s.link);
    assert(cdj_rtl8201fl_advance(&s,deadline)); assert(s.link);
    assert(!(direct(&s,1)&4)); assert(direct(&s,1)&4);
    assert(direct(&s,5)==0x4101); assert(direct(&s,6)==3); assert(direct(&s,6)==1);
    assert(cdj_rtl8201fl_set_peer(&s,false,0x101,s.now)); assert(!s.link);
    assert(cdj_rtl8201fl_set_peer(&s,true,0x101,s.now));
    assert(cdj_rtl8201fl_advance(&s,s.deadline)); assert(s.link);
    assert(!(direct(&s,1)&4)); assert(direct(&s,1)&4);
    wr(&s,1,0,0x3300,false); assert(!s.link);
    assert(cdj_rtl8201fl_advance(&s,s.deadline)); assert(s.link);
    wr(&s,1,31,7,false);
    wr(&s,1,19,rd(&s,1,19,false)|8,false);
    wr(&s,1,17,rd(&s,1,17,false)|3,false);
    bool high=true; assert(cdj_rtl8201fl_led0(&s,&high) && !high);
    wr(&s,1,31,4,false); assert(rd(&s,1,16,false)==0x3300);
    wr(&s,1,16,0,false); wr(&s,1,31,0,false);
    wr(&s,1,13,rd(&s,1,13,false)|7,false);
    wr(&s,1,14,rd(&s,1,14,false)|0x3c,false);
    wr(&s,1,13,rd(&s,1,13,false)|0x4007,false);
    assert(rd(&s,1,14,false)==2); wr(&s,1,14,0,false);
    assert(rd(&s,1,14,false)==0);
    wr(&s,1,0,0x8000,false); assert(!s.link && (s.bmcr&0x8000));
    assert(cdj_rtl8201fl_advance(&s,s.deadline));
    assert(!(s.bmcr&0x8000) && !s.link);
    assert(cdj_rtl8201fl_advance(&s,s.deadline)); assert(s.link);
    assert(rd(&s,1,2,true)==0x1c);
    before=s; uint16_t v=0xbeef;
    assert(!cdj_rtl8201fl_read(&s,7,&v) && v==0xbeef);
    assert(!memcmp(&s,&before,sizeof s));
    assert(!cdj_rtl8201fl_write(&s,0,0x4000));
    assert(!cdj_rtl8201fl_write(&s,31,2));
    assert(!cdj_rtl8201fl_set_peer(&s,true,0x8001,s.now));
    assert(!cdj_rtl8201fl_advance(&s,s.now-1));
    assert(!memcmp(&s,&before,sizeof s));
    cdj_rtl8201fl_reset(&s,0);
    assert(cdj_rtl8201fl_write(&s,4,0x21)); /* Only local10HD, peer100FD. */
    assert(cdj_rtl8201fl_set_peer(&s,true,0x101,0));
    assert(cdj_rtl8201fl_advance(&s,s.deadline)); assert(!s.link);
    assert(!(direct(&s,1)&0x24));
    assert(cdj_rtl8201fl_write(&s,4,0x1e1));
    assert(cdj_rtl8201fl_write(&s,0,0x3300));
    assert(cdj_rtl8201fl_advance(&s,s.deadline)); assert(s.link);
    assert(cdj_rtl8201fl_write(&s,0,0x3900)); assert(!s.link);
    assert(s.deadline==CDJ_PHY_NO_DEADLINE);
    assert(cdj_rtl8201fl_write(&s,0,0x3100));
    assert(cdj_rtl8201fl_advance(&s,s.deadline)); assert(s.link);
    assert(cdj_rtl8201fl_write(&s,0,0x8000));
    assert(cdj_rtl8201fl_advance(&s,s.now+CDJ_PHY_RESET_NS+CDJ_PHY_NEGOTIATE_NS));
    assert(s.link && !s.reset_pending && s.deadline==CDJ_PHY_NO_DEADLINE);
    cdj_rtl8201fl_reset(&s,0); before=s;
    assert(!cdj_rtl8201fl_pir_write(&s,3)); /* No initial preamble. */
    assert(!memcmp(&s,&before,sizeof s));
    send(&s,0xffffffff,32); begin(&s,2,1,7,false);
    bit(&s,false,1);
    before=s;
    assert(!cdj_rtl8201fl_pir_write(&s,0)); /* Unsupported register at TA falling edge. */
    assert(!memcmp(&s,&before,sizeof s));
    return 0;
}
