#include "obj.h"

#include "class.h"
#include "conv.h"
#include "error.h"
#include "func.h"
#include "gc.h"
#include "intrins/intrins.h"
#include "num.h"
#include "ops.h"
#include "shape.h"
#include "vm.h"

/* -- Prototype ----------------------------------- */

/* return -1 (exception) or TRUE/FALSE */
int JS_SetPrototypeInternal(JSContext *ctx, JSValueConst obj,
                            JSValueConst proto_val, BOOL throw_flag) {
  JSObject *proto, *p, *p1;
  JSShape *sh;

  if (throw_flag) {
    if (JS_VALUE_GET_TAG(obj) == JS_TAG_NULL ||
        JS_VALUE_GET_TAG(obj) == JS_TAG_UNDEFINED)
      goto not_obj;
  } else {
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
      goto not_obj;
  }
  p = JS_VALUE_GET_OBJ(obj);
  if (JS_VALUE_GET_TAG(proto_val) != JS_TAG_OBJECT) {
    if (JS_VALUE_GET_TAG(proto_val) != JS_TAG_NULL) {
    not_obj:
      JS_ThrowTypeErrorNotAnObject(ctx);
      return -1;
    }
    proto = NULL;
  } else {
    proto = JS_VALUE_GET_OBJ(proto_val);
  }

  if (throw_flag && JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
    return TRUE;

  if (unlikely(p->class_id == JS_CLASS_PROXY))
    return js_proxy_setPrototypeOf(ctx, obj, proto_val, throw_flag);
  sh = p->shape;
  if (sh->proto == proto)
    return TRUE;
  if (!p->extensible) {
    if (throw_flag) {
      JS_ThrowTypeError(ctx, "object is not extensible");
      return -1;
    } else {
      return FALSE;
    }
  }
  if (proto) {
    /* check if there is a cycle */
    p1 = proto;
    do {
      if (p1 == p) {
        if (throw_flag) {
          JS_ThrowTypeError(ctx, "circular prototype chain");
          return -1;
        } else {
          return FALSE;
        }
      }
      /* Note: for Proxy objects, proto is NULL */
      p1 = p1->shape->proto;
    } while (p1 != NULL);
    JS_DupValue(ctx, proto_val);
  }

  if (js_shape_prepare_update(ctx, p, NULL))
    return -1;
  sh = p->shape;
  if (sh->proto)
    JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, sh->proto));
  sh->proto = proto;
  return TRUE;
}

/* return -1 (exception) or TRUE/FALSE */
int JS_SetPrototype(JSContext *ctx, JSValueConst obj, JSValueConst proto_val) {
  return JS_SetPrototypeInternal(ctx, obj, proto_val, TRUE);
}

/* Only works for primitive types, otherwise return JS_NULL. */
JSValueConst JS_GetPrototypePrimitive(JSContext *ctx, JSValueConst val) {
  switch (JS_VALUE_GET_NORM_TAG(val)) {
#ifdef CONFIG_BIGNUM
  case JS_TAG_BIG_INT:
    val = ctx->class_proto[JS_CLASS_BIG_INT];
    break;
  case JS_TAG_BIG_FLOAT:
    val = ctx->class_proto[JS_CLASS_BIG_FLOAT];
    break;
  case JS_TAG_BIG_DECIMAL:
    val = ctx->class_proto[JS_CLASS_BIG_DECIMAL];
    break;
#endif
  case JS_TAG_INT:
  case JS_TAG_FLOAT64:
    val = ctx->class_proto[JS_CLASS_NUMBER];
    break;
  case JS_TAG_BOOL:
    val = ctx->class_proto[JS_CLASS_BOOLEAN];
    break;
  case JS_TAG_STRING:
    val = ctx->class_proto[JS_CLASS_STRING];
    break;
  case JS_TAG_SYMBOL:
    val = ctx->class_proto[JS_CLASS_SYMBOL];
    break;
  case JS_TAG_OBJECT:
  case JS_TAG_NULL:
  case JS_TAG_UNDEFINED:
  default:
    val = JS_NULL;
    break;
  }
  return val;
}

/* Return an Object, JS_NULL or JS_EXCEPTION in case of Proxy object. */
JSValue JS_GetPrototype(JSContext *ctx, JSValueConst obj) {
  JSValue val;
  if (JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT) {
    JSObject *p;
    p = JS_VALUE_GET_OBJ(obj);
    if (unlikely(p->class_id == JS_CLASS_PROXY)) {
      val = js_proxy_getPrototypeOf(ctx, obj);
    } else {
      p = p->shape->proto;
      if (!p)
        val = JS_NULL;
      else
        val = JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, p));
    }
  } else {
    val = JS_DupValue(ctx, JS_GetPrototypePrimitive(ctx, obj));
  }
  return val;
}

JSValue JS_GetPrototypeFree(JSContext *ctx, JSValue obj) {
  JSValue obj1;
  obj1 = JS_GetPrototype(ctx, obj);
  JS_FreeValue(ctx, obj);
  return obj1;
}

/* return TRUE, FALSE or (-1) in case of exception */
int JS_OrdinaryIsInstanceOf(JSContext *ctx, JSValueConst val,
                            JSValueConst obj) {
  JSValue obj_proto;
  JSObject *proto;
  const JSObject *p, *proto1;
  BOOL ret;

  if (!JS_IsFunction(ctx, obj))
    return FALSE;
  p = JS_VALUE_GET_OBJ(obj);
  if (p->class_id == JS_CLASS_BOUND_FUNCTION) {
    JSBoundFunction *s = p->u.bound_function;
    return JS_IsInstanceOf(ctx, val, s->func_obj);
  }

  /* Only explicitly boxed values are instances of constructors */
  if (JS_VALUE_GET_TAG(val) != JS_TAG_OBJECT)
    return FALSE;
  obj_proto = JS_GetProperty(ctx, obj, JS_ATOM_prototype);
  if (JS_VALUE_GET_TAG(obj_proto) != JS_TAG_OBJECT) {
    if (!JS_IsException(obj_proto))
      JS_ThrowTypeError(ctx, "operand 'prototype' property is not an object");
    ret = -1;
    goto done;
  }
  proto = JS_VALUE_GET_OBJ(obj_proto);
  p = JS_VALUE_GET_OBJ(val);
  for (;;) {
    proto1 = p->shape->proto;
    if (!proto1) {
      /* slow case if proxy in the prototype chain */
      if (unlikely(p->class_id == JS_CLASS_PROXY)) {
        JSValue obj1;
        obj1 = JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, (JSObject *)p));
        for (;;) {
          obj1 = JS_GetPrototypeFree(ctx, obj1);
          if (JS_IsException(obj1)) {
            ret = -1;
            break;
          }
          if (JS_IsNull(obj1)) {
            ret = FALSE;
            break;
          }
          if (proto == JS_VALUE_GET_OBJ(obj1)) {
            JS_FreeValue(ctx, obj1);
            ret = TRUE;
            break;
          }
          /* must check for timeout to avoid infinite loop */
          if (js_poll_interrupts(ctx)) {
            JS_FreeValue(ctx, obj1);
            ret = -1;
            break;
          }
        }
      } else {
        ret = FALSE;
      }
      break;
    }
    p = proto1;
    if (proto == p) {
      ret = TRUE;
      break;
    }
  }
done:
  JS_FreeValue(ctx, obj_proto);
  return ret;
}

/* return TRUE, FALSE or (-1) in case of exception */
int JS_IsInstanceOf(JSContext *ctx, JSValueConst val, JSValueConst obj) {
  JSValue method;

  if (!JS_IsObject(obj))
    goto fail;
  method = JS_GetProperty(ctx, obj, JS_ATOM_Symbol_hasInstance);
  if (JS_IsException(method))
    return -1;
  if (!JS_IsNull(method) && !JS_IsUndefined(method)) {
    JSValue ret;
    ret = JS_CallFree(ctx, method, obj, 1, &val);
    return JS_ToBoolFree(ctx, ret);
  }

  /* legacy case */
  if (!JS_IsFunction(ctx, obj)) {
  fail:
    JS_ThrowTypeError(ctx, "invalid 'instanceof' right operand");
    return -1;
  }
  return JS_OrdinaryIsInstanceOf(ctx, val, obj);
}

JSValue js_instantiate_prototype(JSContext *ctx, JSObject *p, JSAtom atom,
                                 void *opaque) {
  JSValue obj, this_val;
  int ret;

  this_val = JS_MKPTR(JS_TAG_OBJECT, p);
  obj = JS_NewObject(ctx);
  if (JS_IsException(obj))
    return JS_EXCEPTION;
  set_cycle_flag(ctx, obj);
  set_cycle_flag(ctx, this_val);
  ret = JS_DefinePropertyValue(ctx, obj, JS_ATOM_constructor,
                               JS_DupValue(ctx, this_val),
                               JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
  if (ret < 0) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
  return obj;
}

/* -- Property ----------------------------------- */

JSContext *js_autoinit_get_realm(JSProperty *pr) {
  return (JSContext *)(pr->u.init.realm_and_id & ~3);
}

JSAutoInitIDEnum js_autoinit_get_id(JSProperty *pr) {
  return pr->u.init.realm_and_id & 3;
}

static void js_autoinit_free(JSRuntime *rt, JSProperty *pr) {
  JS_FreeContext(js_autoinit_get_realm(pr));
}

void free_property(JSRuntime *rt, JSProperty *pr, int prop_flags) {
  if (unlikely(prop_flags & JS_PROP_TMASK)) {
    if ((prop_flags & JS_PROP_TMASK) == JS_PROP_GETSET) {
      if (pr->u.getset.getter)
        JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_OBJECT, pr->u.getset.getter));
      if (pr->u.getset.setter)
        JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_OBJECT, pr->u.getset.setter));
    } else if ((prop_flags & JS_PROP_TMASK) == JS_PROP_VARREF) {
      free_var_ref(rt, pr->u.var_ref);
    } else if ((prop_flags & JS_PROP_TMASK) == JS_PROP_AUTOINIT) {
      js_autoinit_free(rt, pr);
    }
  } else {
    JS_FreeValueRT(rt, pr->u.value);
  }
}

/* return the value associated to the autoinit property or an exception */
typedef JSValue JSAutoInitFunc(JSContext *ctx, JSObject *p, JSAtom atom,
                               void *opaque);

static JSAutoInitFunc *js_autoinit_func_table[] = {
    js_instantiate_prototype,        /* JS_AUTOINIT_ID_PROTOTYPE */
    js_module_ns_autoinit,           /* JS_AUTOINIT_ID_MODULE_NS */
    JS_InstantiateFunctionListItem2, /* JS_AUTOINIT_ID_PROP */
};

/* warning: 'prs' is reallocated after it */
int JS_AutoInitProperty(JSContext *ctx, JSObject *p, JSAtom prop,
                        JSProperty *pr, JSShapeProperty *prs) {
  JSValue val;
  JSContext *realm;
  JSAutoInitFunc *func;

  if (js_shape_prepare_update(ctx, p, &prs))
    return -1;

  realm = js_autoinit_get_realm(pr);
  func = js_autoinit_func_table[js_autoinit_get_id(pr)];
  /* 'func' shall not modify the object properties 'pr' */
  val = func(realm, p, prop, pr->u.init.opaque);
  js_autoinit_free(ctx->rt, pr);
  prs->flags &= ~JS_PROP_TMASK;
  pr->u.value = JS_UNDEFINED;
  if (JS_IsException(val))
    return -1;
  pr->u.value = val;
  return 0;
}

/* return -1 if exception (proxy case) or TRUE/FALSE */
int JS_IsArray(JSContext *ctx, JSValueConst val) {
  JSObject *p;
  if (JS_VALUE_GET_TAG(val) == JS_TAG_OBJECT) {
    p = JS_VALUE_GET_OBJ(val);
    if (unlikely(p->class_id == JS_CLASS_PROXY))
      return js_proxy_isArray(ctx, val);
    else
      return p->class_id == JS_CLASS_ARRAY;
  } else {
    return FALSE;
  }
}

JSValue JS_GetPropertyInternal(JSContext *ctx, JSValueConst obj, JSAtom prop,
                               JSValueConst this_obj, BOOL throw_ref_error) {
  JSObject *p;
  JSProperty *pr;
  JSShapeProperty *prs;
  uint32_t tag;

  tag = JS_VALUE_GET_TAG(obj);
  if (unlikely(tag != JS_TAG_OBJECT)) {
    switch (tag) {
    case JS_TAG_NULL:
      return JS_ThrowTypeErrorAtom(ctx, "cannot read property '%s' of null",
                                   prop);
    case JS_TAG_UNDEFINED:
      return JS_ThrowTypeErrorAtom(
          ctx, "cannot read property '%s' of undefined", prop);
    case JS_TAG_EXCEPTION:
      return JS_EXCEPTION;
    case JS_TAG_STRING: {
      JSString *p1 = JS_VALUE_GET_STRING(obj);
      if (__JS_AtomIsTaggedInt(prop)) {
        uint32_t idx, ch;
        idx = __JS_AtomToUInt32(prop);
        if (idx < p1->len) {
          if (p1->is_wide_char)
            ch = p1->u.str16[idx];
          else
            ch = p1->u.str8[idx];
          return js_new_string_char(ctx, ch);
        }
      } else if (prop == JS_ATOM_length) {
        return JS_NewInt32(ctx, p1->len);
      }
    } break;
    default:
      break;
    }
    /* cannot raise an exception */
    p = JS_VALUE_GET_OBJ(JS_GetPrototypePrimitive(ctx, obj));
    if (!p)
      return JS_UNDEFINED;
  } else {
    p = JS_VALUE_GET_OBJ(obj);
  }

  for (;;) {
    prs = find_own_property(&pr, p, prop);
    if (prs) {
      /* found */
      if (unlikely(prs->flags & JS_PROP_TMASK)) {
        if ((prs->flags & JS_PROP_TMASK) == JS_PROP_GETSET) {
          if (unlikely(!pr->u.getset.getter)) {
            return JS_UNDEFINED;
          } else {
            JSValue func = JS_MKPTR(JS_TAG_OBJECT, pr->u.getset.getter);
            /* Note: the field could be removed in the getter */
            func = JS_DupValue(ctx, func);
            return JS_CallFree(ctx, func, this_obj, 0, NULL);
          }
        } else if ((prs->flags & JS_PROP_TMASK) == JS_PROP_VARREF) {
          JSValue val = *pr->u.var_ref->pvalue;
          if (unlikely(JS_IsUninitialized(val)))
            return JS_ThrowReferenceErrorUninitialized(ctx, prs->atom);
          return JS_DupValue(ctx, val);
        } else if ((prs->flags & JS_PROP_TMASK) == JS_PROP_AUTOINIT) {
          /* Instantiate property and retry */
          if (JS_AutoInitProperty(ctx, p, prop, pr, prs))
            return JS_EXCEPTION;
          continue;
        }
      } else {
        return JS_DupValue(ctx, pr->u.value);
      }
    }
    if (unlikely(p->is_exotic)) {
      /* exotic behaviors */
      if (p->fast_array) {
        if (__JS_AtomIsTaggedInt(prop)) {
          uint32_t idx = __JS_AtomToUInt32(prop);
          if (idx < p->u.array.count) {
            /* we avoid duplicating the code */
            return JS_GetPropertyUint32(ctx, JS_MKPTR(JS_TAG_OBJECT, p), idx);
          } else if (p->class_id >= JS_CLASS_UINT8C_ARRAY &&
                     p->class_id <= JS_CLASS_FLOAT64_ARRAY) {
            return JS_UNDEFINED;
          }
        } else if (p->class_id >= JS_CLASS_UINT8C_ARRAY &&
                   p->class_id <= JS_CLASS_FLOAT64_ARRAY) {
          int ret;
          ret = JS_AtomIsNumericIndex(ctx, prop);
          if (ret != 0) {
            if (ret < 0)
              return JS_EXCEPTION;
            return JS_UNDEFINED;
          }
        }
      } else {
        const JSClassExoticMethods *em =
            ctx->rt->class_array[p->class_id].exotic;
        if (em) {
          if (em->get_property) {
            JSValue obj1, retval;
            /* XXX: should pass throw_ref_error */
            /* Note: if 'p' is a prototype, it can be
               freed in the called function */
            obj1 = JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, p));
            retval = em->get_property(ctx, obj1, prop, this_obj);
            JS_FreeValue(ctx, obj1);
            return retval;
          }
          if (em->get_own_property) {
            JSPropertyDescriptor desc;
            int ret;
            JSValue obj1;

            /* Note: if 'p' is a prototype, it can be
               freed in the called function */
            obj1 = JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, p));
            ret = em->get_own_property(ctx, &desc, obj1, prop);
            JS_FreeValue(ctx, obj1);
            if (ret < 0)
              return JS_EXCEPTION;
            if (ret) {
              if (desc.flags & JS_PROP_GETSET) {
                JS_FreeValue(ctx, desc.setter);
                return JS_CallFree(ctx, desc.getter, this_obj, 0, NULL);
              } else {
                return desc.value;
              }
            }
          }
        }
      }
    }
    p = p->shape->proto;
    if (!p)
      break;
  }
  if (unlikely(throw_ref_error)) {
    return JS_ThrowReferenceErrorNotDefined(ctx, prop);
  } else {
    return JS_UNDEFINED;
  }
}

static JSValue JS_ThrowTypeErrorPrivateNotFound(JSContext *ctx, JSAtom atom) {
  return JS_ThrowTypeErrorAtom(ctx, "private class field '%s' does not exist",
                               atom);
}

/* Private fields can be added even on non extensible objects or
   Proxies */
int JS_DefinePrivateField(JSContext *ctx, JSValueConst obj, JSValueConst name,
                          JSValue val) {
  JSObject *p;
  JSShapeProperty *prs;
  JSProperty *pr;
  JSAtom prop;

  if (unlikely(JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)) {
    JS_ThrowTypeErrorNotAnObject(ctx);
    goto fail;
  }
  /* safety check */
  if (unlikely(JS_VALUE_GET_TAG(name) != JS_TAG_SYMBOL)) {
    JS_ThrowTypeErrorNotASymbol(ctx);
    goto fail;
  }
  prop = js_symbol_to_atom(ctx, (JSValue)name);
  p = JS_VALUE_GET_OBJ(obj);
  prs = find_own_property(&pr, p, prop);
  if (prs) {
    JS_ThrowTypeErrorAtom(ctx, "private class field '%s' already exists", prop);
    goto fail;
  }
  pr = add_property(ctx, p, prop, JS_PROP_C_W_E);
  if (unlikely(!pr)) {
  fail:
    JS_FreeValue(ctx, val);
    return -1;
  }
  pr->u.value = val;
  return 0;
}

JSValue JS_GetPrivateField(JSContext *ctx, JSValueConst obj,
                           JSValueConst name) {
  JSObject *p;
  JSShapeProperty *prs;
  JSProperty *pr;
  JSAtom prop;

  if (unlikely(JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT))
    return JS_ThrowTypeErrorNotAnObject(ctx);
  /* safety check */
  if (unlikely(JS_VALUE_GET_TAG(name) != JS_TAG_SYMBOL))
    return JS_ThrowTypeErrorNotASymbol(ctx);
  prop = js_symbol_to_atom(ctx, (JSValue)name);
  p = JS_VALUE_GET_OBJ(obj);
  prs = find_own_property(&pr, p, prop);
  if (!prs) {
    JS_ThrowTypeErrorPrivateNotFound(ctx, prop);
    return JS_EXCEPTION;
  }
  return JS_DupValue(ctx, pr->u.value);
}

int JS_SetPrivateField(JSContext *ctx, JSValueConst obj, JSValueConst name,
                       JSValue val) {
  JSObject *p;
  JSShapeProperty *prs;
  JSProperty *pr;
  JSAtom prop;

  if (unlikely(JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)) {
    JS_ThrowTypeErrorNotAnObject(ctx);
    goto fail;
  }
  /* safety check */
  if (unlikely(JS_VALUE_GET_TAG(name) != JS_TAG_SYMBOL)) {
    JS_ThrowTypeErrorNotASymbol(ctx);
    goto fail;
  }
  prop = js_symbol_to_atom(ctx, (JSValue)name);
  p = JS_VALUE_GET_OBJ(obj);
  prs = find_own_property(&pr, p, prop);
  if (!prs) {
    JS_ThrowTypeErrorPrivateNotFound(ctx, prop);
  fail:
    JS_FreeValue(ctx, val);
    return -1;
  }
  set_value(ctx, &pr->u.value, val);
  return 0;
}

int JS_AddBrand(JSContext *ctx, JSValueConst obj, JSValueConst home_obj) {
  JSObject *p, *p1;
  JSShapeProperty *prs;
  JSProperty *pr;
  JSValue brand;
  JSAtom brand_atom;

  if (unlikely(JS_VALUE_GET_TAG(home_obj) != JS_TAG_OBJECT)) {
    JS_ThrowTypeErrorNotAnObject(ctx);
    return -1;
  }
  p = JS_VALUE_GET_OBJ(home_obj);
  prs = find_own_property(&pr, p, JS_ATOM_Private_brand);
  if (!prs) {
    brand = JS_NewSymbolFromAtom(ctx, JS_ATOM_brand, JS_ATOM_TYPE_PRIVATE);
    if (JS_IsException(brand))
      return -1;
    /* if the brand is not present, add it */
    pr = add_property(ctx, p, JS_ATOM_Private_brand, JS_PROP_C_W_E);
    if (!pr) {
      JS_FreeValue(ctx, brand);
      return -1;
    }
    pr->u.value = JS_DupValue(ctx, brand);
  } else {
    brand = JS_DupValue(ctx, pr->u.value);
  }
  brand_atom = js_symbol_to_atom(ctx, brand);

  if (unlikely(JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)) {
    JS_ThrowTypeErrorNotAnObject(ctx);
    JS_FreeAtom(ctx, brand_atom);
    return -1;
  }
  p1 = JS_VALUE_GET_OBJ(obj);
  pr = add_property(ctx, p1, brand_atom, JS_PROP_C_W_E);
  JS_FreeAtom(ctx, brand_atom);
  if (!pr)
    return -1;
  pr->u.value = JS_UNDEFINED;
  return 0;
}

int JS_CheckBrand(JSContext *ctx, JSValueConst obj, JSValueConst func) {
  JSObject *p, *p1, *home_obj;
  JSShapeProperty *prs;
  JSProperty *pr;
  JSValueConst brand;

  /* get the home object of 'func' */
  if (unlikely(JS_VALUE_GET_TAG(func) != JS_TAG_OBJECT)) {
  not_obj:
    JS_ThrowTypeErrorNotAnObject(ctx);
    return -1;
  }
  p1 = JS_VALUE_GET_OBJ(func);
  if (!js_class_has_bytecode(p1->class_id))
    goto not_obj;
  home_obj = p1->u.func.home_object;
  if (!home_obj)
    goto not_obj;
  prs = find_own_property(&pr, home_obj, JS_ATOM_Private_brand);
  if (!prs) {
    JS_ThrowTypeError(ctx, "expecting <brand> private field");
    return -1;
  }
  brand = pr->u.value;
  /* safety check */
  if (unlikely(JS_VALUE_GET_TAG(brand) != JS_TAG_SYMBOL))
    goto not_obj;

  /* get the brand array of 'obj' */
  if (unlikely(JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT))
    goto not_obj;
  p = JS_VALUE_GET_OBJ(obj);
  prs = find_own_property(&pr, p, js_symbol_to_atom(ctx, (JSValue)brand));
  if (!prs) {
    JS_ThrowTypeError(ctx, "invalid brand on object");
    return -1;
  }
  return 0;
}

static int num_keys_cmp(const void *p1, const void *p2, void *opaque) {
  JSContext *ctx = opaque;
  JSAtom atom1 = ((const JSPropertyEnum *)p1)->atom;
  JSAtom atom2 = ((const JSPropertyEnum *)p2)->atom;
  uint32_t v1, v2;
#ifndef NDEBUG
  BOOL atom1_is_integer, atom2_is_integer;

  atom1_is_integer = JS_AtomIsArrayIndex(ctx, &v1, atom1);
  atom2_is_integer = JS_AtomIsArrayIndex(ctx, &v2, atom2);
#else
  JS_AtomIsArrayIndex(ctx, &v1, atom1);
  JS_AtomIsArrayIndex(ctx, &v2, atom2);
#endif

  assert(atom1_is_integer && atom2_is_integer);
  if (v1 < v2)
    return -1;
  else if (v1 == v2)
    return 0;
  else
    return 1;
}

void js_free_prop_enum(JSContext *ctx, JSPropertyEnum *tab, uint32_t len) {
  uint32_t i;
  if (tab) {
    for (i = 0; i < len; i++)
      JS_FreeAtom(ctx, tab[i].atom);
    js_free(ctx, tab);
  }
}

/* return < 0 in case if exception, 0 if OK. ptab and its atoms must
   be freed by the user. */
int __exception JS_GetOwnPropertyNamesInternal(JSContext *ctx,
                                               JSPropertyEnum **ptab,
                                               uint32_t *plen, JSObject *p,
                                               int flags) {
  int i, j;
  JSShape *sh;
  JSShapeProperty *prs;
  JSPropertyEnum *tab_atom, *tab_exotic;
  JSAtom atom;
  uint32_t num_keys_count, str_keys_count, sym_keys_count, atom_count;
  uint32_t num_index, str_index, sym_index, exotic_count, exotic_keys_count;
  BOOL is_enumerable, num_sorted;
  uint32_t num_key;
  JSAtomKindEnum kind;

  /* clear pointer for consistency in case of failure */
  *ptab = NULL;
  *plen = 0;

  /* compute the number of returned properties */
  num_keys_count = 0;
  str_keys_count = 0;
  sym_keys_count = 0;
  exotic_keys_count = 0;
  exotic_count = 0;
  tab_exotic = NULL;
  sh = p->shape;
  for (i = 0, prs = get_shape_prop(sh); i < sh->prop_count; i++, prs++) {
    atom = prs->atom;
    if (atom != JS_ATOM_NULL) {
      is_enumerable = ((prs->flags & JS_PROP_ENUMERABLE) != 0);
      kind = JS_AtomGetKind(ctx, atom);
      if ((!(flags & JS_GPN_ENUM_ONLY) || is_enumerable) &&
          ((flags >> kind) & 1) != 0) {
        /* need to raise an exception in case of the module
           name space (implicit GetOwnProperty) */
        if (unlikely((prs->flags & JS_PROP_TMASK) == JS_PROP_VARREF) &&
            (flags & (JS_GPN_SET_ENUM | JS_GPN_ENUM_ONLY))) {
          JSVarRef *var_ref = p->prop[i].u.var_ref;
          if (unlikely(JS_IsUninitialized(*var_ref->pvalue))) {
            JS_ThrowReferenceErrorUninitialized(ctx, prs->atom);
            return -1;
          }
        }
        if (JS_AtomIsArrayIndex(ctx, &num_key, atom)) {
          num_keys_count++;
        } else if (kind == JS_ATOM_KIND_STRING) {
          str_keys_count++;
        } else {
          sym_keys_count++;
        }
      }
    }
  }

  if (p->is_exotic) {
    if (p->fast_array) {
      if (flags & JS_GPN_STRING_MASK) {
        num_keys_count += p->u.array.count;
      }
    } else if (p->class_id == JS_CLASS_STRING) {
      if (flags & JS_GPN_STRING_MASK) {
        num_keys_count +=
            js_string_obj_get_length(ctx, JS_MKPTR(JS_TAG_OBJECT, p));
      }
    } else {
      const JSClassExoticMethods *em = ctx->rt->class_array[p->class_id].exotic;
      if (em && em->get_own_property_names) {
        if (em->get_own_property_names(ctx, &tab_exotic, &exotic_count,
                                       JS_MKPTR(JS_TAG_OBJECT, p)))
          return -1;
        for (i = 0; i < exotic_count; i++) {
          atom = tab_exotic[i].atom;
          kind = JS_AtomGetKind(ctx, atom);
          if (((flags >> kind) & 1) != 0) {
            is_enumerable = FALSE;
            if (flags & (JS_GPN_SET_ENUM | JS_GPN_ENUM_ONLY)) {
              JSPropertyDescriptor desc;
              int res;
              /* set the "is_enumerable" field if necessary */
              res = JS_GetOwnPropertyInternal(ctx, &desc, p, atom);
              if (res < 0) {
                js_free_prop_enum(ctx, tab_exotic, exotic_count);
                return -1;
              }
              if (res) {
                is_enumerable = ((desc.flags & JS_PROP_ENUMERABLE) != 0);
                js_free_desc(ctx, &desc);
              }
              tab_exotic[i].is_enumerable = is_enumerable;
            }
            if (!(flags & JS_GPN_ENUM_ONLY) || is_enumerable) {
              exotic_keys_count++;
            }
          }
        }
      }
    }
  }

  /* fill them */

  atom_count =
      num_keys_count + str_keys_count + sym_keys_count + exotic_keys_count;
  /* avoid allocating 0 bytes */
  tab_atom = js_malloc(ctx, sizeof(tab_atom[0]) * max_int(atom_count, 1));
  if (!tab_atom) {
    js_free_prop_enum(ctx, tab_exotic, exotic_count);
    return -1;
  }

  num_index = 0;
  str_index = num_keys_count;
  sym_index = str_index + str_keys_count;

  num_sorted = TRUE;
  sh = p->shape;
  for (i = 0, prs = get_shape_prop(sh); i < sh->prop_count; i++, prs++) {
    atom = prs->atom;
    if (atom != JS_ATOM_NULL) {
      is_enumerable = ((prs->flags & JS_PROP_ENUMERABLE) != 0);
      kind = JS_AtomGetKind(ctx, atom);
      if ((!(flags & JS_GPN_ENUM_ONLY) || is_enumerable) &&
          ((flags >> kind) & 1) != 0) {
        if (JS_AtomIsArrayIndex(ctx, &num_key, atom)) {
          j = num_index++;
          num_sorted = FALSE;
        } else if (kind == JS_ATOM_KIND_STRING) {
          j = str_index++;
        } else {
          j = sym_index++;
        }
        tab_atom[j].atom = JS_DupAtom(ctx, atom);
        tab_atom[j].is_enumerable = is_enumerable;
      }
    }
  }

  if (p->is_exotic) {
    int len;
    if (p->fast_array) {
      if (flags & JS_GPN_STRING_MASK) {
        len = p->u.array.count;
        goto add_array_keys;
      }
    } else if (p->class_id == JS_CLASS_STRING) {
      if (flags & JS_GPN_STRING_MASK) {
        len = js_string_obj_get_length(ctx, JS_MKPTR(JS_TAG_OBJECT, p));
      add_array_keys:
        for (i = 0; i < len; i++) {
          tab_atom[num_index].atom = __JS_AtomFromUInt32(i);
          if (tab_atom[num_index].atom == JS_ATOM_NULL) {
            js_free_prop_enum(ctx, tab_atom, num_index);
            return -1;
          }
          tab_atom[num_index].is_enumerable = TRUE;
          num_index++;
        }
      }
    } else {
      /* Note: exotic keys are not reordered and comes after the object own
       * properties. */
      for (i = 0; i < exotic_count; i++) {
        atom = tab_exotic[i].atom;
        is_enumerable = tab_exotic[i].is_enumerable;
        kind = JS_AtomGetKind(ctx, atom);
        if ((!(flags & JS_GPN_ENUM_ONLY) || is_enumerable) &&
            ((flags >> kind) & 1) != 0) {
          tab_atom[sym_index].atom = atom;
          tab_atom[sym_index].is_enumerable = is_enumerable;
          sym_index++;
        } else {
          JS_FreeAtom(ctx, atom);
        }
      }
      js_free(ctx, tab_exotic);
    }
  }

  assert(num_index == num_keys_count);
  assert(str_index == num_keys_count + str_keys_count);
  assert(sym_index == atom_count);

  if (num_keys_count != 0 && !num_sorted) {
    rqsort(tab_atom, num_keys_count, sizeof(tab_atom[0]), num_keys_cmp, ctx);
  }
  *ptab = tab_atom;
  *plen = atom_count;
  return 0;
}

int JS_GetOwnPropertyNames(JSContext *ctx, JSPropertyEnum **ptab,
                           uint32_t *plen, JSValueConst obj, int flags) {
  if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT) {
    JS_ThrowTypeErrorNotAnObject(ctx);
    return -1;
  }
  return JS_GetOwnPropertyNamesInternal(ctx, ptab, plen, JS_VALUE_GET_OBJ(obj),
                                        flags);
}

/* Return -1 if exception,
   FALSE if the property does not exist, TRUE if it exists. If TRUE is
   returned, the property descriptor 'desc' is filled present. */
int JS_GetOwnPropertyInternal(JSContext *ctx, JSPropertyDescriptor *desc,
                              JSObject *p, JSAtom prop) {
  JSShapeProperty *prs;
  JSProperty *pr;

retry:
  prs = find_own_property(&pr, p, prop);
  if (prs) {
    if (desc) {
      desc->flags = prs->flags & JS_PROP_C_W_E;
      desc->getter = JS_UNDEFINED;
      desc->setter = JS_UNDEFINED;
      desc->value = JS_UNDEFINED;
      if (unlikely(prs->flags & JS_PROP_TMASK)) {
        if ((prs->flags & JS_PROP_TMASK) == JS_PROP_GETSET) {
          desc->flags |= JS_PROP_GETSET;
          if (pr->u.getset.getter)
            desc->getter =
                JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, pr->u.getset.getter));
          if (pr->u.getset.setter)
            desc->setter =
                JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, pr->u.getset.setter));
        } else if ((prs->flags & JS_PROP_TMASK) == JS_PROP_VARREF) {
          JSValue val = *pr->u.var_ref->pvalue;
          if (unlikely(JS_IsUninitialized(val))) {
            JS_ThrowReferenceErrorUninitialized(ctx, prs->atom);
            return -1;
          }
          desc->value = JS_DupValue(ctx, val);
        } else if ((prs->flags & JS_PROP_TMASK) == JS_PROP_AUTOINIT) {
          /* Instantiate property and retry */
          if (JS_AutoInitProperty(ctx, p, prop, pr, prs))
            return -1;
          goto retry;
        }
      } else {
        desc->value = JS_DupValue(ctx, pr->u.value);
      }
    } else {
      /* for consistency, send the exception even if desc is NULL */
      if (unlikely((prs->flags & JS_PROP_TMASK) == JS_PROP_VARREF)) {
        if (unlikely(JS_IsUninitialized(*pr->u.var_ref->pvalue))) {
          JS_ThrowReferenceErrorUninitialized(ctx, prs->atom);
          return -1;
        }
      } else if ((prs->flags & JS_PROP_TMASK) == JS_PROP_AUTOINIT) {
        /* nothing to do: delay instantiation until actual value and/or
         * attributes are read */
      }
    }
    return TRUE;
  }
  if (p->is_exotic) {
    if (p->fast_array) {
      /* specific case for fast arrays */
      if (__JS_AtomIsTaggedInt(prop)) {
        uint32_t idx;
        idx = __JS_AtomToUInt32(prop);
        if (idx < p->u.array.count) {
          if (desc) {
            desc->flags =
                JS_PROP_WRITABLE | JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
            desc->getter = JS_UNDEFINED;
            desc->setter = JS_UNDEFINED;
            desc->value =
                JS_GetPropertyUint32(ctx, JS_MKPTR(JS_TAG_OBJECT, p), idx);
          }
          return TRUE;
        }
      }
    } else {
      const JSClassExoticMethods *em = ctx->rt->class_array[p->class_id].exotic;
      if (em && em->get_own_property) {
        return em->get_own_property(ctx, desc, JS_MKPTR(JS_TAG_OBJECT, p),
                                    prop);
      }
    }
  }
  return FALSE;
}

int JS_GetOwnProperty(JSContext *ctx, JSPropertyDescriptor *desc,
                      JSValueConst obj, JSAtom prop) {
  if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT) {
    JS_ThrowTypeErrorNotAnObject(ctx);
    return -1;
  }
  return JS_GetOwnPropertyInternal(ctx, desc, JS_VALUE_GET_OBJ(obj), prop);
}

/* return -1 if exception (Proxy object only) or TRUE/FALSE */
int JS_IsExtensible(JSContext *ctx, JSValueConst obj) {
  JSObject *p;

  if (unlikely(JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT))
    return FALSE;
  p = JS_VALUE_GET_OBJ(obj);
  if (unlikely(p->class_id == JS_CLASS_PROXY))
    return js_proxy_isExtensible(ctx, obj);
  else
    return p->extensible;
}

/* return -1 if exception (Proxy object only) or TRUE/FALSE */
int JS_PreventExtensions(JSContext *ctx, JSValueConst obj) {
  JSObject *p;

  if (unlikely(JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT))
    return FALSE;
  p = JS_VALUE_GET_OBJ(obj);
  if (unlikely(p->class_id == JS_CLASS_PROXY))
    return js_proxy_preventExtensions(ctx, obj);
  p->extensible = FALSE;
  return TRUE;
}

/* return -1 if exception otherwise TRUE or FALSE */
int JS_HasProperty(JSContext *ctx, JSValueConst obj, JSAtom prop) {
  JSObject *p;
  int ret;
  JSValue obj1;

  if (unlikely(JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT))
    return FALSE;
  p = JS_VALUE_GET_OBJ(obj);
  for (;;) {
    if (p->is_exotic) {
      const JSClassExoticMethods *em = ctx->rt->class_array[p->class_id].exotic;
      if (em && em->has_property) {
        /* has_property can free the prototype */
        obj1 = JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, p));
        ret = em->has_property(ctx, obj1, prop);
        JS_FreeValue(ctx, obj1);
        return ret;
      }
    }
    /* JS_GetOwnPropertyInternal can free the prototype */
    JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, p));
    ret = JS_GetOwnPropertyInternal(ctx, NULL, p, prop);
    JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, p));
    if (ret != 0)
      return ret;
    if (p->class_id >= JS_CLASS_UINT8C_ARRAY &&
        p->class_id <= JS_CLASS_FLOAT64_ARRAY) {
      ret = JS_AtomIsNumericIndex(ctx, prop);
      if (ret != 0) {
        if (ret < 0)
          return -1;
        return FALSE;
      }
    }
    p = p->shape->proto;
    if (!p)
      break;
  }
  return FALSE;
}

/* return JS_ATOM_NULL in case of exception */
JSAtom JS_ValueToAtom(JSContext *ctx, JSValueConst val) {
  JSAtom atom;
  uint32_t tag;
  tag = JS_VALUE_GET_TAG(val);
  if (tag == JS_TAG_INT && (uint32_t)JS_VALUE_GET_INT(val) <= JS_ATOM_MAX_INT) {
    /* fast path for integer values */
    atom = __JS_AtomFromUInt32(JS_VALUE_GET_INT(val));
  } else if (tag == JS_TAG_SYMBOL) {
    JSAtomStruct *p = JS_VALUE_GET_PTR(val);
    atom = JS_DupAtom(ctx, js_get_atom_index(ctx->rt, p));
  } else {
    JSValue str;
    str = JS_ToPropertyKey(ctx, val);
    if (JS_IsException(str))
      return JS_ATOM_NULL;
    if (JS_VALUE_GET_TAG(str) == JS_TAG_SYMBOL) {
      atom = js_symbol_to_atom(ctx, str);
    } else {
      atom = JS_NewAtomStr(ctx, JS_VALUE_GET_STRING(str));
    }
  }
  return atom;
}

JSValue JS_GetPropertyValue(JSContext *ctx, JSValueConst this_obj,
                            JSValue prop) {
  JSAtom atom;
  JSValue ret;

  if (likely(JS_VALUE_GET_TAG(this_obj) == JS_TAG_OBJECT &&
             JS_VALUE_GET_TAG(prop) == JS_TAG_INT)) {
    JSObject *p;
    uint32_t idx, len;
    /* fast path for array access */
    p = JS_VALUE_GET_OBJ(this_obj);
    idx = JS_VALUE_GET_INT(prop);
    len = (uint32_t)p->u.array.count;
    if (unlikely(idx >= len))
      goto slow_path;
    switch (p->class_id) {
    case JS_CLASS_ARRAY:
    case JS_CLASS_ARGUMENTS:
      return JS_DupValue(ctx, p->u.array.u.values[idx]);
    case JS_CLASS_INT8_ARRAY:
      return JS_NewInt32(ctx, p->u.array.u.int8_ptr[idx]);
    case JS_CLASS_UINT8C_ARRAY:
    case JS_CLASS_UINT8_ARRAY:
      return JS_NewInt32(ctx, p->u.array.u.uint8_ptr[idx]);
    case JS_CLASS_INT16_ARRAY:
      return JS_NewInt32(ctx, p->u.array.u.int16_ptr[idx]);
    case JS_CLASS_UINT16_ARRAY:
      return JS_NewInt32(ctx, p->u.array.u.uint16_ptr[idx]);
    case JS_CLASS_INT32_ARRAY:
      return JS_NewInt32(ctx, p->u.array.u.int32_ptr[idx]);
    case JS_CLASS_UINT32_ARRAY:
      return JS_NewUint32(ctx, p->u.array.u.uint32_ptr[idx]);
#ifdef CONFIG_BIGNUM
    case JS_CLASS_BIG_INT64_ARRAY:
      return JS_NewBigInt64(ctx, p->u.array.u.int64_ptr[idx]);
    case JS_CLASS_BIG_UINT64_ARRAY:
      return JS_NewBigUint64(ctx, p->u.array.u.uint64_ptr[idx]);
#endif
    case JS_CLASS_FLOAT32_ARRAY:
      return __JS_NewFloat64(ctx, p->u.array.u.float_ptr[idx]);
    case JS_CLASS_FLOAT64_ARRAY:
      return __JS_NewFloat64(ctx, p->u.array.u.double_ptr[idx]);
    default:
      goto slow_path;
    }
  } else {
  slow_path:
    atom = JS_ValueToAtom(ctx, prop);
    JS_FreeValue(ctx, prop);
    if (unlikely(atom == JS_ATOM_NULL))
      return JS_EXCEPTION;
    ret = JS_GetProperty(ctx, this_obj, atom);
    JS_FreeAtom(ctx, atom);
    return ret;
  }
}

JSValue JS_GetPropertyUint32(JSContext *ctx, JSValueConst this_obj,
                             uint32_t idx) {
  return JS_GetPropertyValue(ctx, this_obj, JS_NewUint32(ctx, idx));
}

/* Check if an object has a generalized numeric property. Return value:
   -1 for exception,
   TRUE if property exists, stored into *pval,
   FALSE if property does not exist.
 */
int JS_TryGetPropertyInt64(JSContext *ctx, JSValueConst obj, int64_t idx,
                           JSValue *pval) {
  JSValue val = JS_UNDEFINED;
  JSAtom prop;
  int present;

  if (likely((uint64_t)idx <= JS_ATOM_MAX_INT)) {
    /* fast path */
    present = JS_HasProperty(ctx, obj, __JS_AtomFromUInt32(idx));
    if (present > 0) {
      val = JS_GetPropertyValue(ctx, obj, JS_NewInt32(ctx, idx));
      if (unlikely(JS_IsException(val)))
        present = -1;
    }
  } else {
    prop = JS_NewAtomInt64(ctx, idx);
    present = -1;
    if (likely(prop != JS_ATOM_NULL)) {
      present = JS_HasProperty(ctx, obj, prop);
      if (present > 0) {
        val = JS_GetProperty(ctx, obj, prop);
        if (unlikely(JS_IsException(val)))
          present = -1;
      }
      JS_FreeAtom(ctx, prop);
    }
  }
  *pval = val;
  return present;
}

JSValue JS_GetPropertyInt64(JSContext *ctx, JSValueConst obj, int64_t idx) {
  JSAtom prop;
  JSValue val;

  if ((uint64_t)idx <= INT32_MAX) {
    /* fast path for fast arrays */
    return JS_GetPropertyValue(ctx, obj, JS_NewInt32(ctx, idx));
  }
  prop = JS_NewAtomInt64(ctx, idx);
  if (prop == JS_ATOM_NULL)
    return JS_EXCEPTION;

  val = JS_GetProperty(ctx, obj, prop);
  JS_FreeAtom(ctx, prop);
  return val;
}

JSValue JS_GetPropertyStr(JSContext *ctx, JSValueConst this_obj,
                          const char *prop) {
  JSAtom atom;
  JSValue ret;
  atom = JS_NewAtom(ctx, prop);
  ret = JS_GetProperty(ctx, this_obj, atom);
  JS_FreeAtom(ctx, atom);
  return ret;
}

/* Note: the property value is not initialized. Return NULL if memory
   error. */
JSProperty *add_property(JSContext *ctx, JSObject *p, JSAtom prop,
                         int prop_flags) {
  JSShape *sh, *new_sh;

  sh = p->shape;
  if (sh->is_hashed) {
    /* try to find an existing shape */
    new_sh = find_hashed_shape_prop(ctx->rt, sh, prop, prop_flags);
    if (new_sh) {
      /* matching shape found: use it */
      /*  the property array may need to be resized */
      // prop_size here works like the buffer size, not the prop_count
      if (new_sh->prop_size != sh->prop_size) { 
        JSProperty *new_prop;
        new_prop =
            js_realloc(ctx, p->prop, sizeof(p->prop[0]) * new_sh->prop_size);
        if (!new_prop)
          return NULL;
        p->prop = new_prop;
      }
      p->shape = js_dup_shape(new_sh);
      js_free_shape(ctx->rt, sh);
      return &p->prop[new_sh->prop_count - 1];
    } else if (sh->header.ref_count != 1) {
      /* if the shape is shared, clone it */
      new_sh = js_clone_shape(ctx, sh);
      if (!new_sh)
        return NULL;
      /* hash the cloned shape */
      new_sh->is_hashed = TRUE;
      js_shape_hash_link(ctx->rt, new_sh);
      js_free_shape(ctx->rt, p->shape);
      p->shape = new_sh;
    }
  }
  assert(p->shape->header.ref_count == 1);
  if (add_shape_property(ctx, &p->shape, p, prop, prop_flags))
    return NULL;
  return &p->prop[p->shape->prop_count - 1];
}

/* can be called on Array or Arguments objects. return < 0 if
   memory alloc error. */
no_inline __exception int convert_fast_array_to_array(JSContext *ctx,
                                                      JSObject *p) {
  JSProperty *pr;
  JSShape *sh;
  JSValue *tab;
  uint32_t i, len, new_count;

  if (js_shape_prepare_update(ctx, p, NULL))
    return -1;
  len = p->u.array.count;
  /* resize the properties once to simplify the error handling */
  sh = p->shape;
  new_count = sh->prop_count + len;
  if (new_count > sh->prop_size) {
    if (resize_properties(ctx, &p->shape, p, new_count))
      return -1;
  }

  tab = p->u.array.u.values;
  for (i = 0; i < len; i++) {
    /* add_property cannot fail here but
       __JS_AtomFromUInt32(i) fails for i > INT32_MAX */
    pr = add_property(ctx, p, __JS_AtomFromUInt32(i), JS_PROP_C_W_E);
    pr->u.value = *tab++;
  }
  js_free(ctx, p->u.array.u.values);
  p->u.array.count = 0;
  p->u.array.u.values = NULL; /* fail safe */
  p->u.array.u1.size = 0;
  p->fast_array = 0;
  return 0;
}

int delete_property(JSContext *ctx, JSObject *p, JSAtom atom) {
  JSShape *sh;
  JSShapeProperty *pr, *lpr, *prop;
  JSProperty *pr1;
  uint32_t lpr_idx;
  intptr_t h, h1;

redo:
  sh = p->shape;
  h1 = atom & sh->prop_hash_mask;
  h = prop_hash_end(sh)[-h1 - 1];
  prop = get_shape_prop(sh);
  lpr = NULL;
  lpr_idx = 0; /* prevent warning */
  while (h != 0) {
    pr = &prop[h - 1];
    if (likely(pr->atom == atom)) {
      /* found ! */
      if (!(pr->flags & JS_PROP_CONFIGURABLE))
        return FALSE;
      /* realloc the shape if needed */
      if (lpr)
        lpr_idx = lpr - get_shape_prop(sh);
      if (js_shape_prepare_update(ctx, p, &pr))
        return -1;
      sh = p->shape;
      /* remove property */
      if (lpr) {
        lpr = get_shape_prop(sh) + lpr_idx;
        lpr->hash_next = pr->hash_next;
      } else {
        prop_hash_end(sh)[-h1 - 1] = pr->hash_next;
      }
      sh->deleted_prop_count++;
      /* free the entry */
      pr1 = &p->prop[h - 1];
      free_property(ctx->rt, pr1, pr->flags);
      JS_FreeAtom(ctx, pr->atom);
      /* put default values */
      pr->flags = 0;
      pr->atom = JS_ATOM_NULL;
      pr1->u.value = JS_UNDEFINED;

      /* compact the properties if too many deleted properties */
      if (sh->deleted_prop_count >= 8 &&
          sh->deleted_prop_count >= ((unsigned)sh->prop_count / 2)) {
        compact_properties(ctx, p);
      }
      return TRUE;
    }
    lpr = pr;
    h = pr->hash_next;
  }

  if (p->is_exotic) {
    if (p->fast_array) {
      uint32_t idx;
      if (JS_AtomIsArrayIndex(ctx, &idx, atom) && idx < p->u.array.count) {
        if (p->class_id == JS_CLASS_ARRAY ||
            p->class_id == JS_CLASS_ARGUMENTS) {
          /* Special case deleting the last element of a fast Array */
          if (idx == p->u.array.count - 1) {
            JS_FreeValue(ctx, p->u.array.u.values[idx]);
            p->u.array.count = idx;
            return TRUE;
          }
          if (convert_fast_array_to_array(ctx, p))
            return -1;
          goto redo;
        } else {
          return FALSE;
        }
      }
    } else {
      const JSClassExoticMethods *em = ctx->rt->class_array[p->class_id].exotic;
      if (em && em->delete_property) {
        return em->delete_property(ctx, JS_MKPTR(JS_TAG_OBJECT, p), atom);
      }
    }
  }
  /* not found */
  return TRUE;
}

int call_setter(JSContext *ctx, JSObject *setter, JSValueConst this_obj,
                JSValue val, int flags) {
  JSValue ret, func;
  if (likely(setter)) {
    func = JS_MKPTR(JS_TAG_OBJECT, setter);
    /* Note: the field could be removed in the setter */
    func = JS_DupValue(ctx, func);
    ret = JS_CallFree(ctx, func, this_obj, 1, (JSValueConst *)&val);
    JS_FreeValue(ctx, val);
    if (JS_IsException(ret))
      return -1;
    JS_FreeValue(ctx, ret);
    return TRUE;
  } else {
    JS_FreeValue(ctx, val);
    if ((flags & JS_PROP_THROW) ||
        ((flags & JS_PROP_THROW_STRICT) && is_strict_mode(ctx))) {
      JS_ThrowTypeError(ctx, "no setter for property");
      return -1;
    }
    return FALSE;
  }
}

/* set the array length and remove the array elements if necessary. */
int set_array_length(JSContext *ctx, JSObject *p, JSValue val, int flags) {
  uint32_t len, idx, cur_len;
  int i, ret;

  /* Note: this call can reallocate the properties of 'p' */
  ret = JS_ToArrayLengthFree(ctx, &len, val, FALSE);
  if (ret)
    return -1;
  /* JS_ToArrayLengthFree() must be done before the read-only test */
  if (unlikely(!(p->shape->prop[0].flags & JS_PROP_WRITABLE)))
    return JS_ThrowTypeErrorReadOnly(ctx, flags, JS_ATOM_length);

  if (likely(p->fast_array)) {
    uint32_t old_len = p->u.array.count;
    if (len < old_len) {
      for (i = len; i < old_len; i++) {
        JS_FreeValue(ctx, p->u.array.u.values[i]);
      }
      p->u.array.count = len;
    }
    p->prop[0].u.value = JS_NewUint32(ctx, len);
  } else {
    /* Note: length is always a uint32 because the object is an
       array */
    JS_ToUint32(ctx, &cur_len, p->prop[0].u.value);
    if (len < cur_len) {
      uint32_t d;
      JSShape *sh;
      JSShapeProperty *pr;

      d = cur_len - len;
      sh = p->shape;
      if (d <= sh->prop_count) {
        JSAtom atom;

        /* faster to iterate */
        while (cur_len > len) {
          atom = JS_NewAtomUInt32(ctx, cur_len - 1);
          ret = delete_property(ctx, p, atom);
          JS_FreeAtom(ctx, atom);
          if (unlikely(!ret)) {
            /* unlikely case: property is not
               configurable */
            break;
          }
          cur_len--;
        }
      } else {
        /* faster to iterate thru all the properties. Need two
           passes in case one of the property is not
           configurable */
        cur_len = len;
        for (i = 0, pr = get_shape_prop(sh); i < sh->prop_count; i++, pr++) {
          if (pr->atom != JS_ATOM_NULL &&
              JS_AtomIsArrayIndex(ctx, &idx, pr->atom)) {
            if (idx >= cur_len && !(pr->flags & JS_PROP_CONFIGURABLE)) {
              cur_len = idx + 1;
            }
          }
        }

        for (i = 0, pr = get_shape_prop(sh); i < sh->prop_count; i++, pr++) {
          if (pr->atom != JS_ATOM_NULL &&
              JS_AtomIsArrayIndex(ctx, &idx, pr->atom)) {
            if (idx >= cur_len) {
              /* remove the property */
              delete_property(ctx, p, pr->atom);
              /* WARNING: the shape may have been modified */
              sh = p->shape;
              pr = get_shape_prop(sh) + i;
            }
          }
        }
      }
    } else {
      cur_len = len;
    }
    set_value(ctx, &p->prop[0].u.value, JS_NewUint32(ctx, cur_len));
    if (unlikely(cur_len > len)) {
      return JS_ThrowTypeErrorOrFalse(ctx, flags, "not configurable");
    }
  }
  return TRUE;
}

/* return -1 if exception */
int expand_fast_array(JSContext *ctx, JSObject *p, uint32_t new_len) {
  uint32_t new_size;
  size_t slack;
  JSValue *new_array_prop;
  /* XXX: potential arithmetic overflow */
  new_size = max_int(new_len, p->u.array.u1.size * 3 / 2);
  new_array_prop =
      js_realloc2(ctx, p->u.array.u.values, sizeof(JSValue) * new_size, &slack);
  if (!new_array_prop)
    return -1;
  new_size += slack / sizeof(*new_array_prop);
  p->u.array.u.values = new_array_prop;
  p->u.array.u1.size = new_size;
  return 0;
}

/* Preconditions: 'p' must be of class JS_CLASS_ARRAY, p->fast_array =
   TRUE and p->extensible = TRUE */
int add_fast_array_element(JSContext *ctx, JSObject *p, JSValue val,
                           int flags) {
  uint32_t new_len, array_len;
  /* extend the array by one */
  /* XXX: convert to slow array if new_len > 2^31-1 elements */
  new_len = p->u.array.count + 1;
  /* update the length if necessary. We assume that if the length is
     not an integer, then if it >= 2^31.  */
  if (likely(JS_VALUE_GET_TAG(p->prop[0].u.value) == JS_TAG_INT)) {
    array_len = JS_VALUE_GET_INT(p->prop[0].u.value);
    if (new_len > array_len) {
      if (unlikely(!(get_shape_prop(p->shape)->flags & JS_PROP_WRITABLE))) {
        JS_FreeValue(ctx, val);
        return JS_ThrowTypeErrorReadOnly(ctx, flags, JS_ATOM_length);
      }
      p->prop[0].u.value = JS_NewInt32(ctx, new_len);
    }
  }
  if (unlikely(new_len > p->u.array.u1.size)) {
    if (expand_fast_array(ctx, p, new_len)) {
      JS_FreeValue(ctx, val);
      return -1;
    }
  }
  p->u.array.u.values[new_len - 1] = val;
  p->u.array.count = new_len;
  return TRUE;
}

void js_free_desc(JSContext *ctx, JSPropertyDescriptor *desc) {
  JS_FreeValue(ctx, desc->getter);
  JS_FreeValue(ctx, desc->setter);
  JS_FreeValue(ctx, desc->value);
}

/* generic (and slower) version of JS_SetProperty() for
 * Reflect.set(). 'obj' must be an object.  */
int JS_SetPropertyGeneric(JSContext *ctx, JSValueConst obj, JSAtom prop,
                          JSValue val, JSValueConst this_obj, int flags) {
  int ret;
  JSPropertyDescriptor desc;
  JSValue obj1;
  JSObject *p;

  obj1 = JS_DupValue(ctx, obj);
  for (;;) {
    p = JS_VALUE_GET_OBJ(obj1);
    if (p->is_exotic) {
      const JSClassExoticMethods *em = ctx->rt->class_array[p->class_id].exotic;
      if (em && em->set_property) {
        ret = em->set_property(ctx, obj1, prop, val, this_obj, flags);
        JS_FreeValue(ctx, obj1);
        JS_FreeValue(ctx, val);
        return ret;
      }
    }

    ret = JS_GetOwnPropertyInternal(ctx, &desc, p, prop);
    if (ret < 0) {
      JS_FreeValue(ctx, obj1);
      JS_FreeValue(ctx, val);
      return ret;
    }
    if (ret) {
      if (desc.flags & JS_PROP_GETSET) {
        JSObject *setter;
        if (JS_IsUndefined(desc.setter))
          setter = NULL;
        else
          setter = JS_VALUE_GET_OBJ(desc.setter);
        ret = call_setter(ctx, setter, this_obj, val, flags);
        JS_FreeValue(ctx, desc.getter);
        JS_FreeValue(ctx, desc.setter);
        JS_FreeValue(ctx, obj1);
        return ret;
      } else {
        JS_FreeValue(ctx, desc.value);
        if (!(desc.flags & JS_PROP_WRITABLE)) {
          JS_FreeValue(ctx, obj1);
          goto read_only_error;
        }
      }
      break;
    }
    /* Note: at this point 'obj1' cannot be a proxy. XXX: may have
       to check recursion */
    obj1 = JS_GetPrototypeFree(ctx, obj1);
    if (JS_IsNull(obj1))
      break;
  }
  JS_FreeValue(ctx, obj1);

  if (!JS_IsObject(this_obj)) {
    JS_FreeValue(ctx, val);
    return JS_ThrowTypeErrorOrFalse(ctx, flags, "receiver is not an object");
  }

  p = JS_VALUE_GET_OBJ(this_obj);

  /* modify the property in this_obj if it already exists */
  ret = JS_GetOwnPropertyInternal(ctx, &desc, p, prop);
  if (ret < 0) {
    JS_FreeValue(ctx, val);
    return ret;
  }
  if (ret) {
    if (desc.flags & JS_PROP_GETSET) {
      JS_FreeValue(ctx, desc.getter);
      JS_FreeValue(ctx, desc.setter);
      JS_FreeValue(ctx, val);
      return JS_ThrowTypeErrorOrFalse(ctx, flags, "setter is forbidden");
    } else {
      JS_FreeValue(ctx, desc.value);
      if (!(desc.flags & JS_PROP_WRITABLE) ||
          p->class_id == JS_CLASS_MODULE_NS) {
      read_only_error:
        JS_FreeValue(ctx, val);
        return JS_ThrowTypeErrorReadOnly(ctx, flags, prop);
      }
    }
    ret = JS_DefineProperty(ctx, this_obj, prop, val, JS_UNDEFINED,
                            JS_UNDEFINED, JS_PROP_HAS_VALUE);
    JS_FreeValue(ctx, val);
    return ret;
  }

  ret = JS_CreateProperty(ctx, p, prop, val, JS_UNDEFINED, JS_UNDEFINED,
                          flags | JS_PROP_HAS_VALUE | JS_PROP_HAS_ENUMERABLE |
                              JS_PROP_HAS_WRITABLE | JS_PROP_HAS_CONFIGURABLE |
                              JS_PROP_C_W_E);
  JS_FreeValue(ctx, val);
  return ret;
}

/* return -1 in case of exception or TRUE or FALSE. Warning: 'val' is
   freed by the function. 'flags' is a bitmask of JS_PROP_NO_ADD,
   JS_PROP_THROW or JS_PROP_THROW_STRICT. If JS_PROP_NO_ADD is set,
   the new property is not added and an error is raised. */
int JS_SetPropertyInternal(JSContext *ctx, JSValueConst this_obj, JSAtom prop,
                           JSValue val, int flags) {
  JSObject *p, *p1;
  JSShapeProperty *prs;
  JSProperty *pr;
  uint32_t tag;
  JSPropertyDescriptor desc;
  int ret;
#if 0
    printf("JS_SetPropertyInternal: "); print_atom(ctx, prop); printf("\n");
#endif
  tag = JS_VALUE_GET_TAG(this_obj);
  if (unlikely(tag != JS_TAG_OBJECT)) {
    switch (tag) {
    case JS_TAG_NULL:
      JS_FreeValue(ctx, val);
      JS_ThrowTypeErrorAtom(ctx, "cannot set property '%s' of null", prop);
      return -1;
    case JS_TAG_UNDEFINED:
      JS_FreeValue(ctx, val);
      JS_ThrowTypeErrorAtom(ctx, "cannot set property '%s' of undefined", prop);
      return -1;
    default:
      /* even on a primitive type we can have setters on the prototype */
      p = NULL;
      p1 = JS_VALUE_GET_OBJ(JS_GetPrototypePrimitive(ctx, this_obj));
      goto prototype_lookup;
    }
  }
  p = JS_VALUE_GET_OBJ(this_obj);
retry:
  prs = find_own_property(&pr, p, prop);
  if (prs) {
    if (likely((prs->flags & (JS_PROP_TMASK | JS_PROP_WRITABLE |
                              JS_PROP_LENGTH)) == JS_PROP_WRITABLE)) {
      /* fast case */
      set_value(ctx, &pr->u.value, val);
      return TRUE;
    } else if (prs->flags & JS_PROP_LENGTH) {
      assert(p->class_id == JS_CLASS_ARRAY);
      assert(prop == JS_ATOM_length);
      return set_array_length(ctx, p, val, flags);
    } else if ((prs->flags & JS_PROP_TMASK) == JS_PROP_GETSET) {
      return call_setter(ctx, pr->u.getset.setter, this_obj, val, flags);
    } else if ((prs->flags & JS_PROP_TMASK) == JS_PROP_VARREF) {
      /* JS_PROP_WRITABLE is always true for variable
         references, but they are write protected in module name
         spaces. */
      if (p->class_id == JS_CLASS_MODULE_NS)
        goto read_only_prop;
      set_value(ctx, pr->u.var_ref->pvalue, val);
      return TRUE;
    } else if ((prs->flags & JS_PROP_TMASK) == JS_PROP_AUTOINIT) {
      /* Instantiate property and retry (potentially useless) */
      if (JS_AutoInitProperty(ctx, p, prop, pr, prs)) {
        JS_FreeValue(ctx, val);
        return -1;
      }
      goto retry;
    } else {
      goto read_only_prop;
    }
  }

  p1 = p;
  for (;;) {
    if (p1->is_exotic) {
      if (p1->fast_array) {
        if (__JS_AtomIsTaggedInt(prop)) {
          uint32_t idx = __JS_AtomToUInt32(prop);
          if (idx < p1->u.array.count) {
            if (unlikely(p == p1))
              return JS_SetPropertyValue(ctx, this_obj, JS_NewInt32(ctx, idx),
                                         val, flags);
            else
              break;
          } else if (p1->class_id >= JS_CLASS_UINT8C_ARRAY &&
                     p1->class_id <= JS_CLASS_FLOAT64_ARRAY) {
            goto typed_array_oob;
          }
        } else if (p1->class_id >= JS_CLASS_UINT8C_ARRAY &&
                   p1->class_id <= JS_CLASS_FLOAT64_ARRAY) {
          ret = JS_AtomIsNumericIndex(ctx, prop);
          if (ret != 0) {
            if (ret < 0) {
              JS_FreeValue(ctx, val);
              return -1;
            }
          typed_array_oob:
            val = JS_ToNumberFree(ctx, val);
            JS_FreeValue(ctx, val);
            if (JS_IsException(val))
              return -1;
            return JS_ThrowTypeErrorOrFalse(ctx, flags,
                                            "out-of-bound numeric index");
          }
        }
      } else {
        const JSClassExoticMethods *em =
            ctx->rt->class_array[p1->class_id].exotic;
        if (em) {
          JSValue obj1;
          if (em->set_property) {
            /* set_property can free the prototype */
            obj1 = JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, p1));
            ret = em->set_property(ctx, obj1, prop, val, this_obj, flags);
            JS_FreeValue(ctx, obj1);
            JS_FreeValue(ctx, val);
            return ret;
          }
          if (em->get_own_property) {
            /* get_own_property can free the prototype */
            obj1 = JS_DupValue(ctx, JS_MKPTR(JS_TAG_OBJECT, p1));
            ret = em->get_own_property(ctx, &desc, obj1, prop);
            JS_FreeValue(ctx, obj1);
            if (ret < 0) {
              JS_FreeValue(ctx, val);
              return ret;
            }
            if (ret) {
              if (desc.flags & JS_PROP_GETSET) {
                JSObject *setter;
                if (JS_IsUndefined(desc.setter))
                  setter = NULL;
                else
                  setter = JS_VALUE_GET_OBJ(desc.setter);
                ret = call_setter(ctx, setter, this_obj, val, flags);
                JS_FreeValue(ctx, desc.getter);
                JS_FreeValue(ctx, desc.setter);
                return ret;
              } else {
                JS_FreeValue(ctx, desc.value);
                if (!(desc.flags & JS_PROP_WRITABLE))
                  goto read_only_prop;
                if (likely(p == p1)) {
                  ret =
                      JS_DefineProperty(ctx, this_obj, prop, val, JS_UNDEFINED,
                                        JS_UNDEFINED, JS_PROP_HAS_VALUE);
                  JS_FreeValue(ctx, val);
                  return ret;
                } else {
                  break;
                }
              }
            }
          }
        }
      }
    }
    p1 = p1->shape->proto;
  prototype_lookup:
    if (!p1)
      break;

  retry2:
    prs = find_own_property(&pr, p1, prop);
    if (prs) {
      if ((prs->flags & JS_PROP_TMASK) == JS_PROP_GETSET) {
        return call_setter(ctx, pr->u.getset.setter, this_obj, val, flags);
      } else if ((prs->flags & JS_PROP_TMASK) == JS_PROP_AUTOINIT) {
        /* Instantiate property and retry (potentially useless) */
        if (JS_AutoInitProperty(ctx, p1, prop, pr, prs))
          return -1;
        goto retry2;
      } else if (!(prs->flags & JS_PROP_WRITABLE)) {
      read_only_prop:
        JS_FreeValue(ctx, val);
        return JS_ThrowTypeErrorReadOnly(ctx, flags, prop);
      }
    }
  }

  if (unlikely(flags & JS_PROP_NO_ADD)) {
    JS_FreeValue(ctx, val);
    JS_ThrowReferenceErrorNotDefined(ctx, prop);
    return -1;
  }

  if (unlikely(!p)) {
    JS_FreeValue(ctx, val);
    return JS_ThrowTypeErrorOrFalse(ctx, flags, "not an object");
  }

  if (unlikely(!p->extensible)) {
    JS_FreeValue(ctx, val);
    return JS_ThrowTypeErrorOrFalse(ctx, flags, "object is not extensible");
  }

  if (p->is_exotic) {
    if (p->class_id == JS_CLASS_ARRAY && p->fast_array &&
        __JS_AtomIsTaggedInt(prop)) {
      uint32_t idx = __JS_AtomToUInt32(prop);
      if (idx == p->u.array.count) {
        /* fast case */
        return add_fast_array_element(ctx, p, val, flags);
      } else {
        goto generic_create_prop;
      }
    } else {
    generic_create_prop:
      ret = JS_CreateProperty(
          ctx, p, prop, val, JS_UNDEFINED, JS_UNDEFINED,
          flags | JS_PROP_HAS_VALUE | JS_PROP_HAS_ENUMERABLE |
              JS_PROP_HAS_WRITABLE | JS_PROP_HAS_CONFIGURABLE | JS_PROP_C_W_E);
      JS_FreeValue(ctx, val);
      return ret;
    }
  }

  pr = add_property(ctx, p, prop, JS_PROP_C_W_E);
  if (unlikely(!pr)) {
    JS_FreeValue(ctx, val);
    return -1;
  }
  pr->u.value = val;
  return TRUE;
}

/* flags can be JS_PROP_THROW or JS_PROP_THROW_STRICT */
int JS_SetPropertyValue(JSContext *ctx, JSValueConst this_obj, JSValue prop,
                        JSValue val, int flags) {
  if (likely(JS_VALUE_GET_TAG(this_obj) == JS_TAG_OBJECT &&
             JS_VALUE_GET_TAG(prop) == JS_TAG_INT)) {
    JSObject *p;
    uint32_t idx;
    double d;
    int32_t v;

    /* fast path for array access */
    p = JS_VALUE_GET_OBJ(this_obj);
    idx = JS_VALUE_GET_INT(prop);
    switch (p->class_id) {
    case JS_CLASS_ARRAY:
      if (unlikely(idx >= (uint32_t)p->u.array.count)) {
        JSObject *p1;
        JSShape *sh1;

        /* fast path to add an element to the array */
        if (idx != (uint32_t)p->u.array.count || !p->fast_array ||
            !p->extensible)
          goto slow_path;
        /* check if prototype chain has a numeric property */
        p1 = p->shape->proto;
        while (p1 != NULL) {
          sh1 = p1->shape;
          if (p1->class_id == JS_CLASS_ARRAY) {
            if (unlikely(!p1->fast_array))
              goto slow_path;
          } else if (p1->class_id == JS_CLASS_OBJECT) {
            if (unlikely(sh1->has_small_array_index))
              goto slow_path;
          } else {
            goto slow_path;
          }
          p1 = sh1->proto;
        }
        /* add element */
        return add_fast_array_element(ctx, p, val, flags);
      }
      set_value(ctx, &p->u.array.u.values[idx], val);
      break;
    case JS_CLASS_ARGUMENTS:
      if (unlikely(idx >= (uint32_t)p->u.array.count))
        goto slow_path;
      set_value(ctx, &p->u.array.u.values[idx], val);
      break;
    case JS_CLASS_UINT8C_ARRAY:
      if (JS_ToUint8ClampFree(ctx, &v, val))
        return -1;
      /* Note: the conversion can detach the typed array, so the
         array bound check must be done after */
      if (unlikely(idx >= (uint32_t)p->u.array.count))
        goto ta_out_of_bound;
      p->u.array.u.uint8_ptr[idx] = v;
      break;
    case JS_CLASS_INT8_ARRAY:
    case JS_CLASS_UINT8_ARRAY:
      if (JS_ToInt32Free(ctx, &v, val))
        return -1;
      if (unlikely(idx >= (uint32_t)p->u.array.count))
        goto ta_out_of_bound;
      p->u.array.u.uint8_ptr[idx] = v;
      break;
    case JS_CLASS_INT16_ARRAY:
    case JS_CLASS_UINT16_ARRAY:
      if (JS_ToInt32Free(ctx, &v, val))
        return -1;
      if (unlikely(idx >= (uint32_t)p->u.array.count))
        goto ta_out_of_bound;
      p->u.array.u.uint16_ptr[idx] = v;
      break;
    case JS_CLASS_INT32_ARRAY:
    case JS_CLASS_UINT32_ARRAY:
      if (JS_ToInt32Free(ctx, &v, val))
        return -1;
      if (unlikely(idx >= (uint32_t)p->u.array.count))
        goto ta_out_of_bound;
      p->u.array.u.uint32_ptr[idx] = v;
      break;
#ifdef CONFIG_BIGNUM
    case JS_CLASS_BIG_INT64_ARRAY:
    case JS_CLASS_BIG_UINT64_ARRAY:
      /* XXX: need specific conversion function */
      {
        int64_t v;
        if (JS_ToBigInt64Free(ctx, &v, val))
          return -1;
        if (unlikely(idx >= (uint32_t)p->u.array.count))
          goto ta_out_of_bound;
        p->u.array.u.uint64_ptr[idx] = v;
      }
      break;
#endif
    case JS_CLASS_FLOAT32_ARRAY:
      if (JS_ToFloat64Free(ctx, &d, val))
        return -1;
      if (unlikely(idx >= (uint32_t)p->u.array.count))
        goto ta_out_of_bound;
      p->u.array.u.float_ptr[idx] = d;
      break;
    case JS_CLASS_FLOAT64_ARRAY:
      if (JS_ToFloat64Free(ctx, &d, val))
        return -1;
      if (unlikely(idx >= (uint32_t)p->u.array.count)) {
      ta_out_of_bound:
        return JS_ThrowTypeErrorOrFalse(ctx, flags,
                                        "out-of-bound numeric index");
      }
      p->u.array.u.double_ptr[idx] = d;
      break;
    default:
      goto slow_path;
    }
    return TRUE;
  } else {
    JSAtom atom;
    int ret;
  slow_path:
    atom = JS_ValueToAtom(ctx, prop);
    JS_FreeValue(ctx, prop);
    if (unlikely(atom == JS_ATOM_NULL)) {
      JS_FreeValue(ctx, val);
      return -1;
    }
    ret = JS_SetPropertyInternal(ctx, this_obj, atom, val, flags);
    JS_FreeAtom(ctx, atom);
    return ret;
  }
}

int JS_SetPropertyUint32(JSContext *ctx, JSValueConst this_obj, uint32_t idx,
                         JSValue val) {
  return JS_SetPropertyValue(ctx, this_obj, JS_NewUint32(ctx, idx), val,
                             JS_PROP_THROW);
}

int JS_SetPropertyInt64(JSContext *ctx, JSValueConst this_obj, int64_t idx,
                        JSValue val) {
  JSAtom prop;
  int res;

  if ((uint64_t)idx <= INT32_MAX) {
    /* fast path for fast arrays */
    return JS_SetPropertyValue(ctx, this_obj, JS_NewInt32(ctx, idx), val,
                               JS_PROP_THROW);
  }
  prop = JS_NewAtomInt64(ctx, idx);
  if (prop == JS_ATOM_NULL) {
    JS_FreeValue(ctx, val);
    return -1;
  }
  res = JS_SetProperty(ctx, this_obj, prop, val);
  JS_FreeAtom(ctx, prop);
  return res;
}

int JS_SetPropertyStr(JSContext *ctx, JSValueConst this_obj, const char *prop,
                      JSValue val) {
  JSAtom atom;
  int ret;
  atom = JS_NewAtom(ctx, prop);
  ret = JS_SetPropertyInternal(ctx, this_obj, atom, val, JS_PROP_THROW);
  JS_FreeAtom(ctx, atom);
  return ret;
}

/* compute the property flags. For each flag: (JS_PROP_HAS_x forces
   it, otherwise def_flags is used)
   Note: makes assumption about the bit pattern of the flags
*/
static int get_prop_flags(int flags, int def_flags) {
  int mask;
  mask = (flags >> JS_PROP_HAS_SHIFT) & JS_PROP_C_W_E;
  return (flags & mask) | (def_flags & ~mask);
}

int JS_CreateProperty(JSContext *ctx, JSObject *p, JSAtom prop,
                      JSValueConst val, JSValueConst getter,
                      JSValueConst setter, int flags) {
  JSProperty *pr;
  int ret, prop_flags;

  /* add a new property or modify an existing exotic one */
  if (p->is_exotic) {
    if (p->class_id == JS_CLASS_ARRAY) {
      uint32_t idx, len;

      if (p->fast_array) {
        if (__JS_AtomIsTaggedInt(prop)) {
          idx = __JS_AtomToUInt32(prop);
          if (idx == p->u.array.count) {
            if (!p->extensible)
              goto not_extensible;
            if (flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET))
              goto convert_to_array;
            prop_flags = get_prop_flags(flags, 0);
            if (prop_flags != JS_PROP_C_W_E)
              goto convert_to_array;
            return add_fast_array_element(ctx, p, JS_DupValue(ctx, val), flags);
          } else {
            goto convert_to_array;
          }
        } else if (JS_AtomIsArrayIndex(ctx, &idx, prop)) {
          /* convert the fast array to normal array */
        convert_to_array:
          if (convert_fast_array_to_array(ctx, p))
            return -1;
          goto generic_array;
        }
      } else if (JS_AtomIsArrayIndex(ctx, &idx, prop)) {
        JSProperty *plen;
        JSShapeProperty *pslen;
      generic_array:
        /* update the length field */
        plen = &p->prop[0];
        JS_ToUint32(ctx, &len, plen->u.value);
        if ((idx + 1) > len) {
          pslen = get_shape_prop(p->shape);
          if (unlikely(!(pslen->flags & JS_PROP_WRITABLE)))
            return JS_ThrowTypeErrorReadOnly(ctx, flags, JS_ATOM_length);
          /* XXX: should update the length after defining
             the property */
          len = idx + 1;
          set_value(ctx, &plen->u.value, JS_NewUint32(ctx, len));
        }
      }
    } else if (p->class_id >= JS_CLASS_UINT8C_ARRAY &&
               p->class_id <= JS_CLASS_FLOAT64_ARRAY) {
      ret = JS_AtomIsNumericIndex(ctx, prop);
      if (ret != 0) {
        if (ret < 0)
          return -1;
        return JS_ThrowTypeErrorOrFalse(
            ctx, flags, "cannot create numeric index in typed array");
      }
    } else if (!(flags & JS_PROP_NO_EXOTIC)) {
      const JSClassExoticMethods *em = ctx->rt->class_array[p->class_id].exotic;
      if (em) {
        if (em->define_own_property) {
          return em->define_own_property(ctx, JS_MKPTR(JS_TAG_OBJECT, p), prop,
                                         val, getter, setter, flags);
        }
        ret = JS_IsExtensible(ctx, JS_MKPTR(JS_TAG_OBJECT, p));
        if (ret < 0)
          return -1;
        if (!ret)
          goto not_extensible;
      }
    }
  }

  if (!p->extensible) {
  not_extensible:
    return JS_ThrowTypeErrorOrFalse(ctx, flags, "object is not extensible");
  }

  if (flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET)) {
    prop_flags =
        (flags & (JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE)) | JS_PROP_GETSET;
  } else {
    prop_flags = flags & JS_PROP_C_W_E;
  }
  pr = add_property(ctx, p, prop, prop_flags);
  if (unlikely(!pr))
    return -1;
  if (flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET)) {
    pr->u.getset.getter = NULL;
    if ((flags & JS_PROP_HAS_GET) && JS_IsFunction(ctx, getter)) {
      pr->u.getset.getter = JS_VALUE_GET_OBJ(JS_DupValue(ctx, getter));
    }
    pr->u.getset.setter = NULL;
    if ((flags & JS_PROP_HAS_SET) && JS_IsFunction(ctx, setter)) {
      pr->u.getset.setter = JS_VALUE_GET_OBJ(JS_DupValue(ctx, setter));
    }
  } else {
    if (flags & JS_PROP_HAS_VALUE) {
      pr->u.value = JS_DupValue(ctx, val);
    } else {
      pr->u.value = JS_UNDEFINED;
    }
  }
  return TRUE;
}

/* return -1, FALSE or TRUE. return FALSE if not configurable or
   invalid object. return -1 in case of exception.
   flags can be 0, JS_PROP_THROW or JS_PROP_THROW_STRICT */
int JS_DeleteProperty(JSContext *ctx, JSValueConst obj, JSAtom prop,
                      int flags) {
  JSValue obj1;
  JSObject *p;
  int res;

  obj1 = JS_ToObject(ctx, obj);
  if (JS_IsException(obj1))
    return -1;
  p = JS_VALUE_GET_OBJ(obj1);
  res = delete_property(ctx, p, prop);
  JS_FreeValue(ctx, obj1);
  if (res != FALSE)
    return res;
  if ((flags & JS_PROP_THROW) ||
      ((flags & JS_PROP_THROW_STRICT) && is_strict_mode(ctx))) {
    JS_ThrowTypeError(ctx, "could not delete property");
    return -1;
  }
  return FALSE;
}

int JS_DeletePropertyInt64(JSContext *ctx, JSValueConst obj, int64_t idx,
                           int flags) {
  JSAtom prop;
  int res;

  if ((uint64_t)idx <= JS_ATOM_MAX_INT) {
    /* fast path for fast arrays */
    return JS_DeleteProperty(ctx, obj, __JS_AtomFromUInt32(idx), flags);
  }
  prop = JS_NewAtomInt64(ctx, idx);
  if (prop == JS_ATOM_NULL)
    return -1;
  res = JS_DeleteProperty(ctx, obj, prop, flags);
  JS_FreeAtom(ctx, prop);
  return res;
}

/* return FALSE if not OK */
BOOL check_define_prop_flags(int prop_flags, int flags) {
  BOOL has_accessor, is_getset;

  if (!(prop_flags & JS_PROP_CONFIGURABLE)) {
    if ((flags & (JS_PROP_HAS_CONFIGURABLE | JS_PROP_CONFIGURABLE)) ==
        (JS_PROP_HAS_CONFIGURABLE | JS_PROP_CONFIGURABLE)) {
      return FALSE;
    }
    if ((flags & JS_PROP_HAS_ENUMERABLE) &&
        (flags & JS_PROP_ENUMERABLE) != (prop_flags & JS_PROP_ENUMERABLE))
      return FALSE;
  }
  if (flags & (JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE | JS_PROP_HAS_GET |
               JS_PROP_HAS_SET)) {
    if (!(prop_flags & JS_PROP_CONFIGURABLE)) {
      has_accessor = ((flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET)) != 0);
      is_getset = ((prop_flags & JS_PROP_TMASK) == JS_PROP_GETSET);
      if (has_accessor != is_getset)
        return FALSE;
      if (!has_accessor && !is_getset && !(prop_flags & JS_PROP_WRITABLE)) {
        /* not writable: cannot set the writable bit */
        if ((flags & (JS_PROP_HAS_WRITABLE | JS_PROP_WRITABLE)) ==
            (JS_PROP_HAS_WRITABLE | JS_PROP_WRITABLE))
          return FALSE;
      }
    }
  }
  return TRUE;
}

/* ensure that the shape can be safely modified */
int js_shape_prepare_update(JSContext *ctx, JSObject *p,
                            JSShapeProperty **pprs) {
  JSShape *sh;
  uint32_t idx = 0; /* prevent warning */

  sh = p->shape;
  if (sh->is_hashed) {
    if (sh->header.ref_count != 1) {
      if (pprs)
        idx = *pprs - get_shape_prop(sh);
      /* clone the shape (the resulting one is no longer hashed) */
      sh = js_clone_shape(ctx, sh);
      if (!sh)
        return -1;
      js_free_shape(ctx->rt, p->shape);
      p->shape = sh;
      if (pprs)
        *pprs = get_shape_prop(sh) + idx;
    } else {
      js_shape_hash_unlink(ctx->rt, sh);
      sh->is_hashed = FALSE;
    }
  }
  return 0;
}

static int js_update_property_flags(JSContext *ctx, JSObject *p,
                                    JSShapeProperty **pprs, int flags) {
  if (flags != (*pprs)->flags) {
    if (js_shape_prepare_update(ctx, p, pprs))
      return -1;
    (*pprs)->flags = flags;
  }
  return 0;
}

/* allowed flags:
   JS_PROP_CONFIGURABLE, JS_PROP_WRITABLE, JS_PROP_ENUMERABLE
   JS_PROP_HAS_GET, JS_PROP_HAS_SET, JS_PROP_HAS_VALUE,
   JS_PROP_HAS_CONFIGURABLE, JS_PROP_HAS_WRITABLE, JS_PROP_HAS_ENUMERABLE,
   JS_PROP_THROW, JS_PROP_NO_EXOTIC.
   If JS_PROP_THROW is set, return an exception instead of FALSE.
   if JS_PROP_NO_EXOTIC is set, do not call the exotic
   define_own_property callback.
   return -1 (exception), FALSE or TRUE.
*/
int JS_DefineProperty(JSContext *ctx, JSValueConst this_obj, JSAtom prop,
                      JSValueConst val, JSValueConst getter,
                      JSValueConst setter, int flags) {
  JSObject *p;
  JSShapeProperty *prs;
  JSProperty *pr;
  int mask, res;

  if (JS_VALUE_GET_TAG(this_obj) != JS_TAG_OBJECT) {
    JS_ThrowTypeErrorNotAnObject(ctx);
    return -1;
  }
  p = JS_VALUE_GET_OBJ(this_obj);

redo_prop_update:
  prs = find_own_property(&pr, p, prop);
  if (prs) {
    /* the range of the Array length property is always tested before */
    if ((prs->flags & JS_PROP_LENGTH) && (flags & JS_PROP_HAS_VALUE)) {
      uint32_t array_length;
      if (JS_ToArrayLengthFree(ctx, &array_length, JS_DupValue(ctx, val),
                               FALSE)) {
        return -1;
      }
      /* this code relies on the fact that Uint32 are never allocated */
      val = (JSValueConst)JS_NewUint32(ctx, array_length);
      /* prs may have been modified */
      prs = find_own_property(&pr, p, prop);
      assert(prs != NULL);
    }
    /* property already exists */
    if (!check_define_prop_flags(prs->flags, flags)) {
    not_configurable:
      return JS_ThrowTypeErrorOrFalse(ctx, flags,
                                      "property is not configurable");
    }

    if ((prs->flags & JS_PROP_TMASK) == JS_PROP_AUTOINIT) {
      /* Instantiate property and retry */
      if (JS_AutoInitProperty(ctx, p, prop, pr, prs))
        return -1;
      goto redo_prop_update;
    }

    if (flags & (JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE | JS_PROP_HAS_GET |
                 JS_PROP_HAS_SET)) {
      if (flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET)) {
        JSObject *new_getter, *new_setter;

        if (JS_IsFunction(ctx, getter)) {
          new_getter = JS_VALUE_GET_OBJ(getter);
        } else {
          new_getter = NULL;
        }
        if (JS_IsFunction(ctx, setter)) {
          new_setter = JS_VALUE_GET_OBJ(setter);
        } else {
          new_setter = NULL;
        }

        if ((prs->flags & JS_PROP_TMASK) != JS_PROP_GETSET) {
          if (js_shape_prepare_update(ctx, p, &prs))
            return -1;
          /* convert to getset */
          if ((prs->flags & JS_PROP_TMASK) == JS_PROP_VARREF) {
            free_var_ref(ctx->rt, pr->u.var_ref);
          } else {
            JS_FreeValue(ctx, pr->u.value);
          }
          prs->flags =
              (prs->flags & (JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE)) |
              JS_PROP_GETSET;
          pr->u.getset.getter = NULL;
          pr->u.getset.setter = NULL;
        } else {
          if (!(prs->flags & JS_PROP_CONFIGURABLE)) {
            if ((flags & JS_PROP_HAS_GET) &&
                new_getter != pr->u.getset.getter) {
              goto not_configurable;
            }
            if ((flags & JS_PROP_HAS_SET) &&
                new_setter != pr->u.getset.setter) {
              goto not_configurable;
            }
          }
        }
        if (flags & JS_PROP_HAS_GET) {
          if (pr->u.getset.getter)
            JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, pr->u.getset.getter));
          if (new_getter)
            JS_DupValue(ctx, getter);
          pr->u.getset.getter = new_getter;
        }
        if (flags & JS_PROP_HAS_SET) {
          if (pr->u.getset.setter)
            JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, pr->u.getset.setter));
          if (new_setter)
            JS_DupValue(ctx, setter);
          pr->u.getset.setter = new_setter;
        }
      } else {
        if ((prs->flags & JS_PROP_TMASK) == JS_PROP_GETSET) {
          /* convert to data descriptor */
          if (js_shape_prepare_update(ctx, p, &prs))
            return -1;
          if (pr->u.getset.getter)
            JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, pr->u.getset.getter));
          if (pr->u.getset.setter)
            JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, pr->u.getset.setter));
          prs->flags &= ~(JS_PROP_TMASK | JS_PROP_WRITABLE);
          pr->u.value = JS_UNDEFINED;
        } else if ((prs->flags & JS_PROP_TMASK) == JS_PROP_VARREF) {
          /* Note: JS_PROP_VARREF is always writable */
        } else {
          if ((prs->flags & (JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE)) == 0 &&
              (flags & JS_PROP_HAS_VALUE)) {
            if (!js_same_value(ctx, val, pr->u.value)) {
              goto not_configurable;
            } else {
              return TRUE;
            }
          }
        }
        if ((prs->flags & JS_PROP_TMASK) == JS_PROP_VARREF) {
          if (flags & JS_PROP_HAS_VALUE) {
            if (p->class_id == JS_CLASS_MODULE_NS) {
              /* JS_PROP_WRITABLE is always true for variable
                 references, but they are write protected in module name
                 spaces. */
              if (!js_same_value(ctx, val, *pr->u.var_ref->pvalue))
                goto not_configurable;
            }
            /* update the reference */
            set_value(ctx, pr->u.var_ref->pvalue, JS_DupValue(ctx, val));
          }
          /* if writable is set to false, no longer a
             reference (for mapped arguments) */
          if ((flags & (JS_PROP_HAS_WRITABLE | JS_PROP_WRITABLE)) ==
              JS_PROP_HAS_WRITABLE) {
            JSValue val1;
            if (js_shape_prepare_update(ctx, p, &prs))
              return -1;
            val1 = JS_DupValue(ctx, *pr->u.var_ref->pvalue);
            free_var_ref(ctx->rt, pr->u.var_ref);
            pr->u.value = val1;
            prs->flags &= ~(JS_PROP_TMASK | JS_PROP_WRITABLE);
          }
        } else if (prs->flags & JS_PROP_LENGTH) {
          if (flags & JS_PROP_HAS_VALUE) {
            /* Note: no JS code is executable because
               'val' is guaranteed to be a Uint32 */
            res = set_array_length(ctx, p, JS_DupValue(ctx, val), flags);
          } else {
            res = TRUE;
          }
          /* still need to reset the writable flag if
             needed.  The JS_PROP_LENGTH is kept because the
             Uint32 test is still done if the length
             property is read-only. */
          if ((flags & (JS_PROP_HAS_WRITABLE | JS_PROP_WRITABLE)) ==
              JS_PROP_HAS_WRITABLE) {
            prs = get_shape_prop(p->shape);
            if (js_update_property_flags(ctx, p, &prs,
                                         prs->flags & ~JS_PROP_WRITABLE))
              return -1;
          }
          return res;
        } else {
          if (flags & JS_PROP_HAS_VALUE) {
            JS_FreeValue(ctx, pr->u.value);
            pr->u.value = JS_DupValue(ctx, val);
          }
          if (flags & JS_PROP_HAS_WRITABLE) {
            if (js_update_property_flags(ctx, p, &prs,
                                         (prs->flags & ~JS_PROP_WRITABLE) |
                                             (flags & JS_PROP_WRITABLE)))
              return -1;
          }
        }
      }
    }
    mask = 0;
    if (flags & JS_PROP_HAS_CONFIGURABLE)
      mask |= JS_PROP_CONFIGURABLE;
    if (flags & JS_PROP_HAS_ENUMERABLE)
      mask |= JS_PROP_ENUMERABLE;
    if (js_update_property_flags(ctx, p, &prs,
                                 (prs->flags & ~mask) | (flags & mask)))
      return -1;
    return TRUE;
  }

  /* handle modification of fast array elements */
  if (p->fast_array) {
    uint32_t idx;
    uint32_t prop_flags;
    if (p->class_id == JS_CLASS_ARRAY) {
      if (__JS_AtomIsTaggedInt(prop)) {
        idx = __JS_AtomToUInt32(prop);
        if (idx < p->u.array.count) {
          prop_flags = get_prop_flags(flags, JS_PROP_C_W_E);
          if (prop_flags != JS_PROP_C_W_E)
            goto convert_to_slow_array;
          if (flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET)) {
          convert_to_slow_array:
            if (convert_fast_array_to_array(ctx, p))
              return -1;
            else
              goto redo_prop_update;
          }
          if (flags & JS_PROP_HAS_VALUE) {
            set_value(ctx, &p->u.array.u.values[idx], JS_DupValue(ctx, val));
          }
          return TRUE;
        }
      }
    } else if (p->class_id >= JS_CLASS_UINT8C_ARRAY &&
               p->class_id <= JS_CLASS_FLOAT64_ARRAY) {
      JSValue num;
      int ret;

      if (!__JS_AtomIsTaggedInt(prop)) {
        /* slow path with to handle all numeric indexes */
        num = JS_AtomIsNumericIndex1(ctx, prop);
        if (JS_IsUndefined(num))
          goto typed_array_done;
        if (JS_IsException(num))
          return -1;
        ret = JS_NumberIsInteger(ctx, num);
        if (ret < 0) {
          JS_FreeValue(ctx, num);
          return -1;
        }
        if (!ret) {
          JS_FreeValue(ctx, num);
          return JS_ThrowTypeErrorOrFalse(ctx, flags,
                                          "non integer index in typed array");
        }
        ret = JS_NumberIsNegativeOrMinusZero(ctx, num);
        JS_FreeValue(ctx, num);
        if (ret) {
          return JS_ThrowTypeErrorOrFalse(ctx, flags,
                                          "negative index in typed array");
        }
        if (!__JS_AtomIsTaggedInt(prop))
          goto typed_array_oob;
      }
      idx = __JS_AtomToUInt32(prop);
      /* if the typed array is detached, p->u.array.count = 0 */
      if (idx >= typed_array_get_length(ctx, p)) {
      typed_array_oob:
        return JS_ThrowTypeErrorOrFalse(ctx, flags,
                                        "out-of-bound index in typed array");
      }
      prop_flags = get_prop_flags(flags, JS_PROP_ENUMERABLE | JS_PROP_WRITABLE |
                                             JS_PROP_CONFIGURABLE);
      if (flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET) ||
          prop_flags !=
              (JS_PROP_ENUMERABLE | JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE)) {
        return JS_ThrowTypeErrorOrFalse(ctx, flags, "invalid descriptor flags");
      }
      if (flags & JS_PROP_HAS_VALUE) {
        return JS_SetPropertyValue(ctx, this_obj, JS_NewInt32(ctx, idx),
                                   JS_DupValue(ctx, val), flags);
      }
      return TRUE;
    typed_array_done:;
    }
  }

  return JS_CreateProperty(ctx, p, prop, val, getter, setter, flags);
}

int JS_DefineAutoInitProperty(JSContext *ctx, JSValueConst this_obj,
                              JSAtom prop, JSAutoInitIDEnum id, void *opaque,
                              int flags) {
  JSObject *p;
  JSProperty *pr;

  if (JS_VALUE_GET_TAG(this_obj) != JS_TAG_OBJECT)
    return FALSE;

  p = JS_VALUE_GET_OBJ(this_obj);

  if (find_own_property(&pr, p, prop)) {
    /* property already exists */
    abort();
    return FALSE;
  }

  /* Specialized CreateProperty */
  pr = add_property(ctx, p, prop, (flags & JS_PROP_C_W_E) | JS_PROP_AUTOINIT);
  if (unlikely(!pr))
    return -1;
  pr->u.init.realm_and_id = (uintptr_t)JS_DupContext(ctx);
  assert((pr->u.init.realm_and_id & 3) == 0);
  assert(id <= 3);
  pr->u.init.realm_and_id |= id;
  pr->u.init.opaque = opaque;
  return TRUE;
}

/* shortcut to add or redefine a new property value */
int JS_DefinePropertyValue(JSContext *ctx, JSValueConst this_obj, JSAtom prop,
                           JSValue val, int flags) {
  int ret;
  ret = JS_DefineProperty(ctx, this_obj, prop, val, JS_UNDEFINED, JS_UNDEFINED,
                          flags | JS_PROP_HAS_VALUE | JS_PROP_HAS_CONFIGURABLE |
                              JS_PROP_HAS_WRITABLE | JS_PROP_HAS_ENUMERABLE);
  JS_FreeValue(ctx, val);
  return ret;
}

int JS_DefinePropertyValueValue(JSContext *ctx, JSValueConst this_obj,
                                JSValue prop, JSValue val, int flags) {
  JSAtom atom;
  int ret;
  atom = JS_ValueToAtom(ctx, prop);
  JS_FreeValue(ctx, prop);
  if (unlikely(atom == JS_ATOM_NULL)) {
    JS_FreeValue(ctx, val);
    return -1;
  }
  ret = JS_DefinePropertyValue(ctx, this_obj, atom, val, flags);
  JS_FreeAtom(ctx, atom);
  return ret;
}

int JS_DefinePropertyValueUint32(JSContext *ctx, JSValueConst this_obj,
                                 uint32_t idx, JSValue val, int flags) {
  return JS_DefinePropertyValueValue(ctx, this_obj, JS_NewUint32(ctx, idx), val,
                                     flags);
}

int JS_DefinePropertyValueInt64(JSContext *ctx, JSValueConst this_obj,
                                int64_t idx, JSValue val, int flags) {
  return JS_DefinePropertyValueValue(ctx, this_obj, JS_NewInt64(ctx, idx), val,
                                     flags);
}

int JS_DefinePropertyValueStr(JSContext *ctx, JSValueConst this_obj,
                              const char *prop, JSValue val, int flags) {
  JSAtom atom;
  int ret;
  atom = JS_NewAtom(ctx, prop);
  ret = JS_DefinePropertyValue(ctx, this_obj, atom, val, flags);
  JS_FreeAtom(ctx, atom);
  return ret;
}

/* shortcut to add getter & setter */
int JS_DefinePropertyGetSet(JSContext *ctx, JSValueConst this_obj, JSAtom prop,
                            JSValue getter, JSValue setter, int flags) {
  int ret;
  ret =
      JS_DefineProperty(ctx, this_obj, prop, JS_UNDEFINED, getter, setter,
                        flags | JS_PROP_HAS_GET | JS_PROP_HAS_SET |
                            JS_PROP_HAS_CONFIGURABLE | JS_PROP_HAS_ENUMERABLE);
  JS_FreeValue(ctx, getter);
  JS_FreeValue(ctx, setter);
  return ret;
}

int JS_CreateDataPropertyUint32(JSContext *ctx, JSValueConst this_obj,
                                int64_t idx, JSValue val, int flags) {
  return JS_DefinePropertyValueValue(ctx, this_obj, JS_NewInt64(ctx, idx), val,
                                     flags | JS_PROP_CONFIGURABLE |
                                         JS_PROP_ENUMERABLE | JS_PROP_WRITABLE);
}

/* return TRUE if 'obj' has a non empty 'name' string */
static BOOL js_object_has_name(JSContext *ctx, JSValueConst obj) {
  JSProperty *pr;
  JSShapeProperty *prs;
  JSValueConst val;
  JSString *p;

  prs = find_own_property(&pr, JS_VALUE_GET_OBJ(obj), JS_ATOM_name);
  if (!prs)
    return FALSE;
  if ((prs->flags & JS_PROP_TMASK) != JS_PROP_NORMAL)
    return TRUE;
  val = pr->u.value;
  if (JS_VALUE_GET_TAG(val) != JS_TAG_STRING)
    return TRUE;
  p = JS_VALUE_GET_STRING(val);
  return (p->len != 0);
}

int JS_DefineObjectName(JSContext *ctx, JSValueConst obj, JSAtom name,
                        int flags) {
  if (name != JS_ATOM_NULL && JS_IsObject(obj) &&
      !js_object_has_name(ctx, obj) &&
      JS_DefinePropertyValue(ctx, obj, JS_ATOM_name, JS_AtomToString(ctx, name),
                             flags) < 0) {
    return -1;
  }
  return 0;
}

int JS_DefineObjectNameComputed(JSContext *ctx, JSValueConst obj,
                                JSValueConst str, int flags) {
  if (JS_IsObject(obj) && !js_object_has_name(ctx, obj)) {
    JSAtom prop;
    JSValue name_str;
    prop = JS_ValueToAtom(ctx, str);
    if (prop == JS_ATOM_NULL)
      return -1;
    name_str = js_get_function_name(ctx, prop);
    JS_FreeAtom(ctx, prop);
    if (JS_IsException(name_str))
      return -1;
    if (JS_DefinePropertyValue(ctx, obj, JS_ATOM_name, name_str, flags) < 0)
      return -1;
  }
  return 0;
}

/* -- Utils ----------------------------------- */

static JSValue JS_NewObjectFromShape(JSContext *ctx, JSShape *sh,
                                     JSClassID class_id) {
  JSObject *p;

  js_trigger_gc(ctx->rt, sizeof(JSObject));
  p = js_malloc(ctx, sizeof(JSObject));
  if (unlikely(!p))
    goto fail;
  p->class_id = class_id;
  p->extensible = TRUE;
  p->free_mark = 0;
  p->is_exotic = 0;
  p->fast_array = 0;
  p->is_constructor = 0;
  p->is_uncatchable_error = 0;
  p->tmp_mark = 0;
  p->is_HTMLDDA = 0;
  p->first_weak_ref = NULL;
  p->u.opaque = NULL;
  p->shape = sh;
  p->prop = js_malloc(ctx, sizeof(JSProperty) * sh->prop_size);
  if (unlikely(!p->prop)) {
    js_free(ctx, p);
  fail:
    js_free_shape(ctx->rt, sh);
    return JS_EXCEPTION;
  }

  switch (class_id) {
  case JS_CLASS_OBJECT:
    break;
  case JS_CLASS_ARRAY: {
    JSProperty *pr;
    p->is_exotic = 1;
    p->fast_array = 1;
    p->u.array.u.values = NULL;
    p->u.array.count = 0;
    p->u.array.u1.size = 0;
    /* the length property is always the first one */
    if (likely(sh == ctx->array_shape)) {
      pr = &p->prop[0];
    } else {
      /* only used for the first array */
      /* cannot fail */
      pr = add_property(ctx, p, JS_ATOM_length,
                        JS_PROP_WRITABLE | JS_PROP_LENGTH);
    }
    pr->u.value = JS_NewInt32(ctx, 0);
  } break;
  case JS_CLASS_C_FUNCTION:
    p->prop[0].u.value = JS_UNDEFINED;
    break;
  case JS_CLASS_ARGUMENTS:
  case JS_CLASS_UINT8C_ARRAY:
  case JS_CLASS_INT8_ARRAY:
  case JS_CLASS_UINT8_ARRAY:
  case JS_CLASS_INT16_ARRAY:
  case JS_CLASS_UINT16_ARRAY:
  case JS_CLASS_INT32_ARRAY:
  case JS_CLASS_UINT32_ARRAY:
#ifdef CONFIG_BIGNUM
  case JS_CLASS_BIG_INT64_ARRAY:
  case JS_CLASS_BIG_UINT64_ARRAY:
#endif
  case JS_CLASS_FLOAT32_ARRAY:
  case JS_CLASS_FLOAT64_ARRAY:
    p->is_exotic = 1;
    p->fast_array = 1;
    p->u.array.u.ptr = NULL;
    p->u.array.count = 0;
    break;
  case JS_CLASS_DATAVIEW:
    p->u.array.u.ptr = NULL;
    p->u.array.count = 0;
    break;
  case JS_CLASS_NUMBER:
  case JS_CLASS_STRING:
  case JS_CLASS_BOOLEAN:
  case JS_CLASS_SYMBOL:
  case JS_CLASS_DATE:
#ifdef CONFIG_BIGNUM
  case JS_CLASS_BIG_INT:
  case JS_CLASS_BIG_FLOAT:
  case JS_CLASS_BIG_DECIMAL:
#endif
    p->u.object_data = JS_UNDEFINED;
    goto set_exotic;
  case JS_CLASS_REGEXP:
    p->u.regexp.pattern = NULL;
    p->u.regexp.bytecode = NULL;
    goto set_exotic;
  default:
  set_exotic:
    if (ctx->rt->class_array[class_id].exotic) {
      p->is_exotic = 1;
    }
    break;
  }
  p->header.ref_count = 1;
  add_gc_object(ctx->rt, &p->header, JS_GC_OBJ_TYPE_JS_OBJECT);
  return JS_MKPTR(JS_TAG_OBJECT, p);
}

JSObject *get_proto_obj(JSValueConst proto_val) {
  if (JS_VALUE_GET_TAG(proto_val) != JS_TAG_OBJECT)
    return NULL;
  else
    return JS_VALUE_GET_OBJ(proto_val);
}

/* WARNING: proto must be an object or JS_NULL */
JSValue JS_NewObjectProtoClass(JSContext *ctx, JSValueConst proto_val,
                               JSClassID class_id) {
  JSShape *sh;
  JSObject *proto;

  proto = get_proto_obj(proto_val);
  sh = find_hashed_shape_proto(ctx->rt, proto);
  if (likely(sh)) {
    sh = js_dup_shape(sh);
  } else {
    sh = js_new_shape(ctx, proto);
    if (!sh)
      return JS_EXCEPTION;
  }
  return JS_NewObjectFromShape(ctx, sh, class_id);
}

#if 0
static JSValue JS_GetObjectData(JSContext *ctx, JSValueConst obj)
{
    JSObject *p;

    if (JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT) {
        p = JS_VALUE_GET_OBJ(obj);
        switch(p->class_id) {
        case JS_CLASS_NUMBER:
        case JS_CLASS_STRING:
        case JS_CLASS_BOOLEAN:
        case JS_CLASS_SYMBOL:
        case JS_CLASS_DATE:
#ifdef CONFIG_BIGNUM
        case JS_CLASS_BIG_INT:
        case JS_CLASS_BIG_FLOAT:
        case JS_CLASS_BIG_DECIMAL:
#endif
            return JS_DupValue(ctx, p->u.object_data);
        }
    }
    return JS_UNDEFINED;
}
#endif

int JS_SetObjectData(JSContext *ctx, JSValueConst obj, JSValue val) {
  JSObject *p;

  if (JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT) {
    p = JS_VALUE_GET_OBJ(obj);
    switch (p->class_id) {
    case JS_CLASS_NUMBER:
    case JS_CLASS_STRING:
    case JS_CLASS_BOOLEAN:
    case JS_CLASS_SYMBOL:
    case JS_CLASS_DATE:
#ifdef CONFIG_BIGNUM
    case JS_CLASS_BIG_INT:
    case JS_CLASS_BIG_FLOAT:
    case JS_CLASS_BIG_DECIMAL:
#endif
      JS_FreeValue(ctx, p->u.object_data);
      p->u.object_data = val;
      return 0;
    }
  }
  JS_FreeValue(ctx, val);
  if (!JS_IsException(obj))
    JS_ThrowTypeError(ctx, "invalid object type");
  return -1;
}

JSValue JS_NewObjectClass(JSContext *ctx, int class_id) {
  return JS_NewObjectProtoClass(ctx, ctx->class_proto[class_id], class_id);
}

JSValue JS_NewObjectProto(JSContext *ctx, JSValueConst proto) {
  return JS_NewObjectProtoClass(ctx, proto, JS_CLASS_OBJECT);
}

JSValue JS_NewArray(JSContext *ctx) {
  return JS_NewObjectFromShape(ctx, js_dup_shape(ctx->array_shape),
                               JS_CLASS_ARRAY);
}

JSValue js_create_array(JSContext *ctx, int len, JSValueConst *tab) {
  JSValue obj;
  int i;

  obj = JS_NewArray(ctx);
  if (JS_IsException(obj))
    return JS_EXCEPTION;
  for (i = 0; i < len; i++) {
    if (JS_CreateDataPropertyUint32(ctx, obj, i, JS_DupValue(ctx, tab[i]), 0) <
        0) {
      JS_FreeValue(ctx, obj);
      return JS_EXCEPTION;
    }
  }
  return obj;
}

BOOL js_is_fast_array(JSContext *ctx, JSValueConst obj) {
  /* Try and handle fast arrays explicitly */
  if (JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT) {
    JSObject *p = JS_VALUE_GET_OBJ(obj);
    if (p->class_id == JS_CLASS_ARRAY && p->fast_array) {
      return TRUE;
    }
  }
  return FALSE;
}

/* Access an Array's internal JSValue array if available */
BOOL js_get_fast_array(JSContext *ctx, JSValueConst obj, JSValue **arrpp,
                       uint32_t *countp) {
  /* Try and handle fast arrays explicitly */
  if (JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT) {
    JSObject *p = JS_VALUE_GET_OBJ(obj);
    if (p->class_id == JS_CLASS_ARRAY && p->fast_array) {
      *countp = p->u.array.count;
      *arrpp = p->u.array.u.values;
      return TRUE;
    }
  }
  return FALSE;
}

JSValue JS_NewObject(JSContext *ctx) {
  /* inline JS_NewObjectClass(ctx, JS_CLASS_OBJECT); */
  return JS_NewObjectProtoClass(ctx, ctx->class_proto[JS_CLASS_OBJECT],
                                JS_CLASS_OBJECT);
}

__exception int JS_CopyDataProperties(JSContext *ctx, JSValueConst target,
                                      JSValueConst source,
                                      JSValueConst excluded, BOOL setprop) {
  JSPropertyEnum *tab_atom;
  JSValue val;
  uint32_t i, tab_atom_count;
  JSObject *p;
  JSObject *pexcl = NULL;
  int ret, gpn_flags;
  JSPropertyDescriptor desc;
  BOOL is_enumerable;

  if (JS_VALUE_GET_TAG(source) != JS_TAG_OBJECT)
    return 0;

  if (JS_VALUE_GET_TAG(excluded) == JS_TAG_OBJECT)
    pexcl = JS_VALUE_GET_OBJ(excluded);

  p = JS_VALUE_GET_OBJ(source);

  gpn_flags = JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK | JS_GPN_ENUM_ONLY;
  if (p->is_exotic) {
    const JSClassExoticMethods *em = ctx->rt->class_array[p->class_id].exotic;
    /* cannot use JS_GPN_ENUM_ONLY with e.g. proxies because it
       introduces a visible change */
    if (em && em->get_own_property_names) {
      gpn_flags &= ~JS_GPN_ENUM_ONLY;
    }
  }
  if (JS_GetOwnPropertyNamesInternal(ctx, &tab_atom, &tab_atom_count, p,
                                     gpn_flags))
    return -1;

  for (i = 0; i < tab_atom_count; i++) {
    if (pexcl) {
      ret = JS_GetOwnPropertyInternal(ctx, NULL, pexcl, tab_atom[i].atom);
      if (ret) {
        if (ret < 0)
          goto exception;
        continue;
      }
    }
    if (!(gpn_flags & JS_GPN_ENUM_ONLY)) {
      /* test if the property is enumerable */
      ret = JS_GetOwnPropertyInternal(ctx, &desc, p, tab_atom[i].atom);
      if (ret < 0)
        goto exception;
      if (!ret)
        continue;
      is_enumerable = (desc.flags & JS_PROP_ENUMERABLE) != 0;
      js_free_desc(ctx, &desc);
      if (!is_enumerable)
        continue;
    }
    val = JS_GetProperty(ctx, source, tab_atom[i].atom);
    if (JS_IsException(val))
      goto exception;
    if (setprop)
      ret = JS_SetProperty(ctx, target, tab_atom[i].atom, val);
    else
      ret = JS_DefinePropertyValue(ctx, target, tab_atom[i].atom, val,
                                   JS_PROP_C_W_E);
    if (ret < 0)
      goto exception;
  }
  js_free_prop_enum(ctx, tab_atom, tab_atom_count);
  return 0;
exception:
  js_free_prop_enum(ctx, tab_atom, tab_atom_count);
  return -1;
}

__exception int js_get_length32(JSContext *ctx, uint32_t *pres,
                                JSValueConst obj) {
  JSValue len_val;
  len_val = JS_GetProperty(ctx, obj, JS_ATOM_length);
  if (JS_IsException(len_val)) {
    *pres = 0;
    return -1;
  }
  return JS_ToUint32Free(ctx, pres, len_val);
}

__exception int js_get_length64(JSContext *ctx, int64_t *pres,
                                JSValueConst obj) {
  JSValue len_val;
  len_val = JS_GetProperty(ctx, obj, JS_ATOM_length);
  if (JS_IsException(len_val)) {
    *pres = 0;
    return -1;
  }
  return JS_ToLengthFree(ctx, pres, len_val);
}

/* Note: all the fields are already sealed except length */
int seal_template_obj(JSContext *ctx, JSValueConst obj) {
  JSObject *p;
  JSShapeProperty *prs;

  p = JS_VALUE_GET_OBJ(obj);
  prs = find_own_property1(p, JS_ATOM_length);
  if (prs) {
    if (js_update_property_flags(
            ctx, p, &prs,
            prs->flags & ~(JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE)))
      return -1;
  }
  p->extensible = FALSE;
  return 0;
}

void JS_SetOpaque(JSValue obj, void *opaque) {
  JSObject *p;
  if (JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT) {
    p = JS_VALUE_GET_OBJ(obj);
    p->u.opaque = opaque;
  }
}

/* return NULL if not an object of class class_id */
void *JS_GetOpaque(JSValueConst obj, JSClassID class_id) {
  JSObject *p;
  if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
    return NULL;
  p = JS_VALUE_GET_OBJ(obj);
  if (p->class_id != class_id)
    return NULL;
  return p->u.opaque;
}

void *JS_GetOpaque2(JSContext *ctx, JSValueConst obj, JSClassID class_id) {
  void *p = JS_GetOpaque(obj, class_id);
  if (unlikely(!p)) {
    JS_ThrowTypeErrorInvalidClass(ctx, class_id);
  }
  return p;
}

void JS_SetIsHTMLDDA(JSContext *ctx, JSValueConst obj) {
  JSObject *p;
  if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
    return;
  p = JS_VALUE_GET_OBJ(obj);
  p->is_HTMLDDA = TRUE;
}
