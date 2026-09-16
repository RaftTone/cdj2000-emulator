#include "cdj_sh7764_eth.h"
#include <string.h>
#define R(s,o) ((s)->regs[(o)/4])
#define ACT UINT32_C(0x80000000)
#define END UINT32_C(0x40000000)
#define FP UINT32_C(0x30000000)
#define FE UINT32_C(0x08000000)
#define ECI UINT32_C(0x00400000)
#define TC UINT32_C(0x00200000)
#define FR UINT32_C(0x00040000)
#define RDE UINT32_C(0x00020000)
#define EES_MASK UINT32_C(0x47ff0f9f)
#define MAX_FRAME 2048u
#define MAX_DESC 128u
static bool fail(CdjSh7764Eth *s, const char *e)
{ if (!s->error) s->error = e; return false; }
static uint32_t be32(const uint8_t *p)
{ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static void put32(uint8_t *p, uint32_t v)
{ p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static bool valid_reg(uint32_t o)
{
    switch(o) {
    case 0: case 8: case 0x10: case 0x18: case 0x20: case 0x28:
    case 0x30: case 0x38: case 0x40: case 0x48: case 0x50: case 0x58:
    case 0x64: case 0x68: case 0x6c: case 0x70: case 0x78: case 0x7c:
    case 0xc8: case 0xcc: case 0xd4: case 0xd8: case 0x100: case 0x108:
    case 0x110: case 0x118: case 0x128: case 0x140: case 0x150:
    case 0x154: case 0x158: case 0x160: case 0x164: case 0x168: case 0x16c:
    case 0x1c0: case 0x1c8: case 0x1d0: case 0x1d4: case 0x1d8:
    case 0x1dc: case 0x1e4: case 0x1e8: case 0x1ec: case 0x1f0:
    case 0x1f4: case 0x1f8: return true;
    default: return false; /* PIR deliberately belongs to the PHY adapter. */
    }
}
void cdj_sh7764_eth_reset(CdjSh7764Eth *s)
{
    memset(s->regs,0,sizeof s->regs);
    R(s,0x50)=0x707; R(s,0x70)=0x70007; R(s,0x150)=0x14;
    R(s,0x128)=s->raw_lmon;
    s->tx_cursor=s->rx_cursor=0; s->error=NULL;
}
void cdj_sh7764_eth_init(CdjSh7764Eth *s,const CdjSh7764EthOps *ops,void *p)
{ memset(s,0,sizeof *s); s->ops=ops; s->opaque=p; cdj_sh7764_eth_reset(s); }
static uint32_t eesr(const CdjSh7764Eth *s)
{ return (R(s,0x28)&~ECI) | ((R(s,0x110)&R(s,0x118)) ? ECI : 0); }
bool cdj_sh7764_eth_irq(const CdjSh7764Eth *s)
{ return (eesr(s)&R(s,0x30)) != 0; }
void cdj_sh7764_eth_set_link(CdjSh7764Eth *s,bool up,bool lmon)
{
    if(s->raw_lmon!=lmon) R(s,0x110)|=4;
    s->link=up; s->raw_lmon=lmon; R(s,0x128)=lmon;
}
bool cdj_sh7764_eth_read(CdjSh7764Eth *s,uint32_t o,uint32_t *v)
{
    if(s->error) return false;
    if(!valid_reg(o)) return fail(s,"unsupported Ethernet register read");
    *v=o==0x28 ? eesr(s) : R(s,o); return true;
}
static bool dma(CdjSh7764Eth *s,uint32_t a,void *p,size_t n,bool wr)
{
    if(a>=0x10000000u || n>0x10000000u-a || !s->ops ||
       (wr ? !s->ops->dma_write || !s->ops->dma_write(s->opaque,a,p,n)
           : !s->ops->dma_read || !s->ops->dma_read(s->opaque,a,p,n))) {
        R(s,0x28)|=0x800000; return fail(s,"Ethernet DMA address/access failure");
    }
    return true;
}
static bool descriptor(CdjSh7764Eth *s,uint32_t a,uint8_t *d)
{
    uint32_t words[3];
    if((a&15)||a>0x0ffffff0 || !s->ops || !s->ops->desc_read ||
       !s->ops->desc_read(s->opaque,a,words)) return fail(s,"Ethernet descriptor read failure");
    for(unsigned i=0;i<3;i++) put32(d+4*i,words[i]);
    return true;
}
static bool write_desc(CdjSh7764Eth *s,uint32_t a,const uint8_t *d,size_t n)
{
    uint32_t words[3];
    for(size_t i=0;i<n;i++) words[i]=be32(d+4*i);
    return (s->ops && s->ops->desc_write && s->ops->desc_write(s->opaque,a,words,n)) ||
        fail(s,"Ethernet descriptor write failure");
}
static uint32_t next_desc(uint32_t a,uint32_t w,uint32_t base)
{ return w&END ? base : a+16; }
static bool mode(CdjSh7764Eth *s)
{ return R(s,0)==0x40 || fail(s,"Ethernet DMA requires DL16 and DE1"); }
static bool transmit(CdjSh7764Eth *s)
{
    uint8_t frame[MAX_FRAME], descs[MAX_DESC][12];
    uint32_t addrs[MAX_DESC]; unsigned count=0, work=0; size_t len=0;
    if(!(R(s,0x100)&0x20) || !s->link) return true;
    if(!mode(s)) return false;
    while(work++<MAX_DESC) {
        uint32_t a=s->tx_cursor, w, n, p; uint8_t *d=descs[count];
        R(s,0xd8)=a;
        if(!descriptor(s,a,d)) return false;
        w=be32(d); n=be32(d+4)>>16; p=be32(d+8);
        if(!(w&ACT)) {
            R(s,8)=0;
            if(count) { R(s,0x28)|=0x100000; return fail(s,"TX descriptor empty mid-frame"); }
            return true;
        }
        if((w&0x03ffffff) || (be32(d+4)&0xffff) || !n || n>MAX_FRAME-len ||
           (n<=16 && (p&31)) || ((w&0x20000000)!=0)!=(count==0))
            return fail(s,"unsupported/malformed Ethernet TX descriptor");
        if(!dma(s,p,frame+len,n,false)) return false;
        R(s,0xd4)=p+n; len+=n; addrs[count++]=a;
        s->tx_cursor=next_desc(a,w,R(s,0x18));
        if(w&0x10000000) {
            size_t sendlen=len<60 ? 60 : len;
            memset(frame+len,0,sendlen-len);
            if(!s->ops->send || !s->ops->send(s->opaque,frame,sendlen))
                return fail(s,"Ethernet backend send failure");
            for(unsigned i=0;i<count;i++) {
                put32(descs[i],be32(descs[i])&~(ACT|FE));
                if(!write_desc(s,addrs[i],descs[i],1)) return false;
            }
            R(s,0x28)|=TC; count=0; len=0;
        }
    }
    return fail(s,"Ethernet TX descriptor work bound exceeded");
}
bool cdj_sh7764_eth_write(CdjSh7764Eth *s,uint32_t o,uint32_t v)
{
    if(o==0 && v==1) {
        /* Manual 20.2 reset table: these registers survive software reset. */
        const unsigned keep[]={0x18,0x20,0x40,0x64,0x68};
        uint32_t saved[5];
        for(unsigned i=0;i<5;i++) saved[i]=R(s,keep[i]);
        cdj_sh7764_eth_reset(s);
        for(unsigned i=0;i<5;i++) R(s,keep[i])=saved[i];
        s->tx_cursor=R(s,0x18); s->rx_cursor=R(s,0x20);
        return true;
    }
    if(s->error) return false;
    if(!valid_reg(o)) return fail(s,"unsupported Ethernet register write");
    switch(o) {
    case 0:
        if((v!=0 && v!=0x40) || (R(s,0x100)&0x60)) return fail(s,"unsupported/active EDMR mode change");
        break;
    case 8: case 0x10:
        if(v&~1u) return fail(s,"reserved DMA request bits");
        R(s,o)=v; return o==8 && v ? transmit(s) : true;
    case 0x18: case 0x20:
        if((v&15)||v>=0x10000000u||R(s,o==0x18?8:0x10)) return fail(s,"invalid/active descriptor base");
        if(o==0x18) s->tx_cursor=v; else s->rx_cursor=v;
        break;
    case 0x28: R(s,o)&=~(v&~ECI); return true;
    case 0x110: R(s,o)&=~(v&0x37); return true;
    case 0x30: if(v&~EES_MASK) return fail(s,"reserved EESIPR bits"); break;
    case 0x118: if(v&~0x37u) return fail(s,"reserved ECSIPR bits"); break;
    case 0x100:
        if(v&~0x63u) return fail(s,"unsupported EtherC mode");
        R(s,o)=v; return R(s,8) ? transmit(s) : true;
    case 0x38: if(v&~0xf9fu) return fail(s,"reserved TRSCER bits"); break;
    case 0x58: if(v&~3u) return fail(s,"reserved RMCR bits"); break;
    case 0x50: if(v!=0x707) return fail(s,"unsupported FIFO configuration"); break;
    case 0x70: if(v!=0x70007) return fail(s,"unsupported flow threshold"); break;
    case 0x108: if(v>2048) return fail(s,"invalid maximum frame length"); break;
    case 0x1c0: break;
    case 0x1c8: if(v&0xffff0000u) return fail(s,"reserved MAC bits"); break;
    case 0x150: if(v>255) return fail(s,"invalid IPG"); break;
    case 0xc8: case 0xcc: case 0xd4: case 0xd8: case 0x128:
        return fail(s,"read-only Ethernet register");
    default:
        if(v) return fail(s,"unsupported Ethernet feature/counter write");
        break;
    }
    R(s,o)=v; return true;
}
bool cdj_sh7764_eth_receive(CdjSh7764Eth *s,const uint8_t *p,size_t n)
{
    uint8_t d[MAX_DESC][12]; uint32_t a[MAX_DESC]; unsigned count=0;
    size_t total=0; uint32_t cursor=s->rx_cursor, flags=0;
    if(s->error) return false;
    if(!s->link || !(R(s,0x100)&0x40) || !R(s,0x10)) return true;
    if(!mode(s)) return false;
    if(n<14 || n>MAX_FRAME) return fail(s,"unsupported Ethernet frame size");
    uint8_t mac[6]; put32(mac,R(s,0x1c0)); mac[4]=R(s,0x1c8)>>8; mac[5]=R(s,0x1c8);
    bool broadcast=true; for(unsigned i=0;i<6;i++) if(p[i]!=255) broadcast=false;
    bool multi=(p[0]&1) && !broadcast;
    if(!(R(s,0x100)&1) && !broadcast && !multi && memcmp(mac,p,6)) return true;
    if(multi) { flags|=0x80; R(s,0x1f8)=(R(s,0x1f8)+1)&0xffff; }
    if(n<60) { flags|=4; R(s,0x1ec)=(R(s,0x1ec)+1)&0xffff; }
    if(n+4>(R(s,0x108)?R(s,0x108):1518)) { flags|=8; R(s,0x1f0)=(R(s,0x1f0)+1)&0xffff; }
    while(total<n && count<MAX_DESC) {
        if(!descriptor(s,cursor,d[count])) return false;
        uint32_t w=be32(d[count]), cap=be32(d[count]+4)>>16, buf=be32(d[count]+8);
        if(!(w&ACT)) {
            if(count) return fail(s,"RX descriptor empty mid-frame is not modeled");
            R(s,0x28)|=RDE; R(s,0x40)=(R(s,0x40)+1)&0xffff;
            if(!(R(s,0x58)&2)) R(s,0x10)=0;
            return true; /* Preflight ensures no partial frame is delivered. */
        }
        if(!cap || (cap&31) || (buf&31)) return fail(s,"invalid RX buffer alignment/length");
        for(unsigned i=0;i<count;i++) if(a[i]==cursor) return fail(s,"RX ring too small for frame");
        a[count++]=cursor; total+=cap; cursor=next_desc(cursor,w,R(s,0x20));
    }
    if(total<n) return fail(s,"Ethernet RX descriptor work bound exceeded");
    size_t done=0;
    for(unsigned i=0;i<count;i++) {
        uint32_t w=be32(d[i]), cap=be32(d[i]+4)>>16, buf=be32(d[i]+8);
        size_t take=n-done<cap?n-done:cap;
        if(!dma(s,buf,(void *)(p+done),take,true)) return false;
        uint32_t status=flags&~R(s,0x38);
        w=(w&END) | (i==0?0x20000000:0) | (i+1==count?0x10000000:0) | status;
        if(status) w|=FE;
        put32(d[i],w); put32(d[i]+4,(cap<<16)|(i+1==count?n:0));
        if(!write_desc(s,a[i],d[i],2)) return false;
        done+=take; R(s,0xc8)=buf+take; R(s,0xcc)=a[i];
    }
    s->rx_cursor=cursor; R(s,0x28)|=FR|flags;
    if(!(R(s,0x58)&1)) R(s,0x10)=0;
    return true;
}
