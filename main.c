#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"


// ------------------------------------------------------------------------
//  util
// ------------------------------------------------------------------------
void util_hexdump(uint8_t* mem, size_t n)
{
    int i = 0;
    uint8_t* c;

    printf("hex | ");
    while (i < n) {
        c = &(mem[i]);
        printf("%02x", *c);

        if (i%16 == 15) {
            printf("\n| ");
        }
        else if (i%8 == 7) {
            printf("  ");
        }
        else {
            printf(" ");
        }

        i++;
    }

    if (!i || i%16 != 0) {
        printf("\n");
    }
}

void util_free(void* item) 
{
    // printf("FREE %p\n", item);
    free(item);
}


// ------------------------------------------------------------------------
//  defines
// ------------------------------------------------------------------------
typedef struct bb_t {
    size_t          n;      // Number of bytes in s. This is not length.
    char*           s;
    size_t          l;      // Length of the string stored in the current Bb.

    // These fields are used when Bb is structured into a BbFifo.
    struct bb_t*    next;
    struct bb_t*    prev;
} Bb; // Byte Buffer


typedef struct bb_fifo_t {
    struct bb_t*    head;   // We pull from head.
    struct bb_t*    tail;   // We push to tail.
    size_t          n;      // Size
} BbFifo; // Fifo for Byte Buffer


typedef struct bw_t {
    char*           nail;
    char*           peri;

    // These fields are used when Bw is structured into a BwStack.
    struct bw_t*    prev;
} Bw; // Byte Window


typedef struct bw_stack_t {
    struct bw_t*    top;
    unsigned        n;      // SiZe
} BwStack;


uint16_t ENTRY_TYPE_FIRST = 0;
uint16_t ENTRY_TYPE_CFUNC = 1;
uint16_t ENTRY_TYPE_FORTH = 2;

typedef struct entry_t {
    uint16_t            entry_h;    // counter
    uint16_t            entry_type;
    struct entry_t*     prev;       // Used for stacking and pooling
    uint8_t*            next;       // Points to next peri
    Bw                  bw_name;
    union {
        void*           vp_cfunc;   // ENTRY_TYPE_CFUNC
        Bw              bw_forth;   // ENTRY_TYPE_CFORTH
    };
} Entry; // Dictionary entries


typedef enum token_type_t {
    TOKEN_TYPE_DICT_REF,
    TOKEN_TYPE_INT,
    TOKEN_TYPE_STRING,
    //TOKEN_TYPE_FLOAT,
} TokenType;

typedef struct token_t {
    enum token_type_t   token_type;
    struct token_t*     prev;
    union {
        Bw*             bw;     // TOKEN_TYPE_DICT_REF, _STRING
        int             n;      // TOKEN_TYPE_INT
    };
} Token; // Words that live on the live stack


typedef struct token_stack_t {
    Token*          top;
    int             n;
} TokenStack;

/*
 * Mode transitions. Each transition is bidirectional,
 *
 *      Weir      Read
 *          \    /    \
 *           Work      Rest
 *          /
 *      Slip
 *
 * If we ever need to add temporal scheduling (Time), I expect it should
 * go between Read and Work. This would not be real-time. But it would
 * take precedence over new reads.
 */
enum mill_mode_t {
    MILL_MODE_WEIR, // When the output queue is full.
    MILL_MODE_WORK, // When we are processing words.
    MILL_MODE_READ, // When there may be input for us to consume.
    MILL_MODE_REST, // When there is nothing to do.
    MILL_MODE_SLIP, // When there is an error to be collected.
};


enum parser_t {
    PARSER_NORMAL,
    PARSER_STRING,
};

typedef struct mill_t {
    enum mill_mode_t    mode;
    enum parser_t       parser;

    char                b_quit;
    char                b_echo;
                      
    void*               dict_mem;
    void*               dict_top;

    Bb*                 bb_buf_input;
    Bb*                 bb_buf_output;

    BbFifo*             bb_fifo_in_pool;
    BbFifo*             bb_fifo_in;
        // Stuff that is waiting to become bb_buf_input

    BbFifo*             bb_fifo_out_pool;
    BbFifo*             bb_fifo_out;
        // Stuff that has been sent for output (via bb_buf_output) but
        // not yet collected by whatever is enclosing the mill.

    BwStack*            bw_stack_work;
    BwStack*            bw_stack_pool;
        // Queued-up work

    TokenStack*         token_stack;
        // This is the algorithmic forth stack.
} Mill;

typedef void (*Cfunc)(Mill*);


// ------------------------------------------------------------------------
//  bb
// ------------------------------------------------------------------------
void
bb_debug(Bb* self);

static void
__bb_init(Bb* self, char* s, size_t n) 
{
    self->s = s;
    self->n = n;
    self->l = 0;
    self->next = NULL;
    self->prev = NULL;

    memset(self->s, 0, n);
}

static void
__bb_exit(Bb* self) 
{
    util_free(self->s);
}

Bb*
bb_new(size_t n) 
{
    Bb* bb = (Bb*) malloc(sizeof(Bb));
    char* s = (char*) malloc(n);
    if (s == NULL) { printf("WARNING: could not allocate bb."); }
    __bb_init(bb, s, n);
    return bb;
}

void
bb_del(Bb* self) 
{
    __bb_exit(self);
    util_free(self);
}

void
bb_clear(Bb* self) 
{
    self->l = 0;
}

size_t
bb_size(Bb* self) 
{
    return self->l;
}

void
bb_debug(Bb* self) 
{
    printf("{Bb %p l:%d s:%s n:%d next:%p prev:%p\n",
        self,
        (int) self->l,
        self->s,
        (int) self->n,
        self->next,
        self->prev);
    printf("}Bb\n");
}

void
bb_debug_hex(Bb* self) 
{
    uint8_t* s = (uint8_t*) self->s;
    util_hexdump(s, self->l);
}

void
bb_place(Bb* self, char* src, unsigned src_nail, unsigned src_peri) 
{
    // We can improve on this once we have implemented SLIP.
    if (src_peri - src_nail > self->n) {
        printf("WARNING: crash coming, string too long for bb.\n");
    }

    if (src_peri <= src_nail) {
        self->l = 0;
        return;
    }

    if (src_peri - src_nail > self->n) {
        src_peri = self->n - src_peri;
    }

    char* dst = self->s;
    int n = 0;
    for (int i=src_nail; i<src_peri; i++) {
        *(dst+n) = *(src+i);
        n++;
    }

    self->l = n;
}

void
bb_from_s(Bb* self, char* src) 
{
    unsigned src_nail = 0;
    unsigned src_peri = strlen(src);
    bb_place(self, src, src_nail, src_peri);
}

void
bb_from_bb(Bb* self, Bb* src) 
{
    if (src->l > self->n) {
        printf("WARNING: crash coming. Src Bb is too long for dst Bb.\n");
    }
    char* src_ptr = src->s;
    char* dst_ptr = self->s;
    while ( (*dst_ptr++ = *src_ptr++) );
    self->l = src->l;
}

void
bb_from_bw(Bb* self, Bw* bw) 
{
    int len = bw->peri - bw->nail;
    if (len > self->n) {
        printf("WARNING: crash coming. Bw string too long for Bb.\n");
    }

    char* w = bw->nail;
    for (int i=0; i<len; i++) {
        *(self->s+i) = *w++;
    }
    self->l = bw->peri - bw->nail;
}

void
bb_to_s(Bb* self, char* s) 
{
    char* src_ptr = self->s;
    char* dst_ptr = s;
    while( (*dst_ptr++ = *src_ptr++) );
    *s = 0;
}

// Returns 1 if equiv, 0 otherwise.
int
bb_equals(Bb* self, char* s) 
{
    int len = strlen(s);
    if (self->l != len) {
        return 0;
    }

    for (int i=0; i<len; i++) {
        if (self->s[i] != s[i]) {
            return 0;
        }
    }

    return 1;
}

void
bb_to_string(Bb* self, char* buf, size_t buf_len) 
{
    int peri = (buf_len-1 > self->l) ? self->l : buf_len-1;

    int i;
    for (i=0; i<peri; i++) {
        buf[i] = self->s[i];
    }
    buf[peri] = 0;
}

static char *
bb_test() 
{
    Bb* bb_a = bb_new(40);
    Bb* bb_b = bb_new(40);

    char* s = "a bb ccc   dddd  e  ff g hh i";
    bb_place(bb_a, s, 0, strlen(s));
    mu_assert(bb_equals(bb_a, s), ".");

    bb_del(bb_b);
    bb_del(bb_a);

    return NULL;
}



// ------------------------------------------------------------------------
//  bb fifo
// ------------------------------------------------------------------------
Bb*
bb_fifo_pull(BbFifo* self);

static void
__bb_fifo_init(BbFifo* self) 
{
    self->head = NULL;
    self->tail = NULL;
    self->n = 0;
}

static void
__bb_fifo_exit(BbFifo* self) 
{
    // This deallocates any bb held by the fifo.
    while (self->n > 0) {
        Bb* bb = bb_fifo_pull(self);
        bb_del(bb);
    }
}

BbFifo*
bb_fifo_new() 
{
    BbFifo* bb_fifo = (BbFifo*) malloc(sizeof(BbFifo));
    __bb_fifo_init(bb_fifo);
    return bb_fifo;
}

void
bb_fifo_del(BbFifo* self) 
{
    __bb_fifo_exit(self);
    util_free(self);
}

void
bb_fifo_debug(BbFifo* self) 
{
    printf("{bb_fifo_debug from tail (%zu)\n", self->n);
    Bb* bb = self->tail;
    while (bb != NULL) {
        bb_debug(bb);
        bb = bb->prev;
    }
    printf("} // bb_fifo_debug\n");
}

Bb*
bb_fifo_peek(BbFifo* self) 
{
    return self->head;
}

Bb*
bb_fifo_pull(BbFifo* self) 
{
    if (self->n == 0) {
        return NULL;
    }

    Bb* bb;
    if (self->n == 1) {
        bb = self->head;
        self->head = NULL;
        self->tail = NULL;
    }
    else {
        Bb* future_head = self->head->next;
        future_head->prev = NULL;

        bb = self->head;
        self->head = future_head;
    }
    bb->next = NULL;
    bb->prev = NULL;
    self->n--;
    return bb;
}

void
bb_fifo_push(BbFifo* self, Bb* bb) 
{
    if (self->n == 0) {
        self->head = bb;
        self->tail = bb;
    }
    else {
        bb->prev = self->tail;
        self->tail->next = bb;
        self->tail = bb;
    }

    self->n++;
}

size_t
bb_fifo_size(BbFifo* self) 
{
    return self->n;
}

static char*
bb_fifo_test() 
{
    BbFifo* bb_fifo = bb_fifo_new(); {
        { // Confirm that it starts empty
            mu_assert(
                bb_fifo_size(bb_fifo) == (size_t) 0,
                "BbFifo starts empty.");
        }

        { // Confirm an empty pull gives null
            mu_assert(
                bb_fifo_pull(bb_fifo) == NULL,
                "pull on empty list should be null.");
        }


        { // Create and pull with a single item
            Bb* bb_a = bb_new(20); bb_from_s(bb_a, "aaaaaaaaaa");

            Bb* bb_w = NULL;

            bb_fifo_push(bb_fifo, bb_a);
            mu_assert(
                bb_fifo_size(bb_fifo) == 1,
                "Size check.");

            bb_w = bb_fifo_pull(bb_fifo);
            mu_assert(
                bb_w == bb_a,
                "Retrieved item should be the sole item we pushed.");
            bb_del(bb_a);
        }

        { // Create and pull with multiple items
            Bb* bb_a = bb_new(10); bb_from_s(bb_a, "aaaaa");
            Bb* bb_b = bb_new(10); bb_from_s(bb_b, "bbbbb");
            Bb* bb_c = bb_new(10); bb_from_s(bb_c, "ccccc");
            Bb* bb_d = bb_new(10); bb_from_s(bb_d, "ddddd");
            Bb* bb_e = bb_new(10); bb_from_s(bb_e, "eeeee");

            Bb* bb_w = NULL;

            bb_fifo_push(bb_fifo, bb_a);
            bb_fifo_push(bb_fifo, bb_b);
            bb_fifo_push(bb_fifo, bb_c);
            mu_assert(
                bb_fifo_size(bb_fifo) == 3,
                "Size check.");

            bb_w = bb_fifo_pull(bb_fifo);

            mu_assert(
                bb_w == bb_a,
                "Retrieved item should be the first we pushed.");

            mu_assert(
                bb_fifo_size(bb_fifo) == 2,
                "Size check.");

            bb_fifo_push(bb_fifo, bb_d);
            mu_assert(
                bb_fifo_size(bb_fifo) == 3,
                "Size check.");

            mu_assert(
                bb_fifo_pull(bb_fifo) == bb_b,
                "Retrieved items should be in order.");
            mu_assert(
                bb_fifo_size(bb_fifo) == 2,
                "Size check.");

            mu_assert(
                bb_fifo_pull(bb_fifo) == bb_c,
                "Retrieved items should be in order.");
            mu_assert(
                bb_fifo_size(bb_fifo) == 1,
                "Size check.");

            mu_assert(
                bb_fifo_pull(bb_fifo) == bb_d,
                "Retrieved items should be in order.");
            mu_assert(
                bb_fifo_size(bb_fifo) == 0,
                "Size check.");

            bb_fifo_push(bb_fifo, bb_e);
            mu_assert(
                bb_fifo_size(bb_fifo) == 1,
                "Size check.");
            mu_assert(
                bb_fifo_pull(bb_fifo) == bb_e,
                "Retrieved items should be in order.");
            mu_assert(
                bb_fifo_size(bb_fifo) == 0,
                "Size check.");

            bb_del(bb_a);
            bb_del(bb_b);
            bb_del(bb_c);
            bb_del(bb_d);
            bb_del(bb_e);
        }
    }
    bb_fifo_del(bb_fifo);

    return NULL;
}


// ------------------------------------------------------------------------
//  bw
// ------------------------------------------------------------------------
size_t
bw_size(Bw* self);

static void
bw_init(Bw* self) 
{
    self->nail = NULL;
    self->peri = NULL;

    self->prev = NULL;
}

static
void bw_exit(Bw* self) {}

Bw*
bw_new() 
{
    Bw* self = (Bw*) malloc(sizeof(Bw));
    bw_init(self);
    return self;
}

void
bw_del(Bw* self) 
{
    bw_exit(self);
    util_free(self);
}

void
bw_debug(Bw* self) 
{
    printf("{Bw %p %p %lu {\n", self->nail, self->peri, self->peri-self->nail);
    uint8_t* s = (uint8_t*) self->nail;
    util_hexdump(s, self->peri-self->nail);
    printf("}Bw\n");
}

uint8_t
bw_equals_bw(Bw* self, Bw* other)
{
    size_t self_size = bw_size(self);

    if (self_size != bw_size(other)) return 0;

    for (int i=0; i<self_size; i++) {
        if (*(self->nail+i) != *(other->nail+i))
            return 0;
    }

    return 1;
}

uint8_t
bw_equals_s(Bw* self, char* s) 
{
    int len = strlen(s);
    if (len != self->peri - self->nail) {
        return 0;
    }

    char* t = self->nail;
    while (t < self->peri) {
        if (*t++ != *s++) {
            return 0;
        }
    }

    return 1;
}

void
bw_from_bb(Bw* self, Bb* src) 
{
    self->nail = src->s;
    self->peri = src->s + src->l;
}

void
bw_from_bw(Bw* self, Bw* src)
{
    self->nail = src->nail;
    self->peri = src->peri;
}

void
bw_from_s(Bw* self, char* s) 
{
    self->nail = s;
    self->peri = s + strlen(s);
}

void
bw_set(Bw* self, char* nail, char* peri) 
{
    self->nail = nail;
    self->peri = peri;
}

size_t
bw_size(Bw* self) 
{
    return self->peri - self->nail;
}

void
bw_to_s(Bw* self, char* buf, size_t buf_len) 
{
    int bw_len = self->peri - self->nail;
    if (bw_len >= buf_len-1) {
        bw_len = buf_len-1;
    }

    char* w = self->nail;

    int i;
    for (i=0; i<bw_len; i++) {
        if (w == self->peri) break;
        buf[i] = *w++;
    }
    buf[i] = 0;
}

void
bw_trim_left(Bw* self) 
{
    while (self->nail < self->peri) {
        if (*self->nail != ' ') break;
        self->nail++;
    }
}

void
bw_trim_right(Bw* self) 
{
    while (self->nail < self->peri) {
        if (*self->peri != ' ') break;
        self->peri--;
    }
}

static char*
bw_test() 
{
    Bw* bw = bw_new(); {
        { // bw_trim_left
            char* s = "  aaa";
            bw_set(bw, s, s+strlen(s));
            mu_assert(bw_size(bw) == 5, ".");
            bw_trim_left(bw);
            mu_assert(bw_size(bw) == 3, ".");
        }

        { // bw_set (simple) and bw_to_s
            char* s = "aaa bbb ccc";
            char* nail = s;
            char* peri = s+strlen(s);
            bw_set(bw, nail, peri);
            mu_assert(bw_equals_s(bw, s), ".");

            char buf[20];
            bw_to_s(bw, buf, 20);
            mu_assert(bw_equals_s(bw, buf), ".");
        }

        { // bw_set (offset) and bw_to_s
            char* s = "aaa bbb ccc";
            char* nail = s+4; // first b
            char* peri = s+9; // first c
            bw_set(bw, nail, peri);
            mu_assert(bw_equals_s(bw, "bbb c"), ".");

            char buf[20];
            bw_to_s(bw, buf, 20);
            mu_assert(bw_equals_s(bw, buf), ".");
        }

        { // bw_from_s
            char* s = "aaa bbb ccc";
            bw_from_s(bw, s);
            mu_assert(bw_equals_s(bw, s), ".");

            char buf[20];
            bw_to_s(bw, buf, 20);
            mu_assert(bw_equals_s(bw, buf), ".");
        }
    }
    bw_del(bw);

    return NULL;
}


// ------------------------------------------------------------------------
//  bw stack
// ------------------------------------------------------------------------
static void
__bw_stack_init(BwStack* self) 
{
    self->top = NULL;
    self->n = 0;
}

static void
__bw_stack_exit(BwStack* self) 
{
    Bw* bw = self->top;
    while (bw != NULL) {
        bw_del(bw);
        bw = bw->prev;
    }
}

BwStack*
bw_stack_new() 
{
    BwStack* bw_stack = (BwStack*) malloc(sizeof(BwStack));
    __bw_stack_init(bw_stack);
    return bw_stack;
}

void
bw_stack_del(BwStack* self) 
{
    __bw_stack_exit(self);
    util_free(self);
}

size_t
bw_stack_size(BwStack* self) 
{
    return self->n;
}

void
bw_stack_push(BwStack* self, Bw* bw) 
{
    bw->prev = self->top;
    self->top = bw;
    self->n++;
}

Bw*
bw_stack_top(BwStack* self) 
{
    return self->top;
}

Bw*
bw_stack_pop(BwStack* self) 
{
    Bw* bw = NULL;
    if (self->n) {
        bw = self->top;
        self->top = bw->prev;
        bw->prev = NULL;

        self->n--;
    }
    return bw;
}

// This attempts to pop from the supplied stack. If none are available,
// it creates a new instance.
Bw*
bw_stack_get(BwStack* self) 
{
    if (bw_stack_size(self)) {
        return bw_stack_pop(self);
    }
    else {
        return bw_new();
    }
}

// Moves the top item from self to pool.
void
bw_stack_move(BwStack* self, BwStack* dst) 
{
    Bw* bw = bw_stack_pop(self);
    bw_stack_push(dst, bw);
}

static char*
bw_stack_test() 
{
    // ** Declarations
    BwStack* bw_stack = NULL;
    Bb* bb_a = NULL;
    Bw* bw_a = NULL;
    Bb* bb_b = NULL;
    Bw* bw_b = NULL;

    // ** Test sequence
    bw_stack = bw_stack_new();
    mu_assert(bw_stack_size(bw_stack) == 0, "Stack size");
    mu_assert(bw_stack_top(bw_stack) == NULL, "Stack top");
    mu_assert(bw_stack_pop(bw_stack) == NULL, "Stack pop");

    bb_a = bb_new(40); bb_from_s(bb_a, "aaaaa");
    bw_a = bw_new(); bw_from_bb(bw_a, bb_a);

    bb_b = bb_new(40); bb_from_s(bb_b, "bbbbb");
    bw_b = bw_new(); bw_from_bb(bw_b, bb_b);

    bw_stack_push(bw_stack, bw_a);
    mu_assert(bw_stack_size(bw_stack) == 1, "todo");
    mu_assert(bw_stack_top(bw_stack) == bw_a, "todo");

    bw_stack_push(bw_stack, bw_b);
    mu_assert(bw_stack_size(bw_stack) == 2, "todo");
    mu_assert(bw_stack_top(bw_stack) == bw_b, "todo");

    Bw* bw_w;

    bw_w = bw_stack_pop(bw_stack);
    mu_assert(bw_w == bw_b, "todo");
    mu_assert(bw_stack_size(bw_stack) == 1, "todo");
    mu_assert(bw_stack_top(bw_stack) == bw_a, "todo");

    bw_w = bw_stack_pop(bw_stack);
    mu_assert(bw_w == bw_a, "todo");
    mu_assert(bw_stack_size(bw_stack) == 0, "todo");
    mu_assert(bw_stack_top(bw_stack) == NULL, "todo");

    bw_w = bw_stack_pop(bw_stack);
    mu_assert(bw_w == NULL, "todo");
    mu_assert(bw_stack_size(bw_stack) == 0, "todo");
    mu_assert(bw_stack_top(bw_stack) == NULL, "todo");

    // ** Cleanup
    bw_del(bw_b);
    bb_del(bb_b);
    bw_del(bw_a);
    bb_del(bb_a);
    bw_stack_del(bw_stack);

    return NULL;
}


// ------------------------------------------------------------------------
//  token
// ------------------------------------------------------------------------
static void
token_init(Token* self, TokenType token_type)
{
    self->token_type = token_type;
    self->prev = NULL;
    switch(token_type) {
    case TOKEN_TYPE_DICT_REF:
    case TOKEN_TYPE_STRING:
        self->bw = bw_new();
        break;
    case TOKEN_TYPE_INT:
        self->n = 0;
        break;
    }
}

static void
token_exit(Token* self)
{
    switch(self->token_type) {
    case TOKEN_TYPE_DICT_REF:
    case TOKEN_TYPE_STRING:
        bw_del(self->bw);
    case TOKEN_TYPE_INT:
        self->n = 0;
        break;
    }
}

Token*
token_new(TokenType token_type)
{
    Token* self = (Token*) malloc(sizeof(Token));
    token_init(self, token_type);
    return self;
}

void
token_del(Token* self)
{
    token_exit(self);
    util_free(self);
}

static char*
token_test()
{
    Token* self = token_new(TOKEN_TYPE_INT);
    util_free(self);

    return NULL;
}


// ------------------------------------------------------------------------
//  token stack
// ------------------------------------------------------------------------
static void
__token_stack_init(TokenStack* self)
{
    self->top = NULL;
    self->n = 0;
}

static void
__token_stack_exit(TokenStack* self)
{
    while (self->top != NULL) {
        Token* token = self->top;
        self->top = token->prev;
        token_del(token);
    }
}

static TokenStack*
token_stack_new()
{
    TokenStack* self = (TokenStack*) malloc(sizeof(TokenStack));
    __token_stack_init(self);
    return self;
}

void
token_stack_del(TokenStack* self)
{
    __token_stack_exit(self);
    util_free(self);
}

Token*
token_stack_pop(TokenStack* self)
{
    Token* token = self->top;
    self->top = token->prev;
    token->prev = NULL;
    self->n--;
    return token;
}

void
token_stack_push(TokenStack* self, Token* token)
{
    token->prev = self->top;
    self->top = token;
    self->n++;
}

size_t
token_stack_size(TokenStack* self)
{
    return self->n;
}

Token*
token_stack_top(TokenStack* self)
{
    return self->top;
}

static char*
token_stack_test()
{
    TokenStack* token_stack = token_stack_new();
    mu_assert(token_stack_size(token_stack) == 0, "size");
    mu_assert(token_stack_top(token_stack) == NULL, "top");

    Token* token_a = token_new(TOKEN_TYPE_INT);
    Token* token_b = token_new(TOKEN_TYPE_INT);

    token_stack_push(token_stack, token_a);
    mu_assert(token_stack_size(token_stack) == 1, "size");
    mu_assert(token_stack_top(token_stack) == token_a, "top");

    token_stack_push(token_stack, token_b);
    mu_assert(token_stack_size(token_stack) == 2, "size");
    mu_assert(token_stack_top(token_stack) == token_b, "top");

    Token* w;
    
    w = token_stack_pop(token_stack);
    mu_assert(w == token_b, "pop");
    mu_assert(token_stack_size(token_stack) == 1, "size");

    w = token_stack_pop(token_stack);
    mu_assert(w == token_a, "pop");
    mu_assert(token_stack_size(token_stack) == 0, "size");

    token_del(token_b);
    token_del(token_a);
    token_stack_del(token_stack);

    return NULL;
}


// ------------------------------------------------------------------------
//  cfunc
// ------------------------------------------------------------------------
void cfunc_first(Mill* self) {}

void cfunc_dot_s(Mill* self) {
    
}

void cfunc_dup(Mill* self) {
    printf("xxx cfunc_dup\n");
}


// ------------------------------------------------------------------------
//  mill
// ------------------------------------------------------------------------
//
// xxx implement words as a default word in the dict. this will be the
// bootstrap word.
//
void
mill_input(Mill* self, Bw* bw);

void
mill_dict_register_cfunc(Mill* self, char* ename, Cfunc cfunc);

static void
__mill_init(Mill* self, size_t dict_size, size_t word_size,
        size_t fifo_in_size, size_t fifo_out_size) 
{
    /* dict_size: number of bytes available to the dictionary.
     * word_size: maximum length of forth words in the queues.
     * fifo_in_size: max number of words that can be buffered in fifo_in.
     * fifo_out_size: max number of words that can be buffered in fifo_out.
     */

    self->mode = MILL_MODE_REST;
    self->parser = PARSER_NORMAL;

    self->b_quit = 0;
    self->b_echo = 1;

    self->dict_mem = (uint8_t*) malloc(sizeof(uint8_t) * dict_size);
    self->dict_top = self->dict_mem; {
        // Populate the first entry into the dictionary.
        Entry* entry = (Entry*) self->dict_mem;
        entry->entry_h = 0;
        entry->entry_type = ENTRY_TYPE_FIRST;
        entry->prev = NULL;
        entry->next = self->dict_mem + sizeof(Entry);
        bw_init(&entry->bw_name);
        entry->bw_name.nail = 0;
        entry->bw_name.peri = 0;
        entry->vp_cfunc = NULL;
    }

    self->bb_buf_input = bb_new(word_size);
    self->bb_buf_output = bb_new(word_size);

    self->bb_fifo_in_pool = bb_fifo_new(); {
        for (int i=0; i<fifo_in_size; i++) {
            Bb* bb = bb_new(word_size);
            bb_fifo_push(self->bb_fifo_in_pool, bb);
        }
    }
    self->bb_fifo_in = bb_fifo_new();

    self->bb_fifo_out_pool = bb_fifo_new(); {
        for (int i=0; i<fifo_out_size; i++) {
            Bb* bb = bb_new(word_size);
            bb_fifo_push(self->bb_fifo_out_pool, bb);
        }
    }
    self->bb_fifo_out = bb_fifo_new();

    self->bw_stack_work = bw_stack_new();
    self->bw_stack_pool = bw_stack_new();

    self->token_stack = token_stack_new();
}

static void __mill_exit(Mill* self) 
{
    util_free(self->dict_mem);
    self->dict_mem = 0;
    self->dict_top = 0;

    bb_del(self->bb_buf_input);
    bb_del(self->bb_buf_output);

    bb_fifo_del(self->bb_fifo_in_pool);
    bb_fifo_del(self->bb_fifo_in);
    bb_fifo_del(self->bb_fifo_out_pool);
    bb_fifo_del(self->bb_fifo_out);

    bw_stack_del(self->bw_stack_work);
    bw_stack_del(self->bw_stack_pool);

    token_stack_del(self->token_stack);
}

Mill* mill_new(size_t dict_size, size_t word_size, size_t fifo_in_size,
        size_t fifo_out_size) 
{
    Mill* mill = (Mill*) malloc(sizeof(Mill));
    __mill_init(mill, dict_size, word_size, fifo_in_size, fifo_out_size);
    return mill;
}

void mill_del(Mill* self) 
{
    __mill_exit(self);
    util_free(self);
}

void mill_debug(Mill* self) 
{
    printf("{Mill %p\n", self);
    switch (self->mode) {
    case MILL_MODE_WEIR:
        printf("  MILL_MODE_WEIR\n");
        break;
    case MILL_MODE_WORK:
        printf("  MILL_MODE_WORK\n");
        break;
    case MILL_MODE_READ:
        printf("  MILL_MODE_READ\n");
        break;
    case MILL_MODE_REST:
        printf("  MILL_MODE_REST\n");
        break;
    case MILL_MODE_SLIP:
        printf("  MILL_MODE_SLIP\n");
        break;
    }
    printf("}\n");
}

Entry*
mill_dict_get_next_entry(Mill* mill, uint16_t entry_type)
{
    Entry* old_top = (Entry*) mill->dict_top;

    Entry* new_top = (Entry*) old_top->next;
    new_top->entry_h = old_top->entry_h + 1;
    new_top->entry_type = entry_type;
    new_top->prev = old_top;

    mill->dict_top = (uint8_t*) new_top;

    return new_top;
}

void
mill_dict_register_cfunc(Mill* self, char* ename, Cfunc cfunc)
{
    uint16_t entry_type = ENTRY_TYPE_CFUNC;
    Entry* entry = mill_dict_get_next_entry(self, entry_type);

    size_t len;
    Bw* bw;

    // The character data of the name comes after the entry struct in
    // the dictionary's memory reservation.
    char* next = (char*) entry + sizeof(Entry);
    len = strlen(ename);
    bw = &entry->bw_name;
    bw->nail = next;
    bw->peri = next + len;
    for (int i=0; i<len; i++) {
        *(entry->bw_name.nail + i) = ename[i];
    }

    // The link to the C function takes no extra memory from reservation.
    next = bw->peri;
    entry->vp_cfunc = cfunc;

    entry->next = (uint8_t*) next;
}

void
mill_dict_register_forth(Mill* self, char* ename, char* forth)
{
    uint16_t entry_type = ENTRY_TYPE_FORTH;
    Entry* entry = mill_dict_get_next_entry(self, entry_type);

    size_t len;
    Bw* bw;

    // The character data of the name comes after the entry struct in
    // the dictionary's memory reservation.
    char* next = (char*) entry + sizeof(Entry);
    len = strlen(ename);
    bw = &entry->bw_name;
    bw->nail = next;
    bw->peri = next + len;
    for (int i=0; i<len; i++) {
        *(entry->bw_name.nail + i) = ename[i];
    }

    // The forth code sits in dictionary memory after the name.
    next = bw->peri;
    len = strlen(forth);
    bw = &entry->bw_forth;
    bw->nail = next;
    bw->peri = next + len;
    for (int i=0; i<len; i++) {
        *(entry->bw_forth.nail + i) = forth[i];
    }

    next = bw->peri;
    entry->next = (uint8_t*) next;
}

void
mill_dict_register_defaults(Mill* self) 
{
    Bw bw;
    Cfunc cfunc;

    cfunc = cfunc_dup;
    mill_dict_register_cfunc(self, "dup", cfunc);

    bw_from_s(&bw, ": double dup + ;");
    mill_input(self, &bw);
}

Entry*
mill_dict_search(Mill* self, Bw* bw)
{
    Entry* entry = (Entry*) self->dict_top;
    while (entry->entry_type != ENTRY_TYPE_FIRST) {
        if (bw_equals_bw(bw, &entry->bw_name))
            return entry;

        entry = entry->prev;
    }

    return NULL;
}

static void __mill_to_mode_weir(Mill* self) 
{
    printf("To MILL_MODE_WEIR\n"); // xxx
    self->mode = MILL_MODE_WEIR;
}

static void __mill_to_mode_work(Mill* self) 
{
    printf("To MILL_MODE_WORK\n"); // xxx
    self->mode = MILL_MODE_WORK;
}

static void __mill_to_mode_read(Mill* self) 
{
    printf("To MILL_MODE_READ\n"); // xxx
    self->mode = MILL_MODE_READ;
}

static void __mill_to_mode_rest(Mill* self) 
{
    printf("To MILL_MODE_REST\n"); // xxx
    self->mode = MILL_MODE_REST;
}

static void __mill_to_mode_slip(Mill* self) 
{
    printf("To MILL_MODE_SLIP\n"); // xxx
    self->mode = MILL_MODE_SLIP;
}

// Returns 1 if it successfully parsed an int. Otherwise 0.
static uint8_t
__mill_numbers_parse_int(Mill* self, Bw* bw, int* acc)
{
    int n = 0;

    // Do an initial pass of the bw to make sure it is an int.
    size_t length = bw_size(bw);
    uint8_t b_looks_ok = 1;
    uint8_t b_negate = 0;
    for (int i=0; b_looks_ok && i<length; i++) {
        switch(*(bw->nail+i)) {
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            n = (n<<3) + (n<<1) + *(bw->nail+i) - '0';
            break;
        case '-':
            if (i == 0) {
                b_negate = 1;
                break;
            }
        default:
            b_looks_ok = 0;
        }
    }

    if (!b_looks_ok) {
        return 0;
    }

    if (b_negate) n *= -1;

    *acc = n;
    return 1;
}

static void
__mill_on_word(Mill* self, Bw* bw) 
{
    printf("xxx __mill_on_word\n");

    // Control defines
    if (bw_equals_s(bw, ".q")) {
        self->b_quit = 1;
        return;
    }
    if (bw_equals_s(bw, ".s")) {
        printf("xxx print stack.\n");
        return;
    }
    if (bw_equals_s(bw, ".echo")) {
        self->b_echo = 1;
        return;
    }
    if (bw_equals_s(bw, ".noecho")) {
        self->b_echo = 0;
        return;
    }

    // Attempt to find it in the dictionary
    Entry* entry = mill_dict_search(self, bw);
    if (entry != NULL) {
        printf("xxx use entry.\n");
    }

    // If it wasn't in the dictionary, try to run it through numbers.
    if (entry == NULL) {
        int acc = 0;
        uint8_t rcode;
        
        rcode = __mill_numbers_parse_int(self, bw, &acc);
    }
}

static void
__mill_process_one_word(Mill* self, Bw* bw) 
{
    // Allocate
    Bw* word = bw_stack_get(self->bw_stack_pool);

    // Locate a single word
    word->nail = bw->nail++;
    word->peri = bw->nail;
    while (bw->nail < bw->peri) {
        if (*bw->nail == ' ') break;

        word->peri++;
        bw->nail++;
    }

    // Process it
    __mill_on_word(self, bw);

    // Cleanup
    bw_stack_push(self->bw_stack_pool, word);
}

static void
__mill_do_work(Mill* self) 
{
    Bw* bw = bw_stack_top(self->bw_stack_work);
    bw_trim_left(bw);
    if (bw_size(bw)) {
        __mill_process_one_word(self, bw);
    }
    else {
        bw_stack_move(self->bw_stack_work, self->bw_stack_pool);
    }

    // If there is no work left to do, retreat to read mode.
    if (bw_stack_size(self->bw_stack_work) == 0) {
        __mill_to_mode_read(self);
    }
}

static void
__mill_do_read(Mill* self) 
{
    // If we get to the end of this function and have not done a read, then we
    // will want to tell the Mill to put itself into Rest.
    if (bb_fifo_size(self->bb_fifo_in) > 0) {
        // When there is content to read, we prime the work context to read
        // the word from the input buffer.
        //
        // Move the data from fifo into our input buffer.
        Bb* bb = bb_fifo_pull(self->bb_fifo_in);
        bb_from_bb(self->bb_buf_input, bb);
        bb_fifo_push(self->bb_fifo_in_pool, bb);

        // Prime the mill to be ready for Work against this new buffer.
        Bw* bw = bw_stack_get(self->bw_stack_pool);
        bw_from_bb(bw, self->bb_buf_input);
        bw_stack_push(self->bw_stack_work, bw);

        __mill_to_mode_work(self);
    }
    else {
        // When there is no content to read, the mode falls back to rest.
        __mill_to_mode_rest(self);
    }
}

void
mill_input(Mill* self, Bw* bw) 
{
    void enqueue() {
        Bb* bb = bb_fifo_pull(self->bb_fifo_in_pool);
        if (bb == NULL) {
            // xxx modify later to support Slip behaviour.
            printf("WARNING: input pool was empty, crash coming.\n");
        }

        bw_trim_right(bw);
        bb_from_bw(bb, bw);
        bb_fifo_push(self->bb_fifo_in, bb);
    }

    switch (self->mode) {
    case MILL_MODE_REST:
        __mill_to_mode_read(self);
    case MILL_MODE_WEIR:
    case MILL_MODE_WORK:
    case MILL_MODE_READ:
        enqueue();
    case MILL_MODE_SLIP:
        break;
    }
}

// Tells us whether the mill has input waiting, or work to do.
uint8_t
mill_is_active() 
{
    printf("xxx mill_active\n"); // xxx
    return 1; // xxx
}

int
mill_is_input_ready(Mill* self) 
{
    return bb_fifo_size(self->bb_fifo_in_pool);
}

char
mill_is_quitting(Mill* self) 
{
    return self->b_quit;
}

// Returns any unused gas
unsigned
mill_power(Mill* self, unsigned gas) 
{
    int b_continue = 1;
    while (b_continue && gas) {
        switch (self->mode) {
        case MILL_MODE_WEIR:
            b_continue = 0;
            break;
        case MILL_MODE_WORK:
            __mill_do_work(self);
            break;
        case MILL_MODE_READ:
            __mill_do_read(self);
            break;
        case MILL_MODE_REST:
        case MILL_MODE_SLIP:
            b_continue = 0;
            break;
        }

        gas--;
    }
    return gas;
}

static char*
mill_test() 
{
    { // parse int
        printf("*** mill_test  parse int *********\n");
        Mill* self = NULL;
        Bw* bw = NULL; {
            size_t dict_size = (1024*1024) * 40;
            size_t word_size = 16;
            size_t fifo_in_size = 16;
            size_t fifo_out_size = 16;
            self = mill_new(dict_size, word_size, fifo_in_size, fifo_out_size);

            // Bw: We use this to pass instructions to the mill.
            bw = bw_new();
        }

        int acc = 0;
        uint8_t rcode;

        bw_from_s(bw, "100");
        rcode = __mill_numbers_parse_int(self, bw, &acc);
        mu_assert(rcode == 1, "rcode wrong");
        mu_assert(acc == 100, "value wrong");

        bw_from_s(bw, "-101");
        rcode = __mill_numbers_parse_int(self, bw, &acc);
        mu_assert(rcode == 1, "rcode wrong");
        mu_assert(acc == -101, "value wrong");

        bw_from_s(bw, " ");
        acc = 5;
        rcode = __mill_numbers_parse_int(self, bw, &acc);
        mu_assert(rcode == 0, "rcode wrong");
        mu_assert(acc == 5, "value changed"); // unchanged

        bw_from_s(bw, ".");
        acc = 5;
        rcode = __mill_numbers_parse_int(self, bw, &acc);
        mu_assert(rcode == 0, "rcode wrong");
        mu_assert(acc == 5, "value changed"); // unchanged

        bw_from_s(bw, "4.3");
        acc = 5;
        rcode = __mill_numbers_parse_int(self, bw, &acc);
        mu_assert(rcode == 0, "rcode wrong");
        mu_assert(acc == 5, "value changed"); // unchanged

        bw_from_s(bw, "--10");
        rcode = __mill_numbers_parse_int(self, bw, &acc);
        mu_assert(rcode == 0, "rcode wrong");

        bw_from_s(bw, "000"); // Leading zeros are valid.
        rcode = __mill_numbers_parse_int(self, bw, &acc);
        mu_assert(rcode == 1, "rcode wrong");

        // cleanup
        bw_del(bw);
        mill_del(self);
    }

    { // dictionary basics
        printf("\n\n^^ dictionary basics\n"); // xxx
        Bw* bw = bw_new();
        Mill* self = NULL; {
            size_t dict_size = (1024*1024) * 40;
            size_t word_size = 16;
            size_t fifo_in_size = 16;
            size_t fifo_out_size = 16;
            self = mill_new(dict_size, word_size, fifo_in_size, fifo_out_size);
        }
        mu_assert(((Entry*) self->dict_top)->entry_h == 0, "entry h")

        mill_dict_register_cfunc(self, "dup", cfunc_dup);
        mu_assert(((Entry*) self->dict_top)->entry_h == 1, "entry h")

        mill_dict_register_forth(self, "2dup", "dup dup");
        mu_assert(((Entry*) self->dict_top)->entry_h == 2, "entry h")

        Entry* entry;

        bw_from_s(bw, "unknown_word");
        entry = mill_dict_search(self, bw);
        mu_assert(entry == NULL, ".");

        bw_from_s(bw, "dup");
        entry = mill_dict_search(self, bw);
        mu_assert(entry != NULL, ".");

        printf("xxx test for dictionary basics\n");

        bw_del(bw);
        mill_del(self);
    }

    { // define a new word
        printf("*** mill_test define new word ****\n");
        Mill* self = NULL;
        Bw* bw = NULL; {
            // Mill: this is what we are testing
            size_t dict_size = (1024*1024) * 40;
            size_t word_size = 16;
            size_t fifo_in_size = 16;
            size_t fifo_out_size = 16;
            self = mill_new(dict_size, word_size, fifo_in_size, fifo_out_size);

            // Bw: We use this to pass instructions to the mill.
            bw = bw_new();
        }

        { // use the echo mode to test input and output
            char* s;
            s = ".echo aa bb cc\n"; bw_set(bw, s+0, s+5);

            mill_input(self, bw);

            unsigned gas = 10;
            gas = mill_power(self, gas);
            printf("Remaining gas %d\n", gas); // xxx
        }

        // cleanup
        bw_del(bw);
        mill_del(self);
    }

    printf("*** mill_test() end **************\n");

    return NULL;
}


// ------------------------------------------------------------------------
//  alg
// ------------------------------------------------------------------------
char*
all_tests() 
{
    mu_suite_start();

    mu_run_test(bb_test);
    mu_run_test(bb_fifo_test);
    mu_run_test(bw_test);
    mu_run_test(bw_stack_test);
    mu_run_test(token_test);
    mu_run_test(token_stack_test);
    mu_run_test(mill_test);

    return NULL;
}

#define REPL_LOOP_BUFFER_SIZE 4096
void
repl(Mill* mill) 
{
    // allocate bb to represent a repl line
    Bb* bb_input = bb_new(30); {
        char buf[REPL_LOOP_BUFFER_SIZE];
        size_t n;
        while (!mill_is_quitting(mill)) {
            fgets((char*) buf, REPL_LOOP_BUFFER_SIZE-1, stdin);

            n = strlen((char*) buf) - 1; // eliminates the newline
            bb_place(bb_input, &buf[0], 0, n);

            //__mill_parse(mill, bb_input);
        }
    }
    bb_del(bb_input);
}

void
alg() 
{
    size_t dict_size = (1024*1024) * 40;
    size_t word_size = 16;
    size_t fifo_in_size = 16;
    size_t fifo_out_size = 16;

    printf("aaa\n");

    Mill* mill = mill_new(dict_size, word_size, fifo_in_size, fifo_out_size); {
        mill_dict_register_defaults(mill);

        printf(".\n");
        repl(mill);
    }
    mill_del(mill);
}

//
// Only one line should be enabled here.
//
//RUN_TESTS(all_tests);
int main() { mill_test(); return 0; }

//int main() { alg(); return 0; }

