// This file is a part of Julia. License is MIT: https://julialang.org/license
//

/*
  modules and top-level bindings
*/
#include "julia.h"
#include "julia_internal.h"
#include "julia_assert.h"

#ifdef __cplusplus
extern "C" {
#endif

// In this translation unit and this translation unit only emit this symbol `extern` for use by julia
extern JL_DLLEXPORT jl_binding_partition_t *jl_get_binding_partition(jl_binding_t *b, size_t world) JL_NOTSAFEPOINT;

JL_DLLEXPORT jl_module_t *jl_new_module_(jl_sym_t *name, jl_module_t *parent, uint8_t default_names)
{
    jl_task_t *ct = jl_current_task;
    const jl_uuid_t uuid_zero = {0, 0};
    jl_module_t *m = (jl_module_t*)jl_gc_alloc(ct->ptls, sizeof(jl_module_t),
                                               jl_module_type);
    jl_set_typetagof(m, jl_module_tag, 0);
    assert(jl_is_symbol(name));
    m->name = name;
    m->parent = parent;
    m->istopmod = 0;
    m->uuid = uuid_zero;
    static unsigned int mcounter; // simple counter backup, in case hrtime is not incrementing
    m->build_id.lo = jl_hrtime() + (++mcounter);
    if (!m->build_id.lo)
        m->build_id.lo++; // build id 0 is invalid
    m->build_id.hi = ~(uint64_t)0;
    jl_atomic_store_relaxed(&m->counter, 1);
    m->nospecialize = 0;
    m->optlevel = -1;
    m->compile = -1;
    m->infer = -1;
    m->max_methods = -1;
    m->hash = parent == NULL ? bitmix(name->hash, jl_module_type->hash) :
        bitmix(name->hash, parent->hash);
    JL_MUTEX_INIT(&m->lock, "module->lock");
    jl_atomic_store_relaxed(&m->bindings, jl_emptysvec);
    jl_atomic_store_relaxed(&m->bindingkeyset, (jl_genericmemory_t*)jl_an_empty_memory_any);
    arraylist_new(&m->usings, 0);
    if (jl_core_module && default_names) {
        JL_GC_PUSH1(&m);
        jl_module_using(m, jl_core_module);
        // export own name, so "using Foo" makes "Foo" itself visible
        jl_set_const(m, name, (jl_value_t*)m);
        jl_module_public(m, name, 1);
        JL_GC_POP();
    }
    return m;
}

JL_DLLEXPORT jl_module_t *jl_new_module(jl_sym_t *name, jl_module_t *parent)
{
    return jl_new_module_(name, parent, 1);
}

uint32_t jl_module_next_counter(jl_module_t *m)
{
    return jl_atomic_fetch_add_relaxed(&m->counter, 1);
}

JL_DLLEXPORT jl_value_t *jl_f_new_module(jl_sym_t *name, uint8_t std_imports, uint8_t default_names)
{
    // TODO: should we prohibit this during incremental compilation?
    // TODO: the parent module is a lie
    jl_module_t *m = jl_new_module_(name, jl_main_module, default_names);
    JL_GC_PUSH1(&m);
    if (std_imports)
        jl_add_standard_imports(m);
    JL_GC_POP();
    // TODO: should we somehow try to gc-root this correctly?
    return (jl_value_t*)m;
}

JL_DLLEXPORT void jl_set_module_nospecialize(jl_module_t *self, int on)
{
    self->nospecialize = (on ? -1 : 0);
}

JL_DLLEXPORT void jl_set_module_optlevel(jl_module_t *self, int lvl)
{
    self->optlevel = lvl;
}

JL_DLLEXPORT int jl_get_module_optlevel(jl_module_t *m)
{
    int lvl = m->optlevel;
    while (lvl == -1 && m->parent != m && m != jl_base_module) {
        m = m->parent;
        lvl = m->optlevel;
    }
    return lvl;
}

JL_DLLEXPORT void jl_set_module_compile(jl_module_t *self, int value)
{
    self->compile = value;
}

JL_DLLEXPORT int jl_get_module_compile(jl_module_t *m)
{
    int value = m->compile;
    while (value == -1 && m->parent != m && m != jl_base_module) {
        m = m->parent;
        value = m->compile;
    }
    return value;
}

JL_DLLEXPORT void jl_set_module_infer(jl_module_t *self, int value)
{
    self->infer = value;
    // no reason to specialize if inference is off
    if (!value)
        jl_set_module_nospecialize(self, 1);
}

JL_DLLEXPORT int jl_get_module_infer(jl_module_t *m)
{
    int value = m->infer;
    while (value == -1 && m->parent != m && m != jl_base_module) {
        m = m->parent;
        value = m->infer;
    }
    return value;
}

JL_DLLEXPORT void jl_set_module_max_methods(jl_module_t *self, int value)
{
    self->max_methods = value;
}

JL_DLLEXPORT int jl_get_module_max_methods(jl_module_t *m)
{
    int value = m->max_methods;
    while (value == -1 && m->parent != m && m != jl_base_module) {
        m = m->parent;
        value = m->max_methods;
    }
    return value;
}

JL_DLLEXPORT void jl_set_istopmod(jl_module_t *self, uint8_t isprimary)
{
    self->istopmod = 1;
    if (isprimary) {
        jl_top_module = self;
    }
}

JL_DLLEXPORT uint8_t jl_istopmod(jl_module_t *mod)
{
    return mod->istopmod;
}

static jl_globalref_t *jl_new_globalref(jl_module_t *mod, jl_sym_t *name, jl_binding_t *b)
{
    jl_task_t *ct = jl_current_task;
    jl_globalref_t *g = (jl_globalref_t*)jl_gc_alloc(ct->ptls, sizeof(jl_globalref_t), jl_globalref_type);
    g->mod = mod;
    jl_gc_wb(g, g->mod);
    g->name = name;
    g->binding = b;
    return g;
}

static jl_binding_partition_t *new_binding_partition(void)
{
    jl_binding_partition_t *bpart = (jl_binding_partition_t*)jl_gc_alloc(jl_current_task->ptls, sizeof(jl_binding_partition_t), jl_binding_partition_type);
    jl_atomic_store_relaxed(&bpart->restriction, NULL);
    bpart->min_world = 0;
    jl_atomic_store_relaxed(&bpart->max_world, (size_t)-1);
    jl_atomic_store_relaxed(&bpart->next, NULL);
    bpart->kind = BINDING_KIND_GUARD;
    bpart->padding = 0;
    return bpart;
}

static jl_binding_t *new_binding(jl_module_t *mod, jl_sym_t *name)
{
    jl_task_t *ct = jl_current_task;
    assert(jl_is_module(mod) && jl_is_symbol(name));
    jl_binding_t *b = (jl_binding_t*)jl_gc_alloc(ct->ptls, sizeof(jl_binding_t), jl_binding_type);
    jl_atomic_store_relaxed(&b->value, NULL);
    b->globalref = NULL;
    b->exportp = 0;
    b->publicp = 0;
    b->deprecated = 0;
    JL_GC_PUSH1(&b);
    b->globalref = jl_new_globalref(mod, name, b);
    jl_binding_partition_t *bpart = new_binding_partition();
    jl_atomic_store_relaxed(&b->partitions, bpart);
    jl_gc_wb(b, bpart);
    JL_GC_POP();
    return b;
}

extern jl_mutex_t jl_modules_mutex;

static void check_safe_newbinding(jl_module_t *m, jl_sym_t *var)
{
    if (jl_current_task->ptls->in_pure_callback)
        jl_errorf("new globals cannot be created in a generated function");
    if (jl_options.incremental && jl_generating_output()) {
        JL_LOCK(&jl_modules_mutex);
        int open = ptrhash_has(&jl_current_modules, (void*)m);
        if (!open && jl_module_init_order != NULL) {
            size_t i, l = jl_array_len(jl_module_init_order);
            for (i = 0; i < l; i++) {
                if (m == (jl_module_t*)jl_array_ptr_ref(jl_module_init_order, i)) {
                    open = 1;
                    break;
                }
            }
        }
        JL_UNLOCK(&jl_modules_mutex);
        if (!open) {
            jl_errorf("Creating a new global in closed module `%s` (`%s`) breaks incremental compilation "
                      "because the side effects will not be permanent.",
                      jl_symbol_name(m->name), jl_symbol_name(var));
        }
    }
}

static jl_module_t *jl_binding_dbgmodule(jl_binding_t *b, jl_module_t *m, jl_sym_t *var) JL_GLOBALLY_ROOTED;

// get binding for assignment
JL_DLLEXPORT jl_binding_t *jl_get_binding_wr(jl_module_t *m JL_PROPAGATES_ROOT, jl_sym_t *var, int alloc)
{
    jl_binding_t *b = jl_get_module_binding(m, var, 1);
    jl_binding_partition_t *bpart = jl_get_binding_partition(b, jl_current_task->world_age);
    if (bpart->kind != BINDING_KIND_GLOBAL && bpart->kind != BINDING_KIND_CONST && bpart->kind != BINDING_KIND_CONST_IMPORT) {
        if (jl_bpart_is_some_guard(bpart)) {
            check_safe_newbinding(m, var);
            if (!alloc)
                jl_errorf("Global %s.%s does not exist and cannot be assigned. Declare it using `global` before attempting assignment.", jl_symbol_name(m->name), jl_symbol_name(var));
            bpart->kind = BINDING_KIND_GLOBAL;
            jl_atomic_store_relaxed(&bpart->restriction, (jl_value_t*)jl_any_type);
        } else {
            jl_module_t *from = jl_binding_dbgmodule(b, m, var);
            if (from == m)
                jl_errorf("cannot assign a value to imported variable %s.%s",
                          jl_symbol_name(from->name), jl_symbol_name(var));
            else
                jl_errorf("cannot assign a value to imported variable %s.%s from module %s",
                          jl_symbol_name(from->name), jl_symbol_name(var), jl_symbol_name(m->name));
        }
    }
    return b;
}

// return module of binding
JL_DLLEXPORT jl_module_t *jl_get_module_of_binding(jl_module_t *m, jl_sym_t *var)
{
    jl_binding_t *b = jl_get_binding(m, var);
    if (b == NULL)
        return NULL;
    return b->globalref->mod; // TODO: deprecate this?
}

JL_DLLEXPORT jl_value_t *jl_get_binding_value(jl_binding_t *b)
{
    jl_binding_partition_t *bpart = jl_get_binding_partition(b, jl_current_task->world_age);
    while (jl_bpart_is_some_import(bpart)) {
        b = (jl_binding_t*)jl_atomic_load_relaxed(&bpart->restriction);
        bpart = jl_get_binding_partition(b, jl_current_task->world_age);
    }
    if (jl_bpart_is_some_guard(bpart))
        return NULL;
    if (jl_bpart_is_some_constant(bpart))
        return jl_atomic_load_relaxed(&bpart->restriction);
    return jl_atomic_load_relaxed(&b->value);
}

JL_DLLEXPORT jl_value_t *jl_get_binding_value_if_const(jl_binding_t *b)
{
    jl_binding_partition_t *bpart = jl_get_binding_partition(b, jl_current_task->world_age);
    while (jl_bpart_is_some_import(bpart)) {
        b = (jl_binding_t*)jl_atomic_load_relaxed(&bpart->restriction);
        bpart = jl_get_binding_partition(b, jl_current_task->world_age);
    }
    if (jl_bpart_is_some_guard(bpart))
        return NULL;
    if (!jl_bpart_is_some_constant(bpart))
        return NULL;
    return jl_atomic_load_relaxed(&bpart->restriction);
}

typedef struct _modstack_t {
    jl_module_t *m;
    jl_sym_t *var;
    struct _modstack_t *prev;
} modstack_t;
static jl_binding_t *jl_resolve_owner(jl_binding_t *b/*optional*/, jl_module_t *m JL_PROPAGATES_ROOT, jl_sym_t *var, modstack_t *st);

JL_DLLEXPORT jl_value_t *jl_reresolve_binding_value(jl_binding_t *b)
{
    jl_binding_partition_t *bpart = jl_get_binding_partition(b, jl_current_task->world_age);
    if (jl_bpart_is_some_guard(bpart)) {
        jl_resolve_owner(b, b->globalref->mod, b->globalref->name, NULL);
    }
    return jl_get_binding_value(b);
}

// get binding for adding a method
// like jl_get_binding_wr, but has different error paths and messages
JL_DLLEXPORT jl_binding_t *jl_get_binding_for_method_def(jl_module_t *m, jl_sym_t *var)
{
    jl_binding_t *b = jl_get_module_binding(m, var, 1);
    jl_binding_partition_t *bpart = jl_get_binding_partition(b, jl_current_task->world_age);
    if (bpart->kind != BINDING_KIND_GLOBAL && !jl_bpart_is_some_constant(bpart)) {
        if (jl_bpart_is_some_guard(bpart)) {
            check_safe_newbinding(m, var);
            return b;
        }
        jl_value_t *f = jl_get_binding_value_if_const(b);
        if (f == NULL) {
            jl_module_t *from = jl_binding_dbgmodule(b, m, var);
            // we must have implicitly imported this with using, so call jl_binding_dbgmodule to try to get the name of the module we got this from
            jl_errorf("invalid method definition in %s: exported function %s.%s does not exist",
                        jl_symbol_name(m->name), jl_symbol_name(from->name), jl_symbol_name(var));
        }
        // TODO: we might want to require explicitly importing types to add constructors
        //       or we might want to drop this error entirely
        if (bpart->kind != BINDING_KIND_IMPORTED && !(f && jl_is_type(f) && strcmp(jl_symbol_name(var), "=>") != 0)) {
            jl_module_t *from = jl_binding_dbgmodule(b, m, var);
            jl_errorf("invalid method definition in %s: function %s.%s must be explicitly imported to be extended",
                        jl_symbol_name(m->name), jl_symbol_name(from->name), jl_symbol_name(var));
        }
        return b;
    }
    return b;
}

static inline jl_module_t *module_usings_getidx(jl_module_t *m JL_PROPAGATES_ROOT, size_t i) JL_NOTSAFEPOINT;

#ifndef __clang_gcanalyzer__
// The analyzer doesn't like looking through the arraylist, so just model the
// access for it using this function
static inline jl_module_t *module_usings_getidx(jl_module_t *m JL_PROPAGATES_ROOT, size_t i) JL_NOTSAFEPOINT {
    return (jl_module_t*)m->usings.items[i];
}
#endif

static int eq_bindings(jl_binding_partition_t *owner, jl_binding_t *alias, size_t world)
{
    assert(owner->kind == BINDING_KIND_GLOBAL || jl_bpart_is_some_constant(owner));
    jl_binding_partition_t *alias_bpart = jl_get_binding_partition(alias, world);
    if (owner == alias_bpart)
        return 1;
    if (alias_bpart->kind == BINDING_KIND_GLOBAL || jl_bpart_is_some_guard(alias_bpart))
        return 0;
    while (jl_bpart_is_some_import(alias_bpart)) {
        alias = (jl_binding_t*)jl_atomic_load_relaxed(&alias_bpart->restriction);
        alias_bpart = jl_get_binding_partition(alias, world);
    }
    if (jl_bpart_is_some_constant(owner) && jl_bpart_is_some_constant(alias_bpart) && jl_atomic_load_relaxed(&owner->restriction) && jl_atomic_load_relaxed(&alias_bpart->restriction) == jl_atomic_load_relaxed(&owner->restriction))
        return 1;
    return owner == alias_bpart;
}

// find a binding from a module's `usings` list
static jl_binding_t *using_resolve_binding(jl_module_t *m JL_PROPAGATES_ROOT, jl_sym_t *var, jl_module_t **from, modstack_t *st, int warn)
{
    jl_binding_t *b = NULL;
    jl_binding_partition_t *bpart = NULL;
    jl_module_t *owner = NULL;
    JL_LOCK(&m->lock);
    int i = (int)m->usings.len - 1;
    JL_UNLOCK(&m->lock);
    for (; i >= 0; --i) {
        JL_LOCK(&m->lock);
        jl_module_t *imp = module_usings_getidx(m, i);
        JL_UNLOCK(&m->lock);
        jl_binding_t *tempb = jl_get_module_binding(imp, var, 0);
        if (tempb != NULL && tempb->exportp) {
            tempb = jl_resolve_owner(NULL, imp, var, st); // find the owner for tempb
            if (tempb == NULL)
                // couldn't resolve; try next using (see issue #6105)
                continue;
            jl_binding_partition_t *tempbpart = jl_get_binding_partition(tempb, jl_current_task->world_age);
            assert(tempbpart->kind == BINDING_KIND_GLOBAL || jl_bpart_is_some_constant(tempbpart));
            if (bpart != NULL && !tempb->deprecated && !b->deprecated && !eq_bindings(tempbpart, b, jl_current_task->world_age)) {
                if (warn) {
                    // set usingfailed=1 to avoid repeating this warning
                    // the owner will still be NULL, so it can be later imported or defined
                    tempb = jl_get_module_binding(m, var, 1);
                    tempbpart = jl_get_binding_partition(tempb, jl_current_task->world_age);
                    tempbpart->kind = BINDING_KIND_FAILED;
                    jl_printf(JL_STDERR,
                              "WARNING: both %s and %s export \"%s\"; uses of it in module %s must be qualified\n",
                              jl_symbol_name(owner->name),
                              jl_symbol_name(imp->name), jl_symbol_name(var),
                              jl_symbol_name(m->name));
                }
                return NULL;
            }
            if (owner == NULL || !tempb->deprecated) {
                owner = imp;
                b = tempb;
                bpart = tempbpart;
            }
        }
    }
    *from = owner;
    return b;
}

// for error message printing: look up the module that exported a binding to m as var
// this might not be the same as the owner of the binding, since the binding itself may itself have been imported from elsewhere
static jl_module_t *jl_binding_dbgmodule(jl_binding_t *b, jl_module_t *m, jl_sym_t *var)
{
    jl_binding_partition_t *bpart = jl_get_binding_partition(b, jl_current_task->world_age);
    if (bpart->kind != BINDING_KIND_GLOBAL) {
        // for implicitly imported globals, try to re-resolve it to find the module we got it from most directly
        jl_module_t *from = NULL;
        jl_binding_t *b2 = using_resolve_binding(m, var, &from, NULL, 0);
        if (b2) {
            jl_binding_partition_t *b2part = jl_get_binding_partition(b2, jl_current_task->world_age);
            if (eq_bindings(b2part, b, jl_current_task->world_age))
                return from;
            // if we did not find it (or accidentally found a different one), ignore this
        }
    }
    return m;
}

static void jl_binding_dep_message(jl_module_t *m, jl_sym_t *name, jl_binding_t *b);

// get binding for reading. might return NULL for unbound.
static jl_binding_t *jl_resolve_owner(jl_binding_t *b/*optional*/, jl_module_t *m, jl_sym_t *var, modstack_t *st)
{
    if (b == NULL)
        b = jl_get_module_binding(m, var, 1);
    jl_binding_partition_t *bpart = jl_get_binding_partition(b, jl_current_task->world_age);
    if (bpart->kind == BINDING_KIND_FAILED)
        return NULL;
    if (bpart->kind == BINDING_KIND_DECLARED) {
        return NULL;
    }
    if (bpart->kind == BINDING_KIND_GUARD) {
        jl_binding_t *b2 = NULL;
        modstack_t top = { m, var, st };
        modstack_t *tmp = st;
        for (; tmp != NULL; tmp = tmp->prev) {
            if (tmp->m == m && tmp->var == var) {
                // import cycle without finding actual location
                return NULL;
            }
        }
        jl_module_t *from = NULL; // for error message printing
        b2 = using_resolve_binding(m, var, &from, &top, 1);
        if (b2 == NULL)
            return NULL;
        assert(from);
        JL_GC_PROMISE_ROOTED(from); // gc-analysis does not understand output parameters
        if (b2->deprecated) {
            if (jl_get_binding_value(b2) == jl_nothing) {
                // silently skip importing deprecated values assigned to nothing (to allow later mutation)
                return NULL;
            }
        }
        // do a full import to prevent the result of this lookup from
        // changing, for example if this var is assigned to later.
        bpart->kind = BINDING_KIND_IMPLICIT;
        jl_atomic_store_relaxed(&bpart->restriction, (jl_value_t*)b2);
        if (b2->deprecated) {
            b->deprecated = 1; // we will warn about this below, but we might want to warn at the use sites too
            if (m != jl_main_module && m != jl_base_module &&
                jl_options.depwarn != JL_OPTIONS_DEPWARN_OFF) {
                /* with #22763, external packages wanting to replace
                   deprecated Base bindings should simply export the new
                   binding */
                jl_printf(JL_STDERR,
                          "WARNING: using deprecated binding %s.%s in %s.\n",
                          jl_symbol_name(from->name), jl_symbol_name(var),
                          jl_symbol_name(m->name));
                jl_binding_dep_message(from, var, b2);
            }
        }
        return b2;
    }
    while (jl_bpart_is_some_import(bpart)) {
        b = (jl_binding_t*)jl_atomic_load_relaxed(&bpart->restriction);
        bpart = jl_get_binding_partition(b, jl_current_task->world_age);
    }
    return b;
}

// get the current likely owner of binding when accessing m.var, without resolving the binding (it may change later)
JL_DLLEXPORT jl_binding_t *jl_binding_owner(jl_module_t *m, jl_sym_t *var)
{
    jl_binding_t *b = jl_get_module_binding(m, var, 1);
    jl_binding_partition_t *bpart = jl_get_binding_partition(b, jl_current_task->world_age);
    jl_module_t *from = m;
    if (bpart->kind == BINDING_KIND_GUARD)
        b = using_resolve_binding(m, var, &from, NULL, 0);
    while (jl_bpart_is_some_import(bpart)) {
        b = (jl_binding_t*)jl_atomic_load_relaxed(&bpart->restriction);
        bpart = jl_get_binding_partition(b, jl_current_task->world_age);
    }
    if (bpart && bpart->kind != BINDING_KIND_GLOBAL && !jl_bpart_is_some_constant(bpart))
        return NULL;
    return b;
}

// get type of binding m.var, without resolving the binding
JL_DLLEXPORT jl_value_t *jl_get_binding_type(jl_module_t *m, jl_sym_t *var)
{
    jl_binding_t *b = jl_get_module_binding(m, var, 0);
    jl_binding_partition_t *bpart = jl_get_binding_partition(b, jl_current_task->world_age);
    if (b == NULL)
        return jl_nothing;
    while (jl_bpart_is_some_import(bpart)) {
        b = (jl_binding_t*)jl_atomic_load_relaxed(&bpart->restriction);
        bpart = jl_get_binding_partition(b, jl_current_task->world_age);
    }
    if (jl_bpart_is_some_guard(bpart))
        return jl_nothing;
    if (jl_bpart_is_some_constant(bpart))
        return jl_typeof(jl_atomic_load_relaxed(&bpart->restriction));
    return jl_atomic_load_relaxed(&bpart->restriction);
}

JL_DLLEXPORT jl_binding_t *jl_get_binding(jl_module_t *m, jl_sym_t *var)
{
    return jl_resolve_owner(NULL, m, var, NULL);
}

JL_DLLEXPORT jl_binding_t *jl_get_binding_or_error(jl_module_t *m, jl_sym_t *var)
{
    jl_binding_t *b = jl_get_binding(m, var);
    if (b == NULL)
        jl_undefined_var_error(var, (jl_value_t*)m);
    // XXX: this only considers if the original is deprecated, not the binding in m
    if (b->deprecated)
        jl_binding_deprecation_warning(m, var, b);
    return b;
}

JL_DLLEXPORT jl_value_t *jl_module_globalref(jl_module_t *m, jl_sym_t *var)
{
    jl_binding_t *b = jl_get_module_binding(m, var, 1);
    jl_globalref_t *globalref = b->globalref;
    assert(globalref != NULL);
    return (jl_value_t*)globalref;
}

// does module m explicitly import s?
JL_DLLEXPORT int jl_is_imported(jl_module_t *m, jl_sym_t *var)
{
    jl_binding_t *b = jl_get_module_binding(m, var, 0);
    jl_binding_partition_t *bpart = jl_get_binding_partition(b, jl_current_task->world_age);
    return b && bpart->kind == BINDING_KIND_IMPORTED;
}

extern const char *jl_filename;
extern int jl_lineno;

static char const dep_message_prefix[] = "_dep_message_";

static void jl_binding_dep_message(jl_module_t *m, jl_sym_t *name, jl_binding_t *b)
{
    size_t prefix_len = strlen(dep_message_prefix);
    size_t name_len = strlen(jl_symbol_name(name));
    char *dep_binding_name = (char*)alloca(prefix_len+name_len+1);
    memcpy(dep_binding_name, dep_message_prefix, prefix_len);
    memcpy(dep_binding_name + prefix_len, jl_symbol_name(name), name_len);
    dep_binding_name[prefix_len+name_len] = '\0';
    jl_binding_t *dep_message_binding = jl_get_binding(m, jl_symbol(dep_binding_name));
    jl_value_t *dep_message = NULL;
    if (dep_message_binding != NULL)
        dep_message = jl_get_binding_value(dep_message_binding);
    JL_GC_PUSH1(&dep_message);
    if (dep_message != NULL) {
        if (jl_is_string(dep_message)) {
            jl_uv_puts(JL_STDERR, jl_string_data(dep_message), jl_string_len(dep_message));
        }
        else {
            jl_static_show(JL_STDERR, dep_message);
        }
    }
    else {
        jl_value_t *v = jl_get_binding_value(b);
        dep_message = v; // use as gc-root
        if (v) {
            if (jl_is_type(v) || jl_is_module(v)) {
                jl_printf(JL_STDERR, ", use ");
                jl_static_show(JL_STDERR, v);
                jl_printf(JL_STDERR, " instead.");
            }
            else {
                jl_methtable_t *mt = jl_gf_mtable(v);
                if (mt != NULL) {
                    jl_printf(JL_STDERR, ", use ");
                    if (mt->module != jl_core_module) {
                        jl_static_show(JL_STDERR, (jl_value_t*)mt->module);
                        jl_printf(JL_STDERR, ".");
                    }
                    jl_printf(JL_STDERR, "%s", jl_symbol_name(mt->name));
                    jl_printf(JL_STDERR, " instead.");
                }
            }
        }
    }
    jl_printf(JL_STDERR, "\n");
    JL_GC_POP();
}

// NOTE: we use explici since explicit is a C++ keyword
static void module_import_(jl_module_t *to, jl_module_t *from, jl_sym_t *asname, jl_sym_t *s, int explici)
{
    jl_binding_t *b = jl_get_binding(from, s);
    if (b == NULL) {
        jl_printf(JL_STDERR,
                  "WARNING: could not import %s.%s into %s\n",
                  jl_symbol_name(from->name), jl_symbol_name(s),
                  jl_symbol_name(to->name));
    }
    else {
        jl_binding_partition_t *bpart = jl_get_binding_partition(b, jl_current_task->world_age);
        assert(bpart->kind == BINDING_KIND_GLOBAL || jl_bpart_is_some_constant(bpart));
        if (b->deprecated) {
            if (jl_get_binding_value(b) == jl_nothing) {
                // silently skip importing deprecated values assigned to nothing (to allow later mutation)
                return;
            }
            else if (to != jl_main_module && to != jl_base_module &&
                     jl_options.depwarn != JL_OPTIONS_DEPWARN_OFF) {
                /* with #22763, external packages wanting to replace
                   deprecated Base bindings should simply export the new
                   binding */
                jl_printf(JL_STDERR,
                          "WARNING: importing deprecated binding %s.%s into %s%s%s.\n",
                          jl_symbol_name(from->name), jl_symbol_name(s),
                          jl_symbol_name(to->name),
                          asname == s ? "" : " as ",
                          asname == s ? "" : jl_symbol_name(asname));
                jl_binding_dep_message(from, s, b);
            }
        }

        jl_binding_t *bto = jl_get_module_binding(to, asname, 1);
        if (bto == b) {
            // importing a binding on top of itself. harmless.
            return;
        }
        jl_binding_partition_t *btopart = jl_get_binding_partition(bto, jl_current_task->world_age);
        if (btopart->kind == BINDING_KIND_GUARD || btopart->kind == BINDING_KIND_IMPLICIT) {
            btopart->kind = (explici != 0) ? BINDING_KIND_IMPORTED : BINDING_KIND_EXPLICIT;
            jl_atomic_store_relaxed(&btopart->restriction, (jl_value_t*)b);
            bto->deprecated |= b->deprecated; // we already warned about this above, but we might want to warn at the use sites too
        }
        else {
            if (eq_bindings(bpart, bto, jl_current_task->world_age)) {
                // already imported - potentially upgrade to _IMPORTED or _EXPLICIT
                if (jl_bpart_is_some_import(btopart))
                    btopart->kind = (explici != 0) ? BINDING_KIND_IMPORTED : BINDING_KIND_EXPLICIT;
                // TODO: This will need to be versioned when partitioned
            }
            else if (jl_bpart_is_some_import(btopart)) {
                // already imported from somewhere else
                jl_printf(JL_STDERR,
                          "WARNING: ignoring conflicting import of %s.%s into %s\n",
                          jl_symbol_name(from->name), jl_symbol_name(s),
                          jl_symbol_name(to->name));
            }
            else {
                // conflict with name owned by destination module
                jl_printf(JL_STDERR,
                          "WARNING: import of %s.%s into %s conflicts with an existing identifier; ignored.\n",
                          jl_symbol_name(from->name), jl_symbol_name(s),
                          jl_symbol_name(to->name));
            }
        }
    }
}

JL_DLLEXPORT void jl_module_import(jl_module_t *to, jl_module_t *from, jl_sym_t *s)
{
    module_import_(to, from, s, s, 1);
}

JL_DLLEXPORT void jl_module_import_as(jl_module_t *to, jl_module_t *from, jl_sym_t *s, jl_sym_t *asname)
{
    module_import_(to, from, asname, s, 1);
}

JL_DLLEXPORT void jl_module_use(jl_module_t *to, jl_module_t *from, jl_sym_t *s)
{
    module_import_(to, from, s, s, 0);
}

JL_DLLEXPORT void jl_module_use_as(jl_module_t *to, jl_module_t *from, jl_sym_t *s, jl_sym_t *asname)
{
    module_import_(to, from, asname, s, 0);
}


JL_DLLEXPORT void jl_module_using(jl_module_t *to, jl_module_t *from)
{
    if (to == from)
        return;
    JL_LOCK(&to->lock);
    for (size_t i = 0; i < to->usings.len; i++) {
        if (from == to->usings.items[i]) {
            JL_UNLOCK(&to->lock);
            return;
        }
    }
    arraylist_push(&to->usings, from);
    jl_gc_wb(to, from);
    JL_UNLOCK(&to->lock);

    // print a warning if something visible via this "using" conflicts with
    // an existing identifier. note that an identifier added later may still
    // silently override a "using" name. see issue #2054.
    jl_svec_t *table = jl_atomic_load_relaxed(&from->bindings);
    for (size_t i = 0; i < jl_svec_len(table); i++) {
        jl_binding_t *b = (jl_binding_t*)jl_svecref(table, i);
        if ((void*)b == jl_nothing)
            break;
        jl_binding_partition_t *bpart = jl_get_binding_partition(b, jl_current_task->world_age);
        if (b->exportp && (bpart->kind == BINDING_KIND_GLOBAL || bpart->kind == BINDING_KIND_IMPORTED)) {
            jl_sym_t *var = b->globalref->name;
            jl_binding_t *tob = jl_get_module_binding(to, var, 0);
            if (tob) {
                jl_binding_partition_t *tobpart = jl_get_binding_partition(tob, jl_current_task->world_age);
                while (jl_bpart_is_some_import(tobpart)) {
                    tob = (jl_binding_t*)jl_atomic_load_relaxed(&bpart->restriction);
                    tobpart = jl_get_binding_partition(tob, jl_current_task->world_age);
                }
                if (tob && tobpart->kind != BINDING_KIND_GUARD &&
                    // don't warn for conflicts with the module name itself.
                    // see issue #4715
                    var != to->name &&
                    !eq_bindings(tobpart, b, jl_current_task->world_age)) {
                    jl_printf(JL_STDERR,
                            "WARNING: using %s.%s in module %s conflicts with an existing identifier.\n",
                            jl_symbol_name(from->name), jl_symbol_name(var),
                            jl_symbol_name(to->name));
                }
            }
        }
        table = jl_atomic_load_relaxed(&from->bindings);
    }
}

JL_DLLEXPORT void jl_module_public(jl_module_t *from, jl_sym_t *s, int exported)
{
    jl_binding_t *b = jl_get_module_binding(from, s, 1);
    if (b->publicp) {
        // check for conflicting declarations
        if (b->exportp && !exported)
            jl_errorf("cannot declare %s.%s public; it is already declared exported",
                      jl_symbol_name(from->name), jl_symbol_name(s));
        if (!b->exportp && exported)
            jl_errorf("cannot declare %s.%s exported; it is already declared public",
                      jl_symbol_name(from->name), jl_symbol_name(s));
    }
    b->publicp = 1;
    b->exportp |= exported;
}

JL_DLLEXPORT int jl_boundp(jl_module_t *m, jl_sym_t *var, int allow_import) // unlike most queries here, this is currently seq_cst
{
    jl_binding_t *b = jl_get_module_binding(m, var, allow_import);
    if (!b)
        return 0;
    if (!allow_import) {
        jl_binding_partition_t *bpart = jl_get_binding_partition(b, jl_current_task->world_age);
        if (!bpart || jl_bpart_is_some_import(bpart))
            return 0;
        return jl_get_binding_value(b) != NULL;
    }
    return jl_reresolve_binding_value(b) != NULL;
}

JL_DLLEXPORT int jl_defines_or_exports_p(jl_module_t *m, jl_sym_t *var)
{
    jl_binding_t *b = jl_get_module_binding(m, var, 0);
    jl_binding_partition_t *bpart = jl_get_binding_partition(b, jl_current_task->world_age);
    return b && (b->exportp || bpart->kind == BINDING_KIND_GLOBAL);
}

JL_DLLEXPORT int jl_module_exports_p(jl_module_t *m, jl_sym_t *var)
{
    jl_binding_t *b = jl_get_module_binding(m, var, 0);
    return b && b->exportp;
}

JL_DLLEXPORT int jl_module_public_p(jl_module_t *m, jl_sym_t *var)
{
    jl_binding_t *b = jl_get_module_binding(m, var, 0);
    return b && b->publicp;
}

JL_DLLEXPORT int jl_binding_resolved_p(jl_module_t *m, jl_sym_t *var)
{
    jl_binding_t *b = jl_get_module_binding(m, var, 0);
    jl_binding_partition_t *bpart = jl_get_binding_partition(b, jl_current_task->world_age);
    return !jl_bpart_is_some_guard(bpart);
}

static uint_t bindingkey_hash(size_t idx, jl_value_t *data)
{
    jl_binding_t *b = (jl_binding_t*)jl_svecref(data, idx); // This must always happen inside the lock
    jl_sym_t *var = b->globalref->name;
    return var->hash;
}

static int bindingkey_eq(size_t idx, const void *var, jl_value_t *data, uint_t hv)
{
    if (idx >= jl_svec_len(data))
        return 0; // We got a OOB access, probably due to a data race
    jl_binding_t *b = (jl_binding_t*)jl_svecref(data, idx);
    jl_sym_t *name = b->globalref->name;
    return var == name;
}

JL_DLLEXPORT jl_binding_t *jl_get_module_binding(jl_module_t *m, jl_sym_t *var, int alloc)
{
    uint_t hv = var->hash;
    for (int locked = 0; ; locked++) {
        jl_genericmemory_t *bindingkeyset = jl_atomic_load_acquire(&m->bindingkeyset);
        jl_svec_t *bindings = jl_atomic_load_relaxed(&m->bindings);
        ssize_t idx = jl_smallintset_lookup(bindingkeyset, bindingkey_eq, var, (jl_value_t*)bindings, hv, 0); // acquire
        if (idx != -1) {
            jl_binding_t *b = (jl_binding_t*)jl_svecref(bindings, idx); // relaxed
            if (locked)
                JL_UNLOCK(&m->lock);
            return b;
        }
        if (!alloc) {
            return NULL;
        }
        else if (!locked) {
            JL_LOCK(&m->lock);
        }
        else {
            size_t i, cl = jl_svec_len(bindings);
            for (i = cl; i > 0; i--) {
                jl_value_t *b = jl_svecref(bindings, i - 1);
                if (b != jl_nothing)
                    break;
            }
            if (i == cl) {
                size_t ncl = cl < 8 ? 8 : (cl*3)>>1; // grow 50%
                jl_svec_t *nc = jl_alloc_svec_uninit(ncl);
                if (i > 0)
                    memcpy((char*)jl_svec_data(nc), jl_svec_data(bindings), sizeof(void*) * i);
                for (size_t j = i; j < ncl; j++)
                    jl_svec_data(nc)[j] = jl_nothing;
                jl_atomic_store_release(&m->bindings, nc);
                jl_gc_wb(m, nc);
                bindings = nc;
            }
            jl_binding_t *b = new_binding(m, var);
            assert(jl_svecref(bindings, i) == jl_nothing);
            jl_svecset(bindings, i, b); // relaxed
            jl_smallintset_insert(&m->bindingkeyset, (jl_value_t*)m, bindingkey_hash, i, (jl_value_t*)bindings); // release
            JL_UNLOCK(&m->lock);
            return b;
        }
    }
}


JL_DLLEXPORT jl_value_t *jl_get_globalref_value(jl_globalref_t *gr)
{
    jl_binding_t *b = gr->binding;
    b = jl_resolve_owner(b, gr->mod, gr->name, NULL);
    // ignores b->deprecated
    return b == NULL ? NULL : jl_get_binding_value(b);
}

JL_DLLEXPORT jl_value_t *jl_get_global(jl_module_t *m, jl_sym_t *var)
{
    jl_binding_t *b = jl_get_binding(m, var);
    if (b == NULL)
        return NULL;
    // XXX: this only considers if the original is deprecated, not the binding in m
    if (b->deprecated)
        jl_binding_deprecation_warning(m, var, b);
    return jl_get_binding_value(b);
}

JL_DLLEXPORT void jl_set_global(jl_module_t *m JL_ROOTING_ARGUMENT, jl_sym_t *var, jl_value_t *val JL_ROOTED_ARGUMENT)
{
    jl_binding_t *bp = jl_get_binding_wr(m, var, 0);
    jl_checked_assignment(bp, m, var, val);
}

JL_DLLEXPORT void jl_set_const(jl_module_t *m JL_ROOTING_ARGUMENT, jl_sym_t *var, jl_value_t *val JL_ROOTED_ARGUMENT)
{
    // this function is mostly only used during initialization, so the data races here are not too important to us
    jl_binding_t *bp = jl_get_module_binding(m, var, 1);
    jl_binding_partition_t *bpart = jl_get_binding_partition(bp, jl_current_task->world_age);
    bpart->kind = BINDING_KIND_CONST;
    bpart->restriction = val;
    jl_atomic_store_relaxed(&bpart->restriction, val);
    jl_gc_wb(bpart, val);
}

JL_DLLEXPORT int jl_globalref_is_const(jl_globalref_t *gr)
{
    jl_binding_t *b = gr->binding;
    b = jl_resolve_owner(b, gr->mod, gr->name, NULL);
    jl_binding_partition_t *bpart = jl_get_binding_partition(b, jl_current_task->world_age);
    return jl_bpart_is_some_constant(bpart);
}

JL_DLLEXPORT int jl_globalref_boundp(jl_globalref_t *gr)
{
    jl_binding_t *b = gr->binding;
    b = jl_resolve_owner(b, gr->mod, gr->name, NULL);
    return b && jl_get_binding_value(b) != NULL;
}

JL_DLLEXPORT int jl_is_const(jl_module_t *m, jl_sym_t *var)
{
    jl_binding_t *b = jl_get_binding(m, var);
    jl_binding_partition_t *bpart = jl_get_binding_partition(b, jl_current_task->world_age);
    return b && jl_bpart_is_some_constant(bpart);
}

// set the deprecated flag for a binding:
//   0=not deprecated, 1=renamed, 2=moved to another package
JL_DLLEXPORT void jl_deprecate_binding(jl_module_t *m, jl_sym_t *var, int flag)
{
    // XXX: this deprecates the original value, which might be imported from elsewhere
    jl_binding_t *b = jl_get_binding(m, var);
    if (b) b->deprecated = flag;
}

JL_DLLEXPORT int jl_is_binding_deprecated(jl_module_t *m, jl_sym_t *var)
{
    if (jl_binding_resolved_p(m, var)) {
        // XXX: this only considers if the original is deprecated, not this precise binding
        jl_binding_t *b = jl_get_binding(m, var);
        return b && b->deprecated;
    }
    return 0;
}

void jl_binding_deprecation_warning(jl_module_t *m, jl_sym_t *s, jl_binding_t *b)
{
    // Only print a warning for deprecated == 1 (renamed).
    // For deprecated == 2 (moved to a package) the binding is to a function
    // that throws an error, so we don't want to print a warning too.
    if (b->deprecated == 1 && jl_options.depwarn) {
        if (jl_options.depwarn != JL_OPTIONS_DEPWARN_ERROR)
            jl_printf(JL_STDERR, "WARNING: ");
        jl_printf(JL_STDERR, "%s.%s is deprecated",
                  jl_symbol_name(m->name), jl_symbol_name(s));
        jl_binding_dep_message(m, s, b);

        if (jl_options.depwarn != JL_OPTIONS_DEPWARN_ERROR) {
            if (jl_lineno != 0) {
                jl_printf(JL_STDERR, "  likely near %s:%d\n", jl_filename, jl_lineno);
            }
        }

        if (jl_options.depwarn == JL_OPTIONS_DEPWARN_ERROR) {
            jl_errorf("use of deprecated variable: %s.%s",
                      jl_symbol_name(m->name),
                      jl_symbol_name(s));
        }
    }
}

jl_value_t *jl_check_binding_wr(jl_binding_t *b JL_PROPAGATES_ROOT, jl_module_t *mod, jl_sym_t *var, jl_value_t *rhs JL_MAYBE_UNROOTED, int reassign)
{
    jl_binding_partition_t *bpart = jl_get_binding_partition(b, jl_current_task->world_age);
    assert(!jl_bpart_is_some_guard(bpart) && !jl_bpart_is_some_import(bpart));
    if (jl_bpart_is_some_constant(bpart)) {
        if (rhs == bpart->restriction)
            return NULL;
        jl_errorf("invalid redefinition of constant %s.%s",
                    jl_symbol_name(mod->name), jl_symbol_name(var));
    }
    jl_value_t *old_ty = jl_atomic_load_relaxed(&bpart->restriction);
    if (old_ty != (jl_value_t*)jl_any_type && jl_typeof(rhs) != old_ty) {
        JL_GC_PUSH1(&rhs); // callee-rooted
        if (!jl_isa(rhs, old_ty))
            jl_errorf("cannot assign an incompatible value to the global %s.%s.",
                        jl_symbol_name(mod->name), jl_symbol_name(var));
        JL_GC_POP();
    }
    return old_ty;
}

JL_DLLEXPORT void jl_checked_assignment(jl_binding_t *b, jl_module_t *mod, jl_sym_t *var, jl_value_t *rhs)
{
    if (jl_check_binding_wr(b, mod, var, rhs, 1) != NULL) {
        jl_atomic_store_release(&b->value, rhs);
        jl_gc_wb(b, rhs);
    }
}

JL_DLLEXPORT jl_value_t *jl_checked_swap(jl_binding_t *b, jl_module_t *mod, jl_sym_t *var, jl_value_t *rhs)
{
    jl_check_binding_wr(b, mod, var, rhs, 0);
    jl_value_t *old = jl_atomic_exchange(&b->value, rhs);
    jl_gc_wb(b, rhs);
    if (__unlikely(old == NULL))
        jl_undefined_var_error(var, (jl_value_t*)mod);
    return old;
}

JL_DLLEXPORT jl_value_t *jl_checked_replace(jl_binding_t *b, jl_module_t *mod, jl_sym_t *var, jl_value_t *expected, jl_value_t *rhs)
{
    jl_value_t *ty = jl_check_binding_wr(b, mod, var, rhs, 0);
    return replace_value(ty, &b->value, (jl_value_t*)b, expected, rhs, 1, mod, var);
}

JL_DLLEXPORT jl_value_t *jl_checked_modify(jl_binding_t *b, jl_module_t *mod, jl_sym_t *var, jl_value_t *op, jl_value_t *rhs)
{
    jl_binding_partition_t *bpart = jl_get_binding_partition(b, jl_current_task->world_age);
    assert(!jl_bpart_is_some_guard(bpart) && !jl_bpart_is_some_import(bpart));
    if (jl_bpart_is_some_constant(bpart))
        jl_errorf("invalid redefinition of constant %s.%s",
                  jl_symbol_name(mod->name), jl_symbol_name(var));
    jl_value_t *ty = jl_atomic_load_relaxed(&bpart->restriction);
    return modify_value(ty, &b->value, (jl_value_t*)b, op, rhs, 1, mod, var);
}

JL_DLLEXPORT jl_value_t *jl_checked_assignonce(jl_binding_t *b, jl_module_t *mod, jl_sym_t *var, jl_value_t *rhs )
{
    jl_check_binding_wr(b, mod, var, rhs, 0);
    jl_value_t *old = NULL;
    if (jl_atomic_cmpswap(&b->value, &old, rhs))
        jl_gc_wb(b, rhs);
    return old;
}

JL_DLLEXPORT void jl_declare_constant(jl_binding_t *b)
{
    jl_binding_partition_t *bpart = jl_get_binding_partition(b, jl_current_task->world_age);
    if (jl_bpart_is_some_constant(bpart))
        return;
    if (!jl_bpart_is_some_guard(bpart)) {
        jl_errorf("cannot declare %s.%s constant; it already has a value",
                  jl_symbol_name(b->globalref->mod->name), jl_symbol_name(b->globalref->name));
    }
    bpart->kind = BINDING_KIND_CONST;
}

JL_DLLEXPORT jl_value_t *jl_module_usings(jl_module_t *m)
{
    JL_LOCK(&m->lock);
    int j = m->usings.len;
    jl_array_t *a = jl_alloc_array_1d(jl_array_any_type, j);
    JL_GC_PUSH1(&a);
    for (int i = 0; j > 0; i++) {
        j--;
        jl_module_t *imp = (jl_module_t*)m->usings.items[i];
        jl_array_ptr_set(a, j, (jl_value_t*)imp);
    }
    JL_UNLOCK(&m->lock); // may gc
    JL_GC_POP();
    return (jl_value_t*)a;
}

void _append_symbol_to_bindings_array(jl_array_t* a, jl_sym_t *name) {
    jl_array_grow_end(a, 1);
    //XXX: change to jl_arrayset if array storage allocation for Array{Symbols,1} changes:
    jl_array_ptr_set(a, jl_array_dim0(a)-1, (jl_value_t*)name);
}

void append_module_names(jl_array_t* a, jl_module_t *m, int all, int imported, int usings)
{
    jl_svec_t *table = jl_atomic_load_relaxed(&m->bindings);
    for (size_t i = 0; i < jl_svec_len(table); i++) {
        jl_binding_t *b = (jl_binding_t*)jl_svecref(table, i);
        if ((void*)b == jl_nothing)
            break;
        jl_sym_t *asname = b->globalref->name;
        int hidden = jl_symbol_name(asname)[0]=='#';
        int main_public = (m == jl_main_module && !(asname == jl_eval_sym || asname == jl_include_sym));
        jl_binding_partition_t *bpart = jl_get_binding_partition(b, jl_current_task->world_age);
        if (((b->publicp) ||
             (imported && (bpart->kind == BINDING_KIND_CONST_IMPORT || bpart->kind == BINDING_KIND_IMPORTED)) ||
             (usings && bpart->kind == BINDING_KIND_EXPLICIT) ||
             ((bpart->kind == BINDING_KIND_GLOBAL || bpart->kind == BINDING_KIND_CONST) && (all || main_public))) &&
            (all || (!b->deprecated && !hidden)))
            _append_symbol_to_bindings_array(a, asname);
    }
}

void append_exported_names(jl_array_t* a, jl_module_t *m, int all)
{
    jl_svec_t *table = jl_atomic_load_relaxed(&m->bindings);
    for (size_t i = 0; i < jl_svec_len(table); i++) {
        jl_binding_t *b = (jl_binding_t*)jl_svecref(table, i);
        if ((void*)b == jl_nothing)
            break;
        if (b->exportp && (all || !b->deprecated))
            _append_symbol_to_bindings_array(a, b->globalref->name);
    }
}

JL_DLLEXPORT jl_value_t *jl_module_names(jl_module_t *m, int all, int imported, int usings)
{
    jl_array_t *a = jl_alloc_array_1d(jl_array_symbol_type, 0);
    JL_GC_PUSH1(&a);
    append_module_names(a, m, all, imported, usings);
    if (usings) {
        // If `usings` is specified, traverse the list of `using`-ed modules and incorporate
        // the names exported by those modules into the list.
        for(int i=(int)m->usings.len-1; i >= 0; --i) {
            jl_module_t *usinged = module_usings_getidx(m, i);
            append_exported_names(a, usinged, all);
        }
    }
    JL_GC_POP();
    return (jl_value_t*)a;
}

JL_DLLEXPORT jl_sym_t *jl_module_name(jl_module_t *m) { return m->name; }
JL_DLLEXPORT jl_module_t *jl_module_parent(jl_module_t *m) { return m->parent; }
jl_module_t *jl_module_root(jl_module_t *m)
{
    while (1) {
        if (m->parent == NULL || m->parent == m)
            return m;
        m = m->parent;
    }
}

JL_DLLEXPORT jl_uuid_t jl_module_build_id(jl_module_t *m) { return m->build_id; }
JL_DLLEXPORT jl_uuid_t jl_module_uuid(jl_module_t* m) { return m->uuid; }

// TODO: make this part of the module constructor and read-only?
JL_DLLEXPORT void jl_set_module_uuid(jl_module_t *m, jl_uuid_t uuid) { m->uuid = uuid; }

int jl_is_submodule(jl_module_t *child, jl_module_t *parent) JL_NOTSAFEPOINT
{
    while (1) {
        if (parent == child)
            return 1;
        if (child == NULL || child == child->parent)
            return 0;
        child = child->parent;
    }
}

// Remove implicitly imported identifiers, effectively resetting all the binding
// resolution decisions for a module. This is dangerous, and should only be
// done for modules that are essentially empty anyway. The only use case for this
// is to leave `Main` as empty as possible in the default system image.
JL_DLLEXPORT void jl_clear_implicit_imports(jl_module_t *m)
{
    JL_LOCK(&m->lock);
    jl_svec_t *table = jl_atomic_load_relaxed(&m->bindings);
    for (size_t i = 0; i < jl_svec_len(table); i++) {
        jl_binding_t *b = (jl_binding_t*)jl_svecref(table, i);
        if ((void*)b == jl_nothing)
            break;
        jl_binding_partition_t *bpart = jl_get_binding_partition(b, jl_current_task->world_age);
        if (bpart->kind == BINDING_KIND_IMPLICIT) {
            bpart->kind = BINDING_KIND_GUARD;
            jl_atomic_store_relaxed(&bpart->restriction, NULL);
        }
    }
    JL_UNLOCK(&m->lock);
}

JL_DLLEXPORT void jl_init_restored_module(jl_value_t *mod)
{
    if (!jl_generating_output() || jl_options.incremental) {
        jl_module_run_initializer((jl_module_t*)mod);
    }
    else {
        if (jl_module_init_order == NULL)
            jl_module_init_order = jl_alloc_vec_any(0);
        jl_array_ptr_1d_push(jl_module_init_order, mod);
    }
}

#ifdef __cplusplus
}
#endif
