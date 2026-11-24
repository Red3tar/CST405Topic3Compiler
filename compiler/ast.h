/* =========================================================================
 *  TOPIC 3 · Compiling Complex Variables and Functions
 * FILE: ast.h   —   Phase 2 — Syntax analysis (the tree it builds)
 * -------------------------------------------------------------------------
 * THE PIPELINE, AND WHERE THIS FILE SITS IN IT
 *   scanner -> parser -> ast -> semantic -> tac -> codegen
 *                        ^^^  this file
 *
 * WHAT IS NEW IN TOPIC 3
 *   • New nodes: FUNC_DEF, PARAM, PARAM_LIST, FUNC_CALL, ARG_LIST, RETURN
 *   • New nodes: ARRAY_DECL, ARRAY_INDEX, BLOCK
 *
 * WHAT COMES NEXT
 *   Topic 4 adds loops (while, for, break) and the label/jump machinery they need.
 *
 * YOUR TASK
 *   Everything below marked  TODO (Topic 3)  is yours to write.
 *   Everything else already works — it is the Topic 2 compiler you
 *   have already built and tested.  Do not rewrite it; extend it.
 * ========================================================================= */

#ifndef AST_H
#define AST_H

/* ABSTRACT SYNTAX TREE (AST)
 * The AST is an intermediate representation of the program structure
 * It represents the hierarchical syntax of the source code
 * Each node represents a construct in the language
 */

/* NODE TYPES - Different kinds of AST nodes in our language */
typedef enum {
    NODE_NUM,         /* Numeric literal (e.g., 42) */
    NODE_VAR,         /* Variable reference (e.g., x) */
    NODE_BINOP,       /* Binary operation (e.g., x + y) */
    NODE_DECL,        /* Variable declaration (e.g., int x) */
    NODE_ASSIGN,      /* Assignment statement (e.g., x = 10) */
    NODE_PRINT,       /* Print statement (e.g., print(x)) */
    NODE_STMT_LIST,   /* List of statements (program structure) */
    NODE_FUNC_DEF,    
    NODE_PARAM,
    NODE_PARAM_LIST,
    NODE_FUNC_CALL,
    NODE_ARG_LIST,
    NODE_RETURN,

/* --------------------------------------------------------------
 * TODO (Topic 3)
 * The node kinds functions need: FUNC_DEF, PARAM, PARAM_LIST, FUNC_CALL,
 * ARG_LIST, RETURN.
 * -------------------------------------------------------------- */
/* --------------------------------------------------------------
 * TODO (Topic 3)
 * And the node kinds blocks and arrays need: BLOCK, ARRAY_DECL, ARRAY_INDEX.
 * -------------------------------------------------------------- */
    NODE_BLOCK,       /* { ... } block of statements */
    NODE_ARRAY_DECL,  /* Array declaration (e.g., int a[10]) or array param (int a[]) */
    NODE_ARRAY_INDEX  /* Array element access (e.g., a[i]) */
} NodeType;

/* AST NODE STRUCTURE
 * Uses a union to efficiently store different node data
 * Only the relevant fields for each node type are used
 */
typedef struct ASTNode {
    NodeType type;  /* Identifies what kind of node this is */
    int lineno;     /* Line number in source code for error reporting */

    /* Union allows same memory to store different data types */
    union {
        /* Literal number value (NODE_NUM) */
        int num;
        
        /* Variable reference name (NODE_VAR) */
        char* name;

        /* Declaration structure (NODE_DECL) */
        struct {
            char* name;
            char* varType;
        } decl;


        /* Binary operation structure (NODE_BINOP) */
        struct {
            char op;                    /* Operator character ('+') */
            struct ASTNode* left;       /* Left operand */
            struct ASTNode* right;      /* Right operand */
        } binop;
        
        /* Assignment structure (NODE_ASSIGN) */
        struct {
            char* var;                  /* Variable being assigned to (NULL for array) */
            struct ASTNode* value;      /* Expression being assigned */
            struct ASTNode* arrayLHS;   /* ARRAY_INDEX target for arr[i] = ... (NULL otherwise) */
        } assign;
        
        /* Print expression (NODE_PRINT) */
        struct ASTNode* expr;
        
        /* Statement list structure (NODE_STMT_LIST) */
        struct {
            struct ASTNode* stmt;       /* Current statement */
            struct ASTNode* next;       /* Rest of the list */
        } stmtlist;

        /*Function defintion structure (FUNC_DEF)*/
        struct {
            char* name;                 /* Function name */
            struct ASTNode* params;     /* PARAM_LIST (NULL if no parameters) */
            struct ASTNode* body;       /* BLOCK */
        } func_def;

        /* Scalar parameter (NODE_PARAM) */
        struct {
            char* name;
        } param;

        /* Parameter list (NODE_PARAM_LIST) */
        struct {
            struct ASTNode* param;      /* PARAM or ARRAY_DECL with isParam = 1 */
            struct ASTNode* next;       /* Rest of the list */
        } param_list;

        /* Function call (NODE_FUNC_CALL) */
        struct {
            char* name;                 /* Function being called */
            struct ASTNode* args;       /* ARG_LIST (NULL if no arguments) */
        } func_call;

        /* Argument list (NODE_ARG_LIST) */
        struct {
            struct ASTNode* expr;       /* Current argument expression */
            struct ASTNode* next;       /* Rest of the list */
        } arg_list;

        /* Return statement (NODE_RETURN) */
        struct {
            struct ASTNode* expr;       /* NULL for a bare `return;` */
        } ret;

/* --------------------------------------------------------------
 * TODO (Topic 3)
 * Union members for the function-related node kinds.  Each records
 * exactly what later phases will ask for and nothing more:
 *   func_def  : name, params, body
 *   param     : name          param_list : param, next
 *   func_call : name, args    arg_list   : expr, next
 *   ret       : expr (NULL for a bare `return;`)
 * -------------------------------------------------------------- */

/* --------------------------------------------------------------
 * TODO (Topic 3)
 * Union members for blocks and arrays:
 *   block       : stmt_list
 *   array_decl  : name, size, isParam
 *   array_index : name, index
 * isParam separates `int a[10];` (reserve 40 bytes of storage) from
 * `int a[]` as a parameter (reserve 4 bytes for an address).
 * -------------------------------------------------------------- */
        /* Block structure (NODE_BLOCK) */
        struct {
            struct ASTNode* stmt_list;  /* STMT_LIST inside the braces (NULL if empty) */
        } block;

        /* Array declaration structure (NODE_ARRAY_DECL) */
        struct {
            char* name;                 /* Array name */
            int size;                   /* Number of elements (0 for a parameter) */
            int isParam;                /* 1 = `int a[]` parameter, 0 = real storage */
        } array_decl;

        /* Array element access structure (NODE_ARRAY_INDEX) */
        struct {
            char* name;                 /* Array name */
            struct ASTNode* index;      /* Index expression */
        } array_index;
    } data;
} ASTNode;

/* AST CONSTRUCTION FUNCTIONS
 * These functions are called by the parser to build the tree
 */
/* Basic nodes */
ASTNode* createNum(int value);                                   /* Create number node */
ASTNode* createVar(char* name);                                  /* Create variable node */
ASTNode* createBinOp(char op, ASTNode* left, ASTNode* right);   /* Create binary op node */
ASTNode* createDecl(char* type, char* name);                     /* Create declaration node */
ASTNode* createAssign(char* var, ASTNode* value);               /* Create assignment node */
ASTNode* createPrint(ASTNode* expr);                            /* Create print node */
ASTNode* createStmtList(ASTNode* stmt1, ASTNode* stmt2);        /* Create statement list */

/* Function-related nodes */
/* --------------------------------------------------------------
 * TODO (Topic 3)
 * Declare the constructors for the function-related nodes.
 * -------------------------------------------------------------- */
ASTNode* createFuncDef(char* name, ASTNode* params, ASTNode* body); /* Create function definition */
ASTNode* createParam(char* name);                                /* Create scalar parameter */
ASTNode* createParamList(ASTNode* param, ASTNode* next);        /* Create parameter list */
ASTNode* createFuncCall(char* name, ASTNode* args);             /* Create function call */
ASTNode* createArgList(ASTNode* expr, ASTNode* next);           /* Create argument list */
ASTNode* createReturn(ASTNode* expr);                           /* Create return (expr may be NULL) */

/* Control flow nodes */
/* --------------------------------------------------------------
 * TODO (Topic 3)
 * Declare createBlock.
 * -------------------------------------------------------------- */
ASTNode* createBlock(ASTNode* stmt_list);                       /* Create { ... } block */

/* --------------------------------------------------------------
 * TODO (Topic 3)
 * Declare the array constructors.  Note createArrayParam is separate from
 * createArrayDecl: a parameter has no size and is a reference, not storage.
 * -------------------------------------------------------------- */
ASTNode* createArrayDecl(char* name, int size);                  /* Create array declaration */
ASTNode* createArrayParam(char* name);                           /* Create array parameter (int a[]) */
ASTNode* createArrayIndex(char* name, ASTNode* index);           /* Create array element access */
ASTNode* createArrayAssign(ASTNode* arrayLHS, ASTNode* value);   /* Create arr[i] = expr */

/* Text form of a packed operator code (see createBinOp) */
const char* opText(char op);

/* AST DISPLAY FUNCTION */
void printAST(ASTNode* node, int level);                        /* Pretty-print the AST */

#endif
