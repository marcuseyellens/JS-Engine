#ifndef QUICKJS_GC_H
#define QUICKJS_GC_H

#include "def.h"
#include "utils/kid.h"

/* -- Malloc ----------------------------------- */

size_t js_malloc_usable_size_unknown(const void *ptr);

no_inline int js_realloc_array(JSContext *ctx, void **parray, int elem_size,
                               int *psize, int req_size);

/* resize the array and update its size if req_size > *psize */
static inline int js_resize_array(JSContext *ctx, void **parray, int elem_size,
                                  int *psize, int req_size) {
  if (unlikely(req_size > *psize))
    return js_realloc_array(ctx, parray, elem_size, psize, req_size);
  else
    return 0;
}

/* called by libbf */
void *js_bf_realloc(void *opaque, void *ptr, size_t size);

/* -- Garbage collection ----------------------------------- */

static inline void set_value(JSContext *ctx, JSValue *pval, JSValue new_val) {
  JSValue old_val;
  old_val = *pval;
  *pval = new_val;
  JS_FreeValue(ctx, old_val);
}

void JS_RunGC(JSRuntime *rt);
void js_trigger_gc(JSRuntime *rt, size_t size);
void set_cycle_flag(JSContext *ctx, JSValueConst obj);

void add_gc_object(JSRuntime *rt, JSGCObjectHeader *h, JSGCObjectTypeEnum type);
void remove_gc_object(JSGCObjectHeader *h);
void free_gc_object(JSRuntime *rt, JSGCObjectHeader *gp);
void gc_decref(JSRuntime *rt);

void free_var_ref(JSRuntime *rt, JSVarRef *var_ref);
void free_bytecode_atoms(JSRuntime *rt, const uint8_t *bc_buf, int bc_len,
                         BOOL use_short_opcodes);

/* -- GC dump ----------------------------------- */

typedef struct JSGCDumpEdge {
  uint8_t type;
  uint32_t name_or_idx;
  size_t to;
} JSGCDumpEdge;

#define NODE_FIELD_COUNT 5

typedef struct JSGCDumpNode {
  size_t id;
  JSAtom name;
  uint16_t type;
  size_t self_size;
  KidArray edges; // Array<JSGCDumpEdge>
} JSGCDumpNode;

typedef struct JSGCDumpContext {
  JSContext *jc;
  KidAllocator kid_allocator;
  KidArray nodes;
  size_t edges_len;

  KidArray strs;     // Array<KidString>
  KidHashmap str2id; // Hashmap<JSString*, int>

  KidHashmap obj2node; // Hashmap<obj_ptr, JSGCDumpNode*>
} JSGCDumpContext;

int js_gcdump_node_from_gp(JSGCDumpContext *dc, void *gp);
int js_gcdump_add_cstr(JSGCDumpContext *dc, const char *cstr, size_t len);
int js_gcdump_add_str(JSGCDumpContext *dc, JSString *str);
int js_gcdump_add_atom(JSGCDumpContext *dc, JSAtom atom);
JSValue js_gcdump_objects(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv);

#endif
