/* Compare byte-exact transactions against the full-prefix reference. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cdj_c674x.h"
static unsigned reads, writes, fail_write;
static uint64_t bus_hash;
static bool rd(void *p, uint32_t a, uint32_t *v) {
    (void)p; ++reads; *v = a ^ 0xfeed1234u; bus_hash = bus_hash * 33 + a; return true;
}
static bool wr(void *p, uint32_t a, uint64_t v, unsigned n, bool commit) {
    (void)p; bus_hash = bus_hash * 33 + a + v + n + commit;
    if (commit) ++writes;
    return !(commit && fail_write);
}
static void dump(CdjC674x *c, bool ok) {
    unsigned result = ok;
    fwrite(&result, sizeof result, 1, stdout);
    const char *fault = c->fault ? c->fault : "";
    fwrite(fault, strlen(fault) + 1, 1, stdout);
    c->fault = NULL;
    fwrite(c, sizeof *c, 1, stdout);
    fwrite(&reads, sizeof reads, 1, stdout);
    fwrite(&writes, sizeof writes, 1, stdout);
    fwrite(&bus_hash, sizeof bus_hash, 1, stdout);
}
int main(void) {
    for (unsigned seed = 1; seed <= 16; ++seed) {
        for (unsigned scenario = 0; scenario < 12; ++scenario) {
            CdjC674x c;
            cdj_c674x_reset(&c, 0x1000);
            /* Only dead queue storage and the forbidden loop tail are poisoned.
             * All live bool/count/control fields retain valid reset values. */
            memset(c.stores, seed * 13, sizeof c.stores);
            memset(c.loads, seed * 13, sizeof c.loads);
            memset(&c.loop, seed * 13, sizeof c.loop);
            memset(c.loop_instructions, seed * 13, sizeof c.loop_instructions);
            c.cycles = 10; c.packets = 9;
            c.r[0][4] = 0x1040; c.r[1][4] = 0x1080;
            c.r[1][10] = 0x12345678;
            reads = writes = fail_write = 0; bus_hash = 0;
            CdjC674xPacket p = {.count=1, .next_pc=0x1004,
                .instructions={{.word=(3u<<23)|(123u<<7)|0x28, .pc=0x1000}}};
            if (scenario == 1) p.instructions[0].word = 0x6000; /* NOP 4 */
            if (scenario == 2) p.instructions[0].word = 0x1e000; /* IDLE */
            if (scenario == 3 || scenario == 10) {
                c.branch_due=11; c.branch_target=0x2000; c.loop_active=true;
                c.control[26] |= 1u<<14;
                if (scenario == 10) c.control_ready[31] |= UINT64_C(1)<<54;
            }
            if (scenario == 4 || scenario == 9) {
                c.load_count=2; c.store_count=2;
                memset(c.loads, 0, 2*sizeof c.loads[0]);
                memset(c.stores, 0, 2*sizeof c.stores[0]);
                c.loads[0]=(CdjC674xLoad){.due=11,.value=0x45,.bank=0,.dst=8};
                c.loads[1]=(CdjC674xLoad){.due=13,.value=0x67,.bank=1,.dst=9};
                c.stores[0]=(CdjC674xStore){.due=11,.address=0x1100,.value=seed,.size=4};
                c.stores[1]=(CdjC674xStore){.due=13,.address=0x1104,.value=seed+1,.size=4};
                p.instructions[0].word=0x6000;
                if (scenario == 9) fail_write=1;
            }
            if (scenario == 5) p.instructions[0].word=0x05100264; /* LDW */
            if (scenario == 6) p.instructions[0].word=0x051002f6; /* STW */
            if (scenario == 7) p.count=9;
            if (scenario == 8) { p.count=2; p.instructions[1].word=0xffffffff; p.instructions[1].pc=0x1004; }
            if (scenario == 11) {
                p.instructions[0].word=0x05100264; c.load_count=40;
                memset(c.loads,0,sizeof c.loads);
                for (unsigned j=0;j<40;++j) c.loads[j]=(CdjC674xLoad){.due=100+j,.bank=j%2,.dst=j%32};
            }
            bool ok=cdj_c674x_execute(&c,&p,rd,wr,NULL);
            assert(ok == !(scenario==7 || scenario==8 || scenario==9 || scenario==10 || scenario==11));
            dump(&c,ok);
            if (scenario==5 || scenario==6) {
                p.instructions[0].word=0x6000;
                ok=cdj_c674x_execute(&c,&p,rd,wr,NULL); assert(ok); dump(&c,ok);
            }
        }
    }
    return 0;
}
