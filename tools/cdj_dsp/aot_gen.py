#!/usr/bin/env python3
"""Compile hot lean C674x packets ahead of time.

Input: one or more profiles written by cdj_c674x_aot_profile_dump (replay
with CDJ_DSP_AOT_PROFILE=path, or the board's --dsp-aot-profile).  Output:
one C file for a build to include as CDJ_C674X_AOT_FILE (see "ahead-of-time
regions" in emulator/qemu/cdj_c674x.c).  It is derived from the DSP image
the profile ran, so it lives with the build, not in the repository.

Each node is one variant: a packet (its fetch blocks and bytes), the queue
shape it starts from and its pmask predicates.  Its code is dts_lean's
pieces with the variant's constants (the issue per instruction, the checks,
the bus and retirement programs unrolled), then dt_run's post-packet steps,
then its profiled successors: the next pc picks a packet, its entry must be
current and hold that packet's bytes, the predicates pick the node (whose
entry shape is the one this node leaves).  Nodes reachable from each other
share a C function (goto); an edge into another function is a tail call.
The approach follows Stijn Jacobs' c14_jitgen.py for cdj-nxs2-qemu
(THIRD_PARTY.md); the semantics are this core's.

Steady SPLOOP kernels (the profiles' J lines: a JitKernel's bytes, as
jk_compile made them) become constant JitKernels with jk_burst_t over them,
so the compiler folds each phase's tables into straight-line code; the core
uses one for a loop whose own kernel has the same bytes.

usage: aot_gen.py OUT.c PROFILE [PROFILE...] [--min N] [--group N]
                  [--kernel-min N]
"""
import argparse
import collections
import sys

P_END, P_TICK, P_COMMIT, P_E3 = 0, 1, 2, 3
(P_STAIL, P_SMOVE, P_SFILL, P_SCOUNT, P_LTAIL, P_LE3, P_LRET, P_LMOVE,
 P_LFILL, P_LCOUNT, P_BRANCH) = range(4, 15)
# The core's numbering, from the profiles' K line (and asserted in the
# output, so a profile from another core cannot be built silently).
K = {}
OP_FIELDS = [("kind", "kind"), ("creg", "creg"), ("cb", "creg_bank"),
             ("cr", "creg_reg"), ("z", "z"), ("side", "side"), ("dst", "dst"),
             ("a", "a"), ("b", "b"), ("cross", "cross"), ("bank", "bank"),
             ("mode", "mode"), ("size", "size"), ("scale", "scale"),
             ("pair", "pair"), ("nonaligned", "nonaligned"),
             ("store", "is_store"), ("sx", "sign_extend"),
             ("mul", "multiply"), ("swap", "swap"), ("oper", "operation")]


class Node:
    pass


def fields(tokens):
    return {tokens[i]: int(tokens[i + 1], 0) for i in range(0, len(tokens) - 1, 2)}


def tuples(text):
    return [tuple(int(x) for x in t.split(",")) for t in text.split()]


def shape(tokens):
    """S/O line -> hashable shape key (None when not known)."""
    if tokens[0] == "none":
        return None
    loads, stores, branches = (int(t) for t in tokens[:3])
    land = (int(tokens[3], 0), int(tokens[4], 0))
    rest = tokens[5:]
    ld = tuple(tuple(int(x) for x in t.split(",")) for t in rest[:loads])
    st = tuple(tuple(int(x) for x in t.split(",")) for t in rest[loads:loads + stores])
    br = tuple(int(t) for t in rest[loads + stores:loads + stores + branches])
    return (ld, st, br, land)


KJ = {}
KERNELS = collections.OrderedDict()


def parse(paths):
    nodes = {}
    for path in paths:
        cur = None
        for line in open(path):
            t = line.split()
            if not t:
                continue
            if t[0] == "KJ":
                kj = fields(t[1:])
                if KJ and KJ != kj:
                    sys.exit("%s: kernels laid out by another core" % path)
                KJ.update(kj)
                continue
            if t[0] == "J":
                f = dict(zip(t[1::2], t[2::2]))
                key = (int(f["ii"]), int(f["ops"]), f["op"], f["lat"], f["ph"])
                KERNELS[key] = KERNELS.get(key, 0) + int(f["steady"])
                continue
            if t[0] == "K":
                k = fields(t[1:])
                if K and K != k:
                    sys.exit("%s: numbered by another core" % path)
                K.update(k)
                continue
            if t[0] == "V":
                cur = Node()
                cur.ptr, cur.runs = t[1], int(t[2])
                cur.pc, cur.next = int(t[4], 0), int(t[6], 0)
                nb = int(t[8])
                cur.blocks = []
                for b in t[9:9 + nb]:
                    addr, data = b.split(":")
                    cur.blocks.append((int(addr, 0), bytes.fromhex(data)))
                cur.insns = []
                cur.file = path
            elif t[0] == "I":
                cur.insns.append(fields(t[2:]))
            elif t[0] == "S":
                cur.ins = shape(t[1:])
            elif t[0] == "P":
                head, *parts = line.split("|")
                h = head.split()
                f = fields(h[1:21])
                cur.outcome, cur.pmask = f["outcome"], f["pmask"]
                cur.br_plan, cur.generic = f["branches_plan"], f["generic"]
                cur.ctl = f["ctl"]
                cur.cycles, cur.last = f["cycles"], f["last"]
                cur.ns, cur.nl, cur.nb = f["ns"], f["nl"], f["nb"]
                cur.branch_rd = [int(x) for x in h[21:21 + cur.nb]]
                cur.new_loads = tuples(parts[0])
                cur.new_stores = tuples(parts[1])
                cur.pairs = tuples(parts[2])
            elif t[0] == "G":
                cur.prog = [int(x) for x in t[1:-2]]
                cur.bus_len = int(t[-1])
            elif t[0] == "O":
                cur.out = shape(t[1:])
            elif t[0] == "E":
                cur.succ = [(t[1], int(t[2])), (t[3], int(t[4]))]
                key = (cur.pc, tuple(cur.blocks), cur.ins, cur.outcome)
                old = nodes.get(key)
                if old:                 # several profiles: add up
                    old.runs += cur.runs
                    old.ptrs.add((cur.file, cur.ptr))
                    old.succ_all += [(cur.file, p, r) for p, r in cur.succ]
                else:
                    cur.ptrs = {(cur.file, cur.ptr)}
                    cur.succ_all = [(cur.file, p, r) for p, r in cur.succ]
                    nodes[key] = cur
    return list(nodes.values())


def c_bytes(b):
    return "{" + ",".join("0x%02x" % x for x in b) + "}"


def emit(nodes, out, group):
    # packets: by (pc, blocks); nodes sorted so each packet's are contiguous
    nodes.sort(key=lambda n: (n.pc, tuple(n.blocks), -n.runs))
    packets = collections.OrderedDict()
    for n in nodes:
        packets.setdefault((n.pc, tuple(n.blocks)), []).append(n)
    order = [n for ns in packets.values() for n in ns]
    for i, n in enumerate(order):
        n.id = i
    for k, (key, ns) in enumerate(packets.items()):
        for n in ns:
            n.packet = k
    by_ptr = {p: n for n in order for p in n.ptrs}
    # edges: profiled successors whose entry shape is this node's exit shape
    for n in order:
        succ = collections.Counter()
        for f, p, r in n.succ_all:
            s = by_ptr.get((f, p))
            if s and r and n.out is not None and s.ins == n.out:
                succ[s.id] += r
        n.edges = [order[i] for i, _ in succ.most_common()]
    # groups: weakly connected, in chunks of at most `group` nodes
    parent = list(range(len(order)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    for n in order:
        for s in n.edges:
            parent[find(n.id)] = find(s.id)
    comps = collections.defaultdict(list)
    for n in order:
        comps[find(n.id)].append(n)
    groups = []
    for comp in comps.values():
        # breadth first from the hottest, so chunks keep hot chains together
        comp.sort(key=lambda n: -n.runs)
        seen, seq = set(), []
        for root in comp:
            if root.id in seen:
                continue
            q = collections.deque([root])
            seen.add(root.id)
            while q:
                n = q.popleft()
                seq.append(n)
                for s in n.edges:
                    if s.id not in seen and find(s.id) == find(root.id):
                        seen.add(s.id)
                        q.append(s)
        for i in range(0, len(seq), group):
            groups.append(seq[i:i + group])
    for g, ns in enumerate(groups):
        for k, n in enumerate(ns):
            n.group, n.local = g, k

    # entry and exit shapes, numbered
    shapes = {}
    for n in order:
        n.in_id = shapes.setdefault(n.ins, len(shapes))
    for n in order:
        n.out_id = shapes.setdefault(n.out, len(shapes)) if n.out is not None else None
    w = out.write
    w("/* Generated by tools/cdj_dsp/aot_gen.py from %s: %u packets, %u nodes,"
      " %u functions.  Included by cdj_c674x.c (CDJ_C674X_AOT_FILE). */\n"
      % (" ".join(sorted({n.file for n in order})), len(packets), len(order), len(groups)))
    if not K:
        sys.exit("no K line: profiles from an older core")
    w("_Static_assert(%s, \"aot_gen.py: profile numbered by another core\");\n" % " && ".join(
        "%s == %u" % (name.upper(), value) for name, value in sorted(K.items())
        if name != "lop_inline").replace("LOP_", "LOP_"))
    w("_Static_assert(LOP_ADD_IMM == %u, \"aot_gen.py: profile numbered by another core\");\n" % K["lop_inline"])
    w("static const AotPacket aot_packets[%u] = {\n" % len(packets))
    for k, ((pc, blocks), ns) in enumerate(packets.items()):
        bl = ",".join("%#x" % a for a, _ in blocks)
        by = ",".join(c_bytes(b) for _, b in blocks)
        n0 = ns[0]
        pred = ",".join("{%u,%u,%u}" % (ins["cb"], ins["cr"], ins["z"]) if n0.pmask >> i & 1 else "{0,0,0}"
                        for i, ins in enumerate(n0.insns)) or "{0}"
        w("  {%#x, %u, {%s}, {%s}, %u, %u, %u, {%s}},\n" % (pc, len(blocks), bl, by, n0.id, len(ns),
                                                       n0.pmask, pred))
    w("};\nstatic const unsigned aot_npackets = %u;\n" % len(packets))
    for g in range(len(groups)):
        w("static int aot_g%u(CdjC674x *cpu, unsigned node, CdjC674xCacheEntry *e, AotCtx *x);\n" % g)
    for n in order:
        ld, st, br, _ = n.ins
        if ld:
            w("static const DtsLoad aot_l%u[] = {%s};\n" % (n.id, ",".join("{%u,%u,%u,%u,%u}" % t for t in ld)))
        if st:
            w("static const DtsStore aot_s%u[] = {%s};\n" % (n.id, ",".join("{%u,%u}" % t for t in st)))
    w("static const AotNode aot_nodes[%u] = {\n" % len(order))
    for n in order:
        ld, st, br, _ = n.ins
        w("  {%u, %u, %u, %u, %u, {%s}, %s, %s, aot_g%u, %u},\n" % (
            n.in_id, n.outcome, len(ld), len(st), len(br), ",".join(map(str, br)) or "0",
            "aot_l%u" % n.id if ld else "NULL", "aot_s%u" % n.id if st else "NULL",
            n.group, n.local))
    w("};\n")
    for n in order:
        nl = ",".join("{%u,%u,%u,%u,%u}" % t for t in n.new_loads) or "{0}"
        ns = ",".join("{%u,%u}" % t for t in n.new_stores) or "{0}"
        pr = ",".join("{%u,%u}" % t for t in n.pairs) or "{0}"
        w("static const DtsVariant aot_v%u = {.new_stores = %u, .new_loads = %u, "
          ".branches = %u, .cycles = %u, .last = %u, .branch_rd = {%s}, "
          ".new_load = {%s}, .new_store = {%s}, .pairs = %u, .pair = {%s}};\n" % (
              n.id, n.ns, n.nl, n.nb, n.cycles, n.last,
              ",".join(map(str, n.branch_rd)) or "0", nl, ns, len(n.pairs), pr))
    for g, ns in enumerate(groups):
        emit_group(w, g, ns)


def emit_group(w, g, ns):
    w("\nstatic int aot_g%u(CdjC674x *cpu, unsigned node, CdjC674xCacheEntry *e, AotCtx *x)\n{\n" % g)
    w("    const CdjC674xRead rd = x->read;\n    const CdjC674xWrite wr = x->write;\n"
      "    void *const op = x->opaque;\n    (void)rd; (void)wr; (void)op;\n    switch (node) {\n")
    for n in ns:
        w("    case %u: goto n%u;\n" % (n.local, n.id))
    w("    default: return AOT_DECLINED;\n    }\n")
    for n in ns:
        emit_node(w, n)
    w("}\n")


def op_literal(ins):
    return "&(const JitOp){%s}" % ", ".join(".%s = %u" % (c, ins[f]) for f, c in OP_FIELDS)


def emit_direct(w, n):
    """The direct form (see "direct" in cdj_c674x.c): checks first, then
    the bus phase, the appends, the register writes, retirement.  False
    (nothing written) where it does not apply."""
    if n.generic or n.ctl:
        return False
    ld, st, br, land = n.ins
    s0, l0 = len(st), len(ld)
    writes = []                     # (bank, reg) written at issue
    stores, loads = [], []          # appended, in order: (instruction, kind)
    branches = []                   # instructions queueing a branch
    for i, ins in enumerate(n.insns):
        lop = ins["lop"]
        en = (n.outcome >> i) & 1 if (n.pmask >> i) & 1 else 1
        if lop == LOP_NOP:
            continue
        elif lop in (LOP_MEM, LOP_CB15, LOP_CDPP):
            if lop != LOP_MEM or en:
                (stores if ins["store"] else loads).append((i, "mem"))
            if lop == LOP_MEM and en and ins["mode"] & 8:
                writes.append((ins["bank"], ins["b"]))
            if lop == LOP_CDPP:
                writes.append((1, 15))
        elif lop == LOP_SP:
            if en:
                loads.append((i, "sp"))
        elif lop == LOP_COMPACT:
            writes.append((ins["cside"], ins["cdst"]))
        elif lop == LOP_BRANCH and ins["arm"]:
            if en:
                branches.append(i)
        elif lop >= LOP_INLINE:
            writes.append((ins["side"], ins["dst"]))
        else:
            return False
    if len(set(writes)) != len(writes):
        return False
    if any((land[b] >> r) & 1 for b, r in writes):
        return False
    if len(stores) != n.ns or len(loads) != n.nl:
        return False
    prog = n.prog
    bus, i = [], 0
    while i < n.bus_len:
        o = prog[i]
        if o == P_TICK:
            bus.append(("tick", prog[i + 1]))
        elif o == P_COMMIT:
            if prog[i + 1] >= s0:
                return False
            bus.append(("commit", prog[i + 1]))
        elif o == P_E3:
            if prog[i + 1] >= l0:
                return False
            bus.append(("e3", prog[i + 1]))
        else:
            break
        i += 2
    w("n%u: { /* pc %#x, %u runs, direct */\n" % (n.id, n.pc, n.runs))
    w("    const uint64_t start = cpu->cycles;\n")
    if any(ins["lop"] == LOP_COMPACT for ins in n.insns):
        w("    const uint32_t fpc = cpu->fault_pc, fword = cpu->fault_word;\n")
    decline = "return x->packets ? AOT_REDO : AOT_DECLINED;"
    for i, ins in enumerate(n.insns):
        lop = ins["lop"]
        en = (n.outcome >> i) & 1 if (n.pmask >> i) & 1 else 1
        opl = op_literal(ins)
        if lop == LOP_MEM:
            if en:
                w("    DtsMem m%u;\n    if (!lean_mem_prep(cpu, %s, &m%u, rd, wr, op)) %s\n" % (i, opl, i, decline))
            else:
                w("    if (!lean_mem_ready(cpu, %s)) %s\n" % (opl, decline))
        elif lop in (LOP_CB15, LOP_CDPP):
            w("    DtsMem m%u;\n    if (!lean_%s_prep(cpu, %s, &m%u, rd, wr, op)) %s\n"
              % (i, "cb15" if lop == LOP_CB15 else "cdpp", opl, i, decline))
        elif lop == LOP_COMPACT:
            w("    unsigned cs%u, cd%u;\n    uint32_t cv%u;\n" % (i, i, i))
            w("    const int cr%u = compact_value(cpu, &(const CdjC674xInstruction){.word = %#xu, "
              ".pc = %#xu, .header = %#xu, .compact = true}, %#xu, %u, &cs%u, &cd%u, &cv%u);\n"
              % (i, ins["w"], ins["pc"], ins["header"], ins["w"], ins["form"], i, i, i))
            w("    if (cr%u != 0 && cr%u != 1) { cpu->fault = NULL; cpu->fault_pc = fpc; "
              "cpu->fault_word = fword; %s }\n" % (i, i, decline))
        elif lop == LOP_BRANCH and en:
            arm, wd, pc = ins["arm"], ins["w"], ins["pc"]
            if arm == 1:
                disp = wd >> 7 & 0x1fffff
                disp = disp - (1 << 21) if disp & (1 << 20) else disp
                target = "%#xu" % (((pc & ~31) + disp * 4) & 0xffffffff)
            elif arm == 2:
                disp = wd >> 16 & 4095
                disp = disp - 4096 if disp & 2048 else disp
                target = "%#xu" % (((pc & ~31) + disp * (2 if ins["header"] else 4)) & 0xffffffff)
            elif arm == 3:
                target = "cpu->r[%u][%u]" % (((wd >> 12) & 1) ^ 1, ins["b"])
            else:
                target = "cpu->r[%u][%u]" % (ins["cross"], ins["b"])
            w("    const uint32_t t%u = %s;\n" % (i, target))
        elif lop == LOP_SP and en:
            w("    const CdjC674xSpResult s%u = lean_sp(cpu, %s);\n" % (i, opl))
        elif lop >= LOP_INLINE:
            if ins["creg"]:
                w("    const bool e%u = (cpu->r[%u][%u] != 0) ^ %u;\n" % (i, ins["cb"], ins["cr"], ins["z"]))
            w("    const uint32_t v%u = dts_value(cpu, %u, %s, %#xu);\n" % (i, lop, opl, ins["w"]))
    for li, si in n.pairs:
        def ref(kind, idx):
            if kind == "load":
                if idx < l0:
                    return "cpu->loads[%u].address, cpu->loads[%u].size" % (idx, idx)
                k, how = loads[idx - l0]
                return "m%u.address, m%u.encoded" % (k, k) if how == "mem" else None
            if idx < s0:
                return "cpu->stores[%u].address, cpu->stores[%u].size" % (idx, idx)
            k, _ = stores[idx - s0]
            return "m%u.address, m%u.encoded" % (k, k)
        lr, sr = ref("load", li), ref("store", si)
        if lr is None:
            continue                # an SP result: size 0, no overlap
        w("    if (lean_overlap(%s, %s)) %s\n" % (lr, sr, decline))
    w("    if (aot_stress()) %s\n" % decline)
    fallible = any(k != "tick" for k, _ in bus)
    if fallible:
        w("    const char *broke;\n")
    e3 = 0
    for kind, a in bus:
        if kind == "tick":
            w("    lean_tick(cpu, %u);\n" % a)
        elif kind == "commit":
            w("    if ((broke = lean_commit(cpu, %u, wr, op))) goto broke%u;\n" % (a, n.id))
        else:
            w("    uint64_t d%u;\n    if ((broke = lean_e3_to(cpu, %u, &d%u, rd, op))) goto broke%u;\n"
              % (e3, a, e3, n.id))
            e3 += 1
    for k, (i, _) in enumerate(stores):
        w("    cpu->stores[%u] = (CdjC674xStore){.due = start + 3, .address = m%u.address, "
          ".value = m%u.value, .size = m%u.encoded};\n" % (s0 + k, i, i, i))
    if stores:
        w("    cpu->store_count = %u;\n" % (s0 + len(stores)))
    for k, (i, how) in enumerate(loads):
        ins = n.insns[i]
        if how == "mem" and ins["lop"] != LOP_MEM:
            w("    cpu->loads[%u] = (CdjC674xLoad){.due = start + 5, .address = m%u.address, "
              ".bank = %u, .dst = %u, .size = %u};\n"
              % (l0 + k, i, ins["bank"], ins["dst"], 4 if ins["lop"] == LOP_CB15 else ins["size"]))
        elif how == "mem":
            w("    cpu->loads[%u] = (CdjC674xLoad){.due = start + 5, .address = m%u.address, "
              ".bank = %u, .dst = %u, .size = m%u.encoded, .sign_extend = %u};\n"
              % (l0 + k, i, ins["side"], ins["dst"], i, ins["sx"]))
        else:
            w("    cpu->loads[%u] = (CdjC674xLoad){.due = start + 4, .value = s%u.value, "
              ".address = s%u.status << %u, .bank = %u, .dst = %u, .size = 0, .sign_extend = %u};\n"
              % (l0 + k, i, i, 16 if ins["side"] else 0, ins["side"], ins["dst"], ins["mul"]))
    if loads:
        w("    cpu->load_count = %u;\n" % (l0 + len(loads)))
    for i in branches:
        w("    queue_branch(cpu, start + 6, t%u);\n" % i)
    for i, ins in enumerate(n.insns):
        lop = ins["lop"]
        en = (n.outcome >> i) & 1 if (n.pmask >> i) & 1 else 1
        if lop == LOP_MEM and en and ins["mode"] & 8:
            w("    cpu->r[%u][%u] = m%u.updated;\n" % (ins["bank"], ins["b"], i))
        elif lop == LOP_CDPP:
            w("    cpu->r[1][15] = m%u.updated;\n" % i)
        elif lop == LOP_COMPACT:
            w("    if (cr%u) cpu->r[%u][%u] = cv%u;\n" % (i, ins["cside"], ins["cdst"], i))
        elif lop >= LOP_INLINE:
            w("    %scpu->r[%u][%u] = v%u;\n" % ("if (e%u) " % i if ins["creg"] else "", ins["side"], ins["dst"], i))
    w("    cpu->pc = %#xu;\n" % n.next)
    emit_retire(w, n, "stail", "ltail", ["d%u" % k for k in range(e3)])
    w("    cpu->cycles = start + %u;\n    ++cpu->packets;\n" % n.last)
    emit_tail(w, n)
    if fallible:
        w("broke%u:\n    stop(cpu, cpu->pc, 0, broke);\n    return AOT_FAULT;\n" % n.id)
    w("}\n")
    return True


def emit_retire(w, n, stail, ltail, data):
    """The retirement program (lean's P_ codes), with its tails and E3
    values in the given names."""
    prog, i = n.prog, n.bus_len
    retire_len = len(prog) - n.bus_len
    declared = set()
    reads = iter(data)
    while retire_len > 1 and i < len(prog):
        o = prog[i]
        a = prog[i + 1] if i + 1 < len(prog) else 0
        b = prog[i + 2] if i + 2 < len(prog) else 0
        if o == P_STAIL:
            decl = "" if stail in declared or "." in stail else "CdjC674xStore "
            declared.add(stail)
            w("    %s%s = cpu->stores[%u];\n" % (decl, stail, a)); i += 2
        elif o == P_SMOVE:
            w("    cpu->stores[%u] = cpu->stores[%u];\n" % (a, b)); i += 3
        elif o == P_SFILL:
            for k in range(a, b):
                w("    cpu->stores[%u] = %s;\n" % (k, stail))
            i += 3
        elif o == P_SCOUNT:
            w("    cpu->store_count = %u;\n" % a); i += 2
        elif o == P_LTAIL:
            decl = "" if ltail in declared or "." in ltail else "CdjC674xLoad "
            declared.add(ltail)
            w("    %s%s = cpu->loads[%u];\n" % (decl, ltail, a)); i += 2
        elif o == P_LMOVE:
            w("    cpu->loads[%u] = cpu->loads[%u];\n" % (a, b)); i += 3
        elif o == P_LFILL:
            for k in range(a, b):
                w("    cpu->loads[%u] = %s;\n" % (k, ltail))
            i += 3
        elif o == P_LCOUNT:
            w("    cpu->load_count = %u;\n" % a); i += 2
        elif o == P_LE3:
            w("    cpu->loads[%u].value = %s;\n" % (a, next(reads))); i += 2
        elif o == P_LRET:
            w("    lean_lret(cpu, %u);\n" % a); i += 2
        elif o == P_BRANCH:
            w("    lean_branch(cpu);\n"); i += 1
        else:
            break


DIRECT = True


def emit_node(w, n):
    if DIRECT and emit_direct(w, n):
        return
    br, gen, ctl = n.br_plan, n.generic, n.ctl
    land = n.ins[3]
    w("n%u: { /* pc %#x, %u runs */\n" % (n.id, n.pc, n.runs))
    w("    DtsLean L CDJ_C674X_UNINITIALIZED;\n")
    w("    lean_begin(cpu, &L, %u, %u, %u);\n" % (br, gen, ctl))
    conds = []
    for i, ins in enumerate(n.insns):
        enabled = (n.outcome >> i) & 1 if (n.pmask >> i) & 1 else 1
        op = ", ".join(".%s = %u" % (c, ins[f]) for f, c in OP_FIELDS)
        conds.append("lean_issue(cpu, e, %u, %u, &(const JitOp){%s}, %u, &L, rd, wr, op)"
                     % (i, ins["lop"], op, enabled))
    conds.append("lean_check(cpu, %u, %u, &aot_v%u, %#xu, %#xu, &L)" % (br, gen, n.id, land[0], land[1]))
    w("    if (!(" + " &&\n          ".join(conds) + ") || aot_stress()) {\n")
    w("        lean_decline(cpu, &L, %u, %u);\n" % (br, ctl))
    w("        return x->packets ? AOT_REDO : AOT_DECLINED;\n    }\n")
    prog, i = n.prog, 0
    bus = []
    while i < n.bus_len:
        o = prog[i]
        if o == P_TICK:
            bus.append("lean_tick(cpu, %u);" % prog[i + 1])
            i += 2
        elif o == P_COMMIT:
            bus.append("if ((broke = lean_commit(cpu, %u, wr, op))) goto broke%u;" % (prog[i + 1], n.id))
            i += 2
        elif o == P_E3:
            bus.append("if ((broke = lean_e3(cpu, %u, &L, rd, op))) goto broke%u;" % (prog[i + 1], n.id))
            i += 2
        else:
            break
    fallible = any("broke" in s for s in bus)
    if fallible:
        w("    const char *broke;\n")
    for s in bus:
        w("    %s\n" % s)
    w("    lean_apply(cpu, &L, %#xu);\n" % n.next)
    emit_retire(w, n, "L.stail", "L.ltail", ["L.data[L.reads++]"] * 40)
    w("    lean_end(cpu, &L, %u);\n" % n.last)
    emit_tail(w, n)
    if fallible:
        w("broke%u:\n    lean_broke(cpu, &L, %u, %u, broke);\n    return AOT_FAULT;\n" % (n.id, br, ctl))
    w("}\n")


def emit_tail(w, n):
    """dt_run's post-packet steps, then the successors."""
    w("    ++x->packets;\n    if (++x->n == x->limit) return AOT_LIMIT;\n")
    w("    if (!horizon_skip(cpu)) return AOT_BETWEEN;\n")
    if n.edges:
        w("    if (cpu->loop_active || cpu->idle_cycles || cpu->fault) return AOT_CONT;\n")
        w("    const uint32_t pc = cpu->pc;\n")
        bypc = collections.OrderedDict()
        for s in n.edges:
            bypc.setdefault((s.pc, s.packet), []).append(s)
        for (pc, packet), ss in bypc.items():
            w("    if (pc == %#xu) {\n" % pc)
            w("        CdjC674xCacheEntry *f = &x->cache[packet_cache_index(%#xu)];\n" % pc)
            w("        if (f->blocks && f->pc == pc && f->aot == &aot_packets[%u] && f->dt > 0 &&\n"
              "            aot_current(f, op)) {\n" % packet)
            w("            const uint8_t key = aot_key(cpu, &aot_packets[%u]);\n" % packet)
            for s in ss:
                if s.group == n.group:
                    w("            if (key == %u) { e = f; goto n%u; }\n" % (s.outcome, s.id))
                else:
                    w("            if (key == %u) AOT_TAIL return aot_g%u(cpu, %u, f, x);\n"
                      % (s.outcome, s.group, s.local))
            w("        }\n    }\n")
    if n.out_id is not None:
        w("    AOT_TAIL return aot_next(cpu, %u, e, x);\n" % n.out_id)
    else:
        w("    return AOT_CONT;\n")


def emit_kernels(out, min_steady):
    ks = [(k, n) for k, n in KERNELS.items() if n >= min_steady]
    w = out.write
    w("\n/* Steady kernels: %u. */\n#define CDJ_C674X_AOT_KERNELS 1\n" % len(ks))
    if not ks:
        w("static const AotKernel aot_kernels[1];\nstatic const unsigned aot_nkernels = 0;\n")
        return
    w("_Static_assert(sizeof(JitOp) == %d && sizeof(JitPhase) == %d,"
      " \"profile from another core\");\n" % (KJ["jk_op"], KJ["jk_phase"]))
    w("#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored \"-Wmissing-braces\"\n")
    osz, psz = KJ["jk_op"], KJ["jk_phase"]
    for i, ((ii, ops, op, lat, ph), n) in enumerate(ks):
        ob, lb, pb = bytes.fromhex(op), bytes.fromhex(lat), bytes.fromhex(ph)
        assert len(ob) == ops * osz and len(lb) == ops and len(pb) == ii * psz
        def arr(b):
            return ",".join(str(x) for x in b)
        w("/* %d steady cycles profiled */\n" % n)
        w("static const JitKernel aot_kernel_%d = {.state = 1, .ops = %d,\n" % (i, ops))
        w("  .op = {%s},\n" % ",".join("{%s}" % arr(ob[j * osz:(j + 1) * osz]) for j in range(ops)))
        w("  .lat = {%s},\n" % arr(lb))
        w("  .phase = {%s}};\n" % ",".join("{%s}" % arr(pb[j * psz:(j + 1) * psz]) for j in range(ii)))
        w("static int aot_kx_%d(CdjC674x *cpu, unsigned p, uint64_t c, CdjC674xRead read,\n"
          "                     CdjC674xWrite write, void *opaque)\n{\n    switch (p) {\n" % i)
        for p in range(ii):
            w("    case %d: return jk_exec_t(cpu, &aot_kernel_%d, %d, c, read, write, opaque);\n" % (p, i, p))
        w("    }\n    __builtin_unreachable();\n}\n")
        w("static unsigned aot_kb_%d(CdjC674x *cpu, JitLoop *l, unsigned max, CdjC674xRead read,\n"
          "                         CdjC674xWrite write, void *opaque, bool *between_due,\n"
          "                         bool *fault)\n{\n"
          "    return jk_burst_t(cpu, l, max, read, write, opaque, between_due, fault,\n"
          "                      aot_kx_%d);\n}\n" % (i, i))
    w("#pragma GCC diagnostic pop\n")
    w("static const AotKernel aot_kernels[] = {\n")
    for i, ((ii, *_), n) in enumerate(ks):
        w("    {&aot_kernel_%d, %d, aot_kb_%d},\n" % (i, ii, i))
    w("};\nstatic const unsigned aot_nkernels = %d;\n" % len(ks))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("profiles", nargs="+")
    ap.add_argument("--min", type=int, default=64, help="runs a node needs")
    ap.add_argument("--group", type=int, default=256, help="nodes per C function")
    ap.add_argument("--no-direct", action="store_true", help="the lean form only")
    ap.add_argument("--kernel-min", type=int, default=10000,
                    help="steady cycles a kernel needs")
    a = ap.parse_args()
    global DIRECT
    DIRECT = not a.no_direct
    nodes = [n for n in parse(a.profiles) if n.runs >= a.min and n.ins is not None]
    globals().update({name.upper(): value for name, value in K.items()})
    with open(a.out, "w") as f:
        emit(nodes, f, a.group)
        if KJ:
            emit_kernels(f, a.kernel_min)
    print("%s: %u nodes, %u kernels" % (a.out, len(nodes),
          sum(1 for n in KERNELS.values() if n >= a.kernel_min)), file=sys.stderr)


if __name__ == "__main__":
    main()
