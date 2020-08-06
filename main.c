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
            printf("\nhex | ");
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
    unsigned        n;      // Size
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
 * Mode transitions. Each transition is bidirectional, except to quit.
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
    PARSER_ECHO,    // xxx Remove this parser as the system stablises.
    PARSER_NORMAL,
    PARSER_STRING,
};

typedef struct mill_t {
    enum mill_mode_t    mode;
    enum parser_t       parser;

    char                b_quit;

    void*               dict_mem;
    void*               dict_top;

    Bb*                 bb_buf_input;
        // Src: bb_fifo_in       Dst: MILL_MODE_WORK
    Bb*                 bb_buf_output;
        // Src: MILL_MODE_WORK   Dst: bb_fifo_out

    BbFifo*             bb_fifo_in_pool;
    BbFifo*             bb_fifo_in;
        // Words that are waiting to become bb_buf_input.

    BbFifo*             bb_fifo_out_pool;
    BbFifo*             bb_fifo_out;
        // Words that the composer is yet to collect.

    BwStack*            bw_stack_work;
    BwStack*            bw_stack_pool;
        // Queued-up work

    TokenStack*         token_stack_live;
    TokenStack*         token_stack_pool;
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

size_t
bb_capacity(Bb* self)
{
    return self->n;
}

void
bb_clear(Bb* self) 
{
    self->l = 0;
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

size_t
bb_length(Bb* self) 
{
    return self->l;
}

void
bb_place_to(Bb* self, char* src, unsigned dst_offset, unsigned src_offset_nail, unsigned src_offset_peri)
{
    // We can improve on this once we have implemented SLIP.
    if (src_offset_peri - src_offset_nail > self->n) {
        printf("WARNING: crash coming, string too long for bb.\n");
    }

    if (src_offset_peri <= src_offset_nail) {
        self->l = 0;
        return;
    }

    if (src_offset_peri - src_offset_nail > self->n) {
        src_offset_peri = self->n - src_offset_peri;
    }

    char* dst = self->s;
    int n = dst_offset;
    for (int i=src_offset_nail; i<src_offset_peri; i++) {
        *(dst+n) = *(src+i);
        n++;
    }

    self->l = n;
}

void
bb_place(Bb* self, char* src, unsigned src_offset_nail, unsigned src_offset_peri) 
{
    bb_place_to(self, src, 0, src_offset_nail, src_offset_peri);
}

void
bb_from_s(Bb* self, char* src) 
{
    unsigned src_offset_nail = 0;
    unsigned src_offset_peri = strlen(src);
    bb_place(self, src, src_offset_nail, src_offset_peri);
}

void
bb_from_s_append(Bb* self, char* src) 
{
    unsigned src_offset_nail = 0;
    unsigned src_offset_peri = strlen(src);
    unsigned dst_offset = self->l;
    bb_place_to(self, src, dst_offset, src_offset_nail, src_offset_peri);
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
        printf("Crash coming. Bw too long %d for Bb %zu.\n", len, self->n);
    }

    char* w = bw->nail;
    for (int i=0; i<len; i++) {
        *(self->s+i) = *w++;
    }
    self->l = bw->peri - bw->nail;
}

void
bb_from_bw_append(Bb* self, Bw* bw) 
{
    int dst_offset = self->l;

    int len = bw->peri - bw->nail;
    if (len > self->n) {
        printf("Crash coming. Bw too long %d for Bb %zu.\n", len, self->n);
    }

    char* w = bw->nail;
    for (int i=0; i<len; i++) {
        *(self->s+dst_offset+i) = *w++;
    }
    self->l = bw->peri - bw->nail;
}

void
bb_to_s(Bb* self, char* s) 
{
    char* src_ptr = self->s;
    char* dst_ptr = s;
    size_t len = bb_length(self);
    for (int i=0; i<len; i++) {
        *dst_ptr++ = *src_ptr++;
    }
    *dst_ptr = 0;
}

// Returns 1 if equiv, 0 otherwise.
int
bb_equals_s(Bb* self, char* s) 
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

    { // bb_from_s; bb_length; bb_clear
        bb_from_s(bb_a, "abc");
        mu_assert(bb_length(bb_a) == 3, ".");

        bb_clear(bb_a);
        mu_assert(bb_length(bb_a) == 0, ".");
    } bb_clear(bb_a);
      bb_clear(bb_b);

    { // bb_place basic; bb_to_s;
        char* s = "a bb ccc   dddd  e  ff g hh i";
        bb_place(bb_a, s, 0, strlen(s));
        mu_assert(bb_equals_s(bb_a, s), ".");

        char* dst = (char*) malloc(400);
        bb_to_s(bb_a, dst);
        mu_assert(strcmp(s, dst) == 0, "strcmp");
        free(dst);
    } bb_clear(bb_a);
      bb_clear(bb_b);

    { // bb_place; bb_clear; bb_append
        bb_from_s(bb_a, "jkl");
        bb_from_s_append(bb_a, "mnop");

        char* s = "jklmnop";
        char* dst = (char*) malloc(400);
        bb_to_s(bb_a, dst);
        mu_assert(strcmp(s, dst) == 0, "strcmp");
        free(dst);
    } bb_clear(bb_a);
      bb_clear(bb_b);

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

    printf("|");
    for (int i=0; i < (int) (self->peri - self->nail); i++) {
        printf("%c", *(self->nail+i));
    }
    printf("|\n");

    printf("}Bw\n");
}

void
bw_debug_hex(Bw* self) 
{
    printf("{Bw (hex) %p %p %lu {\n", self->nail, self->peri, self->peri-self->nail);

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
    for (int i=0; i<len; i++) {
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
        if (*self->nail == ' ' || *self->nail == '\n') {
            self->nail++;
        }
        else {
            break;
        }
    }
}

void
bw_trim_right(Bw* self) 
{
    char* last = self->peri - 1;
    while (self->nail <= last) {
        if (*last == ' ' || *last == '\n') {
            last--;
        }
        else {
            break;
        }
    }
    self->peri = last + 1;
}

static char*
bw_test() 
{
    Bw* bw = bw_new(); {
        { // bw_trim_left
            char* s = "  aaa \n";
            bw_set(bw, s, s+strlen(s));
            mu_assert(bw_size(bw) == 7, ".");
            bw_trim_left(bw);
            mu_assert(bw_size(bw) == 5, ".");
            bw_trim_right(bw);
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
Bw*
bw_stack_pop(BwStack* self);

void
bw_stack_push(BwStack* self, Bw* bw);

size_t
bw_stack_size(BwStack* self);

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

void
bw_stack_push(BwStack* self, Bw* bw) 
{
    bw->prev = self->top;
    self->top = bw;
    self->n++;
}

size_t
bw_stack_size(BwStack* self) 
{
    return self->n;
}

Bw*
bw_stack_top(BwStack* self) 
{
    return self->top;
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
token_stack_init(TokenStack* self)
{
    self->top = NULL;
    self->n = 0;
}

static void
token_stack_exit(TokenStack* self)
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
    token_stack_init(self);
    return self;
}

void
token_stack_del(TokenStack* self)
{
    token_stack_exit(self);
    util_free(self);
}

Token*
token_stack_pop(TokenStack* self, TokenType token_type)
{
    Token* token = self->top;
    self->top = token->prev;

    token->token_type = token_type;
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

// Source from pool or create.
Token*
token_stack_get(TokenStack* self, TokenType token_type)
{
    if (token_stack_size(self)) {
        return token_stack_pop(self, token_type);
    }
    else {
        return token_new(token_type);
    }
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
    
    w = token_stack_pop(token_stack, TOKEN_TYPE_INT);
    mu_assert(w == token_b, "pop");
    mu_assert(token_stack_size(token_stack) == 1, "size");

    w = token_stack_pop(token_stack, TOKEN_TYPE_INT);
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

void cfunc_empty(Mill* self) {
    printf("xxx cfunc_empty\n");
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
mill_init(Mill* self, size_t dict_size, size_t word_size,
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

    self->token_stack_live = token_stack_new();
    self->token_stack_pool = token_stack_new();
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

    token_stack_del(self->token_stack_live);
    token_stack_del(self->token_stack_pool);
}

Mill* mill_new(size_t dict_size, size_t word_size, size_t fifo_in_size,
        size_t fifo_out_size) 
{
    Mill* mill = (Mill*) malloc(sizeof(Mill));
    mill_init(mill, dict_size, word_size, fifo_in_size, fifo_out_size);
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

void
mill_dict_debug(Mill* self)
{
    Entry* ent = (Entry*) self->dict_top;
    while (ent->entry_type != ENTRY_TYPE_FIRST) {
        bw_debug(&ent->bw_name);
        ent = ent->prev;
    }
}

Entry*
mill_dict_get_next_entry(Mill* self, uint16_t entry_type)
{
    Entry* old_top = (Entry*) self->dict_top;

    Entry* new_top = (Entry*) old_top->next;
    new_top->entry_h = old_top->entry_h + 1;
    new_top->entry_type = entry_type;
    new_top->prev = old_top;

    self->dict_top = (uint8_t*) new_top;

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

size_t
mill_dict_size(Mill* self)
{
    Entry* ent = (Entry*) self->dict_top;

    // The first entry in the dict is burnt, and not counted.
    size_t n = 0;
    while (ent->entry_type != ENTRY_TYPE_FIRST) {
        n++;
        ent = ent->prev;
    }

    return n;
}

void
mill_dict_register_defaults(Mill* self) 
{
    Bw bw;
    Cfunc cfunc;

    cfunc = cfunc_empty;
    mill_dict_register_cfunc(self, "empty", cfunc);

    cfunc = cfunc_dup;
    mill_dict_register_cfunc(self, "dup", cfunc);

    // xxx
    printf("xxx debug mill dict\n");
    mill_dict_debug(self);
    printf("xxx mill_dict_size %zu\n", mill_dict_size(self));

    //bw_from_s(&bw, ": double dup + ;");
    //mill_input(self, &bw);
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

static int
__mill_is_mode_weir(Mill* self)
{
    return self->mode == MILL_MODE_WEIR;
}

static void
__mill_to_mode_weir(Mill* self) 
{
    printf("    To MILL_MODE_WEIR\n"); // xxx
    self->mode = MILL_MODE_WEIR;
}

static void
__mill_to_mode_work(Mill* self) 
{
    printf("    To MILL_MODE_WORK\n"); // xxx
    self->mode = MILL_MODE_WORK;
}

static void
__mill_to_mode_read(Mill* self) 
{
    printf("    To MILL_MODE_READ\n"); // xxx
    self->mode = MILL_MODE_READ;
}

static void
__mill_to_mode_rest(Mill* self) 
{
    printf("    To MILL_MODE_REST\n"); // xxx
    self->mode = MILL_MODE_REST;
}

static void
__mill_to_mode_slip(Mill* self) 
{
    printf("    To MILL_MODE_SLIP\n"); // xxx
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
__mill_output_words(Mill* self)
{
    // This will end up being similar to __mill_output_stack. Review.
    printf("xxx untested\n");

    Entry* ent = (Entry*) self->dict_mem;
    Entry* top = (Entry*) self->dict_top;

    Bb* bb = self->bb_buf_output;
    char* src = bb->s;
    unsigned src_offset_nail;
    unsigned src_offset_peri;

    unsigned n = 0;

    Bw* bw_name;
    while (ent <= top) {
        bw_name = &ent->bw_name;

        src = bw_name->nail;
        src_offset_nail = 0;
        src_offset_peri = src_offset_peri - src_offset_nail;

        if (bb_length(bb) + src_offset_peri > bb_capacity(bb))
            break;

        bb_from_bw_append(bb, bw_name);

        ent = (Entry*) ent->next;
    }
}

// xxx interesting. at the moment it outputs the stack in reverse order. is
// this what we want? it implies that existing forth implementations implement
// their stack differently than I have done. Not a huge surprise. Their stack
// implementation will be closer to my dict implementation. I will probably
// want to refactor towards that. It would be easier to reason about the
// amount of memory that a system was using if I did this.
static void
__mill_output_stack(Mill* self)
{
    printf("xxx __mill_output_stack\n");

    Entry* ent = (Entry*) self->dict_mem;
    Entry* top = (Entry*) self->dict_top;

    Bw* bw_name;
    while (1) {
        bb_from_bw_append(self->bb_buf_output, &ent->bw_name);
        // xxx
        printf("Adding\n");
        bw_debug(&ent->bw_name);

        if (ent->prev == NULL) {
            break;
        }
        else {
            ent = (Entry*) ent->prev;
        }
    }
    bb_debug(self->bb_buf_output);
}

static void
__mill_on_word(Mill* self, Bw* bw) 
{
    // Control scan
    {
        if (bw_equals_s(bw, ".\"")) {
            self->parser = PARSER_STRING;
            return;
        }
        if (bw_equals_s(bw, ".echo")) {
            self->parser = PARSER_ECHO;
            return;
        }
        if (bw_equals_s(bw, ".w")) {
            __mill_output_words(self);
            return;
        }
        if (bw_equals_s(bw, ".q") || bw_equals_s(bw, "bye")) {
            printf("Quit marked.\n"); // xxx
            self->b_quit = 1;
            return;
        }
        if (bw_equals_s(bw, ".s")) {
            __mill_output_stack(self);
            return;
        }
    }

    // Dictionary scan
    {
        Entry* entry = mill_dict_search(self, bw);
        if (entry != NULL) {
            printf("xxx use entry.\n");
            return;
        }
    }

    // Numbers scan
    {
        int n = 0;
        uint8_t rcode;
        
        rcode = __mill_numbers_parse_int(self, bw, &n);
        if (rcode) {
            Token* token = token_stack_get(self->token_stack_pool,
                    TOKEN_TYPE_INT);
            token->n = n;
            token_stack_push(self->token_stack_live, token);
            return;
        }
    }

    printf("xxx __mill_on_word Error.\n"); // xxx Go to MILL_MODE_SLIP.
}

static void
__mill_parse_echo(Mill* self, Bw* bw)
{
    // Allocate
    Bw* bw_word = bw_stack_get(self->bw_stack_pool);
    
    // Locate a single word
    bw_word->nail = bw->nail++;
    bw_word->peri = bw->nail;
    while (bw->nail < bw->peri) {
        if (*bw->nail == ' ') break;

        bw_word->peri++;
        bw->nail++;
    }

    // Send it to the output fifo (or end echo mode).
    if (bw_equals_s(bw_word, ".")) {
        self->parser = PARSER_NORMAL;
    }
    else {
        bb_from_bw(self->bb_buf_output, bw_word);
    }

    // Cleanup
    bw_stack_push(self->bw_stack_pool, bw_word);
}

static void
__mill_parse_normal(Mill* self, Bw* bw) 
{
    // Allocate
    Bw* bw_word = bw_stack_get(self->bw_stack_pool);

    // Locate a single word
    bw_word->nail = bw->nail++;
    bw_word->peri = bw->nail;
    while (bw->nail < bw->peri) {
        if (*bw->nail == ' ') break;

        bw_word->peri++;
        bw->nail++;
    }

    // Process it
    __mill_on_word(self, bw_word);

    // Cleanup
    bw_stack_push(self->bw_stack_pool, bw_word);
}

static void
__mill_parse_string(Mill* self, Bw* bw) 
{
    printf("xxx string parsing is not yet implemented.\n");
    exit(1);
}

static void
__mill_do_work(Mill* self) 
{
    // If there is no work left to do, retreat to read mode.
    if (bw_stack_size(self->bw_stack_work) == 0) {
        __mill_to_mode_read(self);
        return;
    }

    // The top Bw in the stack may contain several textual words. Hence, we do
    // not pop here, but get a pointer to top.
    Bw* bw = bw_stack_top(self->bw_stack_work);
    bw_trim_left(bw);
    if (bw_size(bw)) {
        switch (self->parser) {
        case PARSER_ECHO:
            __mill_parse_echo(self, bw);
            break;
        case PARSER_NORMAL:
            __mill_parse_normal(self, bw);
            break;
        case PARSER_STRING:
            __mill_parse_string(self, bw);
            break;
        }
    }

    // If the Bw is empty (perhaps as a result of the work above, or perhaps
    // because it was empty to start with), we return it to the pool.
    if (!bw_size(bw)) {
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
    // We can accept input in most occasions, but not when the
    // input pool has run out of entries.
    return (int) bb_fifo_size(self->bb_fifo_in_pool);
}

int
mill_is_output_ready(Mill* self)
{
    return (int) bb_fifo_size(self->bb_fifo_out);
}

char
mill_is_quitting(Mill* self) 
{
    return self->b_quit;
}

void
mill_output(Mill* self, Bb* bb)
{
    Bb* bb_content = bb_fifo_pull(self->bb_fifo_out);
    if (bb == NULL) {
        // xxx
        printf("WARNING: no output ready. Crash expected.\n");
    }

    printf("&&& %d %d\n", bb==NULL, bb_content==NULL); // xxx
    bb_from_bb(bb, bb_content);

    bb_fifo_push(self->bb_fifo_out_pool, bb_content);

    switch (self->mode) {
    case MILL_MODE_REST:
    case MILL_MODE_WORK:
    case MILL_MODE_READ:
    case MILL_MODE_SLIP:
        break;
    case MILL_MODE_WEIR:
        __mill_to_mode_work(self);
        break;
    }
}

// Returns any unused gas
unsigned
mill_power(Mill* self, unsigned gas) 
{
    int b_continue = 1;
    while (gas) {
        switch (self->mode) {
        case MILL_MODE_WEIR:
            // The block below that handles output data handles this Weir
            // state.
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

        // If there is output to be sent towards the user, attempt this.
        //
        // xxx Modify this to do one token at a time. This is going to involve
        // quite a bit of refactoring to allow elegant handling of string
        // output and the like. A good scenario to focus on is handling output
        // from .s.
        if (bb_length(self->bb_buf_output)) {
            if (bb_fifo_size(self->bb_fifo_out_pool)) {
                Bb* bb = bb_fifo_pull(self->bb_fifo_out_pool);
                bb_from_bb(bb, self->bb_buf_output);
                bb_clear(self->bb_buf_output);
                bb_fifo_push(self->bb_fifo_out, bb);

                // Where we are in Weir, this falls us back to Work.
                __mill_to_mode_work(self);
                b_continue = 1;
            }
            else {
                // If there is nowhere for us to send this data at the moment, make
                // sure we are in weir, and return early.
                __mill_to_mode_weir(self);
                b_continue = 0;
            }
        }

        if (b_continue) {
            gas--;
        } else {
            break;
        }
    }
    return gas;
}

static char*
mill_test() 
{
    { // int parsing function
        printf("*** mill_test int parsing function *******\n");
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

    { // placing and removing ints (on/from the stack)
        printf("*** put an int on the stack **********\n"); // xxx
        Bw* bw = bw_new();
        Mill* self = NULL; {
            size_t dict_size = (1024*1024) * 40;
            size_t word_size = 16;
            size_t fifo_in_size = 16;
            size_t fifo_out_size = 16;
            self = mill_new(dict_size, word_size, fifo_in_size, fifo_out_size);
        }
        
        bw_from_s(bw, "450");
        mill_input(self, bw);

        // cleanup
        bw_del(bw);
        mill_del(self);
    }

    { // dictionary basics
        printf("*** dictionary basics ****************\n"); // xxx
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
        Bw* bw = NULL;
        Bb* bb = NULL; {
            // Mill: this is what we are testing
            size_t dict_size = (1024*1024) * 40;
            size_t word_size = 16;
            size_t fifo_in_size = 16;
            size_t fifo_out_size = 16;
            self = mill_new(dict_size, word_size, fifo_in_size, fifo_out_size);

            bw = bw_new();

            bb = bb_new(400);
        }

        unsigned gas;

        /* xxx working point
        bw_from_s(bw, ": star 42 emit ;");
        mill_input(self, bw);
        gas = 10;
        mill_power(self, gas);
        mu_assert(mill_is_output_ready(self) == 1, "Mill output?");

        printf("xxx check output is 'ok'\n"); {
            bb_clear(bb);
            mill_output(self, bb);
            printf("xxx incomplete\n");
        }
        */

        bw_from_s(bw, "star");
        mill_input(self, bw);
        gas = 10;
        mill_power(self, gas);
        printf("xxx check output is '**********'\n");

        printf("--------------------------------------------------------\n");
        bw_from_s(bw, ".s");
        mill_input(self, bw);
        gas = 10;
        mill_power(self, gas);
        printf("--------------------------------------------------------\n");

        // cleanup
        bb_del(bb);
        bw_del(bw);
        mill_del(self);
    }

    printf("*** mill_test() end **************\n");

    return NULL;
}


// ------------------------------------------------------------------------
//  alg
// ------------------------------------------------------------------------
#define REPL_LOOP_BUFFER_SIZE 4096
void
repl(Mill* mill) 
{
    Bb* bb_input = bb_new(30);
    Bw* bw = bw_new(); {
        char buf[REPL_LOOP_BUFFER_SIZE];
        size_t n;
        while (!mill_is_quitting(mill)) { // repl loop
            fgets((char*) buf, REPL_LOOP_BUFFER_SIZE-1, stdin);
            bw_from_s(bw, buf);
            mill_input(mill, bw);

            // Keep doing stuff until we have a cycle where we consume no
            // gas. At that point, give the repl back to the user.
            unsigned gas_per_loop = 10;
            unsigned gas;
            while (!mill_is_quitting(mill)) {
                gas = gas_per_loop;
                gas = mill_power(mill, gas);

                // Get as much output as possible back to the user.
                unsigned b_first_in_line = 1;
                while (!mill_is_quitting(mill) && mill_is_output_ready(mill)) {
                    Bb* bb_out = bb_fifo_pull(mill->bb_fifo_out);

                    int len = bb_length(bb_out);
                    char* s = (char*) malloc(len+1); {
                        bb_to_s(bb_out, s);
                        if (b_first_in_line) {
                            b_first_in_line = 0;
                            printf("//");
                        }
                        printf(" %s", s);
                    }
                    free(s);
                }

                if (gas == gas_per_loop) {
                    printf("\n"); // x
                    break;
                }
            }
        }
    }
    bw_del(bw);
    bb_del(bb_input);
}

void
alg() 
{
    size_t dict_size = (1024*1024) * 40;
    size_t word_size = 64;
    size_t fifo_in_size = 4;
    size_t fifo_out_size = 16;

    Mill* mill = mill_new(dict_size, word_size, fifo_in_size, fifo_out_size); {
        mill_dict_register_defaults(mill);

        printf(".\n");
        repl(mill);
    }
    mill_del(mill);
}

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

//
// Only one line should be enabled here.
//
RUN_TESTS(all_tests);
//int main() { mill_test(); return 0; }

//int main() { alg(); return 0; }

