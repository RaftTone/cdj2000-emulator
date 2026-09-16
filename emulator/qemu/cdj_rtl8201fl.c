#include "cdj_rtl8201fl.h"
/* Realtek RTL8201F/FL/FN datasheet Rev1.4, tables11-17,40-41.
 * Register/framing semantics are documented; 1ms software-reset and 100ms
 * virtual-peer negotiation delays are deterministic approximations, not
 * measured PHY/analog timing. No peer signal is inferred from elapsed time.
 */
static void down(CdjRtl8201fl *s)
{
    s->link = false; s->latch_low = true; s->anlpar = 1; s->aner = 0;
}
static bool arm(CdjRtl8201fl *s, uint64_t delay)
{
    if (s->now >= UINT64_MAX - delay) return false;
    s->deadline = s->now + delay;
    return true;
}
static bool negotiate(CdjRtl8201fl *s)
{
    down(s); s->deadline = CDJ_PHY_NO_DEADLINE;
    if (s->peer && !(s->bmcr & 0x0c00)) return arm(s, CDJ_PHY_NEGOTIATE_NS);
    s->bmcr &= ~0x0200;
    return true;
}
void cdj_rtl8201fl_reset(CdjRtl8201fl *s, uint64_t now)
{
    *s = (CdjRtl8201fl){.bmcr=0x3100, .anar=0x01e1, .anlpar=1,
        .led_control=0x30, .eee_config=0x3300, .eee_advert=2,
        .mdi=1, .latch_low=true, .now=now, .deadline=CDJ_PHY_NO_DEADLINE};
}
bool cdj_rtl8201fl_advance(CdjRtl8201fl *s, uint64_t now)
{
    if (now < s->now || now == UINT64_MAX) return false;
    CdjRtl8201fl n = *s;
    if (n.deadline != CDJ_PHY_NO_DEADLINE && now >= n.deadline) {
        n.now = n.deadline;
        n.deadline = CDJ_PHY_NO_DEADLINE;
        if (n.reset_pending) {
            n.reset_pending=false; n.bmcr=0x3100; n.anar=0x01e1;
            n.page=0; n.led_config=0; n.led_control=0x30;
            n.eee_config=0x3300; n.eee_advert=2;
            n.mmd_control=0; n.mmd_address=0;
            if (!negotiate(&n)) return false;
        }
        if (!n.reset_pending && n.deadline != CDJ_PHY_NO_DEADLINE &&
            now >= n.deadline) n.deadline = CDJ_PHY_NO_DEADLINE;
        if (n.deadline == CDJ_PHY_NO_DEADLINE && n.peer && !(n.bmcr & 0x0c00)) {
            unsigned common = n.anar & n.partner & 0x1e0;
            if (common) {
                unsigned best = common & 0x100 ? 0x100 : common & 0x80 ? 0x80 :
                                common & 0x40 ? 0x40 : 0x20;
                n.link=true; n.anlpar=n.partner | 0x4000; n.aner=3;
                n.bmcr &= ~0x2300;
                if (best & 0x180) n.bmcr |= 0x2000;
                if (best & 0x140) n.bmcr |= 0x0100;
            }
            n.bmcr &= ~0x0200;
        }
    }
    n.now=now; *s=n; return true;
}
bool cdj_rtl8201fl_set_peer(CdjRtl8201fl *s, bool present,
                          uint16_t advertisement, uint64_t now)
{
    if ((advertisement & ~0x0de1) || (advertisement & 31) != 1) return false;
    CdjRtl8201fl n=*s;
    if (!cdj_rtl8201fl_advance(&n, now)) return false;
    if (n.peer != present || n.partner != advertisement) {
        n.peer=present; n.partner=advertisement;
        if (n.reset_pending) down(&n);
        else if (!negotiate(&n)) return false;
    }
    *s=n; return true;
}
uint64_t cdj_rtl8201fl_deadline(const CdjRtl8201fl *s) { return s->deadline; }
bool cdj_rtl8201fl_link(const CdjRtl8201fl *s) { return s->link; }
bool cdj_rtl8201fl_led0(const CdjRtl8201fl *s, bool *high)
{
    if (!high) return false;
    bool on;
    if (s->led_control & 8) {
        unsigned mode=s->led_config & 15;
        if (mode & ~3u) return false; /* Activity pulses not modeled. */
        on=s->link && (mode & (s->bmcr & 0x2000 ? 2 : 1));
    } else {
        /* Default LINK10/ACT10: no activity is synthesized. At 100M this
         * LED is inactive. Other traditional activity modes unsupported. */
        if ((s->led_control & 0x30) != 0x30) return false;
        on=s->link && !(s->bmcr & 0x2000);
    }
    *high=!on; return true;
}
bool cdj_rtl8201fl_read(CdjRtl8201fl *s, unsigned reg, uint16_t *value)
{
    if (!value) return false;
    uint16_t v;
    if (reg == 31) { *value=s->page; return true; }
    if (s->page == 7) {
        if (reg == 17) v=s->led_config;
        else if (reg == 19) v=s->led_control;
        else return false;
        *value=v; return true;
    }
    if (s->page == 4) {
        if (reg != 16) return false;
        *value=s->eee_config; return true;
    }
    switch (reg) {
    case 0: v=s->bmcr; break;
    case 1: v=0x7849 | (s->link ? 0x20 : 0) |
              (s->link && !s->latch_low ? 4 : 0); s->latch_low=false; break;
    case 2: v=0x001c; break;
    case 3: v=0xc816; break;
    case 4: v=s->anar; break;
    case 5: v=s->anlpar; break;
    case 6: v=s->aner; s->aner &= ~2; break;
    /* Table18 marks MACR fields WO. Zero-read is a bounded compatibility
     * assumption for the observed read/OR/write, not hardware-verified. */
    case 13: v=0; break;
    case 14:
        if (!(s->mmd_control & 0x4000)) v=s->mmd_address;
        else if (s->mmd_control == 0x4007 && s->mmd_address == 0x3c) v=s->eee_advert;
        else return false;
        break;
    default: return false;
    }
    *value=v; return true;
}
bool cdj_rtl8201fl_write(CdjRtl8201fl *s, unsigned reg, uint16_t value)
{
    CdjRtl8201fl n=*s;
    if (reg == 31) {
        if (value != 0 && value != 4 && value != 7) return false;
        n.page=value; *s=n; return true;
    }
    if (n.page == 7) {
        if (reg == 17 && !(value & ~0x0033)) n.led_config=value;
        else if (reg == 19 && !(value & ~0x0038)) n.led_control=value;
        else return false;
        *s=n; return true;
    }
    if (n.page == 4) {
        if (reg != 16 || (value & ~0x3300)) return false;
        n.eee_config=value; *s=n; return true;
    }
    if (reg == 13) {
        if (value != 7 && value != 0x4007) return false;
        n.mmd_control=value;
    } else if (reg == 14) {
        if (n.mmd_control == 7 && value == 0x3c) n.mmd_address=value;
        else if (n.mmd_control == 0x4007 && n.mmd_address == 0x3c && !(value & ~2)) n.eee_advert=value;
        else return false;
    } else
    if (reg == 4) {
        if ((value & ~0x0de1) || (value & 31) != 1 || n.reset_pending) return false;
        n.anar=value; /* Changes are advertised on a subsequent restart. */
    } else if (reg == 0) {
        if (value & ~0xbf00) return false; /* loopback/test/reserved unsupported */
        if (value & 0x4000) return false;
        if (value & 0x8000) {
            down(&n); n.bmcr=0xb100; n.reset_pending=true;
            n.synchronized=false; n.preamble=0;
            if (!arm(&n, CDJ_PHY_RESET_NS)) return false;
        } else {
            if (!(value & 0x1000) || n.reset_pending) return false; /* forced mode */
            uint16_t old=n.bmcr; n.bmcr=value;
            if ((value & 0x0200) || ((old ^ value) & 0x0c00)) {
                if (!negotiate(&n)) return false;
            }
        }
    } else return false;
    *s=n; return true;
}
static bool clock_bit(CdjRtl8201fl *s, bool drive, unsigned bit)
{
    if (!s->frame) {
        if (s->idle_needed) {
            if (!bit) return false;
            s->idle_needed=false;
        }
        if (bit) { if (s->preamble < 32) ++s->preamble; return true; }
        if (!drive || (!s->synchronized && s->preamble < 32)) return false;
        s->synchronized=true; s->frame=true; s->bits=1; s->header=0;
        s->preamble=0; return true;
    }
    if (s->bits < 14) {
        if (!drive) return false;
        s->header=(s->header << 1) | bit;
        ++s->bits;
        if (s->bits == 2 && s->header != 1) return false;
        if (s->bits == 4 && (s->header & 3) != 1 && (s->header & 3) != 2) return false;
        return true;
    }
    unsigned op=(s->header >> 10) & 3, addr=(s->header >> 5) & 31;
    unsigned reg=s->header & 31;
    if (op == 2) {
        if (drive) return false;
        /* The PHY drove this bit at the preceding falling edge. Keep it
         * stable across the sampling rising edge. */
    } else {
        if (!drive) return false;
        if (s->bits == 14 && bit != 1) return false;
        if (s->bits == 15 && bit != 0) return false;
        if (s->bits >= 16) s->data=(uint16_t)((s->data << 1) | bit);
        if (s->bits == 31 && addr == 1 && !cdj_rtl8201fl_write(s, reg, s->data)) return false;
    }
    if (++s->bits == 32) { s->frame=false; s->idle_needed=true; }
    return true;
}
static bool falling_edge(CdjRtl8201fl *s)
{
    s->mdi=1;
    if (!s->frame || s->bits < 14 || ((s->header >> 10) & 3) != 2) return true;
    unsigned addr=(s->header >> 5) & 31;
    if (s->bits == 15) {
        s->mdi=addr == 1 ? 0 : 1;
        s->data=0xffff;
        if (addr == 1 && !cdj_rtl8201fl_read(s,s->header & 31,&s->data)) return false;
    } else if (s->bits >= 16) s->mdi=(s->data >> (31-s->bits)) & 1;
    return true;
}
bool cdj_rtl8201fl_pir_write(CdjRtl8201fl *s, uint32_t value)
{
    if (value & ~15u) return false;
    CdjRtl8201fl n=*s;
    if (!(s->pir & 1) && (value & 1) &&
        !clock_bit(&n, (value & 2) != 0, (value & 2) ? !!(value & 4) : 1)) return false;
    /* Realtek Figure6: MDIO read data is valid before MDC rises. NXS
     * samples PIR while low, then raises MDC; sampling after rise also works. */
    if ((s->pir & 1) && !(value & 1) && !falling_edge(&n)) return false;
    n.pir=value & 7; *s=n; return true;
}
uint32_t cdj_rtl8201fl_pir_read(const CdjRtl8201fl *s)
{
    return s->pir | ((s->pir & 2 ? !!(s->pir & 4) : s->mdi) << 3);
}
