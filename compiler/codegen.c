/* =========================================================================
 *  TOPIC 3 · Compiling Complex Variables and Functions
 * FILE: codegen.c   —   Phase 6 — MIPS code generation
 * -------------------------------------------------------------------------
 * THE PIPELINE, AND WHERE THIS FILE SITS IN IT
 *   scanner -> parser -> ast -> semantic -> tac -> codegen
 *                                                  ^^^^^^^  this file
 *
 * WHAT IS NEW IN TOPIC 3
 *   • Real activation records: prologue, epilogue, saved $ra
 *   • Globals emitted into .data; locals addressed off $sp
 *   • Arguments passed in $a0-$a3; arrays passed by reference
 *
 * WHAT COMES NEXT
 *   Topic 4 adds loops (while, for, break) and the label/jump machinery they need.
 *
 * YOUR TASK
 *   Everything below marked  TODO (Topic 3)  is yours to write.
 *   Everything else already works — it is the Topic 2 compiler you
 *   have already built and tested.  Do not rewrite it; extend it.
 * ========================================================================= */

/* ============================================================================
 * PHASE 6 — MIPS CODE GENERATION
 * ----------------------------------------------------------------------------
 * Reads the optimized three-address code and writes MIPS assembly.
 *
 * The structure of this file mirrors the two problems named in codegen.h:
 *
 *   PART 1  Register cache   — allocate, spill, reload  ($t0-$t9)
 *   PART 2  Addressing       — turn a name into an address (global / local /
 *                              array element / array passed by reference)
 *   PART 3  Frame layout     — measure a function, emit prologue and epilogue
 *   PART 4  Instruction emit — one case per TAC opcode
 *
 * Read it in that order.  Nothing in PART 4 is complicated once PARTS 1-3
 * are understood; that is the whole point of separating them.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codegen.h"
#include "symtab.h"
#include "trace.h"

static FILE* out;                 /* The .s file being written              */
RegisterAllocator regAlloc;       /* The $t0-$t9 cache                      */

/* Registers the CURRENT TAC instruction is using.  The allocator must never
 * evict one of these to make room for another operand of the same instruction:
 * with all ten registers busy, loading the second operand of `a + b` could
 * otherwise reuse the register that holds `a`, and the add would silently
 * compute b + b.  The mask is cleared at the start of every instruction. */
static unsigned pinnedMask = 0;

/* CALL SUPPORT.  An ARG instruction only RECORDS which name is being passed;
 * the CALL that follows says how many of the most recent ARGs belong to it
 * (see generateArgTAC in tac.c), which is what makes f(1, g(2)) work. */
#define MAX_PENDING_ARGS 64
static char* pendingArgs[MAX_PENDING_ARGS];
static int   pendingCount = 0;

static int frameSize  = 0;        /* Size of the frame being generated       */
static const char* currentFunc = NULL;

/* ==========================================================================
 * NAME MANGLING
 * --------------------------------------------------------------------------
 * Source identifiers and assembler labels share one namespace, and the
 * assembler's namespace already contains every instruction mnemonic.  A user
 * who writes
 *         int add(int a, int b) { return a + b; }
 * would otherwise make us emit the label `add:` — and SPIM would reject the
 * file, pointing at a line the programmer never wrote.
 *
 * The fix every real compiler uses is to keep the two namespaces apart by
 * decorating names on the way out:
 *
 *      function add   ->   fn_add          global counter  ->  g_counter
 *      function main  ->   main            (the entry point must keep its name)
 *
 * Two static buffers are enough because no call site needs more than two
 * mangled names alive at once.
 * ========================================================================*/
static const char* funcLabel(const char* name) {
    static char buf[2][MAX_VAR_NAME + 8];
    static int which = 0;
    if (strcmp(name, "main") == 0) return "main";   /* the entry point */
    which ^= 1;
    snprintf(buf[which], sizeof buf[which], "fn_%s", name);
    return buf[which];
}

static const char* dataLabel(const char* name) {
    static char buf[2][MAX_VAR_NAME + 8];
    static int which = 0;
    which ^= 1;
    snprintf(buf[which], sizeof buf[which], "g_%s", name);
    return buf[which];
}

/* ==========================================================================
 * Small predicates used throughout
 * ========================================================================*/

/* Is this TAC operand a literal integer rather than a name? */
static int isConstant(const char* s) {
    if (!s || !*s) return 0;
    int i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (!s[i]) return 0;
    for (; s[i]; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* Is this a compiler-generated temporary (t0, t1, ...)?  Temporaries are
 * reserved identifiers — semantic.c rejects user variables of this shape —
 * so the test is safe. */
static int isTemp(const char* s) {
    if (!s || s[0] != 't' || !s[1]) return 0;
    for (int i = 1; s[i]; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* ==========================================================================
 * PART 1 — REGISTER CACHE
 * --------------------------------------------------------------------------
 * Ten registers stand in for an unbounded set of TAC names.  The rules:
 *
 *   ensureReg(name)  the register must hold the CURRENT value of `name`
 *                    (load it from memory if it is not already cached)
 *   defReg(name)     the register is about to be OVERWRITTEN with a new
 *                    value of `name` (no load needed; mark it dirty)
 *   scratchReg()     an anonymous register for address arithmetic
 *
 * When no register is free the least-recently-used one is evicted.  Eviction
 * writes the value back first if it is dirty AND the name has a memory home.
 * Literals and scratch values have no home, so evicting them is free.
 * ========================================================================*/

static void emitStoreHome(int reg, const char* name);
static void emitLoadHome(int reg, const char* name);

void initRegAlloc(void) {
    for (int i = 0; i < NUM_TEMP_REGS; i++) {
        regAlloc.regs[i].varName[0] = '\0';
        regAlloc.regs[i].isDirty    = 0;
        regAlloc.regs[i].inUse      = 0;
        regAlloc.regs[i].hasHome    = 0;
        regAlloc.regs[i].lastUsed   = 0;
    }
    regAlloc.timestamp  = 0;
    pinnedMask          = 0;
    regAlloc.spillCount = 0;
    regAlloc.loadCount  = 0;
}

/* Which register currently caches `name`?  -1 if none. */
static int findVarReg(const char* name) {
    for (int i = 0; i < NUM_TEMP_REGS; i++) {
        if (regAlloc.regs[i].inUse && strcmp(regAlloc.regs[i].varName, name) == 0) {
            regAlloc.regs[i].lastUsed = ++regAlloc.timestamp;
            pinnedMask |= 1u << i;
            return i;
        }
    }
    return -1;
}

/* Least-recently-used register — the one whose value we can most afford
 * to lose.  A real compiler would use liveness information here; LRU is a
 * good approximation and is easy to reason about in class. */
static int selectVictimReg(void) {
    int victim = -1, oldest = 0;
    for (int i = 0; i < NUM_TEMP_REGS; i++) {
        if (pinnedMask & (1u << i)) continue;     /* in use by this instruction */
        /* Values with no memory home are the cheapest to discard */
        if (!regAlloc.regs[i].hasHome && regAlloc.regs[i].inUse) return i;
        if (victim < 0 || regAlloc.regs[i].lastUsed < oldest) {
            oldest = regAlloc.regs[i].lastUsed;
            victim = i;
        }
    }
    if (victim < 0) {
        fprintf(stderr, "codegen: out of registers in function '%s' "
                        "(internal error)\n", currentFunc ? currentFunc : "?");
        exit(1);
    }
    return victim;
}

/* Write a register back to its memory home and mark it clean. */
static void spillReg(int r) {
    if (regAlloc.regs[r].inUse && regAlloc.regs[r].isDirty && regAlloc.regs[r].hasHome) {
        fprintf(out, "    # spill: %s -> memory\n", regAlloc.regs[r].varName);
        emitStoreHome(r, regAlloc.regs[r].varName);
        regAlloc.spillCount++;
    }
    regAlloc.regs[r].isDirty = 0;
}

/* Grab a register for `name`.  `load` says whether the register must be
 * primed with the value already in memory. */
static int getReg(const char* name, int hasHome, int load) {
    int r = findVarReg(name);
    if (r != -1) return r;                       /* Already cached          */

    for (r = 0; r < NUM_TEMP_REGS; r++)          /* A free register?        */
        if (!regAlloc.regs[r].inUse) break;

    if (r == NUM_TEMP_REGS) {                    /* None free — evict LRU   */
        r = selectVictimReg();
        spillReg(r);
    }

    strncpy(regAlloc.regs[r].varName, name, MAX_VAR_NAME - 1);
    regAlloc.regs[r].varName[MAX_VAR_NAME - 1] = '\0';
    regAlloc.regs[r].inUse    = 1;
    regAlloc.regs[r].isDirty  = 0;
    regAlloc.regs[r].hasHome  = hasHome;
    regAlloc.regs[r].lastUsed = ++regAlloc.timestamp;
    pinnedMask |= 1u << r;

    if (load) {
        emitLoadHome(r, name);
        regAlloc.loadCount++;
    }
    return r;
}

/* Register holding the current value of `name` (loading it if necessary). */
static int ensureReg(const char* name) { return getReg(name, 1, 1); }

/* Register that will RECEIVE a new value for `name`. */
static int defReg(const char* name) {
    int r = getReg(name, 1, 0);
    regAlloc.regs[r].isDirty = 1;
    return r;
}

/* Forget what a register holds (no write-back: callers spill first if needed). */
static void releaseReg(int r) {
    if (r < 0) return;
    regAlloc.regs[r].inUse      = 0;
    regAlloc.regs[r].isDirty    = 0;
    regAlloc.regs[r].hasHome    = 0;
    regAlloc.regs[r].varName[0] = '\0';
}

/* Materialise any TAC operand — literal or name — into a register. */
static int operandReg(const char* s) {
    if (isConstant(s)) {
        char n[MAX_VAR_NAME];
        snprintf(n, sizeof n, "#k%s", s);
        int r = findVarReg(n);
        if (r != -1) return r;
        r = getReg(n, 0, 0);
        fprintf(out, "    li   $t%d, %s\n", r, s);
        return r;
    }
    return ensureReg(s);
}

/* Write every dirty register back to memory, then forget all cached values.
 * Called before `jal` (the callee owns $t0-$t9) and before any label, since
 * control may reach a label from somewhere with a different cache state. */
static void flushRegisters(const char* why) {
    int any = 0;
    for (int i = 0; i < NUM_TEMP_REGS; i++)
        if (regAlloc.regs[i].inUse && regAlloc.regs[i].isDirty && regAlloc.regs[i].hasHome) any = 1;
    if (any) fprintf(out, "    # write back live values (%s)\n", why);
    for (int i = 0; i < NUM_TEMP_REGS; i++) {
        spillReg(i);
        releaseReg(i);
    }
}

void printRegAllocStats(void) {
    printf("  Register spills (store to memory) : %d\n", regAlloc.spillCount);
    printf("  Register reloads (load from memory): %d\n", regAlloc.loadCount);
}

/* ==========================================================================
 * PART 2 — ADDRESSING
 * --------------------------------------------------------------------------
 * Turning a name into an address is the whole difference between a global,
 * a local, and an array parameter.  All three cases live here so that the
 * instruction cases in PART 4 never have to think about it.
 * ========================================================================*/

static void emitLoadHome(int reg, const char* name) {
    Symbol* s = lookupSymbol(name);
    if (!s) {
        fprintf(stderr, "codegen: '%s' has no storage assigned "
                        "(internal error in function '%s')\n",
                name, currentFunc ? currentFunc : "?");
        exit(1);
    }
    if (s->isGlobal) fprintf(out, "    lw   $t%d, %s\n", reg, dataLabel(s->name));
    else             fprintf(out, "    lw   $t%d, %d($sp)        # %s\n", reg, s->offset, s->name);
}

static void emitStoreHome(int reg, const char* name) {
    Symbol* s = lookupSymbol(name);
    if (!s) {
        fprintf(stderr, "codegen: '%s' has no storage assigned "
                        "(internal error in function '%s')\n",
                name, currentFunc ? currentFunc : "?");
        exit(1);
    }
    if (s->isGlobal) fprintf(out, "    sw   $t%d, %s\n", reg, dataLabel(s->name));
    else             fprintf(out, "    sw   $t%d, %d($sp)        # %s\n", reg, s->offset, s->name);
}

/* ARRAY ADDRESSING.  Three cases, and telling them apart IS the lesson:
 *   global array      la   $r, g_name          address of a .data label
 *   local array       addi $r, $sp, offset     the elements are in the frame
 *   array parameter   lw   $r, offset($sp)     the slot holds someone
 *                                              ELSE'S base address
 * Then element address = base + index*4  (sll by 2, then add).
 *
 * `reg` is the text of a machine register ("$v1", "$a0"): the base address is
 * produced directly in it, so no allocator register is spent on it. */
static void emitArrayBase(const char* reg, const char* name) {
    Symbol* s = lookupSymbol(name);
    if (!s || !s->isArray) {
        fprintf(stderr, "codegen: '%s' is not an array "
                        "(internal error in function '%s')\n",
                name, currentFunc ? currentFunc : "?");
        exit(1);
    }
    if (s->isGlobal)
        fprintf(out, "    la   %s, %s              # &%s[0]  (.data)\n", reg, dataLabel(s->name), s->name);
    else if (s->isParamArray)
        fprintf(out, "    lw   %s, %d($sp)        # %s: caller's base address\n", reg, s->offset, s->name);
    else
        fprintf(out, "    addi %s, $sp, %d        # &%s[0]  (this frame)\n", reg, s->offset, s->name);
}

/* An anonymous register for address arithmetic (no memory home). */
static int scratchReg(void) {
    static int serial = 0;
    char nm[32];
    snprintf(nm, sizeof nm, "#addr%d", serial++);
    return getReg(nm, 0, 0);
}

/* Register holding the ADDRESS of arr[index].  The caller does one lw or sw
 * through it and then releases it.  $v1 carries the base address while the
 * index is scaled; it is never used for anything else. */
static int elementAddrReg(const char* arr, const char* index) {
    int idx  = operandReg(index);
    int addr = scratchReg();
    emitArrayBase("$v1", arr);
    fprintf(out, "    sll  $t%d, $t%d, 2         # index * 4\n", addr, idx);
    fprintf(out, "    add  $t%d, $t%d, $v1       # + base of %s\n", addr, addr, arr);
    return addr;
}

/* ==========================================================================
 * PART 3 — FRAME LAYOUT
 * --------------------------------------------------------------------------
 * Before emitting a single instruction of a function we walk its TAC once and
 * hand a memory home to every name it mentions.  Doing this up front is what
 * lets the register allocator spill anything at any moment.
 * ========================================================================*/

/* Give `name` a home if it does not have one yet (used for temporaries). */
static void reserveTemp(const char* name) {
    if (!name || isConstant(name)) return;
    if (!isTemp(name)) return;              /* declared names get homes from
                                             * their DECL/PARAM instruction */
    if (lookupSymbol(name)) return;
    addVar((char*)name, "int");
}

/* Walk one function's TAC (FUNC_BEGIN .. FUNC_END), populate the local symbol
 * table, and return the total frame size in bytes. */
static int layoutFrame(TACInstr* funcBegin) {
    initSymTab();

    for (TACInstr* c = funcBegin->next; c && c->op != TAC_FUNC_END; c = c->next) {
        switch (c->op) {
            case TAC_PARAM:
                /* arg1 carries "int[]" for arrays passed by reference */
                if (c->arg1 && strcmp(c->arg1, "int[]") == 0) addArrayParam(c->result);
                else                                          addVar(c->result, "int");
                break;
            case TAC_DECL:
                addVar(c->result, c->arg1 ? c->arg1 : "int");
                break;
            case TAC_ARRAY_DECL:
                addArray(c->result, atoi(c->arg1));
                break;
            default: break;
        }
        /* Any temporary mentioned anywhere needs a home too */
        reserveTemp(c->result);
        reserveTemp(c->arg1);
        reserveTemp(c->arg2);
    }

    /* locals + saved $ra + alignment padding, rounded up to a multiple of 8 */
    int size = getLocalBytes() + 8;
    if (size % 8) size += 8 - (size % 8);
    return size;
}

static void emitPrologue(const char* name, int size) {
    fprintf(out, "\n# ==== function %s ====\n", name);
    fprintf(out, "%s:\n", funcLabel(name));
    fprintf(out, "    addi $sp, $sp, -%d        # build activation record\n", size);
    fprintf(out, "    sw   $ra, %d($sp)        # save return address\n", size - 4);
}

static void emitEpilogue(const char* name, int size) {
    fprintf(out, "%s__epilogue:\n", funcLabel(name));
    fprintf(out, "    lw   $ra, %d($sp)        # restore return address\n", size - 4);
    fprintf(out, "    addi $sp, $sp, %d        # discard activation record\n", size);
    if (strcmp(name, "main") == 0) {
        fprintf(out, "    li   $v0, 10             # syscall 10 = exit\n");
        fprintf(out, "    syscall\n");
    } else {
        fprintf(out, "    jr   $ra                 # return to caller\n");
    }
}

/* ==========================================================================
 * PART 4 — INSTRUCTION SELECTION
 * ========================================================================*/

/* MIPS mnemonic for each arithmetic / relational / logical TAC opcode.
 * SPIM accepts the three-operand pseudo-instructions used here (mul, div,
 * slt, sgt, sle, sge, seq, sne), expanding them to real instructions. */
static const char* mnemonicFor(TACOp op) {
    switch (op) {
        case TAC_ADD: return "add";
        case TAC_SUB: return "sub";
        case TAC_MUL: return "mul";
        case TAC_DIV: return "div";
        default:      return NULL;
    }
}

/* Emit the .data section: one entry per global declaration. */
static void emitDataSection(TACInstr* head) {
    initGlobalScope();
    fprintf(out, "# ============ Generated MIPS ============\n");
    fprintf(out, ".data\n");
    /* The newline string is emitted first and everything after it is word
     * aligned explicitly: .asciiz leaves the location counter on an odd byte,
     * and `lw`/`sw` on a misaligned address raises an address-error exception
     * in SPIM.  Forgetting .align here is a classic first bug. */
    fprintf(out, "__nl: .asciiz \"\\n\"\n");

    int inFunction = 0;
    for (TACInstr* c = head; c; c = c->next) {
        if (c->op == TAC_FUNC_BEGIN) { inFunction = 1; continue; }
        if (c->op == TAC_FUNC_END)   { inFunction = 0; continue; }
        if (inFunction) continue;                 /* only file-scope declarations */

        if (c->op == TAC_DECL) {
            addGlobalVar(c->result, c->arg1 ? c->arg1 : "int");
            fprintf(out, ".align 2\n%s: .word 0            # int %s\n",
                    dataLabel(c->result), c->result);
        } else if (c->op == TAC_ARRAY_DECL) {
            int n = atoi(c->arg1);
            addGlobalArray(c->result, n);
            fprintf(out, ".align 2\n%s: .space %d          # int %s[%d]\n",
                    dataLabel(c->result), n * 4, c->result, n);
        }
    }
    fprintf(out, "\n.text\n.globl main\n");
}

void generateMIPSFromTAC(const char* filename) {
    out = fopen(filename, "w");
    if (!out) { fprintf(stderr, "Cannot open output file %s\n", filename); exit(1); }

    initRegAlloc();

    TACList* tac = getOptimizedTAC();
    if (!tac || !tac->head) {
        fprintf(stderr, "Error: no TAC to translate\n");
        fclose(out);
        return;
    }

    /* Pass 1: globals -> .data */
    emitDataSection(tac->head);

    /* Pass 2: one function at a time */
    for (TACInstr* c = tac->head; c; c = c->next) {

        if (c->op != TAC_FUNC_BEGIN) continue;   /* file-scope decls already done */

        currentFunc = c->result;
        frameSize   = layoutFrame(c);
        trace("\n  Activation record for '%s': %d bytes\n", currentFunc, frameSize);
        printSymTab();
        initRegAlloc();
        pendingCount = 0;
        int paramIndex = 0;                      /* which $a register is next */

        emitPrologue(currentFunc, frameSize);

        for (TACInstr* i = c->next; i && i->op != TAC_FUNC_END; i = i->next) {
            pinnedMask = 0;                      /* a fresh instruction pins nothing */
            switch (i->op) {

            /* ---------- storage declarations: no code, just a comment ----- */
            case TAC_DECL: {
                Symbol* s = lookupSymbol(i->result);
                fprintf(out, "    # int %s  ->  %d($sp)\n", i->result, s ? s->offset : -1);
                break;
            }
            case TAC_ARRAY_DECL: {
                Symbol* s = lookupSymbol(i->result);
                fprintf(out, "    # int %s[%s]  ->  %d($sp)\n",
                        i->result, i->arg1, s ? s->offset : -1);
                break;
            }

            /* ---------- parameters arrive in $a0-$a3 --------------------- */
            /* The prologue's job is to copy each one into its frame slot,
             * because $a0-$a3 will be needed again the moment this function
             * calls something else.  An array parameter's slot receives the
             * caller's base ADDRESS. */
            case TAC_PARAM: {
                Symbol* s = lookupSymbol(i->result);
                if (paramIndex >= MAX_ARGS) {
                    fprintf(stderr, "codegen: function '%s' has more than %d parameters\n",
                            currentFunc, MAX_ARGS);
                    exit(1);
                }
                fprintf(out, "    sw   $a%d, %d($sp)        # parameter %s%s\n",
                        paramIndex, s ? s->offset : -1, i->result,
                        (s && s->isParamArray) ? " (array address)" : "");
                paramIndex++;
                break;
            }
            /* ---------- arithmetic, comparison ---------------------------- */
            case TAC_ADD: case TAC_SUB: case TAC_MUL: case TAC_DIV:
            {
                int a = operandReg(i->arg1);
                int b = operandReg(i->arg2);
                int d = defReg(i->result);
                fprintf(out, "    %-4s $t%d, $t%d, $t%d       # %s = %s %s %s\n",
                        mnemonicFor(i->op), d, a, b,
                        i->result, i->arg1, mnemonicFor(i->op), i->arg2);
                break;
            }

            case TAC_NEG: {
                int a = operandReg(i->arg1);
                int d = defReg(i->result);
                fprintf(out, "    sub  $t%d, $zero, $t%d     # %s = -%s\n", d, a, i->result, i->arg1);
                break;
            }


            /* ---------- assignment --------------------------------------- */
            case TAC_ASSIGN: {
                int a = operandReg(i->arg1);
                int d = defReg(i->result);
                if (a != d) fprintf(out, "    move $t%d, $t%d            # %s = %s\n", d, a, i->result, i->arg1);
                break;
            }

            /* ---------- print: syscall 1 (int) then syscall 4 (newline) --- */
            case TAC_PRINT: {
                int a = operandReg(i->arg1);
                fprintf(out, "    move $a0, $t%d            # print %s\n", a, i->arg1);
                fprintf(out, "    li   $v0, 1\n    syscall\n");
                fprintf(out, "    la   $a0, __nl\n    li   $v0, 4\n    syscall\n");
                break;
            }


            /* ---------- CALLS AND RETURNS: the calling convention --------- */
            /* ARG only records the name; the CALL that follows says how many of
             * the most recent ARGs are its own. */
            case TAC_ARG:
                if (pendingCount >= MAX_PENDING_ARGS) {
                    fprintf(stderr, "codegen: too many pending arguments\n");
                    exit(1);
                }
                pendingArgs[pendingCount++] = strdup(i->arg1);
                break;

            /* 1. put the arguments in $a0-$a3 while the $t registers are valid
             * 2. THEN flush every dirty register to memory ($t0-$t9 are
             *    caller-saved: the callee may clobber all of them)
             * 3. jal to the function label
             * 4. the result comes back in $v0
             * Steps 1 and 2 in the wrong order is the classic bug. */
            case TAC_CALL: {
                int n = atoi(i->arg2);
                if (n > MAX_ARGS) {
                    fprintf(stderr, "codegen: call to '%s' passes %d arguments; the limit is %d\n",
                            i->arg1, n, MAX_ARGS);
                    exit(1);
                }
                if (n > pendingCount) {
                    fprintf(stderr, "codegen: CALL %s expects %d arguments but only %d were passed "
                                    "(internal error)\n", i->arg1, n, pendingCount);
                    exit(1);
                }
                int first = pendingCount - n;
                for (int k = 0; k < n; k++) {
                    const char* a = pendingArgs[first + k];
                    char areg[8];
                    snprintf(areg, sizeof areg, "$a%d", k);
                    pinnedMask = 0;
                    Symbol* s = isConstant(a) ? NULL : lookupSymbol(a);
                    if (isConstant(a)) {
                        fprintf(out, "    li   %s, %s              # argument %d\n", areg, a, k + 1);
                    } else if (s && s->isArray) {
                        emitArrayBase(areg, a);              /* arrays go by ADDRESS */
                    } else {
                        int r = operandReg(a);
                        fprintf(out, "    move %s, $t%d            # argument %d = %s\n",
                                areg, r, k + 1, a);
                    }
                }
                for (int k = first; k < pendingCount; k++) free(pendingArgs[k]);
                pendingCount = first;

                flushRegisters("before call");
                fprintf(out, "    jal  %s\n", funcLabel(i->arg1));

                pinnedMask = 0;
                if (i->result) {
                    int d = defReg(i->result);
                    fprintf(out, "    move $t%d, $v0            # %s = result of %s\n",
                            d, i->result, i->arg1);
                }
                break;
            }

            /* Put the value in $v0, then JUMP TO THE EPILOGUE.  Do not just
             * fall through — a `return` in the middle of a function has to skip
             * everything after it, and the epilogue is the only place that
             * restores $ra and pops the frame.  Dirty registers are written
             * back first: a global assigned just before `return` must reach
             * memory. */
            case TAC_RETURN:
                if (!i->arg1) {
                    fprintf(out, "    li   $v0, 0              # bare return\n");
                } else if (isConstant(i->arg1)) {
                    fprintf(out, "    li   $v0, %s              # return %s\n", i->arg1, i->arg1);
                } else {
                    int r = operandReg(i->arg1);
                    fprintf(out, "    move $v0, $t%d            # return %s\n", r, i->arg1);
                }
                flushRegisters("before return");
                if (i->next && i->next->op != TAC_FUNC_END)
                    fprintf(out, "    j    %s__epilogue\n", funcLabel(currentFunc));
                break;

            /* ---------- arrays -------------------------------------------- */
            /* Compute the element address once with elementAddrReg, then a
             * single lw or sw.  ARRAY_STORE keeps the value being stored in
             * the `result` field:  arg1[arg2] = result */
            case TAC_ARRAY_LOAD: {
                int addr = elementAddrReg(i->arg1, i->arg2);
                int d    = defReg(i->result);
                fprintf(out, "    lw   $t%d, 0($t%d)        # %s = %s[%s]\n",
                        d, addr, i->result, i->arg1, i->arg2);
                releaseReg(addr);
                break;
            }

            case TAC_ARRAY_STORE: {
                int v    = operandReg(i->result);
                int addr = elementAddrReg(i->arg1, i->arg2);
                fprintf(out, "    sw   $t%d, 0($t%d)        # %s[%s] = %s\n",
                        v, addr, i->arg1, i->arg2, i->result);
                releaseReg(addr);
                break;
            }

            default: {
                /* Control flow and logic arrive in later topics.  Say so in
                 * the output rather than silently dropping the instruction. */
                char buf[160];
                formatTAC(i, buf, sizeof buf);
                fprintf(out, "    # (not generated in this milestone: %s)\n", buf);
                break;
            }
            }
        }

        flushRegisters("end of function body");
        emitEpilogue(currentFunc, frameSize);
    }

    fclose(out);
}

