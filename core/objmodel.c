//
// objmodel.c
// much of this is based on the work of ian piumarta
// <http://www.piumarta.com/pepsi/objmodel.pdf>
//
// (c) 2008 why the lucky stiff, the freelance professor
//
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "potion.h"
#include "internal.h"
#include "khash.h"
#include "table.h"
#include "asm.h"

PN potion_closure_new(Potion *P, PN_F meth, PN sig, PN_SIZE extra) {
  PN_SIZE i;
  vPN(Closure) c = PN_ALLOC_N(PN_TCLOSURE, struct PNClosure, extra * sizeof(PN));
  c->method = meth;
  c->sig = sig;
  c->extra = extra;
  for (i = 0; i < c->extra; i++)
    c->data[i] = PN_NIL;
  return (PN)c;
}

PN potion_closure_code(Potion *P, PN cl, PN self) {
  if (PN_CLOSURE(self)->extra > 0 && PN_IS_PROTO(PN_CLOSURE(self)->data[0])) 
    return PN_CLOSURE(self)->data[0];
  return PN_NIL;
}

PN potion_closure_string(Potion *P, PN cl, PN self, PN len) {
  int x = 0;
  PN out = potion_byte_str(P, "Function(");
  if (PN_IS_TUPLE(PN_CLOSURE(self)->sig)) {
    PN_TUPLE_EACH(PN_CLOSURE(self)->sig, i, v, {
      if (PN_IS_STR(v)) {
        if (x++ > 0) pn_printf(P, out, ", ");
        potion_bytes_obj_string(P, out, v);
      }
    });
  }
  pn_printf(P, out, ")");
  return out;
}

PN potion_type_new(Potion *P, PNType t, PN self) {
  vPN(Vtable) vt = PN_CALLOC_N(PN_TVTABLE, struct PNVtable, sizeof(kh_PN_t));
  vt->type = t;
  vt->parent = self;
#ifdef JIT_MCACHE
  vt->mcache = (PN_MCACHE_FUNC)PN_ALLOC_FUNC(8192);
#endif
  PN_VTABLE(t) = (PN)vt;
  return (PN)vt;
}

void potion_type_call_is(PN vt, PN cl) {
  ((struct PNVtable *)vt)->call = cl;
}

PN potion_obj_get_call(Potion *P, PN obj) {
  PN cl = ((struct PNVtable *)PN_VTABLE(PN_TYPE(obj)))->call;
  if (cl == PN_NIL) cl = P->call;
  return cl;
}

void potion_type_callset_is(PN vt, PN cl) {
  ((struct PNVtable *)vt)->callset = cl;
}

PN potion_obj_get_callset(Potion *P, PN obj) {
  PN cl = ((struct PNVtable *)PN_VTABLE(PN_TYPE(obj)))->callset;
  if (cl == PN_NIL) cl = P->callset;
  return cl;
}

PN potion_no_call(Potion *P, PN cl, PN self) {
  return self;
}

PN potion_class(Potion *P, PN cl, PN self, PN ivars) {
  PN parent = (self == P->lobby ? PN_VTABLE(PN_TOBJECT) : self);
  PN pvars = ((struct PNVtable *)parent)->ivars;
  PNType t = PN_FLEX_SIZE(P->vts) + PN_TNIL;
  PN_FLEX_NEEDS(1, P->vts, PN_TFLEX, PNFlex, TYPE_BATCH_SIZE);
  self = potion_type_new(P, t, parent);
  if (PN_IS_TUPLE(pvars)) {
    if (!PN_IS_TUPLE(ivars)) ivars = PN_TUP0();
    PN_TUPLE_EACH(pvars, i, v, {PN_PUT(ivars, v);});
  }
  if (PN_IS_TUPLE(ivars))
    potion_ivars(P, PN_NIL, self, ivars);

  if (!PN_IS_CLOSURE(cl))
    cl = ((struct PNVtable *)parent)->ctor;
  ((struct PNVtable *)self)->ctor = cl;

  PN_FLEX_SIZE(P->vts)++;
  PN_TOUCH(P->vts);
  return self;
}

PN potion_ivars(Potion *P, PN cl, PN self, PN ivars) {
  struct PNVtable *vt = (struct PNVtable *)self;
#if POTION_JIT == 1
  // TODO: allocate assembled instructions together into single pages
  // since many times these tables are <100 bytes.
  PNAsm * volatile asmb = potion_asm_new(P);
  P->targets[POTION_JIT_TARGET].ivars(P, ivars, &asmb);
  vt->ivfunc = PN_ALLOC_FUNC(asmb->len);
  PN_MEMCPY_N(vt->ivfunc, asmb->ptr, u8, asmb->len);
#endif
  vt->ivlen = PN_TUPLE_LEN(ivars);
  vt->ivars = ivars;
  return self;
}

static inline long potion_obj_find_ivar(Potion *P, PN self, PN ivar) {
  PNType t = PN_TYPE(self);
  vPN(Vtable) vt = (struct PNVtable *)PN_VTABLE(t);
  if (vt->ivfunc != NULL)
    return vt->ivfunc(PN_UNIQ(ivar));

  if (t > PN_TUSER) {
    PN ivars = ((struct PNVtable *)PN_VTABLE(t))->ivars;
    if (ivars != PN_NIL)
      return potion_tuple_binary_search(ivars, ivar);
  }
  return -1;
}

PN potion_obj_get(Potion *P, PN cl, PN self, PN ivar) {
  long i = potion_obj_find_ivar(P, self, ivar);
  if (i >= 0)
    return ((struct PNObject *)self)->ivars[i];
  return PN_NIL;
}

PN potion_obj_set(Potion *P, PN cl, PN self, PN ivar, PN value) {
  long i = potion_obj_find_ivar(P, self, ivar);
  if (i >= 0) {
    ((struct PNObject *)self)->ivars[i] = value;
    PN_TOUCH(self);
  }
  return value;
}

PN potion_proto_method(Potion *P, PN cl, PN self, PN args) {
  return potion_vm(P, PN_CLOSURE(cl)->data[0], P->lobby, args, 0, NULL);
}

PN potion_getter_method(Potion *P, PN cl, PN self) {
  return PN_CLOSURE(cl)->data[0];
}

PN potion_def_method(Potion *P, PN closure, PN self, PN key, PN method) {
  int ret;
  PN cl;
  vPN(Vtable) vt = (struct PNVtable *)self;
  unsigned k = kh_put(PN, vt->kh, key, &ret);
  if (!PN_IS_CLOSURE(method)) {
    if (PN_IS_PROTO(method))
      cl = potion_closure_new(P, (PN_F)potion_proto_method, PN_PROTO(method)->sig, 1);
    else
      cl = potion_closure_new(P, (PN_F)potion_getter_method, PN_NIL, 1);
    PN_CLOSURE(cl)->data[0] = method;
    method = cl;
  }
  kh_value(vt->kh, k) = method;
  PN_TOUCH(self);
#ifdef JIT_MCACHE
#define X86(ins) *asmb = (u8)(ins); asmb++
#define X86I(pn) *((unsigned int *)asmb) = (unsigned int)(pn); asmb += sizeof(unsigned int)
#define X86N(pn) *((PN *)asmb) = (PN)(pn); asmb += sizeof(PN)
  {
    u8 *asmb = (u8 *)vt->mcache;
#if __WORDSIZE != 64
    X86(0x55); // push %ebp
    X86(0x89); X86(0xE5); // mov %esp %ebp
    X86(0x8B); X86(0x55); X86(0x08); // mov 0x8(%ebp) %edx
#define X86C(op32, op64) op32
#else
#define X86C(op32, op64) op64
#endif
    for (k = kh_end(vt->kh); k > kh_begin(vt->kh); k--) {
      if (kh_exist(vt->kh, k - 1)) {
        X86(0x81); X86(X86C(0xFA, 0xFF));
          X86I(kh_key(vt->kh, k - 1)); // cmp NAME %edi
        X86(0x75); X86(X86C(8, 11)); // jne +11
        X86(0x48); X86(0xB8); X86N(kh_value(vt->kh, k - 1)); // mov CL %rax
#if __WORDSIZE != 64
        X86(0x5D);
#endif
        X86(0xC3); // retq
      }
    }
    X86(0xB8); X86I(0); // mov NIL %eax
#if __WORDSIZE != 64
    X86(0x5D);
#endif
    X86(0xC3); // retq
  }
#endif
  return method;
}

PN potion_lookup(Potion *P, PN closure, PN self, PN key) {
  vPN(Vtable) vt = (struct PNVtable *)self;
#ifdef JIT_MCACHE
  return vt->mcache(PN_UNIQ(key));
#else
  unsigned k = kh_get(PN, vt->kh, key);
  if (k != kh_end(vt->kh)) return kh_value(vt->kh, k);
  return PN_NIL;
#endif
}

PN potion_bind(Potion *P, PN rcv, PN msg) {
  PN closure = PN_NIL;
  PN vt = PN_NIL;
  PNType t = PN_TYPE(rcv);
  if (!PN_TYPECHECK(t)) return PN_NIL;
  vt = PN_VTABLE(t);
  while (PN_IS_PTR(vt)) {
    closure = ((msg == PN_lookup) && (t == PN_TVTABLE))
      ? potion_lookup(P, 0, vt, msg)
      : potion_send(vt, PN_lookup, msg);
    if (closure) break;
    vt = ((struct PNVtable *)vt)->parent; 
  }
  return closure;
}

PN potion_ref(Potion *P, PN data) {
  if (PN_IS_REF(data)) return data;
  vPN(WeakRef) ref = PN_ALLOC(PN_TWEAK, struct PNWeakRef);
  ref->data = data;
  return (PN)ref;
}

PN potion_ref_string(Potion *P, PN cl, PN self, PN len) {
  return potion_byte_str(P, "#<ref>");
}

PN potion_object_string(Potion *P, PN cl, PN self, PN len) {
  return potion_byte_str(P, "#<object>");
}

PN potion_object_forward(Potion *P, PN cl, PN self, PN method) {
  printf("#<object>");
  return PN_NIL;
}

PN potion_object_send(Potion *P, PN cl, PN self, PN method) {
  return potion_send_dyn(self, method);
}

PN potion_object_new(Potion *P, PN cl, PN self) {
  vPN(Vtable) vt = (struct PNVtable *)self;
  return (PN)PN_ALLOC_N(vt->type, struct PNObject, vt->ivlen * sizeof(PN));
}

static PN potion_lobby_self(Potion *P, PN cl, PN self) {
  return self;
}

PN potion_lobby_kind(Potion *P, PN cl, PN self) {
  PNType t = PN_TYPE(self);
  if (!PN_TYPECHECK(t)) return PN_NIL; // TODO: error
  return PN_VTABLE(t);
}

PN potion_about(Potion *P, PN cl, PN self) {
  PN about = potion_table_empty(P);
  potion_table_put(P, PN_NIL, about, potion_str(P, "_why"),
    potion_str(P, "“I love _why, but learning Ruby from him is like trying to learn to pole vault "
      "by having Salvador Dali punch you in the face.” - Steven Frank"));
  potion_table_put(P, PN_NIL, about, potion_str(P, "minimalism"),
    potion_str(P, "“The sad thing about ‘minimalism’ is that it has a name.” "
      "- Steve Dekorte"));
  potion_table_put(P, PN_NIL, about, potion_str(P, "stage fright"),
    potion_str(P, "“Recently no move on Potion. I git pull everyday.” "
      "- matz"));
  potion_table_put(P, PN_NIL, about, potion_str(P, "terms of use"),
    potion_str(P, "“Setting up my new anarchist bulletin board so that during registration, if you accept "
      "the terms and conditions, you are banned forever.” - Dr. Casey Hall"));
  potion_table_put(P, PN_NIL, about, potion_str(P, "help"),
    potion_str(P, "`man which` - Evan Weaver"));
  potion_table_put(P, PN_NIL, about, potion_str(P, "ts"),
    potion_str(P, "“pigeon%” - Guy Decoux (1955 - 2008)"));
  potion_table_put(P, PN_NIL, about, potion_str(P, "summary"),
    potion_str(P, "“I smell as how a leprechaun looks.” - Alana Post"));
  return about;
}

PN potion_exit(Potion *P, PN cl, PN self) {
  potion_destroy(P);
  exit(0);
}

void potion_object_init(Potion *P) {
  PN clo_vt = PN_VTABLE(PN_TCLOSURE);
  PN ref_vt = PN_VTABLE(PN_TWEAK);
  PN obj_vt = PN_VTABLE(PN_TOBJECT);
  potion_method(clo_vt, "code", potion_closure_code, 0);
  potion_method(clo_vt, "string", potion_closure_string, 0);
  potion_method(ref_vt, "string", potion_ref_string, 0);
  potion_method(obj_vt, "forward", potion_object_forward, 0);
  potion_method(obj_vt, "send", potion_object_send, 0);
  potion_method(obj_vt, "string", potion_object_string, 0);
}

void potion_lobby_init(Potion *P) {
  potion_send(P->lobby, PN_def, potion_str(P, "Lobby"),    P->lobby);
  potion_send(P->lobby, PN_def, potion_str(P, "Mixin"),    PN_VTABLE(PN_TVTABLE));
  potion_send(P->lobby, PN_def, potion_str(P, "Object"),   PN_VTABLE(PN_TOBJECT));
  potion_send(P->lobby, PN_def, potion_str(P, "NilKind"),  PN_VTABLE(PN_TNIL));
  potion_send(P->lobby, PN_def, potion_str(P, "Number"),   PN_VTABLE(PN_TNUMBER));
  potion_send(P->lobby, PN_def, potion_str(P, "Boolean"),  PN_VTABLE(PN_TBOOLEAN));
  potion_send(P->lobby, PN_def, potion_str(P, "String"),   PN_VTABLE(PN_TSTRING));
  potion_send(P->lobby, PN_def, potion_str(P, "Table"),    PN_VTABLE(PN_TTABLE));
  potion_send(P->lobby, PN_def, potion_str(P, "Function"), PN_VTABLE(PN_TCLOSURE));
  potion_send(P->lobby, PN_def, potion_str(P, "Tuple"),    PN_VTABLE(PN_TTUPLE));
  potion_send(P->lobby, PN_def, potion_str(P, "Potion"),   PN_VTABLE(PN_TSTATE));
  potion_send(P->lobby, PN_def, potion_str(P, "Source"),   PN_VTABLE(PN_TSOURCE));
  potion_send(P->lobby, PN_def, potion_str(P, "Bytes"),    PN_VTABLE(PN_TBYTES));
  potion_send(P->lobby, PN_def, potion_str(P, "Compiled"), PN_VTABLE(PN_TPROTO));
  potion_send(P->lobby, PN_def, potion_str(P, "Ref"),      PN_VTABLE(PN_TWEAK));
  potion_send(P->lobby, PN_def, potion_str(P, "Lick"),     PN_VTABLE(PN_TLICK));

  P->call = P->callset = PN_FUNC(potion_no_call, 0);
  potion_type_call_is(PN_VTABLE(PN_TVTABLE), PN_FUNC(potion_object_new, 0));
  potion_method(P->lobby, "about", potion_about, 0);
  potion_method(P->lobby, "callcc", potion_callcc, 0);
  potion_method(P->lobby, "exit", potion_exit, 0);
  potion_method(P->lobby, "kind", potion_lobby_kind, 0);
  potion_method(P->lobby, "srand", potion_srand, "seed=N");
  potion_method(P->lobby, "rand", potion_rand, 0);
  potion_method(P->lobby, "self", potion_lobby_self, 0);
  potion_send(P->lobby, PN_def, PN_string, potion_str(P, "Lobby"));
}
