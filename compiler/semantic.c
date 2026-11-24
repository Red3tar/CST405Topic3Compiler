/* =========================================================================
 *  TOPIC 3 · Compiling Complex Variables and Functions
 * FILE: semantic.c   —   Phase 3 — Semantic analysis
 * -------------------------------------------------------------------------
 * THE PIPELINE, AND WHERE THIS FILE SITS IN IT
 *   scanner -> parser -> ast -> semantic -> tac -> codegen
 *                               ^^^^^^^^  this file
 *
 * WHAT IS NEW IN TOPIC 3
 *   • A scope STACK replaces the single flat scope
 *   • Function signatures are collected first, so calls may appear before definitions
 *   • Argument count is checked against the declaration
 *
 * WHAT COMES NEXT
 *   Topic 4 adds loops (while, for, break) and the label/jump machinery they need.
 * ========================================================================= */

/* SEMANTIC ANALYSIS IMPLEMENTATION - WITH FUNCTION SUPPORT
 * Performs semantic checks on the Abstract Syntax Tree
 * Now supports functions, scopes, parameters, arrays
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "semantic.h"
#include "symtab.h"
#include "trace.h"

#define MAX_FUNCTIONS 100
#define MAX_PARAMS 20
#define MAX_SCOPE_DEPTH 10
#define MAX_REGISTER_ARGS 4      /* $a0-$a3: the back end's calling convention */

/* --------------------------------------------------------------
 * FUNCTION TABLE
 * Variables and functions live in separate namespaces, which is why a
 * program may declare both `int total;` and `int total(...)`.  Just enough
 * is recorded to check a call: the name, the parameter count, and which
 * parameters are arrays.
 * -------------------------------------------------------------- */
typedef struct {
    char* name;
    int   paramCount;
    int   isArrayParam[MAX_PARAMS];   /* 1 = `int a[]`, 0 = `int a` */
    int   lineno;
} FuncInfo;

static FuncInfo funcTable[MAX_FUNCTIONS];
static int      funcCount = 0;
static const char* currentFunction = NULL;   /* NULL while at file scope */

/* Scope for variables.  isArr records which names are arrays, because an
 * array may only be used with a subscript (or passed whole to an array
 * parameter) and a scalar may never be subscripted. */
typedef struct {
    char* names[MAX_VARS];
    int   isArr[MAX_VARS];
    int   count;
} Scope;

/* Global semantic information */
static SemanticInfo semInfo;
static Scope scopes[MAX_SCOPE_DEPTH];
static int scopeDepth = 0;

/* ---------------------------------------------------------------------------
 * Error reporting.  Every semantic error has the same shape: a title, a line
 * number, what went wrong, and (usually) how to fix it.
 * -------------------------------------------------------------------------*/
static void semError(const char* title, int lineno, const char* suggestion,
                     const char* fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    char head[80];
    snprintf(head, sizeof head, "SEMANTIC ERROR - %s", title);
    fprintf(stderr, "\n╔════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║ %-58s ║\n", head);
    fprintf(stderr, "╚════════════════════════════════════════════════════════════╝\n");
    fprintf(stderr, "  📍 Location: Line %d\n", lineno);
    fprintf(stderr, "  ❌ Error: %s\n", msg);
    if (suggestion) fprintf(stderr, "  💡 Suggestion: %s\n", suggestion);
    fprintf(stderr, "\n");
    semInfo.errorCount++;
}

static void semWarning(int lineno, const char* fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "  ⚠️  Warning (line %d): %s\n", lineno, msg);
    semInfo.warningCount++;
}

/* Initialize semantic analyzer */
void initSemantic() {
    semInfo.errorCount = 0;
    semInfo.warningCount = 0;
    scopeDepth = 0;
    currentFunction = NULL;

    /* Reset the function table and seed it with the one function the
     * language provides for free: `print`, which takes one argument.
     * Registering it here is what makes `print(x)` type-check like any
     * other call instead of needing a special case. */
    for (int i = 0; i < funcCount; i++) free(funcTable[i].name);
    funcCount = 0;
    funcTable[0].name = strdup("print");
    funcTable[0].paramCount = 1;
    memset(funcTable[0].isArrayParam, 0, sizeof funcTable[0].isArrayParam);
    funcTable[0].lineno = 0;
    funcCount = 1;

    trace("SEMANTIC ANALYZER: initialized\n\n");
}

/* Scope management.  enterScope reports whether it actually pushed, so the
 * caller only pops what it pushed. */
static int enterScope() {
    if (scopeDepth >= MAX_SCOPE_DEPTH) {
        fprintf(stderr, "SEMANTIC ERROR: Maximum scope depth exceeded\n");
        semInfo.errorCount++;
        return 0;
    }
    scopes[scopeDepth].count = 0;
    scopeDepth++;
    return 1;
}

static void exitScope() {
    if (scopeDepth > 0) {
        /* Free variable names in this scope */
        for (int i = 0; i < scopes[scopeDepth - 1].count; i++) {
            free(scopes[scopeDepth - 1].names[i]);
        }
        scopeDepth--;
    }
}

/* Print current semantic scopes for debugging */
static void printSemanticScopes() {
    trace("\n┌─────────────────────────────────────────────────────────┐\n");
    trace("│ SEMANTIC SCOPE STACK (Depth: %d)                        \n", scopeDepth);
    trace("├─────────────────────────────────────────────────────────┤\n");

    if (scopeDepth == 0) {
        trace("│ (no active scopes)                                      │\n");
    } else {
        for (int depth = 0; depth < scopeDepth; depth++) {
            if (depth == 0) {
                trace("│ Scope[%d] GLOBAL (%d variables)                        \n", depth, scopes[depth].count);
            } else if (depth == 1 && currentFunction) {
                trace("│ Scope[%d] FUNCTION %s (%d names: parameters + locals)  \n",
                      depth, currentFunction, scopes[depth].count);
            } else {
                trace("│ Scope[%d] BLOCK (%d variables)                         \n", depth, scopes[depth].count);
            }

            if (scopes[depth].count > 0) {
                trace("│   Variables: ");
                for (int i = 0; i < scopes[depth].count; i++) {
                    trace("%s%s", scopes[depth].names[i], scopes[depth].isArr[i] ? "[]" : "");
                    if (i < scopes[depth].count - 1) trace(", ");
                }
                trace("\n");
            } else {
                trace("│   (empty)\n");
            }
        }
    }
    trace("└─────────────────────────────────────────────────────────┘\n\n");
}

/* RESERVED IDENTIFIERS
 * The intermediate-code generator invents names of its own: temporaries
 * t0, t1, ... and labels L0, L1, ...  If a user variable had one of those
 * shapes the back end could not tell them apart, so the language reserves
 * that namespace.  Reporting it here — in the semantic analyzer, with a line
 * number — is far kinder than a mysterious wrong answer at run time. */
static int isReservedName(const char* name) {
    if (!name || !name[0]) return 0;
    if (strncmp(name, "__sw", 4) == 0) return 1;          /* switch temporaries */
    if (name[0] != 't' && name[0] != 'L') return 0;
    if (!name[1]) return 0;
    for (int i = 1; name[i]; i++)
        if (name[i] < '0' || name[i] > '9') return 0;
    return 1;
}

static void reportReserved(const char* name, int lineno) {
    fprintf(stderr, "\n╔════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║ SEMANTIC ERROR - Reserved Identifier                      ║\n");
    fprintf(stderr, "╚════════════════════════════════════════════════════════════╝\n");
    fprintf(stderr, "  📍 Location: Line %d\n", lineno);
    fprintf(stderr, "  ❌ Error: '%s' is reserved for the compiler's own use\n", name);
    fprintf(stderr, "  📖 Note: names of the form t0, t1, ... are three-address-code\n");
    fprintf(stderr, "           temporaries and L0, L1, ... are generated labels.\n");
    fprintf(stderr, "  💡 Suggestion: rename the variable, for example '%s_' or 'total'\n\n", name);
}

/* Add a name to the current scope.  Returns -1 if it is already declared in
 * THIS scope (shadowing an outer scope is legal), 0 on success. */
static int addToScope(char* name, int isArr) {
    if (scopeDepth == 0) {
        fprintf(stderr, "SEMANTIC ERROR: No scope to add variable to\n");
        return -1;
    }

    Scope* currentScope = &scopes[scopeDepth - 1];

    /* Check if already declared in current scope */
    for (int i = 0; i < currentScope->count; i++) {
        if (strcmp(currentScope->names[i], name) == 0) {
            return -1;  /* Already declared in this scope */
        }
    }

    /* Add to current scope */
    if (currentScope->count >= MAX_VARS) {
        fprintf(stderr, "SEMANTIC ERROR: Too many variables in scope\n");
        return -1;
    }

    currentScope->names[currentScope->count] = strdup(name);
    currentScope->isArr[currentScope->count] = isArr;
    currentScope->count++;
    return 0;
}

static int addVarToScope(char* name) { return addToScope(name, 0); }

/* Arrays get their own helper — not because the storage question is
 * different (symtab.c settles that later) but because the error messages are. */
static int addArrayToScope(char* name) { return addToScope(name, 1); }

/* Look a name up, innermost scope first.
 * Returns -1 = not declared, 0 = scalar, 1 = array. */
static int findVarInScopes(const char* name) {
    for (int depth = scopeDepth - 1; depth >= 0; depth--) {
        for (int i = 0; i < scopes[depth].count; i++) {
            if (strcmp(scopes[depth].names[i], name) == 0) {
                return scopes[depth].isArr[i];
            }
        }
    }
    return -1;
}

/* --------------------------------------------------------------
 * FUNCTION TABLE HELPERS
 * countParams walks the parameter list parser.y built; countArgs walks
 * the argument list at a call site.  Comparing those two numbers is the
 * whole of call checking.
 * -------------------------------------------------------------- */
static FuncInfo* findFunction(const char* name) {
    for (int i = 0; i < funcCount; i++)
        if (strcmp(funcTable[i].name, name) == 0) return &funcTable[i];
    return NULL;
}

static int countParams(ASTNode* params) {
    int n = 0;
    for (ASTNode* p = params; p; p = p->data.param_list.next) n++;
    return n;
}

static int countArgs(ASTNode* args) {
    int n = 0;
    for (ASTNode* a = args; a; a = a->data.arg_list.next) n++;
    return n;
}

/* Returns 0 on success, -1 if the name is already a function. */
static int addFunction(ASTNode* def) {
    const char* name = def->data.func_def.name;
    if (findFunction(name)) return -1;
    if (funcCount >= MAX_FUNCTIONS) {
        fprintf(stderr, "SEMANTIC ERROR: Too many functions (limit %d)\n", MAX_FUNCTIONS);
        semInfo.errorCount++;
        return 0;
    }
    FuncInfo* f = &funcTable[funcCount++];
    f->name = strdup(name);
    f->lineno = def->lineno;
    f->paramCount = countParams(def->data.func_def.params);
    memset(f->isArrayParam, 0, sizeof f->isArrayParam);
    int i = 0;
    for (ASTNode* p = def->data.func_def.params; p && i < MAX_PARAMS;
         p = p->data.param_list.next, i++) {
        ASTNode* param = p->data.param_list.param;
        f->isArrayParam[i] = (param && param->type == NODE_ARRAY_DECL);
    }
    return 0;
}

/* Forward declarations: the checkers are mutually recursive, which is
 * exactly what you want when the thing being checked is a tree. */
static void checkStmtList(ASTNode* node);
static void checkStmt(ASTNode* node);
static void checkExpr(ASTNode* node);

/* Check one argument of a call against the parameter it will bind to.
 * wantArray: 1 = parameter is `int a[]`, 0 = scalar, -1 = unknown callee. */
static void checkArgument(ASTNode* e, int wantArray, const char* fname, int position) {
    if (!e) return;

    if (e->type == NODE_VAR) {
        int kind = findVarInScopes(e->data.name);
        if (kind < 0) { checkExpr(e); return; }          /* reports "undeclared" */
        if (wantArray == 1 && kind == 0) {
            semError("Argument Type Mismatch", e->lineno,
                     "pass the name of an array declared like  int name[10];",
                     "argument %d of '%s' must be an array, but '%s' is a scalar",
                     position, fname, e->data.name);
        } else if (wantArray == 0 && kind == 1) {
            semError("Argument Type Mismatch", e->lineno,
                     "subscript it:  name[i]",
                     "argument %d of '%s' must be a scalar, but '%s' is an array",
                     position, fname, e->data.name);
        }
        return;
    }

    if (wantArray == 1) {
        semError("Argument Type Mismatch", e->lineno,
                 "pass the name of an array variable",
                 "argument %d of '%s' must be an array, not an expression",
                 position, fname);
    }
    checkExpr(e);
}

/* Check expression for semantic correctness */
static void checkExpr(ASTNode* node) {
    if (!node) return;

    switch(node->type) {
        case NODE_NUM:
            /* Literal numbers are always valid */
            break;

        case NODE_VAR: {
            int kind = findVarInScopes(node->data.name);
            if (kind < 0) {
                fprintf(stderr, "\n╔════════════════════════════════════════════════════════════╗\n");
                fprintf(stderr, "║ SEMANTIC ERROR - Undeclared Variable                      ║\n");
                fprintf(stderr, "╚════════════════════════════════════════════════════════════╝\n");
                fprintf(stderr, "  📍 Location: Line %d\n", node->lineno);
                fprintf(stderr, "  ❌ Error: Variable '%s' is used before being declared\n", node->data.name);
                fprintf(stderr, "  💡 Suggestion: Add a declaration before using this variable:\n");
                fprintf(stderr, "     → int %s;  (add this before line %d)\n", node->data.name, node->lineno);
                fprintf(stderr, "  📖 Note: In C-minus, all variables must be declared before use\n\n");
                semInfo.errorCount++;
            } else if (kind == 1) {
                semError("Array Used Without Subscript", node->lineno,
                         "write an element, e.g. name[0] (whole arrays can only be passed to functions)",
                         "'%s' is an array and cannot be used as a single value", node->data.name);
            }
            break;
        }

        case NODE_BINOP:
            checkExpr(node->data.binop.left);
            if (node->data.binop.right) {  /* Unary minus has no right operand */
                checkExpr(node->data.binop.right);
                /* Division by a literal zero is knowable now; say so */
                if (node->data.binop.op == '/' && node->data.binop.right->type == NODE_NUM &&
                    node->data.binop.right->data.num == 0) {
                    semWarning(node->lineno, "division by the constant 0");
                }
            }
            break;

        /* A call: does the function exist, and does the call supply the
         * right number of arguments?  Then check each argument expression. */
        case NODE_FUNC_CALL: {
            const char* fname = node->data.func_call.name;
            FuncInfo* f = findFunction(fname);
            int nargs = countArgs(node->data.func_call.args);

            if (!f) {
                semError("Undefined Function", node->lineno,
                         "define the function (anywhere in the file) before calling it",
                         "function '%s' is called but never defined", fname);
            } else if (f->paramCount != nargs) {
                semError("Wrong Number of Arguments", node->lineno,
                         "make the call match the definition",
                         "'%s' takes %d argument%s but the call supplies %d",
                         fname, f->paramCount, f->paramCount == 1 ? "" : "s", nargs);
            }

            int pos = 0;
            for (ASTNode* a = node->data.func_call.args; a; a = a->data.arg_list.next, pos++) {
                int want = (f && pos < f->paramCount && pos < MAX_PARAMS) ? f->isArrayParam[pos] : -1;
                checkArgument(a->data.arg_list.expr, want, fname, pos + 1);
            }
            break;
        }

        /* The array must be declared, and the index is an expression that
         * must itself check out.  Note what we do NOT check — whether the
         * index is in range.  That is not knowable at compile time in
         * general, and this language has no run-time check either. */
        case NODE_ARRAY_INDEX: {
            int kind = findVarInScopes(node->data.array_index.name);
            if (kind < 0) {
                semError("Undeclared Array", node->lineno,
                         "declare it first, e.g.  int name[10];",
                         "array '%s' is used before being declared", node->data.array_index.name);
            } else if (kind == 0) {
                semError("Not An Array", node->lineno,
                         "only variables declared with [size] can be subscripted",
                         "'%s' is a scalar variable and cannot be subscripted",
                         node->data.array_index.name);
            }
            checkExpr(node->data.array_index.index);
            break;
        }

        default:
            break;
    }
}

/* Check statement */
static void checkStmt(ASTNode* node) {
    if (!node) return;

    switch(node->type) {
        case NODE_DECL:
            if (isReservedName(node->data.decl.name)) {
                reportReserved(node->data.decl.name, node->lineno);
                semInfo.errorCount++;
                break;
            }
            if (addVarToScope(node->data.decl.name) == -1) {
                fprintf(stderr, "\n╔════════════════════════════════════════════════════════════╗\n");
                fprintf(stderr, "║ SEMANTIC ERROR - Duplicate Variable Declaration           ║\n");
                fprintf(stderr, "╚════════════════════════════════════════════════════════════╝\n");
                fprintf(stderr, "  📍 Location: Line %d\n", node->lineno);
                fprintf(stderr, "  ❌ Error: Variable '%s' is already declared in the current scope\n", node->data.decl.name);
                fprintf(stderr, "  💡 Suggestion: Choose a different variable name or remove the duplicate declaration\n");
                fprintf(stderr, "  📖 Note: Each variable can only be declared once per scope\n");
                fprintf(stderr, "     → Use assignment (=) to change the value of an existing variable\n");
                fprintf(stderr, "     → Or use a different name: %s2, my%s, etc.\n\n", node->data.decl.name, node->data.decl.name);
                semInfo.errorCount++;
            } else {
                trace("  ✓ Variable '%s' declared (line %d)\n", node->data.decl.name, node->lineno);
            }
            break;

        case NODE_ASSIGN:
            if (node->data.assign.arrayLHS) {
                /* arr[i] = ... : checking the ARRAY_INDEX node checks both
                 * the array name and the index expression */
                checkExpr(node->data.assign.arrayLHS);
                trace("  ✓ Assignment to element of '%s' checked (line %d)\n",
                      node->data.assign.arrayLHS->data.array_index.name, node->lineno);
            } else {
                int kind = findVarInScopes(node->data.assign.var);
                if (kind < 0) {
                    fprintf(stderr, "\n╔════════════════════════════════════════════════════════════╗\n");
                    fprintf(stderr, "║ SEMANTIC ERROR - Assignment to Undeclared Variable        ║\n");
                    fprintf(stderr, "╚════════════════════════════════════════════════════════════╝\n");
                    fprintf(stderr, "  📍 Location: Line %d\n", node->lineno);
                    fprintf(stderr, "  ❌ Error: Trying to assign to variable '%s' which hasn't been declared\n", node->data.assign.var);
                    fprintf(stderr, "  💡 Suggestion: Declare the variable before assigning to it:\n");
                    fprintf(stderr, "     → Add this before line %d:\n", node->lineno);
                    fprintf(stderr, "       int %s;\n", node->data.assign.var);
                    fprintf(stderr, "  📖 Remember: Variables must be declared before use in C-minus\n\n");
                    semInfo.errorCount++;
                } else if (kind == 1) {
                    semError("Assignment to Whole Array", node->lineno,
                             "assign to one element:  name[i] = value;",
                             "'%s' is an array; a whole array cannot be assigned", node->data.assign.var);
                } else {
                    trace("  ✓ Assignment to '%s' is valid (line %d)\n", node->data.assign.var, node->lineno);
                }
            }
            checkExpr(node->data.assign.value);
            break;

        case NODE_PRINT:
            checkExpr(node->data.expr);
            trace("  ✓ Print statement is valid\n");
            break;

        /* `return` OUTSIDE any function is legal to parse and meaningless to
         * translate — exactly the gap this phase owns. */
        case NODE_RETURN:
            if (!currentFunction) {
                semError("Return Outside Function", node->lineno,
                         "move the return statement inside a function body",
                         "'return' can only appear inside a function");
            } else if (!node->data.ret.expr) {
                semWarning(node->lineno,
                           "bare 'return;' in int function '%s' returns 0", currentFunction);
            }
            checkExpr(node->data.ret.expr);
            break;

        /* A block opens a scope and closes it again.  That is what makes
         *     int x;  { int x; }      legal (inner x shadows outer x)
         * and    { int x; int x; }     an error (same name twice in one scope). */
        case NODE_BLOCK: {
            int pushed = enterScope();
            checkStmtList(node->data.block.stmt_list);
            if (pushed) exitScope();
            break;
        }

        /* A call used as a statement: result evaluated and discarded */
        case NODE_FUNC_CALL:
            checkExpr(node);
            break;

        case NODE_STMT_LIST:
            /* The grammar is left-recursive, so a statement list can contain
             * another statement list.  Handling that here (not only in
             * checkStmtList) is what lets the recursion bottom out. */
            checkStmtList(node);
            break;

        /* An array declaration needs a positive size.  Reject `int a[0];`
         * here — nothing downstream will. */
        case NODE_ARRAY_DECL:
            if (isReservedName(node->data.array_decl.name)) {
                reportReserved(node->data.array_decl.name, node->lineno);
                semInfo.errorCount++;
                break;
            }
            if (!node->data.array_decl.isParam && node->data.array_decl.size <= 0) {
                semError("Invalid Array Size", node->lineno,
                         "use a positive number of elements, e.g.  int a[10];",
                         "array '%s' is declared with size %d", node->data.array_decl.name,
                         node->data.array_decl.size);
            }
            if (addArrayToScope(node->data.array_decl.name) == -1) {
                semError("Duplicate Array Declaration", node->lineno,
                         "choose a different name",
                         "'%s' is already declared in the current scope", node->data.array_decl.name);
            } else {
                trace("  ✓ Array '%s[%d]' declared (line %d)\n", node->data.array_decl.name,
                      node->data.array_decl.size, node->lineno);
            }
            break;

        default:
            break;
    }
}

/* Check statement list */
static void checkStmtList(ASTNode* node) {
    if (!node) return;

    if (node->type == NODE_STMT_LIST) {
        checkStmt(node->data.stmtlist.stmt);
        checkStmtList(node->data.stmtlist.next);
    } else {
        checkStmt(node);
    }
}

/* --------------------------------------------------------------
 * FUNCTIONS — the TWO-PASS structure.
 *
 * Pass 1 (registerFunctions) records every function signature.
 * Pass 2 (checkFunctions) checks the bodies.
 * Two passes and not one, so that a function may call another defined
 * LATER in the file.
 * -------------------------------------------------------------- */

/* Parameters live in the SAME scope as the function body's top-level
 * declarations, so `int f(int x) { int x; }` is a duplicate. */
static void addParamsToScope(ASTNode* params) {
    for (ASTNode* p = params; p; p = p->data.param_list.next) {
        ASTNode* param = p->data.param_list.param;
        if (!param) continue;
        const char* name = (param->type == NODE_ARRAY_DECL) ? param->data.array_decl.name
                                                            : param->data.param.name;
        if (isReservedName(name)) {
            reportReserved(name, param->lineno);
            semInfo.errorCount++;
            continue;
        }
        int rc = (param->type == NODE_ARRAY_DECL) ? addArrayToScope((char*)name)
                                                  : addVarToScope((char*)name);
        if (rc == -1) {
            semError("Duplicate Parameter", param->lineno,
                     "give each parameter its own name",
                     "parameter '%s' appears twice in '%s'", name, currentFunction);
        }
    }
}

static void checkFuncDef(ASTNode* def) {
    currentFunction = def->data.func_def.name;
    trace("Checking function '%s' (line %d)\n", currentFunction, def->lineno);

    int pushed = enterScope();
    addParamsToScope(def->data.func_def.params);
    printSemanticScopes();

    ASTNode* body = def->data.func_def.body;
    if (body && body->type == NODE_BLOCK)
        checkStmtList(body->data.block.stmt_list);   /* same scope as the params */
    else
        checkStmt(body);

    if (pushed) exitScope();
    currentFunction = NULL;
}

static void registerFunction(ASTNode* def) {
    const char* name = def->data.func_def.name;
    int nparams = countParams(def->data.func_def.params);

    if (isReservedName(name)) {
        reportReserved(name, def->lineno);
        semInfo.errorCount++;
    }
    if (nparams > MAX_REGISTER_ARGS) {
        semError("Too Many Parameters", def->lineno,
                 "pass at most 4 values (use an array to pass more data)",
                 "function '%s' has %d parameters; arguments travel in $a0-$a3, so the limit is %d",
                 name, nparams, MAX_REGISTER_ARGS);
    }
    if (addFunction(def) == -1) {
        semError("Duplicate Function Definition", def->lineno,
                 "rename one of the functions",
                 "function '%s' is already defined", name);
    } else {
        trace("  ✓ Function '%s' registered with %d parameter%s (line %d)\n",
              name, nparams, nparams == 1 ? "" : "s", def->lineno);
    }
}

/* Pass 1: walk the top-level list recording signatures */
static void registerFunctions(ASTNode* node) {
    if (!node) return;
    if (node->type == NODE_STMT_LIST) {
        registerFunctions(node->data.stmtlist.stmt);
        registerFunctions(node->data.stmtlist.next);
    } else if (node->type == NODE_FUNC_DEF) {
        registerFunction(node);
    }
}

/* Pass 2: walk the top-level list again.  Declarations go into the global
 * scope; function definitions get their own scope. */
static void checkFunctions(ASTNode* node) {
    if (!node) return;
    if (node->type == NODE_STMT_LIST) {
        checkFunctions(node->data.stmtlist.stmt);
        checkFunctions(node->data.stmtlist.next);
    } else if (node->type == NODE_FUNC_DEF) {
        checkFuncDef(node);
    } else {
        checkStmt(node);        /* a file-scope declaration */
    }
}

int performSemanticAnalysis(ASTNode* root) {
    if (!root) {
        fprintf(stderr, "SEMANTIC ERROR: No AST to analyze\n");
        return -1;
    }

    trace("Running semantic analysis with function support...\n\n");

    /* Enter global scope */
    enterScope();
    trace("Entered global scope\n");
    printSemanticScopes();

    /* Pass 1: record every signature.  Pass 2: check every body. */
    trace("Pass 1: collecting function signatures\n");
    registerFunctions(root);
    trace("\nPass 2: checking declarations and function bodies\n");
    checkFunctions(root);

    /* The program's entry point */
    FuncInfo* m = findFunction("main");
    if (!m || m->lineno == 0) {
        semError("Missing main", 1,
                 "add an entry point:  int main() { ... }",
                 "the program does not define a function named 'main'");
    } else if (m->paramCount != 0) {
        semError("Invalid main", m->lineno,
                 "declare it as  int main()",
                 "'main' cannot take parameters (nothing supplies them)");
    }

    /* Exit global scope */
    exitScope();

    return semInfo.errorCount > 0 ? -1 : 0;
}

/* Print semantic analysis summary */
void printSemanticSummary() {
    trace("═══════════════════════════════════════════\n");
    trace("SEMANTIC ANALYSIS SUMMARY\n");
    trace("═══════════════════════════════════════════\n");
    trace("Errors found:       %d\n", semInfo.errorCount);
    trace("Warnings found:     %d\n", semInfo.warningCount);
    trace("\n");

    if (semInfo.errorCount == 0) {
        trace("✓ Semantic analysis passed - program is semantically correct!\n");
    } else {
        trace("✗ Semantic analysis failed - fix errors before proceeding\n");
    }
    trace("═══════════════════════════════════════════\n\n");
}

