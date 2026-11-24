/* =========================================================================
 *  TOPIC 3 · Compiling Complex Variables and Functions
 * FILE: tac.c   —   Phases 4 & 5 — Intermediate code and optimization
 * -------------------------------------------------------------------------
 * THE PIPELINE, AND WHERE THIS FILE SITS IN IT
 *   scanner -> parser -> ast -> semantic -> tac -> codegen
 *                                           ^^^  this file
 *
 * WHAT IS NEW IN TOPIC 3
 *   • FUNC_BEGIN / FUNC_END / PARAM / ARG / CALL / RETURN instructions
 *   • ARRAY_DECL / ARRAY_LOAD / ARRAY_STORE instructions
 *
 * WHAT COMES NEXT
 *   Topic 4 adds loops (while, for, break) and the label/jump machinery they need.
 *
 * YOUR TASK
 *   Everything below marked  TODO (Topic 3)  is yours to write.
 *   Everything else already works — it is the Topic 2 compiler you
 *   have already built and tested.  Do not rewrite it; extend it.
 * ========================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "tac.h"
#include "trace.h"

TACList tacList;
TACList optimizedList;
TempAllocator tempAlloc;

void initTAC() {
    tacList.head = NULL;
    tacList.tail = NULL;
    tacList.tempCount = 0;
    tacList.labelCount = 0;
    optimizedList.head = NULL;
    optimizedList.tail = NULL;

    /* Initialize temporary allocator */
    for (int i = 0; i < MAX_TEMPS; i++) {
        tempAlloc.allocated[i] = 0;
    }
    tempAlloc.maxUsed = 0;
    tempAlloc.freeCount = 0;
    trace("TAC: Temporary allocator initialized\n");
}

char* newTemp() {
    char* temp = malloc(10);
    sprintf(temp, "t%d", tacList.tempCount++);
    return temp;
}

char* allocTemp() {
    int tempNum;

    if (tempAlloc.freeCount > 0) {
        tempNum = tempAlloc.freeList[--tempAlloc.freeCount];
        trace("    [TAC ALLOC] Reusing temporary t%d\n", tempNum);
    } else {
        tempNum = tempAlloc.maxUsed++;
        if (tempNum >= MAX_TEMPS) {
            fprintf(stderr, "ERROR: Exceeded maximum temporaries\n");
            exit(1);
        }
        trace("    [TAC ALLOC] Allocating new temporary t%d\n", tempNum);
    }

    tempAlloc.allocated[tempNum] = 1;
    char* temp = malloc(10);
    sprintf(temp, "t%d", tempNum);
    return temp;
}

/* Is `s` exactly a compiler temporary: 't' followed only by digits?  The test
 * must be this strict.  A user variable such as `total` starts with 't' too,
 * and atoi("otal") is 0 -- so a loose test would "free" t0 every time `total`
 * was used, while t0 was still live. */
static int isTempName(const char* s) {
    if (!s || s[0] != 't' || !s[1]) return 0;
    for (int i = 1; s[i]; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

void freeTemp(char* temp) {
    if (!isTempName(temp)) return;

    int tempNum = atoi(temp + 1);
    if (tempNum >= MAX_TEMPS || !tempAlloc.allocated[tempNum]) return;

    tempAlloc.allocated[tempNum] = 0;
    if (tempAlloc.freeCount < MAX_TEMPS) {
        tempAlloc.freeList[tempAlloc.freeCount++] = tempNum;
        trace("    [TAC FREE] Released temporary %s\n", temp);
    }
}


void printTempAllocatorState() {
    trace("\n┌──────────────────────────────────────────────────────────┐\n");
    trace("│ TEMPORARY ALLOCATOR STATISTICS                           │\n");
    trace("├──────────────────────────────────────────────────────────┤\n");
    trace("│ Total temporaries used:      %3d                         │\n", tempAlloc.maxUsed);
    trace("│ Currently allocated:         %3d                         │\n",
           tempAlloc.maxUsed - tempAlloc.freeCount);
    trace("│ Available for reuse:         %3d                         │\n", tempAlloc.freeCount);
    trace("└──────────────────────────────────────────────────────────┘\n\n");
}

TACInstr* createTAC(TACOp op, char* arg1, char* arg2, char* result) {
    TACInstr* instr = malloc(sizeof(TACInstr));
    instr->op = op;
    instr->arg1 = arg1 ? strdup(arg1) : NULL;
    instr->arg2 = arg2 ? strdup(arg2) : NULL;
    instr->result = result ? strdup(result) : NULL;
    instr->next = NULL;
    return instr;
}

void appendTAC(TACInstr* instr) {
    if (!tacList.head) {
        tacList.head = tacList.tail = instr;
    } else {
        tacList.tail->next = instr;
        tacList.tail = instr;
    }
}

void appendOptimizedTAC(TACInstr* instr) {
    if (!optimizedList.head) {
        optimizedList.head = optimizedList.tail = instr;
    } else {
        optimizedList.tail->next = instr;
        optimizedList.tail = instr;
    }
}

/* ── BREAK LABEL STACK ──────────────────────────────────────────────────────
 * When entering a switch, for, or while, push the "end" label so that any
 * nested NODE_BREAK can emit GOTO to the right exit point.
 */

/* Forward declarations */
static void generateTACStmt(ASTNode* node);

/* ==========================================================================
 * NAME RESOLUTION FOR LOCALS
 * --------------------------------------------------------------------------
 * The semantic analyzer lets a block shadow an outer name:
 *         int x;  { int x; ... }
 * but the back end gives every NAME exactly one frame slot per function.  If
 * both x's reached it under the same name, the inner one would silently share
 * (and overwrite) the outer one's storage.  So the TAC generator gives every
 * local declaration a name that is unique inside its function:
 *
 *     first  `int x`        ->  x
 *     second `int x`        ->  x.1      (a '.' can never appear in source)
 *     a local that shadows a GLOBAL is renamed too, otherwise code that meant
 *     the global before the local's declaration would hit the local's slot.
 *
 * Uses of a name are resolved innermost-scope-first, which is the same rule
 * semantic.c applied when it checked the program.
 * ========================================================================*/
#define MAX_NAMES 256
typedef struct { char* orig; char* actual; } NameBinding;

static NameBinding bindings[MAX_NAMES];      /* visible local names (a stack) */
static int         bindingCount = 0;
static int         scopeMarks[64];           /* bindingCount at each scope entry */
static int         markDepth = 0;
static char*       declaredHere[MAX_NAMES];  /* every local name used in this function */
static int         declaredCount = 0;
static char*       globalNames[MAX_NAMES];   /* file-scope names */
static int         globalNameCount = 0;
static int         tacInFunction = 0;        /* 0 while emitting file-scope declarations */

static int inList(char** list, int n, const char* s) {
    for (int i = 0; i < n; i++) if (strcmp(list[i], s) == 0) return 1;
    return 0;
}

static void nameOverflow(void) {
    fprintf(stderr, "ERROR: too many distinct names in one function (limit %d)\n", MAX_NAMES);
    exit(1);
}

static void beginFunctionNames(void) {
    for (int i = 0; i < declaredCount; i++) free(declaredHere[i]);
    declaredCount = 0;
    bindingCount  = 0;
    markDepth     = 0;
}

static void pushNameScope(void) {
    if (markDepth >= 64) nameOverflow();
    scopeMarks[markDepth++] = bindingCount;
}

static void popNameScope(void) {
    if (markDepth > 0) bindingCount = scopeMarks[--markDepth];
}

/* The name a source identifier currently refers to. */
static const char* resolveName(const char* name) {
    for (int i = bindingCount - 1; i >= 0; i--)
        if (strcmp(bindings[i].orig, name) == 0) return bindings[i].actual;
    return name;                              /* a global (or already unique) */
}

/* Declare a local (or parameter) and return the unique name to emit. */
static char* declareLocalName(const char* orig, int isParam) {
    char buf[96];
    snprintf(buf, sizeof buf, "%s", orig);
    if (!isParam && (inList(declaredHere, declaredCount, orig) ||
                     inList(globalNames, globalNameCount, orig))) {
        for (int k = 1; ; k++) {
            snprintf(buf, sizeof buf, "%s.%d", orig, k);
            if (!inList(declaredHere, declaredCount, buf)) break;
        }
    }
    if (declaredCount >= MAX_NAMES || bindingCount >= MAX_NAMES) nameOverflow();
    declaredHere[declaredCount++] = strdup(buf);
    bindings[bindingCount].orig   = strdup(orig);
    bindings[bindingCount].actual = strdup(buf);
    bindingCount++;
    return strdup(buf);
}

/* Pre-pass: remember the names declared at file scope. */
static void collectGlobalNames(ASTNode* node) {
    if (!node) return;
    switch (node->type) {
        case NODE_STMT_LIST:
            collectGlobalNames(node->data.stmtlist.stmt);
            collectGlobalNames(node->data.stmtlist.next);
            break;
        case NODE_DECL:
            if (globalNameCount < MAX_NAMES) globalNames[globalNameCount++] = strdup(node->data.decl.name);
            break;
        case NODE_ARRAY_DECL:
            if (globalNameCount < MAX_NAMES) globalNames[globalNameCount++] = strdup(node->data.array_decl.name);
            break;
        default: break;
    }
}

/* Most arguments one call may carry through the TAC generator.  (The back end
 * accepts only four; semantic.c rejects anything beyond that earlier.) */
#define MAX_CALL_ARGS 32

/* generateArgTAC: walk the argument list, emit code for each argument
 * expression and then an ARG instruction for it, and return the count.
 * The count goes into the CALL instruction so the back end knows how
 * many of the buffered ARGs belong to this call — which is what makes
 * nested calls like f(1, g(2)) come out right:
 *
 *      ARG 1 ; ARG 2 ; t0 = CALL g, 1 ; ARG t0 ; t1 = CALL f, 2
 *
 * The CALL for g takes the LAST one ARG; the CALL for f takes the last two.
 *
 * The names holding each argument are returned in `names`.  Their temporaries
 * must stay allocated until the CALL has been emitted: freeing t0 early would
 * let the next argument reuse it, and f(a+b, c+d) would pass c+d twice. */
static int generateArgTAC(ASTNode* args, char* names[]) {
    int n = 0;
    for (ASTNode* a = args; a; a = a->data.arg_list.next) {
        if (n >= MAX_CALL_ARGS) {
            fprintf(stderr, "ERROR: call with more than %d arguments\n", MAX_CALL_ARGS);
            exit(1);
        }
        char* v = generateTACExpr(a->data.arg_list.expr);
        appendTAC(createTAC(TAC_ARG, v, NULL, NULL));
        names[n++] = v;
    }
    return n;
}

/* Generate TAC for expression - returns the temp/var holding result */
char* generateTACExpr(ASTNode* node) {
    if (!node) return NULL;

    switch(node->type) {
        case NODE_NUM: {
            char* temp = malloc(20);
            sprintf(temp, "%d", node->data.num);
            return temp;
        }

        case NODE_VAR:
            return strdup(resolveName(node->data.name));

        case NODE_BINOP: {
            char* left = generateTACExpr(node->data.binop.left);
            char* right = NULL;

            /* Unary operators have no right operand: unary minus from
             * Topic 3, and logical NOT from Topic 5. */
            if (node->data.binop.op != 'u' && node->data.binop.op != '!') {
                right = generateTACExpr(node->data.binop.right);
            }

            char* temp = allocTemp();

            /* Select operation type */
            TACOp op;
            switch(node->data.binop.op) {
                case '+': op = TAC_ADD; break;
                case '-': op = TAC_SUB; break;
                case '*': op = TAC_MUL; break;
                case '/': op = TAC_DIV; break;
                case 'u': op = TAC_NEG; break; /* unary - */
                default:  op = TAC_ADD; break;
            }

            appendTAC(createTAC(op, left, right, temp));

            freeTemp(left);
            if (right) freeTemp(right);

            return temp;
        }

        /* A call becomes:   ARG a1 ; ARG a2 ; t = CALL f, 2
         * An index becomes: t = arr[i]     (a single ARRAY_LOAD)
         *
         * Both return the NAME of the temporary holding the result, because
         * that is the contract generateTACExpr has with its callers. */
        case NODE_FUNC_CALL: {
            char* names[MAX_CALL_ARGS];
            int n = generateArgTAC(node->data.func_call.args, names);
            char* result = allocTemp();
            char count[16];
            snprintf(count, sizeof count, "%d", n);
            appendTAC(createTAC(TAC_CALL, node->data.func_call.name, count, result));
            for (int i = 0; i < n; i++) { freeTemp(names[i]); free(names[i]); }
            return result;
        }

        case NODE_ARRAY_INDEX: {
            char* idx = generateTACExpr(node->data.array_index.index);
            char* temp = allocTemp();
            appendTAC(createTAC(TAC_ARRAY_LOAD, (char*)resolveName(node->data.array_index.name), idx, temp));
            freeTemp(idx);
            free(idx);
            return temp;
        }

        default:
            return NULL;
    }
}

/* Generate TAC for statement list */
static void generateTACStmtList(ASTNode* node) {
    if (!node) return;

    if (node->type == NODE_STMT_LIST) {
        generateTACStmt(node->data.stmtlist.stmt);
        generateTACStmtList(node->data.stmtlist.next);
    } else {
        generateTACStmt(node);
    }
}

/* Generate TAC for a statement */
static void generateTACStmt(ASTNode* node) {
    if (!node) return;

    switch(node->type) {
        case NODE_DECL:
            if (tacInFunction) {
                char* nm = declareLocalName(node->data.decl.name, 0);
                appendTAC(createTAC(TAC_DECL, node->data.decl.varType, NULL, nm));
                free(nm);
            } else {
                appendTAC(createTAC(TAC_DECL, node->data.decl.varType, NULL, node->data.decl.name));
            }
            break;

        /* Assignment has two shapes.  If arrayLHS is set, evaluate the
         * index and emit ARRAY_STORE; otherwise emit a plain ASSIGN. */
        case NODE_ASSIGN: {
            char* expr = generateTACExpr(node->data.assign.value);

            if (node->data.assign.arrayLHS) {
                ASTNode* target = node->data.assign.arrayLHS;
                char* idx = generateTACExpr(target->data.array_index.index);
                /* arr[idx] = expr   — note the VALUE travels in `result` */
                appendTAC(createTAC(TAC_ARRAY_STORE,
                                    (char*)resolveName(target->data.array_index.name), idx, expr));
                freeTemp(idx);
                free(idx);
            } else {
                appendTAC(createTAC(TAC_ASSIGN, expr, NULL,
                                    (char*)resolveName(node->data.assign.var)));
            }
            freeTemp(expr);
            free(expr);
            break;
        }

        case NODE_PRINT: {
            char* expr = generateTACExpr(node->data.expr);
            appendTAC(createTAC(TAC_PRINT, expr, NULL, NULL));
            freeTemp(expr);
            break;
        }

        /* Evaluate the expression (if any) and emit RETURN.  Emitting RETURN
         * is easy; making it actually leave the function is codegen.c's job. */
        case NODE_RETURN: {
            char* expr = node->data.ret.expr ? generateTACExpr(node->data.ret.expr) : NULL;
            appendTAC(createTAC(TAC_RETURN, expr, NULL, NULL));
            if (expr) { freeTemp(expr); free(expr); }
            break;
        }

        /* A block's scope mattered to semantic analysis, but its names must
         * still be resolved here so a shadowing declaration gets its own
         * storage (see NAME RESOLUTION above). */
        case NODE_BLOCK:
            pushNameScope();
            generateTACStmtList(node->data.block.stmt_list);
            popNameScope();
            break;

        /* A call used as a statement: evaluate it and discard the result. */
        case NODE_FUNC_CALL: {
            char* t = generateTACExpr(node);
            freeTemp(t);
            free(t);
            break;
        }

        case NODE_STMT_LIST:
            /* Left recursion in the grammar means a statement list may
             * contain another statement list; handle that here too. */
            generateTACStmtList(node);
            break;

        /* One node type, two very different meanings: a real array reserves
         * size*4 bytes (ARRAY_DECL); an array parameter receives an address
         * (PARAM of type "int[]"). */
        case NODE_ARRAY_DECL: {
            const char* src = node->data.array_decl.name;
            if (node->data.array_decl.isParam) {
                char* nm = declareLocalName(src, 1);
                appendTAC(createTAC(TAC_PARAM, "int[]", NULL, nm));
                free(nm);
            } else {
                char size[16];
                snprintf(size, sizeof size, "%d", node->data.array_decl.size);
                if (tacInFunction) {
                    char* nm = declareLocalName(src, 0);
                    appendTAC(createTAC(TAC_ARRAY_DECL, size, NULL, nm));
                    free(nm);
                } else {
                    appendTAC(createTAC(TAC_ARRAY_DECL, size, NULL, (char*)src));
                }
            }
            break;
        }
        default:
            break;
    }
}

/* A function definition becomes:
 *     FUNC_BEGIN f ; PARAM int a ; ... body ... ; FUNC_END f
 * An array parameter is marked with the type "int[]" so the code generator
 * knows the slot holds an address rather than the data. */
static void generateParamTAC(ASTNode* params) {
    for (ASTNode* p = params; p; p = p->data.param_list.next) {
        ASTNode* par = p->data.param_list.param;
        if (!par) continue;
        if (par->type == NODE_ARRAY_DECL) {
            char* nm = declareLocalName(par->data.array_decl.name, 1);
            appendTAC(createTAC(TAC_PARAM, "int[]", NULL, nm));
            free(nm);
        } else {
            char* nm = declareLocalName(par->data.param.name, 1);
            appendTAC(createTAC(TAC_PARAM, "int", NULL, nm));
            free(nm);
        }
    }
}

/* Does the last statement of this list (looking through nested lists and
 * blocks) happen to be a return?  If so no implicit `return 0` is needed. */
static int endsWithReturn(ASTNode* n) {
    if (!n) return 0;
    if (n->type == NODE_RETURN) return 1;
    if (n->type == NODE_STMT_LIST)
        return n->data.stmtlist.next ? endsWithReturn(n->data.stmtlist.next)
                                     : endsWithReturn(n->data.stmtlist.stmt);
    return 0;
}

static void generateTACFuncDef(ASTNode* def) {
    const char* name = def->data.func_def.name;
    ASTNode* body = def->data.func_def.body;
    ASTNode* stmts = (body && body->type == NODE_BLOCK) ? body->data.block.stmt_list : body;

    tacInFunction = 1;
    beginFunctionNames();
    pushNameScope();                       /* parameters + top-level locals */

    appendTAC(createTAC(TAC_FUNC_BEGIN, NULL, NULL, (char*)name));
    generateParamTAC(def->data.func_def.params);
    generateTACStmtList(stmts);
    /* Falling off the end of a function returns 0 (this is what makes
     * `int main() { ... }` with no return statement exit cleanly). */
    if (!endsWithReturn(stmts))
        appendTAC(createTAC(TAC_RETURN, "0", NULL, NULL));
    appendTAC(createTAC(TAC_FUNC_END, NULL, NULL, (char*)name));

    popNameScope();
    tacInFunction = 0;
}

/* The program is a list of declarations and function definitions.  File-scope
 * declarations are emitted between functions; the back end places them in
 * .data.  Every function gets its own FUNC_BEGIN/FUNC_END pair. */
static void generateTACList(ASTNode* node) {
    if (!node) return;
    switch (node->type) {
        case NODE_STMT_LIST:
            generateTACList(node->data.stmtlist.stmt);
            generateTACList(node->data.stmtlist.next);
            break;
        case NODE_FUNC_DEF:
            generateTACFuncDef(node);
            break;
        default:
            generateTACStmt(node);       /* a file-scope declaration */
            break;
    }
}

void generateTAC(ASTNode* node) {
    if (!node) return;
    globalNameCount = 0;
    collectGlobalNames(node);
    generateTACList(node);
}

/* Get operation name for printing */
static const char* getOpName(TACOp op) {
    switch(op) {
        case TAC_ADD: return "+";
        case TAC_SUB: return "-";
        case TAC_MUL: return "*";
        case TAC_DIV: return "/";
        case TAC_NEG: return "NEG";
        case TAC_LT: return "<";
        case TAC_GT: return ">";
        case TAC_LE: return "<=";
        case TAC_GE: return ">=";
        case TAC_EQ: return "==";
        case TAC_NE: return "!=";
        case TAC_AND: return "&&";
        case TAC_OR: return "||";
        case TAC_NOT: return "!";
        default: return "?";
    }
}


/* ==========================================================================
 * FORMATTING
 * --------------------------------------------------------------------------
 * One function renders a TAC instruction as text.  Everything that displays
 * or saves three-address code goes through it, so the listing you read on
 * screen and the listing saved to the .tac file can never drift apart.
 * ========================================================================*/
void formatTAC(const TACInstr* i, char* buf, size_t n) {
    if (!i) { snprintf(buf, n, "(null)"); return; }
    switch (i->op) {
        case TAC_FUNC_BEGIN: snprintf(buf, n, "FUNC_BEGIN %s", i->result); break;
        case TAC_FUNC_END:   snprintf(buf, n, "FUNC_END %s", i->result); break;
        case TAC_PARAM:      snprintf(buf, n, "PARAM %s %s",
                                      i->arg1 ? i->arg1 : "int", i->result); break;
        case TAC_DECL:       snprintf(buf, n, "DECL %s %s",
                                      i->arg1 ? i->arg1 : "int", i->result); break;

        case TAC_ADD: case TAC_SUB: case TAC_MUL: case TAC_DIV:
        case TAC_LT:  case TAC_GT:  case TAC_LE:  case TAC_GE:
        case TAC_EQ:  case TAC_NE:  case TAC_AND: case TAC_OR:
            snprintf(buf, n, "%s = %s %s %s", i->result, i->arg1,
                     getOpName(i->op), i->arg2); break;

        case TAC_NEG:        snprintf(buf, n, "%s = -%s", i->result, i->arg1); break;
        case TAC_NOT:        snprintf(buf, n, "%s = !%s", i->result, i->arg1); break;
        case TAC_ASSIGN:     snprintf(buf, n, "%s = %s", i->result, i->arg1); break;
        case TAC_PRINT:      snprintf(buf, n, "PRINT %s", i->arg1); break;
        case TAC_ARG:        snprintf(buf, n, "ARG %s", i->arg1); break;
        case TAC_CALL:       snprintf(buf, n, "%s = CALL %s, %s",
                                      i->result, i->arg1, i->arg2); break;
        case TAC_RETURN:     if (i->arg1) { snprintf(buf, n, "RETURN %s", i->arg1); }
                             else         { snprintf(buf, n, "RETURN"); }
                             break;
        case TAC_LABEL:      snprintf(buf, n, "%s:", i->result); break;
        case TAC_GOTO:       snprintf(buf, n, "GOTO %s", i->arg1); break;
        case TAC_IF_FALSE:   snprintf(buf, n, "IF_FALSE %s GOTO %s", i->arg1, i->arg2); break;
        case TAC_IF_TRUE:    snprintf(buf, n, "IF_TRUE %s GOTO %s", i->arg1, i->arg2); break;
        case TAC_ARRAY_DECL: snprintf(buf, n, "ARRAY_DECL %s[%s]", i->result, i->arg1); break;
        case TAC_ARRAY_LOAD: snprintf(buf, n, "%s = %s[%s]", i->result, i->arg1, i->arg2); break;
        case TAC_ARRAY_STORE:snprintf(buf, n, "%s[%s] = %s", i->arg1, i->arg2, i->result); break;
        default:             snprintf(buf, n, "UNKNOWN"); break;
    }
}

/* Count instructions in a list — the crudest possible code-size metric, and
 * the one Topic 4 uses to quantify what optimization bought us. */
int countTAC(const TACList* list) {
    int n = 0;
    for (TACInstr* c = list->head; c; c = c->next) n++;
    return n;
}

static void dumpList(const TACList* list, const char* title) {
    char buf[256];
    trace("%s\n", title);
    trace("─────────────────────────────────────────────\n");
    int line = 1;
    for (TACInstr* c = list->head; c; c = c->next) {
        formatTAC(c, buf, sizeof buf);
        if (c->op == TAC_LABEL || c->op == TAC_FUNC_BEGIN || c->op == TAC_FUNC_END)
            trace("%3d: %s\n", line++, buf);
        else
            trace("%3d:     %s\n", line++, buf);
    }
    trace("─────────────────────────────────────────────\n");
    trace("     %d instructions\n\n", countTAC(list));
}

void printTAC(void)          { dumpList(&tacList,       "UNOPTIMIZED THREE-ADDRESS CODE"); }
void printOptimizedTAC(void) { dumpList(&optimizedList, "OPTIMIZED THREE-ADDRESS CODE"); }

TACList* getOptimizedTAC(void) { return &optimizedList; }
TACList* getUnoptimizedTAC(void) { return &tacList; }

static void saveList(const TACList* list, const char* filename, const char* banner) {
    FILE* f = fopen(filename, "w");
    if (!f) { fprintf(stderr, "Cannot write %s\n", filename); return; }
    char buf[256];
    fprintf(f, "; %s\n", banner);
    fprintf(f, "; Generated by the mini compiler\n;\n");
    int line = 1;
    for (TACInstr* c = list->head; c; c = c->next) {
        formatTAC(c, buf, sizeof buf);
        if (c->op == TAC_LABEL || c->op == TAC_FUNC_BEGIN || c->op == TAC_FUNC_END)
            fprintf(f, "%3d: %s\n", line++, buf);
        else
            fprintf(f, "%3d:     %s\n", line++, buf);
    }
    fprintf(f, ";\n; %d instructions\n", countTAC(list));
    fclose(f);
}

void saveTACToFile(const char* filename)          { saveList(&tacList,       filename, "Unoptimized three-address code"); }
void saveOptimizedTACToFile(const char* filename) { saveList(&optimizedList, filename, "Optimized three-address code"); }

/* ==========================================================================
 * PHASE 5 — OPTIMIZATION
 * --------------------------------------------------------------------------
 * The optimizer rewrites the TAC list into a shorter, cheaper list that
 * computes the same thing.  It runs the same set of transformations REPEATEDLY
 * until a pass changes nothing, because optimizations feed each other:
 *
 *      t0 = 2 * 3        constant folding  ->  t0 = 6
 *      x  = t0                                  x  = 6      (copy propagation)
 *      y  = x + 0                               y  = 6      (algebraic + folding)
 *      t0 = ...                                 (t0 now dead -> removed)
 *
 * No single pass finds all of that; three passes do.  Reaching a fixed point
 * is the standard way real optimizers are structured.
 *
 * Transformations implemented, in the order they are applied within a pass:
 *
 *   1. Algebraic simplification   x+0, x-0, x*1, x*0, x/1
 *   2. Constant folding           evaluate operations on two literals
 *   3. Constant propagation       replace a variable by a constant known to
 *                                 hold it at that point
 *   4. Copy propagation           replace t1 by x after "t1 = x"
 *   5. Dead code elimination      drop assignments to names never read again
 *   6. Unreachable code removal   drop code between GOTO and the next label
 *   7. Branch simplification      constant conditions become GOTO or nothing
 *
 * SAFETY RULES the passes obey, and that students must not break when they
 * extend this file:
 *   • A LABEL ends a basic block: forget everything known about values,
 *     because control can arrive here from anywhere.
 *   • A CALL may modify globals and arrays: forget facts about non-temporaries.
 *   • Never eliminate a store to an array or a global as "dead"; we do not
 *     track those precisely enough to prove it.
 * ========================================================================*/

#define MAX_FACTS 256

typedef struct {
    char* name;      /* The variable or temporary the fact is about */
    char* value;     /* The constant, or the name it is a copy of   */
    int   isConst;   /* 1 = value is a literal, 0 = value is a name */
} Fact;

static Fact  facts[MAX_FACTS];
static int   factCount = 0;
static int   changesThisPass = 0;

/* Optimization bookkeeping, reported to the user and used by Topic 4 to
 * quantify the gain from each technique. */
static OptStats optStats;

OptStats getOptStats(void) { return optStats; }

static void clearFacts(void) { factCount = 0; }

static void dropFactsAbout(const char* name) {
    for (int i = 0; i < factCount; i++) {
        if (strcmp(facts[i].name, name) == 0 ||
            (!facts[i].isConst && strcmp(facts[i].value, name) == 0)) {
            facts[i] = facts[--factCount];
            i--;
        }
    }
}

/* A call can change any global or array element, so only facts about
 * compiler temporaries survive it. */
static void dropNonTempFacts(void) {
    for (int i = 0; i < factCount; i++) {
        /* A fact dies if it is ABOUT a non-temporary, or if it says a temporary
         * is a copy of a non-temporary (t1 = g; call; t1 must not become g). */
        int stale = !isTempName(facts[i].name) ||
                    (!facts[i].isConst && !isTempName(facts[i].value));
        if (stale) { facts[i] = facts[--factCount]; i--; }
    }
}

static void recordFact(const char* name, const char* value, int isConst) {
    dropFactsAbout(name);
    if (factCount >= MAX_FACTS) return;
    facts[factCount].name    = (char*)name;
    facts[factCount].value   = (char*)value;
    facts[factCount].isConst = isConst;
    factCount++;
}

static const char* lookupFact(const char* name, int wantConst) {
    if (!name) return NULL;
    for (int i = 0; i < factCount; i++)
        if (strcmp(facts[i].name, name) == 0 && (!wantConst || facts[i].isConst))
            return facts[i].value;
    return NULL;
}

/* True for opcodes that only compute a value into `result` and have no other
 * effect.  Only these are candidates for dead-code elimination: a CALL may
 * print or modify globals, a STORE writes memory, a branch changes control. */
static int mnemonicIsPure(TACOp op) {
    switch (op) {
        case TAC_ADD: case TAC_SUB: case TAC_MUL: case TAC_DIV: case TAC_NEG:
        case TAC_LT:  case TAC_GT:  case TAC_LE:  case TAC_GE:
        case TAC_EQ:  case TAC_NE:  case TAC_AND: case TAC_OR: case TAC_NOT:
        case TAC_ARRAY_LOAD:
            return 1;
        default:
            return 0;
    }
}

/* Is this operand a literal integer? */
static int isConstantNumber(const char* s) {
    if (!s || !*s) return 0;
    int i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (!s[i]) return 0;
    for (; s[i]; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* Evaluate an operation whose operands are both literals. */
static char* foldConstants(TACOp op, const char* a1, const char* a2, int* folded) {
    *folded = 0;
    if (!isConstantNumber(a1) || !isConstantNumber(a2)) return NULL;
    long a = atol(a1), b = atol(a2), r;
    switch (op) {
        case TAC_ADD: r = a + b; break;
        case TAC_SUB: r = a - b; break;
        case TAC_MUL: r = a * b; break;
        case TAC_DIV: if (b == 0) return NULL;   /* leave division by zero
                                                  * alone: folding it would
                                                  * change a run-time trap
                                                  * into a compile-time one */
                      r = a / b; break;
        case TAC_LT:  r = a <  b; break;
        case TAC_GT:  r = a >  b; break;
        case TAC_LE:  r = a <= b; break;
        case TAC_GE:  r = a >= b; break;
        case TAC_EQ:  r = a == b; break;
        case TAC_NE:  r = a != b; break;
        case TAC_AND: r = (a != 0) && (b != 0); break;
        case TAC_OR:  r = (a != 0) || (b != 0); break;
        default: return NULL;
    }
    char* s = malloc(24);
    snprintf(s, 24, "%ld", r);
    *folded = 1;
    return s;
}

/* Does `name` get read anywhere at or after instruction `from`?
 * A conservative liveness test: it stops being conservative only for
 * compiler temporaries, which is exactly where dead code piles up. */
static int isReadLater(TACInstr* from, const char* name) {
    for (TACInstr* c = from; c; c = c->next) {
        if (c->arg1 && strcmp(c->arg1, name) == 0) return 1;
        if (c->arg2 && strcmp(c->arg2, name) == 0) return 1;
        /* ARRAY_STORE reads its `result` field (the value being stored) */
        if (c->op == TAC_ARRAY_STORE && c->result && strcmp(c->result, name) == 0) return 1;
        if (c->op == TAC_RETURN && c->arg1 && strcmp(c->arg1, name) == 0) return 1;
    }
    return 0;
}

/* Copy a TAC list so a pass can rewrite it without destroying the original. */
static TACList copyList(const TACList* src) {
    TACList d = { NULL, NULL, src->tempCount, src->labelCount };
    for (TACInstr* c = src->head; c; c = c->next) {
        TACInstr* n = createTAC(c->op, c->arg1, c->arg2, c->result);
        if (!d.head) d.head = d.tail = n;
        else { d.tail->next = n; d.tail = n; }
    }
    return d;
}

/* -------------------------------------------------------------------------
 * One optimization pass over `in`, producing `out`.
 * Returns the number of changes made.
 * -----------------------------------------------------------------------*/
static TACList optimizePass(TACList* in) {
    TACList out = { NULL, NULL, in->tempCount, in->labelCount };
    clearFacts();
    int unreachable = 0;

    for (TACInstr* c = in->head; c; c = c->next) {

        /* --- 6. UNREACHABLE CODE ------------------------------------------
         * Everything between an unconditional GOTO and the next label can
         * never execute. */
        if (unreachable) {
            if (c->op == TAC_LABEL || c->op == TAC_FUNC_BEGIN || c->op == TAC_FUNC_END) {
                unreachable = 0;
            } else {
                optStats.unreachable++; changesThisPass++;
                continue;
            }
        }

        /* --- block boundaries: facts stop being valid --------------------- */
        if (c->op == TAC_LABEL || c->op == TAC_FUNC_BEGIN || c->op == TAC_FUNC_END) clearFacts();
        if (c->op == TAC_CALL) dropNonTempFacts();

        char* a1 = c->arg1;
        char* a2 = c->arg2;

        /* --- 3/4. CONSTANT AND COPY PROPAGATION --------------------------
         * Substitute into the operands (never into `result`, which is being
         * defined rather than read). ARRAY_STORE is the exception: its
         * `result` field is the value being stored, so it is a read. */
        int isRead1 = (c->op != TAC_LABEL && c->op != TAC_GOTO &&
                       c->op != TAC_FUNC_BEGIN && c->op != TAC_FUNC_END &&
                       c->op != TAC_DECL && c->op != TAC_PARAM &&
                       c->op != TAC_ARRAY_DECL && c->op != TAC_CALL);
        if (isRead1 && a1 && !isConstantNumber(a1)) {
            const char* k = lookupFact(a1, 1);
            if (k) { a1 = (char*)k; optStats.constProp++; changesThisPass++; }
            else {
                const char* cp = lookupFact(a1, 0);
                if (cp) { a1 = (char*)cp; optStats.copyProp++; changesThisPass++; }
            }
        }
        if (a2 && !isConstantNumber(a2) && c->op != TAC_IF_FALSE && c->op != TAC_IF_TRUE
               && c->op != TAC_CALL) {
            const char* k = lookupFact(a2, 1);
            if (k) { a2 = (char*)k; optStats.constProp++; changesThisPass++; }
            else {
                const char* cp = lookupFact(a2, 0);
                if (cp) { a2 = (char*)cp; optStats.copyProp++; changesThisPass++; }
            }
        }

        TACInstr* emit = NULL;

        /* --- 1. ALGEBRAIC SIMPLIFICATION ---------------------------------
         * Identities that hold for every integer value, so they are safe
         * without knowing anything about the operands. */
        if (!emit && (c->op == TAC_ADD || c->op == TAC_SUB)) {
            if (a2 && isConstantNumber(a2) && atol(a2) == 0) {           /* x ± 0 */
                emit = createTAC(TAC_ASSIGN, a1, NULL, c->result);
                optStats.algebraic++; changesThisPass++;
            } else if (c->op == TAC_ADD && a1 && isConstantNumber(a1) && atol(a1) == 0) {
                emit = createTAC(TAC_ASSIGN, a2, NULL, c->result);       /* 0 + x */
                optStats.algebraic++; changesThisPass++;
            }
        }
        if (!emit && c->op == TAC_MUL) {
            if ((a2 && isConstantNumber(a2) && atol(a2) == 0) ||
                (a1 && isConstantNumber(a1) && atol(a1) == 0)) {         /* x * 0 */
                emit = createTAC(TAC_ASSIGN, "0", NULL, c->result);
                optStats.algebraic++; changesThisPass++;
            } else if (a2 && isConstantNumber(a2) && atol(a2) == 1) {    /* x * 1 */
                emit = createTAC(TAC_ASSIGN, a1, NULL, c->result);
                optStats.algebraic++; changesThisPass++;
            } else if (a1 && isConstantNumber(a1) && atol(a1) == 1) {    /* 1 * x */
                emit = createTAC(TAC_ASSIGN, a2, NULL, c->result);
                optStats.algebraic++; changesThisPass++;
            }
        }
        if (!emit && c->op == TAC_DIV && a2 && isConstantNumber(a2) && atol(a2) == 1) {
            emit = createTAC(TAC_ASSIGN, a1, NULL, c->result);           /* x / 1 */
            optStats.algebraic++; changesThisPass++;
        }

        /* --- 2. CONSTANT FOLDING ------------------------------------------ */
        if (!emit) {
            int folded = 0;
            char* v = foldConstants(c->op, a1, a2, &folded);
            if (folded) {
                emit = createTAC(TAC_ASSIGN, v, NULL, c->result);
                optStats.constFold++; changesThisPass++;
                free(v);
            }
        }
        if (!emit && c->op == TAC_NEG && isConstantNumber(a1)) {
            char v[24]; snprintf(v, sizeof v, "%ld", -atol(a1));
            emit = createTAC(TAC_ASSIGN, v, NULL, c->result);
            optStats.constFold++; changesThisPass++;
        }
        if (!emit && c->op == TAC_NOT && isConstantNumber(a1)) {
            emit = createTAC(TAC_ASSIGN, atol(a1) == 0 ? "1" : "0", NULL, c->result);
            optStats.constFold++; changesThisPass++;
        }


        /* Nothing rewrote it: keep the instruction, with propagated operands. */
        if (!emit) emit = createTAC(c->op, a1, a2, c->result);

        /* --- 5. DEAD CODE ELIMINATION ------------------------------------
         * An assignment to a TEMPORARY that nothing reads afterward can go.
         * Only temporaries: a store to a user variable or a global might be
         * observed by code this simple analysis cannot see. */
        if (emit->op == TAC_ASSIGN || (mnemonicIsPure(emit->op))) {
            const char* d = emit->result;
            if (isTempName(d) && !isReadLater(c->next, d)) {
                optStats.deadCode++; changesThisPass++;
                continue;
            }
        }

        /* --- record what we now know for later instructions ---------------- */
        switch (emit->op) {
            case TAC_ASSIGN:
                if (isConstantNumber(emit->arg1)) recordFact(emit->result, emit->arg1, 1);
                else                              recordFact(emit->result, emit->arg1, 0);
                break;
            case TAC_ARRAY_STORE:
                /* We do not track array contents; nothing to record. */
                break;
            default:
                if (emit->result) dropFactsAbout(emit->result);
                break;
        }

        if (emit->op == TAC_GOTO) unreachable = 1;

        if (!out.head) out.head = out.tail = emit;
        else { out.tail->next = emit; out.tail = emit; }
    }
    return out;
}

void optimizeTAC(void) {
    memset(&optStats, 0, sizeof optStats);
    optStats.instructionsBefore = countTAC(&tacList);

    TACList work = copyList(&tacList);

    int pass = 0;
    trace("Running optimization passes until no further change:\n");
    do {
        changesThisPass = 0;
        TACList next = optimizePass(&work);
        work = next;
        pass++;
        trace("  pass %d: %3d change(s), %3d instructions\n",
               pass, changesThisPass, countTAC(&work));
    } while (changesThisPass > 0 && pass < 20);

    optimizedList = work;
    optStats.passes = pass;
    optStats.instructionsAfter = countTAC(&optimizedList);

    int removed = optStats.instructionsBefore - optStats.instructionsAfter;
    double pct = optStats.instructionsBefore
               ? (100.0 * removed / optStats.instructionsBefore) : 0.0;

    trace("\n  Technique                     Applications\n");
    trace("  ─────────────────────────────────────────\n");
    trace("  Algebraic simplification      %6d\n", optStats.algebraic);
    trace("  Constant folding              %6d\n", optStats.constFold);
    trace("  Constant propagation          %6d\n", optStats.constProp);
    trace("  Copy propagation              %6d\n", optStats.copyProp);
    trace("  Dead code elimination         %6d\n", optStats.deadCode);
    trace("  Unreachable code removal      %6d\n", optStats.unreachable);
    trace("  Branch simplification         %6d\n", optStats.branch);
    trace("  ─────────────────────────────────────────\n");
    trace("  TAC instructions   %d -> %d  (%d removed, %.1f%% smaller)\n\n",
           optStats.instructionsBefore, optStats.instructionsAfter, removed, pct);
}

