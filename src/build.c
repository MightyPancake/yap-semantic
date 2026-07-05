#include "yap_semantic.h"
#include "build.h"
#include <float.h>
#include <stdint.h>

static yap_expr yap_build_blob_cast(yap_source* src, yap_expr blob_expr, yap_type_id target_type, yap_loc loc);
static yap_expr yap_build_macro_expr(yap_source* src, yap_macro_call_node* call);
static bool yap_is_comptime_type(yap_ctx* ctx, yap_type_id id);
static void* yap_exec_macro_call(yap_source* src, yap_macro_call_node* call, yap_type_id* out_ret_type);
yap_type_id yap_build_type_from_type_node(yap_source* src, yap_type_node* tnode);

/*
 * Empty-type helper (not yet declared in ctx.h, defined in ctx.c).
 */
static yap_type yap_empty_type(yap_type_kind kind){
    return (yap_type){ .kind = kind, .is_const = false };
}

/*
 * Error helper – push a positioned error to ctx.
 */
static void yap_build_push_error(yap_source* src, yap_loc loc, const char* fmt, ...){
    if (!src || !src->ctx || !fmt) return;
    yap_ctx* ctx = src->ctx;

    va_list ap;
    va_start(ap, fmt);
    char* msg = NULL;
    int fmt_res = vasprintf(&msg, fmt, ap);
    va_end(ap);

    if (fmt_res < 0 || !msg){
        msg = strus_copy("(failed to format build error)");
    }

    yap_log("BUILD ERROR: %s", msg);
    yap_ctx_push_error(ctx, (yap_error){
        .kind  = yap_error_pos,
        .src   = loc.src ? loc.src : src,
        .range = loc.range,
        .msg   = msg
    });
}

/* The module whose scope/prefix govern how 'src' names its own top-level
 * declarations: the module it was imported from (modules/<name>/mod.yap),
 * or -- for the root project's own source -- whatever module the project
 * declared itself as via a top-level 'module{...}' block (ctx's single
 * "current module" tracker is only ever switched to that root module, once,
 * during import resolution; it never points at an imported module). Used
 * everywhere a bare identifier's or call's declaring module needs to be
 * recovered, since ctx_current_module() alone only answers that correctly
 * for the root project itself. */
static yap_module* yap_source_owning_module(yap_ctx* ctx, yap_source* src){
    if (src->from_module_import) return yap_ctx_get_module(ctx, src->from_module_import);
    return yap_ctx_current_module(ctx);
}

/*
 * Recursive post-order build: leaves first, deduplicated by origin.
 * visited_origins is a pointer-to-darr so a realloc in a deep call stays
 * visible to shallower frames instead of leaving them a dangling pointer.
 */
static void yap_build_source_postorder(yap_ctx* ctx, yap_source* src, darr(char*)* visited_origins){
    if (!src || !src->source_node) return;

    // Skip if already visited (by origin)
    if (src->origin){
        for_darr(i, vo, *visited_origins){
            if (strcmp(vo, src->origin) == 0) return;
        }
        darr_push(*visited_origins, src->origin);
    }

    // Recurse into file-import children first (leaves before parents)
    for_darr(i, imp, src->imports){
        if (imp.kind != yap_import_file) continue;
        yap_source* child = find_source_by_identity(ctx, imp.identity);
        if (child)
            yap_build_source_postorder(ctx, child, visited_origins);
    }

    // Now build this source's own declarations
    yap_source_node* snode = src->source_node;
    if (snode->was_built) return;
    snode->was_built = true;
    yap_log("Building source: %s (%d declarations)", src->identity, darr_len(snode->declarations));

    // If this source is from a module import, push the module's scope
    bool pushed_module_scope = false;
    if (src->from_module_import){
        yap_module* mod = yap_ctx_get_module(ctx, src->from_module_import);
        if (mod && mod->scope){
            darr_push(ctx->current_scopes, mod->scope);
            pushed_module_scope = true;
            yap_log("Pushed scope for module '%s'", src->from_module_import);
        }
    }

    /* Pass 1 – register top-level signatures for mutual recursion */
    for_darr(j, dnode, snode->declarations){
        yap_build_top_level_declaration(src, &dnode);
    }

    /* Pass 2 – build full declarations (skip comptime functions already built in Pass 1) */
    char* decl_prefix = NULL;
    {
        yap_module* owning_mod = yap_source_owning_module(ctx, src);
        if (owning_mod && owning_mod->prefix && owning_mod->prefix[0])
            decl_prefix = owning_mod->prefix;
    }
    for_darr(j, dnode, snode->declarations){
        yap_decl decl = yap_build_decl(src, &dnode);
        decl.module_prefix = decl_prefix;
        yap_log("Pass 2: built declaration kind=%d", decl.kind);
        darr_push(ctx->semantic_decls, decl);
        if (ctx->gen_decl)
            ctx->gen_decl(ctx, decl);
    }

    if (pushed_module_scope){
        darr_pop(ctx->current_scopes);
        yap_log("Popped scope for module '%s'", src->from_module_import);
    }
}

/*
 * Top-level entry point.
 */
yap_ctx* yap_build(yap_ctx* ctx, yap_args args){
    (void)args;
    yap_log("\n\nBuild phase\n");

    if (!ctx->root_source){
        yap_ctx_push_error(ctx, (yap_error){
            .kind = yap_error_no_pos,
            .msg  = strus_copy("No root source – nothing to build")
        });
        return ctx;
    }

    /* Walk the source tree post-order: deepest imports first,
     * deduplicating by origin so each source file is built once. */
    darr(char*) visited_origins = darr_new(char*);
    for_darr(i, imp, ctx->root_source->imports){
        yap_source* src = NULL;
        if (imp.kind == yap_import_file){
            src = find_source_by_identity(ctx, imp.identity);
        } else if (imp.kind == yap_import_module){
            for_darr(si, s, ctx->sources){
                if (s && s->from_module_import && strcmp(s->from_module_import, imp.module_name) == 0){
                    src = s;
                    break;
                }
            }
        }
        if (!src){
            yap_log("Failed to find source for import (kind=%d)", imp.kind);
            continue;
        }
        yap_build_source_postorder(ctx, src, &visited_origins);
    }
    darr_free(visited_origins);

    return ctx;
}

/* Named struct/union/enum types are the only eligible method subjects. */
static const char* yap_named_type_owner_name(yap_type* t){
    if (!t) return NULL;
    if (t->kind == yap_type_struct) return t->structure.name;
    if (t->kind == yap_type_union)  return t->uni.name;
    if (t->kind == yap_type_enum)   return t->enumeration.name;
    return NULL;
}

/* Functions declared with an explicit 'subj_type subj_name:' subject become
 * methods: only reachable as 'recv:name(args)', registered under a mangled
 * "TypeName_funcname" symbol so each type can have its own function of that
 * name. Computed independently (not cached on the AST node) so Pass 1 and
 * Pass 2 - which each iterate their own by-value copy of the declaration -
 * agree on the same name. */
static char* yap_func_decl_emit_name(yap_ctx* ctx, yap_func_decl_node* fnode){
    if (!fnode->has_subject || !fnode->subject_type_node
        || fnode->subject_type_node->kind != yap_type_node_identifier
        || !fnode->subject_type_node->identifier.value)
        return fnode->name.value;

    yap_type_id tid = yap_ctx_get_type_id_by_name(ctx, fnode->subject_type_node->identifier.value);
    yap_type* t = tid ? yap_ctx_get_type(ctx, tid) : NULL;
    const char* owner_name = yap_named_type_owner_name(t);
    if (!owner_name || !owner_name[0]) return fnode->name.value;

    return yap_ctx_strus_newf(ctx, "%s_%s", owner_name, fnode->name.value);
}

void yap_build_top_level_declaration(yap_source* src, yap_decl_node* node){
    yap_ctx* ctx = src->ctx;

    switch (node->kind){
        case yap_decl_func_decl:
        case yap_decl_func_def: {
            yap_func_decl_node* f = &node->func_decl;
            if (!f->name.value) return;

            yap_type_id return_type = ctx->void_type_id;
            if (f->has_return_type && f->return_type_node){
                return_type = yap_build_type_from_type_node(src, f->return_type_node);
                if (!return_type){
                    yap_build_push_error(src, f->return_type_node->loc,
                        "Invalid return type in function '%s'", f->name.value);
                    return;
                }
            }

            darr(yap_type_id) arg_type_ids = yap_ctx_darr_new(ctx, yap_type_id,
                .cap = darr_len(f->args) + (f->has_subject ? 1 : 0), .len = 0);
            darr(char*) arg_names = yap_ctx_darr_new(ctx, char*,
                .cap = darr_len(f->args), .len = 0);

            if (f->has_subject){
                if (!f->subject_name.value){
                    yap_build_push_error(src, f->loc, "Missing subject name in function '%s'", f->name.value);
                    return;
                }
                yap_type_id subj_tid = yap_build_type_from_type_node(src, f->subject_type_node);
                if (!subj_tid){
                    yap_build_push_error(src, f->subject_type_node->loc,
                        "Invalid subject type in function '%s'", f->name.value);
                    return;
                }
                if (!yap_named_type_owner_name(yap_ctx_get_type(ctx, subj_tid))){
                    yap_build_push_error(src, f->subject_type_node->loc,
                        "Subject type must be a named struct, union or enum in function '%s'", f->name.value);
                    return;
                }
                darr_push(arg_type_ids, subj_tid);
            }

            for_darr(ai, arg_node, f->args){
                if (!arg_node.is_valid) continue;
                yap_type_id tid = ctx->untyped_int_type_id;
                if (arg_node.has_type && arg_node.type_node){
                    tid = yap_build_type_from_type_node(src, arg_node.type_node);
                    if (!tid){
                        yap_build_push_error(src, arg_node.loc,
                            "Invalid type for argument '%s'", arg_node.name.value);
                        return;
                    }
                }
                darr_push(arg_type_ids, tid);
                darr_push(arg_names,  arg_node.name.value);
            }

            yap_type func_type = {
                .kind     = yap_type_func,
                .func     = { .args = arg_type_ids, .return_type = return_type },
                .is_const = false
            };
            yap_type_id func_type_id = yap_ctx_insert_type_if_not_exists(ctx, func_type);

            char* emit_name = yap_func_decl_emit_name(ctx, f);
            yap_var func_var = { .name = emit_name, .type = func_type_id };
            yap_ctx_push_var(ctx, func_var);
            yap_log("Pass 1: registered function '%s'", emit_name);
            break;
        }
        case yap_decl_named_type: {
            const char *name = node->named_type_decl.name.value;
            if (!name) break;
            yap_type_id existing = yap_ctx_get_type_id_by_name(ctx, (char*)name);
            if (!existing) {
                yap_type placeholder = yap_empty_type(yap_type_struct);
                placeholder.structure = (yap_struct_type){
                    .fields = NULL, .c_name = (char*)name, .name = (char*)name
                };
                yap_ctx_push_named_type(ctx, (char*)name, (char*)name, placeholder);
                yap_log("Pass 1: registered type placeholder '%s'", name);
            }
            break;
        }
        default:
            break;
    }
}

/* ----------------------------------------------------------------
 *  Declarations
 * ---------------------------------------------------------------- */

yap_decl yap_build_decl(yap_source* src, yap_decl_node* node){
    yap_decl res = { .kind = yap_decl_error };

    switch (node->kind){
        case yap_decl_func_def:
            res = yap_build_fn_def(src, &node->func_decl);
            break;
        case yap_decl_func_decl:
            res = yap_build_fn_declaration(src, &node->func_decl);
            break;
        case yap_decl_named_type:
            res = yap_build_named_type_decl(src, &node->named_type_decl);
            break;
        case yap_decl_module_import:
        case yap_decl_file_import:
        case yap_decl_module_decl:
            break;
        case yap_decl_macro: {
            yap_expr macro_result = yap_build_macro_expr(src, &node->macro_call);
            (void)macro_result;
            break;
        }
        default:
            yap_build_push_error(src, node->loc, "Unhandled declaration kind");
            break;
    }
    res.loc   = node->loc;
    res.range = node->loc.range;
    return res;
}

/* Builds the receiver declared via 'subj_type subj_name:' as the function's
 * implicit first argument, mirroring yap_build_func_arg for regular args. */
static yap_func_arg yap_build_subject_arg(yap_source* src, yap_func_decl_node* fnode){
    yap_type_id type = yap_build_type_from_type_node(src, fnode->subject_type_node);
    if (!type) return (yap_func_arg){ .kind = yap_func_arg_error };

    return (yap_func_arg){
        .kind = yap_func_arg_valid,
        .name = fnode->subject_name.value,
        .type = type
    };
}

yap_decl yap_build_fn_def(yap_source* src, yap_func_decl_node* fnode){
    yap_ctx* ctx = src->ctx;

    if (!fnode->name.value){
        yap_build_push_error(src, fnode->loc, "Missing function name");
        return (yap_decl){ .kind = yap_decl_error };
    }

    char* emit_name = yap_func_decl_emit_name(ctx, fnode);
    yap_log("Building function '%s' (emit name '%s')", fnode->name.value, emit_name);

    const yap_var* func_var = yap_scope_get_var_recursive(
        yap_ctx_current_scope(ctx), emit_name);

    if (!func_var){
        yap_build_push_error(src, fnode->loc,
            "Internal error: function '%s' not registered in pass 1",
            fnode->name.value);
        return (yap_decl){ .kind = yap_decl_error };
    }

    yap_type* t = yap_ctx_get_type(ctx, func_var->type);
    if (!t || t->kind != yap_type_func){
        yap_build_push_error(src, fnode->loc,
            "Internal error: function '%s' type mismatch in pass 2",
            fnode->name.value);
        return (yap_decl){ .kind = yap_decl_error };
    }

    yap_fn_type fn_type = t->func;

    darr(yap_func_arg) args = yap_ctx_darr_new(ctx, yap_func_arg,
        .cap = darr_len(fnode->args) + (fnode->has_subject ? 1 : 0), .len = 0);
    if (fnode->has_subject){
        darr_push(args, yap_build_subject_arg(src, fnode));
    }
    for_darr(ai, arg_node, fnode->args){
        darr_push(args, yap_build_func_arg(src, &arg_node));
    }

    yap_scope* func_scope = yap_ctx_push_new_scope(ctx);
    for_darr(ai, arg, args){
        if (arg.kind == yap_func_arg_valid){
            yap_scope_set_var(func_scope,
                (yap_var){ .name = arg.name, .type = arg.type });
        }
    }

    yap_block body = yap_build_block(src, &fnode->body);
    yap_ctx_pop_scope(ctx);

    return (yap_decl){
        .kind      = yap_decl_func_def,
        .func_decl = (yap_func_decl){
            .name    = emit_name,
            .args    = args,
            .ret_typ = fn_type.return_type,
            .body    = body
        }
    };
}

yap_decl yap_build_fn_declaration(yap_source* src, yap_func_decl_node* fnode){
    yap_ctx* ctx = src->ctx;

    if (!fnode->name.value){
        yap_build_push_error(src, fnode->loc, "Missing function name");
        return (yap_decl){ .kind = yap_decl_error };
    }

    char* emit_name = yap_func_decl_emit_name(ctx, fnode);
    yap_log("Building function declaration '%s' (emit name '%s')", fnode->name.value, emit_name);

    const yap_var* func_var = yap_scope_get_var_recursive(
        yap_ctx_current_scope(ctx), emit_name);

    if (!func_var){
        yap_build_push_error(src, fnode->loc,
            "Internal error: function '%s' not registered in pass 1",
            fnode->name.value);
        return (yap_decl){ .kind = yap_decl_error };
    }

    yap_type* t = yap_ctx_get_type(ctx, func_var->type);
    if (!t || t->kind != yap_type_func){
        yap_build_push_error(src, fnode->loc,
            "Internal error: function '%s' type mismatch in pass 2",
            fnode->name.value);
        return (yap_decl){ .kind = yap_decl_error };
    }

    yap_fn_type fn_type = t->func;

    darr(yap_func_arg) args = yap_ctx_darr_new(ctx, yap_func_arg,
        .cap = darr_len(fnode->args) + (fnode->has_subject ? 1 : 0), .len = 0);
    if (fnode->has_subject){
        darr_push(args, yap_build_subject_arg(src, fnode));
    }
    for_darr(ai, arg_node, fnode->args){
        darr_push(args, yap_build_func_arg(src, &arg_node));
    }

    return (yap_decl){
        .kind      = yap_decl_func_decl,
        .func_decl = (yap_func_decl){
            .name    = emit_name,
            .args    = args,
            .ret_typ = fn_type.return_type,
            .body    = { .kind = yap_block_none }
        }
    };
}

yap_decl yap_build_named_type_decl(yap_source* src, yap_named_type_decl_node* tnode){
    yap_ctx* ctx = src->ctx;
    char* name = (char*)(tnode->name.value ? tnode->name.value : "(anon)");

    switch (tnode->kind){
        case yap_named_type_decl_struct: {
            darr(yap_struct_field) fields = yap_ctx_darr_new(ctx, yap_struct_field,
                .cap = darr_len(tnode->as_struct.fields), .len = 0);
            for_darr(fi, fv, tnode->as_struct.fields){
                darr_push(fields, yap_build_struct_field(src, &fv));
            }
            yap_type t = yap_empty_type(yap_type_struct);
            t.structure = (yap_struct_type){
                .fields = fields,
                .c_name = name,
                .name   = name
            };
            yap_type_id id = yap_ctx_push_named_type(ctx, name, name, t);
            return (yap_decl){
                .kind = yap_decl_named_type,
                .named_type_decl = (yap_named_type_decl){
                    .name    = name,
                    .c_name  = name,
                    .kind    = yap_named_type_decl_struct,
                    .type_id = id
                }
            };
        }
        case yap_named_type_decl_enum: {
            darr(yap_enum_variant) variants = yap_ctx_darr_new(ctx, yap_enum_variant,
                .cap = darr_len(tnode->as_enum.variants), .len = 0);
            for_darr(vi, ev, tnode->as_enum.variants){
                darr_push(variants, yap_build_enum_variant(src, &ev));
            }
            yap_type t = yap_empty_type(yap_type_enum);
            t.enumeration = (yap_enum_type){
                .variants = variants,
                .c_name   = name,
                .name     = name
            };
            yap_type_id id = yap_ctx_push_named_type(ctx, name, name, t);
            return (yap_decl){
                .kind = yap_decl_named_type,
                .named_type_decl = (yap_named_type_decl){
                    .name    = name,
                    .c_name  = name,
                    .kind    = yap_named_type_decl_enum,
                    .type_id = id
                }
            };
        }
        case yap_named_type_decl_union: {
            darr(yap_struct_field) variants = yap_ctx_darr_new(ctx, yap_struct_field,
                .cap = darr_len(tnode->as_union.variants), .len = 0);
            for_darr(vi, uv, tnode->as_union.variants){
                darr_push(variants, yap_build_struct_field(src, &uv));
            }
            yap_type t = yap_empty_type(yap_type_union);
            t.uni = (yap_union_type){
                .variants = variants,
                .c_name   = name,
                .name     = name
            };
            yap_type_id id = yap_ctx_push_named_type(ctx, name, name, t);
            return (yap_decl){
                .kind = yap_decl_named_type,
                .named_type_decl = (yap_named_type_decl){
                    .name    = name,
                    .c_name  = name,
                    .kind    = yap_named_type_decl_union,
                    .type_id = id
                }
            };
        }
        case yap_named_type_decl_alias: {
            yap_type t = yap_empty_type(yap_type_struct);
            t.structure = (yap_struct_type){
                .fields = yap_ctx_darr_new(ctx, yap_struct_field, .cap = 0, .len = 0),
                .c_name = name,
                .name   = name
            };
            yap_type_id id = yap_ctx_push_named_type(ctx, name, name, t);
            return (yap_decl){
                .kind = yap_decl_named_type,
                .named_type_decl = (yap_named_type_decl){
                    .name    = name,
                    .c_name  = name,
                    .kind    = yap_named_type_decl_alias,
                    .type_id = id
                }
            };
        }
        default:
            yap_build_push_error(src, tnode->loc,
                "Unhandled named type declaration kind");
            return (yap_decl){ .kind = yap_decl_error };
    }
}

/* ----------------------------------------------------------------
 *  Statements
 * ---------------------------------------------------------------- */

yap_statement yap_build_statement(yap_source* src, yap_statement_node* node){
    yap_statement ret = { .kind = yap_statement_error };

    switch (node->kind){
        case yap_statement_empty:     ret = yap_build_empty_statement(src);             break;
        case yap_statement_expr:      ret = yap_build_expr_statement(src, &node->expr); break;
        case yap_statement_var_decl:  ret = yap_build_var_decl_statement(src, &node->var_decl); break;
        case yap_statement_return:    ret = yap_build_return_statement(src, &node->return_stmt); break;
        case yap_statement_if:        ret = yap_build_if_statement(src, &node->if_stmt); break;
        case yap_statement_if_else:   ret = yap_build_if_else_statement(src, &node->if_else_stmt); break;
        case yap_statement_while:     ret = yap_build_while_statement(src, &node->while_stmt); break;
        case yap_statement_for:       ret = yap_build_for_statement(src, &node->for_stmt); break;
        case yap_statement_break:     ret = yap_build_break_statement(src, node);        break;
        case yap_statement_continue:  ret = yap_build_continue_statement(src, node);      break;
        case yap_statement_block:     ret = yap_build_block_statement(src, &node->block); break;
        case yap_statement_macro: {
            yap_expr macro_result = yap_build_macro_expr(src, &node->macro_call);
            if (macro_result.kind == yap_expr_error)
                ret = (yap_statement){ .kind = yap_statement_error };
            else
                ret = (yap_statement){ .kind = yap_statement_expr, .expr = macro_result };
            break;
        }
        default:
            yap_build_push_error(src, node->loc, "Unhandled statement kind");
            break;
    }
    ret.loc   = node->loc;
    ret.range = node->loc.range;
    return ret;
}

/* Walks an already-*built* statement tree (not parse nodes -- this runs
 * after a macro has already executed and returned its yStmt result) looking
 * for yap_statement_deferred sentinels: raw, unbuilt fragments captured from
 * a yap_macro_param_statement macro argument (e.g. the '{ }' body of
 * `a:for:(+i, +v, { ... });`), which couldn't be built during argument
 * marshalling because it may reference hygienic idents the macro itself
 * hadn't introduced yet at that point.
 *
 * Mirrors what yap_build_block does for ordinary parsed code -- pushes a
 * real scope on entering a nested block, and replays var_decl into the
 * current scope as it's walked past -- so that by the time a deferred
 * fragment is reached (however deeply nested inside macro-constructed
 * while/if/block statements), yap_ctx_current_scope(ctx) correctly reflects
 * every hygienic var the macro declared ahead of it, and yap_build_statement
 * can resolve ordinary identifier references in the deferred fragment
 * exactly as if it had been written directly at that position in the
 * source. Mutates the tree in place (overwrites each deferred sentinel with
 * its built replacement). */
static void yap_resolve_deferred_fragments(yap_source* src, yap_statement* stmt){
    yap_ctx* ctx = src->ctx;
    switch (stmt->kind){
        case yap_statement_var_decl:
            if (stmt->var_decl.kind == yap_var_decl_valid)
                yap_ctx_push_var(ctx, stmt->var_decl.var);
            break;
        case yap_statement_block: {
            yap_ctx_push_new_scope(ctx);
            for (darr_size_t i = 0; i < darr_len(stmt->block.statements); i++)
                yap_resolve_deferred_fragments(src, &stmt->block.statements[i]);
            yap_ctx_pop_scope(ctx);
            break;
        }
        case yap_statement_if:
            yap_resolve_deferred_fragments(src, stmt->if_stmt.then_branch);
            break;
        case yap_statement_if_else:
            yap_resolve_deferred_fragments(src, stmt->if_else_stmt.then_branch);
            yap_resolve_deferred_fragments(src, stmt->if_else_stmt.else_branch);
            break;
        case yap_statement_while:
            yap_resolve_deferred_fragments(src, stmt->while_stmt.body);
            break;
        case yap_statement_for:
            yap_resolve_deferred_fragments(src, stmt->for_stmt.body);
            break;
        case yap_statement_deferred: {
            yap_statement_node* raw = stmt->deferred_raw;
            yap_statement built = yap_build_statement(src, raw);
            *stmt = built;
            break;
        }
        default:
            break;
    }
}

yap_statement yap_build_empty_statement(yap_source* src){
    (void)src;
    return (yap_statement){ .kind = yap_statement_empty };
}

yap_statement yap_build_expr_statement(yap_source* src, yap_expr_node* expr_node){
    if (expr_node->kind == yap_expr_macro){
        yap_ctx* ctx = src->ctx;
        yap_type_id ret_type_id = 0;
        void* result = yap_exec_macro_call(src, &expr_node->macro_call, &ret_type_id);
        if (ret_type_id == ctx->void_type_id){
            yap_log("Macro executed (void return)");
            return (yap_statement){ .kind = yap_statement_empty };
        }
        if (!result){
            if (darr_len(ctx->errors) == 0)
                yap_build_push_error(src, expr_node->loc, "Macro returned NULL");
            return (yap_statement){ .kind = yap_statement_error };
        }
        if (ret_type_id == ctx->ystmt_type_id){
            yap_statement* expanded = (yap_statement*)result;
            yap_log("Macro expanded to statement (kind=%d)", expanded->kind);
            yap_statement ret = *expanded;
            if (ret.kind == yap_statement_var_decl && ret.var_decl.kind == yap_var_decl_valid){
                yap_ctx_push_var(ctx, ret.var_decl.var);
                yap_log("Macro introduced variable '%s' into scope", ret.var_decl.var.name);
            } else if (ret.kind == yap_statement_block && ret.block.kind == yap_block_valid){
                /* A macro-returned yStmt may be a flat block wrapping a
                 * var_decl alongside other statements (e.g. stmt${ }'s
                 * var_decl-with-initializer, desugared as declare-then-assign
                 * -- see bp_wrap_stmts_in_block in this file). Only the
                 * top-level statements of THIS block are in the caller's flat
                 * scope (a var_decl nested inside an if/while stays properly
                 * scoped to its own block, same as ordinary code), so this
                 * doesn't recurse into nested branches. */
                for_darr(i, st, ret.block.statements){
                    if (st.kind == yap_statement_var_decl && st.var_decl.kind == yap_var_decl_valid){
                        yap_ctx_push_var(ctx, st.var_decl.var);
                        yap_log("Macro introduced variable '%s' into scope (from block)", st.var_decl.var.name);
                    }
                }
            }
            /* Resolve any yap_statement_deferred sentinels (raw macro-arg
             * fragments, e.g. `for`'s body block) wherever they ended up
             * nested in the returned tree. This pushes its own scope(s) as
             * it walks -- redundant with, but harmless alongside, the
             * flat top-level registration just above: any var_decl it
             * re-registers lands in a scope chained *under* the real
             * ambient one already updated above, so lookups from inside a
             * deferred fragment still resolve correctly either way. */
            yap_resolve_deferred_fragments(src, &ret);
            return ret;
        }
        if (ret_type_id == ctx->yexpr_type_id){
            yap_expr* expanded = (yap_expr*)result;
            yap_expr ret_expr = *expanded;
            if (ret_expr.kind == yap_expr_var && ret_expr.var_name && !ret_expr.type){
                const yap_var* var = yap_scope_get_var_recursive(
                    yap_ctx_current_scope(ctx), ret_expr.var_name);
                if (var) ret_expr.type = var->type;
            }
            return (yap_statement){ .kind = yap_statement_expr, .expr = ret_expr };
        }
        if (ret_type_id == ctx->ytype_type_id){
            return (yap_statement){ .kind = yap_statement_empty };
        }
        yap_build_push_error(src, expr_node->loc, "Unsupported macro return type in statement position");
        return (yap_statement){ .kind = yap_statement_error };
    }
    yap_expr e = yap_build_expr(src, expr_node);
    if (e.kind == yap_expr_error)
        return (yap_statement){ .kind = yap_statement_error };
    return (yap_statement){ .kind = yap_statement_expr, .expr = e };
}

// Validates a literal's value against its target type's width (yap_ctx_type_id_assignable only checks kind, not magnitude).
static bool yap_check_literal_range(yap_source* src, yap_loc loc, yap_expr expr, yap_type_id target_type_id){
    yap_ctx* ctx = src->ctx;

    bool negated = false;
    if (expr.kind == yap_expr_unary && expr.unary_op == '-'){
        negated = true;
        expr = *expr.subexpr;
    }
    if (expr.kind != yap_expr_literal || expr.literal.kind != yap_literal_numerical)
        return true;

    yap_type* target = yap_ctx_get_type(ctx, yap_ctx_coerce_type_id_to_id(ctx, target_type_id));
    if (!target || target->kind != yap_type_primitive)
        return true;

    char* text = expr.literal.text;
    bool is_float_lit = strchr(text, '.') != NULL || strchr(text, 'e') != NULL || strchr(text, 'E') != NULL;

    if (target->primitive.is_float){
        double d = strtod(text, NULL);
        double max = target->primitive.bytes == 4 ? (double)FLT_MAX : DBL_MAX;
        if (d > max){
            yap_build_push_error(src, loc, "Numeric literal '%s%s' overflows type '%s'",
                negated ? "-" : "", text, target->primitive.name);
            return false;
        }
        return true;
    }

    if (is_float_lit){
        yap_build_push_error(src, loc, "Cannot use a fractional literal '%s%s' as integer type '%s'",
            negated ? "-" : "", text, target->primitive.name);
        return false;
    }

    unsigned long long mag = strtoull(text, NULL, 10);
    int bits = (int)(target->primitive.bytes * 8);

    if (!target->primitive.is_signed){
        if (negated){
            if (mag != 0){
                yap_build_push_error(src, loc, "Cannot assign negative literal '-%s' to unsigned type '%s'",
                    text, target->primitive.name);
                return false;
            }
            return true;
        }
        unsigned long long max = (bits >= 64) ? UINT64_MAX : ((1ULL << bits) - 1ULL);
        if (mag > max){
            yap_build_push_error(src, loc, "Numeric literal '%s' overflows type '%s'", text, target->primitive.name);
            return false;
        }
        return true;
    }

    unsigned long long max_pos = (1ULL << (bits - 1)) - 1ULL;
    unsigned long long limit = negated ? max_pos + 1ULL : max_pos;
    if (mag > limit){
        yap_build_push_error(src, loc, "Numeric literal '%s%s' overflows type '%s'",
            negated ? "-" : "", text, target->primitive.name);
        return false;
    }
    return true;
}

yap_statement yap_build_var_decl_statement(yap_source* src, yap_var_decl_node* vnode){
    yap_ctx* ctx = src->ctx;
    yap_var var = {0};
    yap_expr init = {0};
    bool has_init = vnode->has_init;

    if (!vnode->name.value){
        yap_build_push_error(src, vnode->loc, "Missing variable name");
        return (yap_statement){ .kind = yap_statement_error };
    }

    if (vnode->has_type && vnode->type_node){
        yap_type_id declared_type = yap_build_type_from_type_node(src, vnode->type_node);
        if (!declared_type){
            yap_build_push_error(src, vnode->loc,
                "Invalid type in variable declaration");
            return (yap_statement){ .kind = yap_statement_error };
        }
        var = (yap_var){ .name = vnode->name.value, .type = declared_type };
        if (vnode->has_init){
            init = yap_build_expr(src, &vnode->init);
            if (init.kind == yap_expr_error)
                return (yap_statement){ .kind = yap_statement_error };
            yap_type* init_t = yap_ctx_get_type(ctx, init.type);
            if (init_t && init_t->kind == yap_type_blob){
                init = yap_build_blob_cast(src, init, declared_type, vnode->loc);
                if (init.kind == yap_expr_error)
                    return (yap_statement){ .kind = yap_statement_error };
            } else if (!yap_ctx_type_id_assignable(ctx, declared_type, init.type)){
                char* rhs_str = yap_ctx_type_id_to_string(ctx, init.type);
                char* lhs_str = yap_ctx_type_id_to_string(ctx, declared_type);
                yap_build_push_error(src, vnode->loc,
                    "Cannot initialize variable of type '%s' with value of type '%s'",
                    lhs_str, rhs_str);
                free(rhs_str);
                free(lhs_str);
                return (yap_statement){ .kind = yap_statement_error };
            } else if (!yap_check_literal_range(src, vnode->loc, init, declared_type)){
                return (yap_statement){ .kind = yap_statement_error };
            }
        }
    } else if (vnode->has_init){
        init = yap_build_expr(src, &vnode->init);
        if (init.kind == yap_expr_error)
            return (yap_statement){ .kind = yap_statement_error };
        yap_type* init_t = yap_ctx_get_type(ctx, init.type);
        if (init_t && init_t->kind == yap_type_blob){
            yap_build_push_error(src, vnode->loc,
                "Cannot infer type of blob literal");
            return (yap_statement){ .kind = yap_statement_error };
        }
        /* Only untyped literals actually need coercion (yap_ctx_coerce_type
         * is a no-op pass-through for anything else) -- re-inserting an
         * already-concrete type's *copy* via insert_type_if_not_exists is
         * unnecessary for a struct/union/enum, and doesn't dedupe reliably
         * against the type's own existing entry (its structural-equality
         * check isn't what named types rely on -- those dedupe by name via
         * finish()), so a `_`-inferred var could end up with a *different*
         * type_id than the initializer's own, breaking exact-type_id
         * comparisons downstream (e.g. yapi->register_macro_method's
         * receiver lookup). Keep init.type as-is unless coercion is
         * actually meaningful. */
        yap_type_id var_type_id = init.type;
        if (init_t->kind == yap_type_untyped)
            var_type_id = yap_ctx_insert_type_if_not_exists(ctx, yap_ctx_coerce_type(ctx, *init_t));
        var = (yap_var){
            .name = vnode->name.value,
            .type = var_type_id
        };
    } else {
        yap_build_push_error(src, vnode->loc,
            "Variable '%s' has no type and no initializer", vnode->name.value);
        return (yap_statement){ .kind = yap_statement_error };
    }

    yap_ctx_push_var(ctx, var);
    return (yap_statement){
        .kind     = yap_statement_var_decl,
        .var_decl = (yap_var_decl){
            .kind     = yap_var_decl_valid,
            .var      = var,
            .has_init = has_init,
            .init     = init
        }
    };
}

yap_statement yap_build_return_statement(yap_source* src, yap_return_statement_node* rnode){
    yap_ctx* ctx = src->ctx;

    if (!rnode->has_value){
        return (yap_statement){
            .kind        = yap_statement_return,
            .return_stmt = (yap_return_statement){
                .value = (yap_expr){ .type = ctx->void_type_id }
            }
        };
    }

    yap_expr val = yap_build_expr(src, &rnode->value);
    if (val.kind == yap_expr_error)
        return (yap_statement){ .kind = yap_statement_error };

    return (yap_statement){
        .kind        = yap_statement_return,
        .return_stmt = (yap_return_statement){ .value = val }
    };
}

yap_statement yap_build_if_statement(yap_source* src, yap_if_node* inode){
    yap_ctx* ctx = src->ctx;

    yap_expr cond = yap_build_expr(src, &inode->condition);
    if (cond.kind == yap_expr_error)
        return (yap_statement){ .kind = yap_statement_error };

    yap_statement then_branch = yap_build_statement(src, inode->then_branch);
    if (then_branch.kind == yap_statement_error)
        return (yap_statement){ .kind = yap_statement_error };

    return (yap_statement){
        .kind    = yap_statement_if,
        .if_stmt = (yap_if){
            .condition   = cond,
            .then_branch = yap_ctx_one_cpy(ctx, then_branch)
        }
    };
}

yap_statement yap_build_if_else_statement(yap_source* src, yap_if_else_node* inode){
    yap_ctx* ctx = src->ctx;

    yap_expr cond = yap_build_expr(src, &inode->condition);
    if (cond.kind == yap_expr_error)
        return (yap_statement){ .kind = yap_statement_error };

    yap_statement then_branch = yap_build_statement(src, inode->then_branch);
    if (then_branch.kind == yap_statement_error)
        return (yap_statement){ .kind = yap_statement_error };

    yap_statement else_branch = yap_build_statement(src, inode->else_branch);
    if (else_branch.kind == yap_statement_error)
        return (yap_statement){ .kind = yap_statement_error };

    return (yap_statement){
        .kind         = yap_statement_if_else,
        .if_else_stmt = (yap_if_else){
            .condition   = cond,
            .then_branch = yap_ctx_one_cpy(ctx, then_branch),
            .else_branch = yap_ctx_one_cpy(ctx, else_branch)
        }
    };
}

yap_statement yap_build_while_statement(yap_source* src, yap_while_node* wnode){
    yap_ctx* ctx = src->ctx;

    yap_ctx_push_new_loop_scope(ctx);

    yap_expr cond = yap_build_expr(src, &wnode->condition);
    if (cond.kind == yap_expr_error){ yap_ctx_pop_scope(ctx); return (yap_statement){ .kind = yap_statement_error }; }

    yap_statement body = yap_build_statement(src, wnode->body);
    if (body.kind == yap_statement_error){ yap_ctx_pop_scope(ctx); return (yap_statement){ .kind = yap_statement_error }; }

    yap_ctx_pop_scope(ctx);

    return (yap_statement){
        .kind       = yap_statement_while,
        .while_stmt = (yap_while){
            .condition = cond,
            .body      = yap_ctx_one_cpy(ctx, body)
        }
    };
}

yap_statement yap_build_for_statement(yap_source* src, yap_for_node* fnode){
    yap_ctx* ctx = src->ctx;

    yap_ctx_push_new_loop_scope(ctx);

    yap_statement init = yap_build_statement(src, fnode->init);
    if (init.kind == yap_statement_error){ yap_ctx_pop_scope(ctx); return (yap_statement){ .kind = yap_statement_error }; }

    yap_expr cond = yap_build_expr(src, &fnode->condition);
    if (cond.kind == yap_expr_error){ yap_ctx_pop_scope(ctx); return (yap_statement){ .kind = yap_statement_error }; }

    yap_expr update = yap_build_expr(src, &fnode->update);
    if (update.kind == yap_expr_error){ yap_ctx_pop_scope(ctx); return (yap_statement){ .kind = yap_statement_error }; }

    yap_statement body = yap_build_statement(src, fnode->body);
    if (body.kind == yap_statement_error){ yap_ctx_pop_scope(ctx); return (yap_statement){ .kind = yap_statement_error }; }

    yap_ctx_pop_scope(ctx);

    return (yap_statement){
        .kind     = yap_statement_for,
        .for_stmt = (yap_for){
            .init      = yap_ctx_one_cpy(ctx, init),
            .condition = cond,
            .update    = update,
            .body      = yap_ctx_one_cpy(ctx, body)
        }
    };
}

yap_statement yap_build_break_statement(yap_source* src, yap_statement_node* node){
    yap_ctx* ctx = src->ctx;
    if (!yap_scope_in_loop(yap_ctx_current_scope(ctx))){
        yap_build_push_error(src, node->loc, "Break outside of loop");
        return (yap_statement){ .kind = yap_statement_error };
    }
    return (yap_statement){ .kind = yap_statement_break };
}

yap_statement yap_build_continue_statement(yap_source* src, yap_statement_node* node){
    yap_ctx* ctx = src->ctx;
    if (!yap_scope_in_loop(yap_ctx_current_scope(ctx))){
        yap_build_push_error(src, node->loc, "Continue outside of loop");
        return (yap_statement){ .kind = yap_statement_error };
    }
    return (yap_statement){ .kind = yap_statement_continue };
}

yap_statement yap_build_block_statement(yap_source* src, yap_block_node* bnode){
    yap_block block = yap_build_block(src, bnode);
    if (block.kind == yap_block_error)
        return (yap_statement){ .kind = yap_statement_error };
    return (yap_statement){ .kind = yap_statement_block, .block = block };
}

/* ----------------------------------------------------------------
 *  Blueprints ; the $(...) quasi-quote literal
 *
 *  A blueprint is pure sugar over the yapi-> builder API: $(...) is rewritten
 *  into ordinary yapi->hole/int/bin_op/... calls (parse nodes) that are built
 *  normally, so they codegen into the enclosing macro's body and run under TCC
 *  like any hand-written builder call. The result is stamped yExprBlueprint
 *  (which shares a C representation with yExpr ; both are yap_expr*), so the
 *  only front-end bridging is this one type override.
 *
 *  Filling and finishing are NOT handled here: they are ordinary methods on
 *  yExprBlueprint (yExprBlueprint_fill / yExprBlueprint_finish, registered in
 *  ctx.c, implemented in build_state.c) dispatched through the normal
 *  obj:method(args) path ; e.g. $($x+1):fill(c"x", a):finish().
 * ---------------------------------------------------------------- */

// yapi->NAME(args...) as a parse node.
static yap_expr_node bp_yapi_call(yap_source* src, const char* name,
                                  yap_expr_node* args, int argc, yap_loc loc){
    yap_ctx* ctx = src->ctx;
    yap_expr_node callee = { .kind = yap_expr_module_access,
        .module_access = { .module = { .value = (char*)"yapi", .loc = loc },
                           .field  = { .value = (char*)name,   .loc = loc },
                           .loc = loc }, .loc = loc };
    darr(yap_call_arg_node) argd = yap_ctx_darr_new(ctx, yap_call_arg_node, .cap = argc, .len = 0);
    for (int i = 0; i < argc; i++)
        darr_push(argd, ((yap_call_arg_node){ .is_named = false, .value = yap_ctx_one_cpy(ctx, args[i]), .loc = loc }));
    return (yap_expr_node){ .kind = yap_expr_func_call,
        .func_call = { .func = yap_ctx_one_cpy(ctx, callee), .args = argd, .loc = loc },
        .loc = loc };
}

// A cstring literal parse node (used for the byte@ name args of hole/fill/var_value).
static yap_expr_node bp_cstr_node(yap_source* src, char* s, yap_loc loc){
    return (yap_expr_node){ .kind = yap_expr_literal,
        .literal = { .kind = yap_literal_cstring, .string = { .value = s, .loc = loc }, .loc = loc },
        .loc = loc };
}

// A decimal integer literal parse node.
static yap_expr_node bp_int_node(yap_source* src, long v, yap_loc loc){
    yap_ctx* ctx = src->ctx;
    return (yap_expr_node){ .kind = yap_expr_literal,
        .literal = { .kind = yap_literal_numerical, .numerical = yap_ctx_strus_newf(ctx, "%ld", v), .loc = loc },
        .loc = loc };
}

// `eager`: type${}/fn$ pass true ($T -> bare var-ref, resolved immediately --
// unchanged, Model A stays eager-only there); the new lazy var_decl path
// (stmt${ }) and casts reached from a lazy expr${ }/stmt${ } template pass
// false ($T -> yapi->type_hole(c"T"), closed later via :fill_type()).
static yap_expr_node bp_type_to_yexpr(yap_source* src, yap_type_node* t, bool* ok, bool eager); // fwd (used by cast)
static yap_expr_node bp_build_type_body_chain(yap_source* src, yap_type_node* body, bool* ok, bool eager); // fwd (mutually recursive w/ bp_type_to_yexpr, for anon nested fields)

// Rewrite a blueprint template expr into the yapi-> builder call that rebuilds
// it. Supports literals, variable refs, holes, parens, unary minus, ternary,
// binary arithmetic + comparisons, and (added for stmt${ }/fn$ bodies)
// assignment, member/index, deref, address-of, cast, and calls (<=3 args).
// `eager`: expr${ }/stmt${ } pass false ($name -> yapi->hole(...), a lazy hole
// closed later via :fill_expr()); fn$ passes true for its body ($name -> a
// bare var-ref to the in-scope comptime value, spliced immediately ; fn$ has
// no fill methods at all, so a hole here could never be closed).
static yap_expr_node bp_desugar_template(yap_source* src, yap_expr_node* t, bool* ok, bool eager){
    yap_ctx* ctx = src->ctx;
    yap_loc loc = t->loc;
    switch (t->kind){
        case yap_expr_blueprint_hole: {
            if (eager)
                return (yap_expr_node){ .kind = yap_expr_var,
                    .var = { .value = yap_ctx_strus_cpy(ctx, t->blueprint_hole.name.value), .loc = loc }, .loc = loc };
            yap_expr_node arg = bp_cstr_node(src, yap_ctx_strus_cpy(ctx, t->blueprint_hole.name.value), loc);
            return bp_yapi_call(src, "hole", &arg, 1, loc);
        }
        case yap_expr_var: {
            yap_expr_node arg = bp_cstr_node(src, yap_ctx_strus_cpy(ctx, t->var.value), loc);
            return bp_yapi_call(src, "var_value", &arg, 1, loc);
        }
        case yap_expr_paren:
            return bp_desugar_template(src, t->paren.expr, ok, eager);
        case yap_expr_unary: {
            // prefix '-' (neg), '!' (not), '~' (bnot)
            yap_expr_node inner = bp_desugar_template(src, t->unary.expr, ok, eager);
            const char* builder;
            switch (t->unary.op){
                case '!': builder = "not";  break;
                case '~': builder = "bnot"; break;
                default:  builder = "neg";  break;
            }
            return bp_yapi_call(src, builder, &inner, 1, loc);
        }
        case yap_expr_ternary: {
            yap_expr_node c  = bp_desugar_template(src, t->ternary.condition, ok, eager);
            yap_expr_node th = bp_desugar_template(src, t->ternary.then_expr, ok, eager);
            yap_expr_node el = bp_desugar_template(src, t->ternary.else_expr, ok, eager);
            yap_expr_node args[3] = { c, th, el };
            return bp_yapi_call(src, "ternary", args, 3, loc);
        }
        case yap_expr_literal: {
            yap_expr_node arg = *t;
            switch (t->literal.kind){
                case yap_literal_numerical: {
                    bool is_float = strchr(t->literal.numerical, '.')
                        || strchr(t->literal.numerical, 'e') || strchr(t->literal.numerical, 'E');
                    return bp_yapi_call(src, is_float ? "float" : "int", &arg, 1, loc);
                }
                case yap_literal_string:  return bp_yapi_call(src, "string", &arg, 1, loc);
                case yap_literal_bool:    return bp_yapi_call(src, "bool",   &arg, 1, loc);
                default:
                    *ok = false;
                    yap_build_push_error(src, loc, "unsupported literal kind in blueprint (first cut supports numbers, strings, bools)");
                    return arg;
            }
        }
        case yap_expr_bin: {
            // The parse-AST op is a single char: arithmetic ops keep their ASCII
            // value (which equals the semtree op enum), while comparisons were
            // remapped by the parser (== -> 'e', != -> 'n', <= -> 'l', >= -> 'g',
            // < -> '<', > -> '>'). yapi->bin_op wants the *semtree* op enum, so
            // translate here.
            int sem_op;
            switch (t->bin.op){
                case '+': sem_op = yap_bin_expr_add; break;
                case '-': sem_op = yap_bin_expr_sub; break;
                case '*': sem_op = yap_bin_expr_mul; break;
                case '/': sem_op = yap_bin_expr_div; break;
                case '%': sem_op = yap_bin_expr_mod; break;
                case 'e': sem_op = yap_bin_expr_eq;  break;
                case 'n': sem_op = yap_bin_expr_neq; break;
                case '<': sem_op = yap_bin_expr_lt;  break;
                case '>': sem_op = yap_bin_expr_gt;  break;
                case 'l': sem_op = yap_bin_expr_le;  break;
                case 'g': sem_op = yap_bin_expr_ge;  break;
                case '&': sem_op = yap_bin_expr_band; break;
                case '|': sem_op = yap_bin_expr_bor;  break;
                case '^': sem_op = yap_bin_expr_bxor; break;
                case 'a': sem_op = yap_bin_expr_and;  break;
                case 'o': sem_op = yap_bin_expr_or;   break;
                case 'L': sem_op = yap_bin_expr_shl;  break;
                case 'R': sem_op = yap_bin_expr_shr;  break;
                default:
                    *ok = false;
                    yap_build_push_error(src, loc, "unsupported binary operator in blueprint");
                    return *t;
            }
            yap_expr_node l = bp_desugar_template(src, t->bin.left, ok, eager);
            yap_expr_node r = bp_desugar_template(src, t->bin.right, ok, eager);
            // yapi->bin_op(left, op, right) ; op is the middle (int) argument
            yap_expr_node args[3] = { l, bp_int_node(src, (long)sem_op, loc), r };
            return bp_yapi_call(src, "bin_op", args, 3, loc);
        }
        case yap_expr_assignment: {
            // op is a string like "=", "+=", "*="; op[0] is what yapi->assign wants
            // ('=' means plain assign; '+' means +=, etc. ; the builder appends '=').
            yap_expr_node l = bp_desugar_template(src, t->assignment.left, ok, eager);
            yap_expr_node r = bp_desugar_template(src, t->assignment.right, ok, eager);
            yap_expr_node args[3] = { l, bp_int_node(src, (long)t->assignment.op[0], loc), r };
            return bp_yapi_call(src, "assign", args, 3, loc);
        }
        case yap_expr_member_access: {
            yap_expr_node obj = bp_desugar_template(src, t->member_access.object, ok, eager);
            yap_expr_node fld = bp_cstr_node(src, yap_ctx_strus_cpy(ctx, t->member_access.member.value), loc);
            yap_expr_node args[2] = { obj, fld };
            return bp_yapi_call(src, "member", args, 2, loc);
        }
        case yap_expr_index_access: {
            yap_expr_node obj = bp_desugar_template(src, t->index_access.object, ok, eager);
            yap_expr_node idx = bp_desugar_template(src, t->index_access.index, ok, eager);
            yap_expr_node args[2] = { obj, idx };
            return bp_yapi_call(src, "index", args, 2, loc);
        }
        case yap_expr_deref: {
            yap_expr_node inner = bp_desugar_template(src, t->deref.expr, ok, eager);
            return bp_yapi_call(src, "deref", &inner, 1, loc);
        }
        case yap_expr_at_op: {
            yap_expr_node inner = bp_desugar_template(src, t->at_op.expr, ok, eager);
            return bp_yapi_call(src, "addr_of", &inner, 1, loc);
        }
        case yap_expr_cast: {
            yap_expr_node inner = bp_desugar_template(src, t->cast.expr, ok, eager);
            // A cast's $T used to always eagerly splice regardless of the
            // enclosing template's own eagerness -- a shortcut from before
            // type holes existed. Now it follows the ambient `eager` like
            // everything else: lazy inside expr${ }/stmt${ } (closed later via
            // :fill_type()), still eager inside fn$ bodies.
            yap_expr_node ty    = bp_type_to_yexpr(src, t->cast.type_node, ok, eager);
            yap_expr_node args[2] = { inner, ty };
            return bp_yapi_call(src, "cast", args, 2, loc);
        }
        case yap_expr_func_call: {
            int argc = (int)darr_len(t->func_call.args);
            if (argc > 3){
                *ok = false;
                yap_build_push_error(src, loc, "call in blueprint supports up to 3 args (yapi->call0..3)");
                return *t;
            }
            for_darr(idx, a, t->func_call.args){
                if (a.is_named){
                    *ok = false;
                    yap_build_push_error(src, loc, "named arguments are not supported inside an expression blueprint");
                    return *t;
                }
            }
            yap_expr_node cargs[4];
            cargs[0] = bp_desugar_template(src, t->func_call.func, ok, eager);
            int ci = 0;
            for_darr(idx, a, t->func_call.args){ cargs[1 + ci] = bp_desugar_template(src, a.value, ok, eager); ci++; }
            const char* callname = argc == 0 ? "call0" : argc == 1 ? "call1" : argc == 2 ? "call2" : "call3";
            return bp_yapi_call(src, callname, cargs, argc + 1, loc);
        }
        default:
            *ok = false;
            yap_build_push_error(src, loc, "unsupported expression inside blueprint");
            return *t;
    }
}

static yap_expr yap_build_blueprint_expr(yap_source* src, yap_blueprint_node* bp){
    yap_ctx* ctx = src->ctx;
    if (!bp->template){
        yap_build_push_error(src, bp->loc, "empty blueprint");
        return (yap_expr){ .kind = yap_expr_error };
    }
    bool ok = true;
    yap_expr_node desugared = bp_desugar_template(src, bp->template, &ok, false);
    if (!ok) return (yap_expr){ .kind = yap_expr_error };
    yap_expr built = yap_build_expr(src, &desugared);
    if (built.kind == yap_expr_error) return built;
    built.type = ctx->yexprblueprint_type_id; // a template, not yet a usable yExpr
    return built;
}

/* ----------------------------------------------------------------
 *  Type blueprints ; the eager type${ struct/enum/union {...} } quasi-quote
 *
 *  Model A: sugar over the *construction* phase only. The body desugars into a
 *  chained yapi->struct_t()/union_t()/enum_t() + add_field/add_variant call that
 *  evaluates to a yStructT/yEnumT/yUnionT template ; you then :finish("name") it
 *  yourself (naming/hash/dedup/existed/methods all stay on the existing API).
 *  Eager: $T in a field/variant type splices the in-scope comptime yType now.
 * ---------------------------------------------------------------- */

// base:method(args...) as a parse node (one link of a builder method chain).
static yap_expr_node bp_method_call(yap_source* src, yap_expr_node base, const char* method,
                                    yap_expr_node* args, int argc, yap_loc loc){
    yap_ctx* ctx = src->ctx;
    yap_expr_node callee = { .kind = yap_expr_method_access,
        .method_access = { .caller = yap_ctx_one_cpy(ctx, base),
                           .name = { .value = (char*)method, .loc = loc }, .loc = loc }, .loc = loc };
    darr(yap_call_arg_node) argd = yap_ctx_darr_new(ctx, yap_call_arg_node, .cap = argc, .len = 0);
    for (int i = 0; i < argc; i++)
        darr_push(argd, ((yap_call_arg_node){ .is_named = false, .value = yap_ctx_one_cpy(ctx, args[i]), .loc = loc }));
    return (yap_expr_node){ .kind = yap_expr_func_call,
        .func_call = { .func = yap_ctx_one_cpy(ctx, callee), .args = argd, .loc = loc },
        .loc = loc };
}

// v1 guard: a lazy ($T when eager=false) type hole may only appear as the
// direct, unwrapped type of a var_decl -- not nested inside a pointer/slice/
// anon-struct-or-union field. Wrapping would bake the CURRENT (unfilled) hole
// type_id into a brand-new aggregate type_id right now, at construction time;
// there's no "rebuild the wrapper once the hole is later filled" pass (real
// but separable follow-on work), so allowing it today would silently produce
// a wrapper that points at the stale unfilled hole forever, even after a
// later :fill_type() call. Only checks the immediate child -- deeper nesting
// (a pointer to a pointer to a hole, a struct field of pointer-to-hole type,
// etc.) is still caught because each wrapping level makes its own recursive
// bp_type_to_yexpr call, which re-runs this same check on its own immediate
// child.
static bool bp_reject_wrapped_lazy_hole(yap_source* src, yap_type_node* subtype, bool eager, yap_loc loc){
    if (eager || !subtype || subtype->kind != yap_type_node_blueprint_hole) return false;
    yap_build_push_error(src, loc,
        "a lazy type hole can't be wrapped in a pointer/slice/anon type yet ; only a bare $T as the direct type");
    return true;
}

// Chained yapi->struct_t()/union_t()/enum_t() + add_field/add_variant call for
// an anonymous struct/union/enum body (no trailing :finish() -- the top-level
// type${ } caller appends its own; nested anon field types (bp_type_to_yexpr)
// append a compiler-generated :finish() name instead, same as normal
// (non-blueprint) anonymous nested types do via ctx->anon_id). `eager`: see
// bp_type_to_yexpr below -- type${ }'s own top-level call always passes true
// (unchanged); a recursive call from bp_type_to_yexpr's anon-nested case
// forwards whatever eagerness it itself received.
static yap_expr_node bp_build_type_body_chain(yap_source* src, yap_type_node* body, bool* ok, bool eager){
    yap_ctx* ctx = src->ctx;
    yap_loc loc = body->loc;
    yap_expr_node chain;
    switch (body->kind){
        case yap_type_node_anon_struct:
            chain = bp_yapi_call(src, "struct_t", NULL, 0, loc);
            for_darr(i, f, body->anon_struct.fields){
                if (bp_reject_wrapped_lazy_hole(src, f.type_node, eager, loc)){
                    *ok = false;
                    return (yap_expr_node){ .kind = yap_expr_error, .loc = loc };
                }
                yap_expr_node args[2] = { bp_type_to_yexpr(src, f.type_node, ok, eager),
                                          bp_cstr_node(src, yap_ctx_strus_cpy(ctx, f.name.value), loc) };
                chain = bp_method_call(src, chain, "add_field", args, 2, loc);
            }
            break;
        case yap_type_node_anon_union:
            chain = bp_yapi_call(src, "union_t", NULL, 0, loc);
            for_darr(i, f, body->anon_union.variants){
                if (bp_reject_wrapped_lazy_hole(src, f.type_node, eager, loc)){
                    *ok = false;
                    return (yap_expr_node){ .kind = yap_expr_error, .loc = loc };
                }
                yap_expr_node args[2] = { bp_type_to_yexpr(src, f.type_node, ok, eager),
                                          bp_cstr_node(src, yap_ctx_strus_cpy(ctx, f.name.value), loc) };
                chain = bp_method_call(src, chain, "add_field", args, 2, loc);
            }
            break;
        case yap_type_node_anon_enum:
            chain = bp_yapi_call(src, "enum_t", NULL, 0, loc);
            for_darr(i, v, body->anon_enum.variants){
                yap_expr_node nm = bp_cstr_node(src, yap_ctx_strus_cpy(ctx, v.name.value), loc);
                chain = bp_method_call(src, chain, "add_variant", &nm, 1, loc);
            }
            break;
        default:
            *ok = false;
            yap_build_push_error(src, loc, "type blueprint body must be an anonymous struct/enum/union");
            return (yap_expr_node){ .kind = yap_expr_error, .loc = loc };
    }
    return chain;
}

// Recursively desugar a field/variant type node into the yExpr that yields its
// yType at comptime. $T (hole): eager -> bare splice of the in-scope comptime
// yType (type${}/fn$, unchanged); lazy -> yapi->type_hole(c"T") (stmt${ }'s new
// var_decl path, and casts reached from a lazy expr${ }/stmt${ } template).
// A named type -> yapi->type(c"name"); pointer/slice -> yapi->ptr_of/slice_of
// wrapping the recursively-resolved element type (rejecting a lazy hole
// directly underneath, see bp_reject_wrapped_lazy_hole); an anon nested
// struct/union/enum -> the same struct_t/union_t/enum_t chain as a top-level
// type${ } body, closed with a compiler-generated :finish() name. Deliberately
// NOT yap_ctx_get_anon_name's "__anon_*" convention: that name is a marker
// normal (non-blueprint) anonymous nested types rely on to mean "embed inline
// at the use site, never emit a standalone declaration" (yap_gen_type_decl
// skips any type whose name starts with "__" as C-reserved) -- Model A's
// blueprint types are always independently finish()ed/nameable/dedup'd, the
// opposite of that, so a "__"-prefixed name here would silently vanish from
// codegen while still being referenced by field type, producing an
// undeclared-type compile error. Const/array/function/macro field types
// remain an extension point (clear error).
static yap_expr_node bp_type_to_yexpr(yap_source* src, yap_type_node* t, bool* ok, bool eager){
    yap_ctx* ctx = src->ctx;
    yap_loc loc = t ? t->loc : (yap_loc){0};
    if (!t){ *ok = false; return (yap_expr_node){ .kind = yap_expr_error, .loc = loc }; }
    switch (t->kind){
        case yap_type_node_blueprint_hole: {
            if (eager)
                return (yap_expr_node){ .kind = yap_expr_var,
                    .var = { .value = yap_ctx_strus_cpy(ctx, t->identifier.value), .loc = loc }, .loc = loc };
            yap_expr_node arg = bp_cstr_node(src, yap_ctx_strus_cpy(ctx, t->identifier.value), loc);
            return bp_yapi_call(src, "type_hole", &arg, 1, loc);
        }
        case yap_type_node_identifier: {
            yap_expr_node arg = bp_cstr_node(src, yap_ctx_strus_cpy(ctx, t->identifier.value), loc);
            return bp_yapi_call(src, "type", &arg, 1, loc);
        }
        case yap_type_node_pointer: {
            if (bp_reject_wrapped_lazy_hole(src, t->pointer_subtype, eager, loc)){
                *ok = false;
                return (yap_expr_node){ .kind = yap_expr_error, .loc = loc };
            }
            yap_expr_node inner = bp_type_to_yexpr(src, t->pointer_subtype, ok, eager);
            return bp_yapi_call(src, "ptr_of", &inner, 1, loc);
        }
        case yap_type_node_slice: {
            if (bp_reject_wrapped_lazy_hole(src, t->slice_subtype, eager, loc)){
                *ok = false;
                return (yap_expr_node){ .kind = yap_expr_error, .loc = loc };
            }
            yap_expr_node inner = bp_type_to_yexpr(src, t->slice_subtype, ok, eager);
            return bp_yapi_call(src, "slice_of", &inner, 1, loc);
        }
        case yap_type_node_anon_struct:
        case yap_type_node_anon_union:
        case yap_type_node_anon_enum: {
            yap_expr_node chain = bp_build_type_body_chain(src, t, ok, eager);
            const char* kind_str = t->kind == yap_type_node_anon_struct ? "struct"
                                  : t->kind == yap_type_node_anon_union ? "union" : "enum";
            char* name = yap_ctx_strus_newf(ctx, "bp_anon_%s_%lu", kind_str, (unsigned long)ctx->anon_id++);
            yap_expr_node nm = bp_cstr_node(src, name, loc);
            return bp_method_call(src, chain, "finish", &nm, 1, loc);
        }
        default:
            *ok = false;
            yap_build_push_error(src, loc, "unsupported field type in type blueprint (first cut: identifiers, $holes, pointers, slices, anon struct/union/enum; const/array/function types are an extension point)");
            return (yap_expr_node){ .kind = yap_expr_error, .loc = loc };
    }
}

static yap_expr yap_build_type_blueprint(yap_source* src, yap_type_blueprint_node* tb){
    yap_loc loc = tb->loc;
    yap_type_node* body = tb->body;
    if (!body){
        yap_build_push_error(src, loc, "empty type blueprint");
        return (yap_expr){ .kind = yap_expr_error };
    }
    bool ok = true;
    yap_expr_node chain = bp_build_type_body_chain(src, body, &ok, /*eager=*/true); // type${ } stays eager-only, unchanged
    if (!ok) return (yap_expr){ .kind = yap_expr_error };
    return yap_build_expr(src, &chain); // chain types as yStructT/yEnumT/yUnionT via chainable builders
}

/* ----------------------------------------------------------------
 *  Statement blueprints ; the lazy stmt${ ...stmts... } quasi-quote
 *
 *  Each statement desugars to a yapi->expr_stmt/return_stmt/if_stmt/... builder
 *  call (exprs go through bp_desugar_template, so $holes become yapi->hole).
 *  A sequence >1 is wrapped in yapi->block(stmt_list). The result is stamped
 *  yStmtBlueprint; :fill_expr(...)/:finish() (build_state.c) clone + close it.
 * ---------------------------------------------------------------- */
static yap_expr_node bp_desugar_stmt(yap_source* src, yap_statement_node* s, bool* ok, bool eager); // fwd

// $name in identifier position (currently: a var_decl's name). Same eager/lazy
// split as $T (bp_type_to_yexpr) and bare $body (bp_desugar_stmt's expr-hole
// case): eager -> bare var-ref splice of an in-scope comptime yIdent value;
// lazy -> yapi->ident_hole(c"name"), a real hole closed later via :fill_ident().
static yap_expr_node bp_ident_hole_or_splice(yap_source* src, char* name, bool eager, yap_loc loc){
    if (eager)
        return (yap_expr_node){ .kind = yap_expr_var,
            .var = { .value = yap_ctx_strus_cpy(src->ctx, name), .loc = loc }, .loc = loc };
    yap_expr_node arg = bp_cstr_node(src, yap_ctx_strus_cpy(src->ctx, name), loc);
    return bp_yapi_call(src, "ident_hole", &arg, 1, loc);
}

// Combine already-desugared "build a yStmt" expr nodes into one via
// yapi->stmt_list_new()+push...+block(...) -- the same shape bp_desugar_stmt_seq
// below uses for a multi-statement template body, but starting from nodes
// that are already built (used by the var_decl case in bp_desugar_stmt, which
// always desugars to exactly two statements: the declaration, then a second
// statement assigning the initializer).
static yap_expr_node bp_wrap_stmts_in_block(yap_source* src, yap_expr_node* stmts, int n, yap_loc loc){
    yap_expr_node list = bp_yapi_call(src, "stmt_list_new", NULL, 0, loc);
    for (int i = 0; i < n; i++){
        yap_expr_node args[2] = { list, stmts[i] };
        list = bp_yapi_call(src, "stmt_list_push", args, 2, loc);
    }
    return bp_yapi_call(src, "block", &list, 1, loc);
}

static yap_expr_node bp_desugar_stmt_seq(yap_source* src, darr(yap_statement_node) body, yap_loc loc, bool* ok, bool eager){
    unsigned int n = darr_len(body);
    if (n == 0){
        *ok = false;
        yap_build_push_error(src, loc, "empty statement sequence in blueprint");
        return (yap_expr_node){ .kind = yap_expr_error, .loc = loc };
    }
    if (n == 1) return bp_desugar_stmt(src, &body[0], ok, eager);
    yap_expr_node list = bp_yapi_call(src, "stmt_list_new", NULL, 0, loc); // empty; push below
    for_darr(idx, st, body){
        yap_expr_node ds = bp_desugar_stmt(src, &st, ok, eager);
        yap_expr_node args[2] = { list, ds };
        list = bp_yapi_call(src, "stmt_list_push", args, 2, loc);
    }
    return bp_yapi_call(src, "block", &list, 1, loc);
}

// `eager`: see bp_desugar_template. A bare `$body;` statement in lazy mode is a
// statement hole (fill_stmt); in eager mode (fn$ bodies) it's a direct splice
// of the in-scope comptime yStmt value -- a bare var-ref used AS the "build a
// yStmt" expression, since that value already IS one (no hole, no fill needed).
static yap_expr_node bp_desugar_stmt(yap_source* src, yap_statement_node* s, bool* ok, bool eager){
    yap_loc loc = s->loc;
    switch (s->kind){
        case yap_statement_expr: {
            if (s->expr.kind == yap_expr_blueprint_hole){
                char* name = s->expr.blueprint_hole.name.value;
                if (eager)
                    return (yap_expr_node){ .kind = yap_expr_var,
                        .var = { .value = yap_ctx_strus_cpy(src->ctx, name), .loc = loc }, .loc = loc };
                yap_expr_node nm = bp_cstr_node(src, yap_ctx_strus_cpy(src->ctx, name), loc);
                return bp_yapi_call(src, "hole_stmt", &nm, 1, loc);
            }
            yap_expr_node e = bp_desugar_template(src, &s->expr, ok, eager);
            return bp_yapi_call(src, "expr_stmt", &e, 1, loc);
        }
        case yap_statement_return: {
            if (!s->return_stmt.has_value){
                *ok = false;
                yap_build_push_error(src, loc, "bare 'ret;' not supported in stmt blueprint (needs a value)");
                return (yap_expr_node){ .kind = yap_expr_error, .loc = loc };
            }
            yap_expr_node e = bp_desugar_template(src, &s->return_stmt.value, ok, eager);
            return bp_yapi_call(src, "return_stmt", &e, 1, loc);
        }
        case yap_statement_if: {
            yap_expr_node cond = bp_desugar_template(src, &s->if_stmt.condition, ok, eager);
            yap_expr_node then = bp_desugar_stmt(src, s->if_stmt.then_branch, ok, eager);
            yap_expr_node args[2] = { cond, then };
            return bp_yapi_call(src, "if_stmt", args, 2, loc);
        }
        case yap_statement_while: {
            yap_expr_node cond = bp_desugar_template(src, &s->while_stmt.condition, ok, eager);
            yap_expr_node body = bp_desugar_stmt(src, s->while_stmt.body, ok, eager);
            yap_expr_node args[2] = { cond, body };
            return bp_yapi_call(src, "while_stmt", args, 2, loc);
        }
        case yap_statement_block:
            return bp_desugar_stmt_seq(src, s->block.statements, loc, ok, eager);
        case yap_statement_var_decl: {
            if (!s->var_decl.has_type){
                *ok = false;
                yap_build_push_error(src, loc, "var_decl in a blueprint needs an explicit type (inferred '_' type is not supported yet)");
                return (yap_expr_node){ .kind = yap_expr_error, .loc = loc };
            }
            if (!s->var_decl.name.is_hole){
                *ok = false;
                yap_build_push_error(src, loc,
                    "var_decl name in a blueprint must be a $hole -- a literal name has no way to become a yIdent (identifiers can only come from +ident or yapi->uniq_name())");
                return (yap_expr_node){ .kind = yap_expr_error, .loc = loc };
            }
            char* name = s->var_decl.name.value;

            // yapi->var_decl(type, name) only *declares* the var (no init
            // param), so an initializer needs a second statement assigning
            // into it. Type and name both follow the ambient `eager` (same
            // split $T/$body already use): eager (fn$ bodies) -> bare splice
            // of an in-scope comptime value -- e.g. a $varname parameter of
            // the enclosing macro function, mirroring exactly how $bonus
            // already splices an outer yExpr into an fn$ body; lazy (stmt${ }/
            // expr${ }) -> a real hole (yapi->type_hole/ident_hole) closed
            // later via :fill_type()/:fill_ident(). Each of the two yapi calls
            // that need the type/name (var_decl below, and new_var if there's
            // an init) re-desugars them rather than reusing one shared parse
            // node in two places -- safe and correct because both
            // re-*construct* the same value: eager re-splices the same
            // in-scope var-ref, lazy re-constructs the same hole (type_hole
            // dedupes by name at intern time, see ct_make_type_hole; ident_hole
            // just produces the same "$name" string both times).
            yap_expr_node decl_ty = bp_type_to_yexpr(src, s->var_decl.type_node, ok, eager);
            yap_expr_node decl_nm = bp_ident_hole_or_splice(src, name, eager, loc);
            yap_expr_node decl_args[2] = { decl_ty, decl_nm };
            yap_expr_node decl_stmt = bp_yapi_call(src, "var_decl", decl_args, 2, loc);

            if (!s->var_decl.has_init) return decl_stmt;

            yap_expr_node init_ty = bp_type_to_yexpr(src, s->var_decl.type_node, ok, eager);
            yap_expr_node init_nm = bp_ident_hole_or_splice(src, name, eager, loc);
            yap_expr_node newvar_args[2] = { init_ty, init_nm };
            yap_expr_node newvar_call = bp_yapi_call(src, "new_var", newvar_args, 2, loc);

            yap_expr_node init_val = bp_desugar_template(src, &s->var_decl.init, ok, eager);
            yap_expr_node assign_args[3] = { newvar_call, bp_int_node(src, (long)'=', loc), init_val };
            yap_expr_node assign_expr = bp_yapi_call(src, "assign", assign_args, 3, loc);
            yap_expr_node assign_stmt = bp_yapi_call(src, "expr_stmt", &assign_expr, 1, loc);

            yap_expr_node both[2] = { decl_stmt, assign_stmt };
            return bp_wrap_stmts_in_block(src, both, 2, loc);
        }
        default:
            *ok = false;
            yap_build_push_error(src, loc, "unsupported statement in stmt blueprint (first cut: expression statements, 'ret expr;', if, while, blocks, var_decl)");
            return (yap_expr_node){ .kind = yap_expr_error, .loc = loc };
    }
}

static yap_expr yap_build_stmt_blueprint(yap_source* src, yap_stmt_blueprint_node* sb){
    yap_ctx* ctx = src->ctx;
    bool ok = true;
    yap_expr_node result = bp_desugar_stmt_seq(src, sb->body, sb->loc, &ok, false);
    if (!ok) return (yap_expr){ .kind = yap_expr_error };
    yap_expr built = yap_build_expr(src, &result);
    if (built.kind == yap_expr_error) return built;
    built.type = ctx->ystmtblueprint_type_id; // a template, not yet a usable yStmt
    return built;
}

/* ----------------------------------------------------------------
 *  Function blueprints ; the eager (RET fn$ params){body} quasi-quote
 *
 *  Model A: sugar over the yFnT construction phase, yielding a yFnT template you
 *  :finish("name") yourself. fn_t()/add_param/set_return_type/set_body aren't
 *  chainable (add_param returns a param yExpr), so we desugar to a block-expr:
 *    ({ _ __fnt = yapi->fn_t(); __fnt:set_return_type(R); __fnt:add_param(...);
 *       __fnt:set_body(<body block>); __fnt })
 *  whose value (the trailing __fnt ref) is the yFnT. $T in a param/return type
 *  eagerly splices the in-scope comptime yType (via bp_type_to_yexpr).
 * ---------------------------------------------------------------- */

// Always wrap a statement sequence in yapi->block(stmt_list) ; a fn body is a
// block. Always eager (true): fn$ bodies have no fill methods, so any $hole
// here must splice immediately, not defer to a fill that could never come.
static yap_expr_node bp_desugar_block(yap_source* src, darr(yap_statement_node) body, yap_loc loc, bool* ok){
    yap_expr_node list = bp_yapi_call(src, "stmt_list_new", NULL, 0, loc);
    for_darr(idx, st, body){
        yap_expr_node ds = bp_desugar_stmt(src, &st, ok, true);
        yap_expr_node args[2] = { list, ds };
        list = bp_yapi_call(src, "stmt_list_push", args, 2, loc);
    }
    return bp_yapi_call(src, "block", &list, 1, loc);
}

// small parse-node constructors for the block-expr desugar
static yap_expr_node bp_var_ref(yap_source* src, const char* name, yap_loc loc){
    (void)src;
    return (yap_expr_node){ .kind = yap_expr_var, .var = { .value = (char*)name, .loc = loc }, .loc = loc };
}
static yap_statement_node bp_expr_stmt_node(yap_source* src, yap_expr_node e, yap_loc loc){
    (void)src;
    return (yap_statement_node){ .kind = yap_statement_expr, .expr = e, .loc = loc };
}
static yap_statement_node bp_infer_var_decl_node(yap_source* src, const char* name, yap_expr_node init, yap_loc loc){
    (void)src;
    return (yap_statement_node){ .kind = yap_statement_var_decl,
        .var_decl = { .name = { .value = (char*)name, .loc = loc }, .has_type = false, .type_node = NULL,
                      .has_init = true, .init = init, .loc = loc },
        .loc = loc };
}

static yap_expr yap_build_fn_blueprint(yap_source* src, yap_func_literal_node* fb){
    yap_ctx* ctx = src->ctx;
    yap_loc loc = fb->loc;
    bool ok = true;
    const char* ftname = "__fnt";

    darr(yap_statement_node) stmts = yap_ctx_darr_new(ctx, yap_statement_node, .cap = darr_len(fb->args) + 4, .len = 0);
    // _ __fnt = yapi->fn_t();
    darr_push(stmts, bp_infer_var_decl_node(src, ftname, bp_yapi_call(src, "fn_t", NULL, 0, loc), loc));
    // __fnt:set_return_type(RET);
    if (fb->has_return_type){
        yap_expr_node rt   = bp_type_to_yexpr(src, fb->return_type_node, &ok, /*eager=*/true); // fn$ return type stays eager, unchanged
        yap_expr_node call = bp_method_call(src, bp_var_ref(src, ftname, loc), "set_return_type", &rt, 1, loc);
        darr_push(stmts, bp_expr_stmt_node(src, call, loc));
    }
    // __fnt:add_param(<type>, c"name"); per param
    for_darr(i, a, fb->args){
        if (!a.has_type){ ok = false; yap_build_push_error(src, loc, "fn blueprint parameter needs a type"); continue; }
        yap_expr_node args[2] = { bp_type_to_yexpr(src, a.type_node, &ok, /*eager=*/true), // fn$ param types stay eager, unchanged
                                  bp_cstr_node(src, yap_ctx_strus_cpy(ctx, a.name.value), loc) };
        yap_expr_node call = bp_method_call(src, bp_var_ref(src, ftname, loc), "add_param", args, 2, loc);
        darr_push(stmts, bp_expr_stmt_node(src, call, loc));
    }
    // __fnt:set_body(<body block>);
    yap_expr_node body   = bp_desugar_block(src, fb->body.statements, loc, &ok);
    yap_expr_node sbcall = bp_method_call(src, bp_var_ref(src, ftname, loc), "set_body", &body, 1, loc);
    darr_push(stmts, bp_expr_stmt_node(src, sbcall, loc));
    // trailing value: __fnt (the block-expr yields this ; a yFnT)
    darr_push(stmts, bp_expr_stmt_node(src, bp_var_ref(src, ftname, loc), loc));

    if (!ok) return (yap_expr){ .kind = yap_expr_error };
    yap_block_node blk = { .statements = stmts, .loc = loc };
    yap_expr_node blockexpr = { .kind = yap_expr_block, .block = blk, .loc = loc };
    return yap_build_expr(src, &blockexpr);
}

/* ----------------------------------------------------------------
 *  Expressions
 * ---------------------------------------------------------------- */

yap_expr yap_build_expr(yap_source* src, yap_expr_node* node){
    yap_expr ret = { .kind = yap_expr_error };

    switch (node->kind){
        case yap_expr_literal:       ret = yap_build_literal_expr(src, &node->literal);        break;
        case yap_expr_var:           ret = yap_build_var_access_expr(src, &node->var);          break;
        case yap_expr_bin:           ret = yap_build_bin_expr(src, &node->bin);                break;
        case yap_expr_unary:         ret = yap_build_unary_expr(src, &node->unary);            break;
        case yap_expr_assignment:    ret = yap_build_assignment_expr(src, &node->assignment);   break;
        case yap_expr_func_call:     ret = yap_build_func_call_expr(src, &node->func_call);     break;
        case yap_expr_cast:          ret = yap_build_cast_expr(src, &node->cast);              break;
        case yap_expr_at_op:         ret = yap_build_at_op_expr(src, &node->at_op);             break;
        case yap_expr_paren:         ret = yap_build_paren_expr(src, &node->paren);            break;
        case yap_expr_increment:     ret = yap_build_increment_expr(src, &node->increment); break;
        case yap_expr_decrement:     ret = yap_build_decrement_expr(src, &node->decrement); break;
        case yap_expr_ternary:       ret = yap_build_ternary_expr(src, &node->ternary);         break;
        case yap_expr_member_access: ret = yap_build_member_access_expr(src, &node->member_access); break;
        case yap_expr_optional_member_access: ret = yap_build_optional_member_access_expr(src, &node->member_access); break;
        case yap_expr_deref:         ret = yap_build_deref_expr(src, &node->deref);              break;
        case yap_expr_index_access:  ret = yap_build_index_access_expr(src, &node->index_access);  break;
        case yap_expr_block:         ret = yap_build_block_expr(src, &node->block);                break;
        case yap_expr_module_access: ret = yap_build_module_access_expr(src, &node->module_access); break;
        case yap_expr_macro:         ret = yap_build_macro_expr(src, &node->macro_call); break;
        case yap_expr_blueprint:     ret = yap_build_blueprint_expr(src, &node->blueprint); break;
        case yap_expr_type_blueprint: ret = yap_build_type_blueprint(src, &node->type_blueprint); break;
        case yap_expr_stmt_blueprint: ret = yap_build_stmt_blueprint(src, &node->stmt_blueprint); break;
        case yap_expr_fn_blueprint:   ret = yap_build_fn_blueprint(src, &node->fn_blueprint); break;
        case yap_expr_blueprint_hole:
            yap_build_push_error(src, node->loc,
                "blueprint hole '$%s' used outside a $(...) blueprint",
                node->blueprint_hole.name.value ? node->blueprint_hole.name.value : "?");
            ret = (yap_expr){ .kind = yap_expr_error };
            break;
        case yap_expr_method_access:
            yap_build_push_error(src, node->loc, "Method access must be called, e.g. 'obj:%s(args)'", node->method_access.name.value);
            break;
        default:
            yap_build_push_error(src, node->loc, "Unhandled expression kind");
            break;
    }
    ret.loc   = node->loc;
    ret.range = node->loc.range;
    return ret;
}

/* Function literal: desugar to a hoisted, static, top-level C function --
 * same shape as ct_func_finish (yap-c/build_state.c), but triggered from
 * ordinary expression building instead of a comptime yapi call. The literal
 * is capture-free by design: its body scope is parented to the *global*
 * scope, not the enclosing function's, so referencing an enclosing local is
 * an "Undefined variable" error at yap level rather than broken C output.
 * The expression's value is just a var reference to the hoisted name (C
 * auto-decays a function name to its pointer). */
static yap_expr yap_build_func_literal_expr(yap_source* src, yap_func_literal_node* fnode){
    yap_ctx* ctx = src->ctx;

    yap_type_id return_type = ctx->void_type_id;
    if (fnode->has_return_type && fnode->return_type_node){
        return_type = yap_build_type_from_type_node(src, fnode->return_type_node);
        if (!return_type){
            yap_build_push_error(src, fnode->return_type_node->loc,
                "Invalid return type in function literal");
            return (yap_expr){ .kind = yap_expr_error };
        }
    }

    darr(yap_func_arg) args = yap_ctx_darr_new(ctx, yap_func_arg,
        .cap = darr_len(fnode->args), .len = 0);
    darr(yap_type_id) arg_type_ids = yap_ctx_darr_new(ctx, yap_type_id,
        .cap = darr_len(fnode->args), .len = 0);
    for_darr(ai, arg_node, fnode->args){
        yap_func_arg arg = yap_build_func_arg(src, &arg_node);
        if (arg.kind != yap_func_arg_valid)
            return (yap_expr){ .kind = yap_expr_error };
        darr_push(args, arg);
        darr_push(arg_type_ids, arg.type);
    }

    yap_type func_type = {
        .kind     = yap_type_func,
        .func     = { .args = arg_type_ids, .return_type = return_type },
        .is_const = false
    };
    yap_type_id func_type_id = yap_ctx_insert_type_if_not_exists(ctx, func_type);

    char* emit_name = yap_ctx_get_anon_name(ctx, "func", ctx->anon_id++);
    yap_scope_set_var(ctx->global_scope, (yap_var){ .name = emit_name, .type = func_type_id });

    yap_scope* func_scope = yap_ctx_new_scope(ctx, ctx->global_scope);
    darr_push(ctx->current_scopes, func_scope);
    for_darr(ai, arg, args){
        yap_scope_set_var(func_scope, (yap_var){ .name = arg.name, .type = arg.type });
    }
    yap_block body = yap_build_block(src, &fnode->body);
    yap_ctx_pop_scope(ctx);
    if (body.kind == yap_block_error)
        return (yap_expr){ .kind = yap_expr_error };

    yap_decl decl = {
        .kind      = yap_decl_func_def,
        .func_decl = (yap_func_decl){
            .name    = emit_name,
            .args    = args,
            .ret_typ = return_type,
            .body    = body
        },
        .loc = fnode->loc,
        // Explicit, not left to fall back to ctx->current_module->prefix --
        // the emitted name is already unique and the call site references it
        // unprefixed (see ct_func_finish for the same reasoning).
        .module_prefix = "",
    };
    if (ctx->gen_decl)
        ctx->gen_decl(ctx, decl);
    yap_log("Hoisted function literal as '%s'", emit_name);

    return (yap_expr){
        .kind        = yap_expr_var,
        .var_name    = emit_name,
        .type        = func_type_id,
        .is_lvalue   = false,
        .is_comptime = false
    };
}

yap_expr yap_build_literal_expr(yap_source* src, yap_literal_node* lit){
    yap_ctx* ctx = src->ctx;
    yap_expr res = { .kind = yap_expr_literal, .is_comptime = true, .is_lvalue = false };

    switch (lit->kind){
        case yap_literal_numerical: {
            // hex/octal/binary are already normalized to decimal text, so a bare 'e' is always a decimal exponent
            bool is_float = strchr(lit->numerical, '.') != NULL
                || strchr(lit->numerical, 'e') != NULL
                || strchr(lit->numerical, 'E') != NULL;
            res.type = is_float ? ctx->untyped_float_type_id : ctx->untyped_int_type_id;
            res.literal = (yap_literal){ .kind = yap_literal_numerical, .text = lit->numerical };
            break;
        }
        case yap_literal_string: {
            yap_type_id byte_id = yap_ctx_get_type_id_by_name(ctx, "byte");
            yap_type slice_t = { .kind = yap_type_slice, .slice = { .element_type = byte_id }, .is_const = false };
            res.type = yap_ctx_insert_type_if_not_exists(ctx, slice_t);
            res.literal = (yap_literal){ .kind = yap_literal_string, .text = lit->string.value };
            break;
        }
        case yap_literal_cstring: {
            yap_type_id byte_id = yap_ctx_get_type_id_by_name(ctx, "byte");
            res.type = yap_ctx_get_pointer_of_type_id(ctx, byte_id);
            res.literal = (yap_literal){ .kind = yap_literal_cstring, .text = lit->string.value };
            break;
        }
        case yap_literal_bool:
            res.type = ctx->bool_type_id;
            res.literal = (yap_literal){ .kind = yap_literal_bool, .text = lit->numerical };
            break;
        case yap_literal_byte: {
            res.type = ctx->untyped_byte_type_id;
            res.literal = (yap_literal){ .kind = yap_literal_byte, .text = lit->numerical };
            break;
        }
        case yap_literal_null:
            res.type = ctx->void_type_id;  // null is a void pointer-like value
            res.literal = (yap_literal){ .kind = yap_literal_null, .text = "0" };
            break;
        case yap_literal_func:
            return yap_build_func_literal_expr(src, &lit->func_literal);
        case yap_literal_blob: {
            unsigned int count = darr_len(lit->blob_elements);
            darr(yap_expr) elements = yap_ctx_darr_new(ctx, yap_expr, .cap = count, .len = 0);
            darr(char*) names = yap_ctx_darr_new(ctx, char*, .cap = count, .len = 0);
            for_darr(i, elem, lit->blob_elements){
                yap_expr e = yap_build_expr(src, elem.value);
                if (e.kind == yap_expr_error)
                    return (yap_expr){ .kind = yap_expr_error };
                darr_push(elements, e);
                darr_push(names, elem.is_named ? elem.name.value : NULL);
            }
            yap_type_id blob_tid = yap_push_blob_type(ctx, count);
            res.type = blob_tid;
            res.literal = (yap_literal){
                .kind = yap_literal_blob,
                .blob = (yap_blob){ .elements = elements, .names = names, .field_count = count }
            };
            break;
        }
        default:
            yap_build_push_error(src, lit->loc, "Unhandled literal kind");
            return (yap_expr){ .kind = yap_expr_error };
    }
    return res;
}

yap_expr yap_build_var_access_expr(yap_source* src, yap_identifier_node* ident){
    yap_ctx* ctx = src->ctx;

    if (!ident->value){
        yap_build_push_error(src, ident->loc, "Missing identifier in variable access");
        return (yap_expr){ .kind = yap_expr_error };
    }

    const yap_var* var = yap_scope_get_var_recursive(yap_ctx_current_scope(ctx), ident->value);
    if (!var){
        yap_build_push_error(src, ident->loc, "Undefined variable '%s'", ident->value);
        return (yap_expr){ .kind = yap_expr_error };
    }

    char* emit_name = var->name;
    yap_module* cur_mod = yap_source_owning_module(ctx, src);
    if (cur_mod && cur_mod->prefix[0]
        && cur_mod->scope
        && yap_scope_get_var(cur_mod->scope, ident->value)
        && strcmp(ident->value, "main") != 0) {
        emit_name = yap_ctx_strus_newf(ctx, "%s%s", cur_mod->prefix, var->name);
    }

    return (yap_expr){
        .kind        = yap_expr_var,
        .var_name    = emit_name,
        .type        = var->type,
        .is_lvalue   = true,
        .is_comptime = false
    };
}

// Shared desugar target for '??' (coalesce_op) and '?=' (assignment):
// builds the ternary 'left ? left : right' - yields 'left' if it's
// truthy/non-zero, else 'right'. 'left' is duplicated as both the condition
// and the true-branch value, so codegen evaluates it twice if it has side
// effects (matches the operators' spec literally).
static yap_expr yap_build_coalesce_ternary(yap_source* src, yap_loc loc, yap_expr left, yap_expr right){
    yap_ctx* ctx = src->ctx;

    yap_type_id common = yap_ctx_find_common_type(ctx, left.type, right.type);
    if (!common){
        char* l_s = yap_ctx_type_id_to_string(ctx, left.type);
        char* r_s = yap_ctx_type_id_to_string(ctx, right.type);
        yap_build_push_error(src, loc, "Incompatible types in coalesce expression: '%s' and '%s'", l_s, r_s);
        free(l_s); free(r_s);
        return (yap_expr){ .kind = yap_expr_error };
    }

    return (yap_expr){
        .kind    = yap_expr_ternary,
        .ternary = (yap_ternary_expr){
            .condition = yap_ctx_one_cpy(ctx, left),
            .then_expr = yap_ctx_one_cpy(ctx, left),
            .else_expr = yap_ctx_one_cpy(ctx, right)
        },
        .type        = common,
        .is_lvalue   = false,
        .is_comptime = left.is_comptime && right.is_comptime
    };
}

yap_expr yap_build_bin_expr(yap_source* src, yap_bin_op_node* bin){
    yap_ctx* ctx = src->ctx;

    yap_expr left  = yap_build_expr(src, bin->left);
    yap_expr right = yap_build_expr(src, bin->right);
    if (left.kind == yap_expr_error || right.kind == yap_expr_error)
        return (yap_expr){ .kind = yap_expr_error };

    // '??' (coalesce_op) never becomes a yap_bin_expr - it's pure sugar for
    // a ternary, built and returned directly here instead of falling through
    // to the generic binary-operator allow-list/common-type logic below.
    if (bin->op == 'c')
        return yap_build_coalesce_ternary(src, bin->loc, left, right);

    bool is_comparison = strchr("<>enlg", bin->op) != NULL;
    if (!strchr("+-*/%<>enlgaoLR&|^", bin->op)){
        yap_build_push_error(src, bin->loc, "Unsupported binary operator '%c'", bin->op);
        return (yap_expr){ .kind = yap_expr_error };
    }

    yap_type_id common = yap_ctx_find_common_type(ctx, left.type, right.type);
    if (common == ctx->internal_error_type_id){
        yap_build_push_error(src, bin->loc, "Incompatible types in binary expression");
        return (yap_expr){ .kind = yap_expr_error };
    }

    int sem_op = bin->op;
    if (bin->op == 'e') sem_op = yap_bin_expr_eq;
    else if (bin->op == 'n') sem_op = yap_bin_expr_neq;
    else if (bin->op == 'l') sem_op = yap_bin_expr_le;
    else if (bin->op == 'g') sem_op = yap_bin_expr_ge;
    else if (bin->op == '<') sem_op = yap_bin_expr_lt;
    else if (bin->op == '>') sem_op = yap_bin_expr_gt;

    yap_type_id result_type = is_comparison ? ctx->bool_type_id : common;
    return (yap_expr){
        .kind     = yap_expr_bin,
        .bin_expr = (yap_bin_expr){
            .op    = sem_op,
            .left  = yap_ctx_one_cpy(ctx, left),
            .right = yap_ctx_one_cpy(ctx, right)
        },
        .type        = result_type,
        .is_comptime = left.is_comptime && right.is_comptime,
        .is_lvalue   = false
    };
}

yap_expr yap_build_unary_expr(yap_source* src, yap_unary_op_node* un){
    yap_ctx* ctx = src->ctx;
    yap_expr expr = yap_build_expr(src, un->expr);
    if (expr.kind == yap_expr_error)
        return (yap_expr){ .kind = yap_expr_error };

    yap_type_id coerced = yap_ctx_coerce_type_id_to_id(ctx, expr.type);
    yap_type* operand_type = yap_ctx_get_type(ctx, coerced);
    yap_type_id result_type = expr.type;

    if (un->op == '!'){
        // Logical not: truthy-checks any scalar (primitive or pointer), same
        // as C's own '!' and matching this language's existing truthy-check
        // operators ('??'/'?=', see yap_build_coalesce_ternary, and if/while
        // conditions) which never restrict to bool either -- codegen defers
        // to C's native truthiness. Result is always bool.
        bool is_scalar = operand_type
            && (operand_type->kind == yap_type_primitive || operand_type->kind == yap_type_ptr);
        if (!is_scalar){
            yap_build_push_error(src, un->loc, "Operand of unary '!' must be a scalar type");
            return (yap_expr){ .kind = yap_expr_error };
        }
        result_type = ctx->bool_type_id;
    } else {
        // '-' (negation) and '~' (bitwise not) both require a non-bool
        // numeric operand; result keeps the operand's own type, same as '-'
        // always did (mirrors the existing bitwise binary ops, which also
        // don't distinguish int from float beyond this).
        bool is_numeric = operand_type && operand_type->kind == yap_type_primitive
            && !yap_ctx_type_ids_eq(ctx, coerced, ctx->bool_type_id);
        if (!is_numeric){
            yap_build_push_error(src, un->loc, "Operand of unary '%c' must be a numeric type", un->op);
            return (yap_expr){ .kind = yap_expr_error };
        }
    }

    return (yap_expr){
        .kind        = yap_expr_unary,
        .subexpr     = yap_ctx_one_cpy(ctx, expr),
        .type        = result_type,
        .unary_op    = un->op,
        .is_lvalue   = false,
        .is_comptime = expr.is_comptime
    };
}

yap_expr yap_build_assignment_expr(yap_source* src, yap_assignment_node* assign){
    yap_ctx* ctx = src->ctx;

    yap_expr left  = yap_build_expr(src, assign->left);
    yap_expr right = yap_build_expr(src, assign->right);
    if (left.kind == yap_expr_error || right.kind == yap_expr_error)
        return (yap_expr){ .kind = yap_expr_error };

    if (!left.is_lvalue){
        yap_build_push_error(src, assign->loc, "Left side of assignment must be an lvalue");
        return (yap_expr){ .kind = yap_expr_error };
    }

    yap_type* left_type = yap_ctx_get_type(ctx, left.type);
    if (left_type && left_type->is_const){
        yap_build_push_error(src, assign->loc, "Cannot assign to a const value");
        return (yap_expr){ .kind = yap_expr_error };
    }

    // '?=' desugars to a plain '=' whose right side is the coalesce ternary
    // 'left ? left : right' - i.e. only overwrite 'left' when it's falsy/zero.
    bool is_coalesce_assign = strcmp(assign->op, "?=") == 0;
    if (is_coalesce_assign){
        right = yap_build_coalesce_ternary(src, assign->loc, left, right);
        if (right.kind == yap_expr_error)
            return (yap_expr){ .kind = yap_expr_error };
    }

    // Type assignability check: right-hand side must be assignable to left.
    // Uses yap_ctx_type_id_assignable which coerces untyped types before comparison.
    if (!yap_ctx_type_id_assignable(ctx, left.type, right.type)){
        char* rhs_str = yap_ctx_type_id_to_string(ctx, right.type);
        char* lhs_str = yap_ctx_type_id_to_string(ctx, left.type);
        yap_build_push_error(src, assign->loc,
            "Cannot assign value of type '%s' to variable of type '%s'",
            rhs_str, lhs_str);
        free(rhs_str);
        free(lhs_str);
        return (yap_expr){ .kind = yap_expr_error };
    }

    if (!yap_check_literal_range(src, assign->loc, right, left.type))
        return (yap_expr){ .kind = yap_expr_error };

    yap_assignment a = {
        .kind  = yap_assignment_valid,
        .left  = yap_ctx_one_cpy(ctx, left),
        .right = yap_ctx_one_cpy(ctx, right)
    };
    snprintf(a.op, sizeof(a.op), "%s", is_coalesce_assign ? "=" : assign->op);

    return (yap_expr){
        .kind       = yap_expr_assignment,
        .assignment = a,
        .type       = left.type,
        .is_lvalue  = false
    };
}

/* Struct/union/enum name, or (for builtin opaque comptime types like yType,
 * yStructT, yFnT, ... which have no nominal struct/union/enum name but can
 * still have builtin methods registered under "PrimitiveName_methodname",
 * see ctx.c) the primitive's own declared name. Harmless for every other
 * primitive since none has methods. Shared by real method dispatch
 * (yap_build_method_callee) and receiver-dispatched macro calls
 * (yap_exec_macro_call), both of which mangle "OwnerName_name" the same way. */
static const char* yap_owner_name_for_type(yap_ctx* ctx, yap_type_id type_id){
    yap_type* t = yap_ctx_get_type(ctx, type_id);
    if (!t) return NULL;
    if (t->kind == yap_type_struct) return t->structure.name;
    if (t->kind == yap_type_union) return t->uni.name;
    if (t->kind == yap_type_enum) return t->enumeration.name;
    if (t->kind == yap_type_primitive) return t->primitive.name;
    return NULL;
}

/* Resolves 'recv:name' to the mangled "TypeName_name" function var and
 * builds the receiver expression that becomes the call's first argument. */
static yap_expr yap_build_method_callee(yap_source* src, yap_method_access_node* ma, yap_expr* out_receiver){
    yap_ctx* ctx = src->ctx;

    yap_expr receiver = yap_build_expr(src, ma->caller);
    if (receiver.kind == yap_expr_error)
        return (yap_expr){ .kind = yap_expr_error };

    const char* owner_name = yap_owner_name_for_type(ctx, receiver.type);
    if (!owner_name || !owner_name[0]){
        char* type_str = yap_ctx_type_id_to_string(ctx, receiver.type);
        yap_build_push_error(src, ma->loc, "Type '%s' has no methods", type_str);
        free(type_str);
        return (yap_expr){ .kind = yap_expr_error };
    }

    if (!ma->name.value){
        yap_build_push_error(src, ma->loc, "Missing method name");
        return (yap_expr){ .kind = yap_expr_error };
    }

    char* mangled_name = yap_ctx_strus_newf(ctx, "%s_%s", owner_name, ma->name.value);
    const yap_var* method_var = yap_scope_get_var_recursive(yap_ctx_current_scope(ctx), mangled_name);
    if (!method_var){
        yap_build_push_error(src, ma->loc, "No method '%s' found for type '%s'", ma->name.value, owner_name);
        return (yap_expr){ .kind = yap_expr_error };
    }

    *out_receiver = receiver;
    return (yap_expr){
        .kind        = yap_expr_var,
        .var_name    = method_var->name,
        .type        = method_var->type,
        .is_lvalue   = true,
        .is_comptime = false
    };
}

/* Looks up the top-level declaration a call targets (a plain function name,
 * or the mangled "TypeName_method" name a method call resolves to), so
 * default values and parameter names can be read off it. Returns NULL for
 * anything called indirectly through a function-typed variable/expression,
 * since there's no declaration to resolve names/defaults against there. */
static yap_func_decl* yap_find_func_decl_for_call(yap_source* src, yap_expr func_expr){
    yap_ctx* ctx = src->ctx;
    if (func_expr.kind != yap_expr_var || !func_expr.var_name) return NULL;

    /* A call from inside the module's own source resolves the callee through
     * ordinary scope lookup, which gives back its bare, unprefixed registered
     * name (Pass 1 only mangles methods, never plain module functions) -- so
     * a declaration's own module_prefix alone can't tell us whether the
     * CALLER's var_name needs the prefix stripped or not. Track which module
     * (if any) src itself belongs to, so a bare name is only matched against
     * that module's declarations, not any module in the flat decl list. */
    const char* current_module_prefix = NULL;
    {
        yap_module* owning_mod = yap_source_owning_module(ctx, src);
        if (owning_mod && owning_mod->prefix && owning_mod->prefix[0])
            current_module_prefix = owning_mod->prefix;
    }

    for (darr_size_t di = 0; di < darr_len(ctx->semantic_decls); di++){
        yap_decl* d = &ctx->semantic_decls[di];
        if ((d->kind == yap_decl_func_def || d->kind == yap_decl_func_decl) && d->func_decl.name){
            bool match;
            if (d->module_prefix && d->module_prefix[0]) {
                size_t plen = strlen(d->module_prefix);
                size_t nlen = strlen(d->func_decl.name);
                size_t vlen = strlen(func_expr.var_name);
                // Called from outside via 'module->func(...)': var_name already carries the prefix.
                match = (vlen == plen + nlen)
                     && memcmp(func_expr.var_name, d->module_prefix, plen) == 0
                     && memcmp(func_expr.var_name + plen, d->func_decl.name, nlen) == 0;
                // Called by bare name from inside that same module.
                if (!match && current_module_prefix && strcmp(current_module_prefix, d->module_prefix) == 0)
                    match = strcmp(d->func_decl.name, func_expr.var_name) == 0;
            } else {
                match = strcmp(d->func_decl.name, func_expr.var_name) == 0;
            }
            if (match) return &d->func_decl;
        }
    }
    return NULL;
}

yap_expr yap_build_func_call_expr(yap_source* src, yap_func_call_node* call){
    yap_ctx* ctx = src->ctx;

    bool is_method_call = call->func->kind == yap_expr_method_access;
    yap_expr receiver = {0};
    yap_expr func_expr = is_method_call
        ? yap_build_method_callee(src, &call->func->method_access, &receiver)
        : yap_build_expr(src, call->func);
    if (func_expr.kind == yap_expr_error)
        return (yap_expr){ .kind = yap_expr_error };

    yap_type* func_type = yap_ctx_get_type(ctx, func_expr.type);
    if (!func_type || func_type->kind != yap_type_func){
        yap_build_push_error(src, call->loc, "Cannot call a non-function type");
        return (yap_expr){ .kind = yap_expr_error };
    }

    darr(yap_type_id) expected_args = func_type->func.args;
    unsigned int nexpected = darr_len(expected_args);

    /* Each declared parameter fills exactly one slot: positional args claim
     * the next unclaimed slot in order, named args ('.name = value') claim
     * their slot by name wherever it falls, and either kind can leave gaps
     * for later args (positional or default) to fill -- same scheme
     * yap_build_blob_cast already uses for struct-literal fields. */
    bool*     used  = yap_ctx_malloc(ctx, sizeof(bool) * (nexpected ? nexpected : 1));
    yap_expr* slots = yap_ctx_malloc(ctx, sizeof(yap_expr) * (nexpected ? nexpected : 1));
    for (unsigned int i = 0; i < nexpected; i++) used[i] = false;

    if (is_method_call && nexpected > 0){
        if (!yap_ctx_type_id_compatible(ctx, receiver.type, expected_args[0])){
            /* Pointer-receiver method (subject type is 'T@'): auto-take-address of an
             * lvalue receiver of type 'T', same as Go/C++ implicit &this binding, so
             * mutating methods (e.g. a growable array's push()) can write back to the
             * caller's variable without the caller writing '&a:push(x)' by hand. */
            yap_type* expected_t = yap_ctx_get_type(ctx, expected_args[0]);
            yap_type* receiver_t = yap_ctx_get_type(ctx, receiver.type);
            yap_type* pointee_t  = (expected_t && expected_t->kind == yap_type_ptr)
                ? yap_ctx_get_type(ctx, expected_t->pointer_type) : NULL;
            bool auto_ref = pointee_t && receiver_t && receiver.is_lvalue
                && yap_ctx_types_eq(ctx, *pointee_t, *receiver_t);
            if (auto_ref){
                receiver = (yap_expr){
                    .kind      = yap_expr_at_op,
                    .subexpr   = yap_ctx_one_cpy(ctx, receiver),
                    .type      = expected_args[0],
                    .is_lvalue = false
                };
            } else {
                char* expected_str = yap_ctx_type_id_to_string(ctx, expected_args[0]);
                char* actual_str   = yap_ctx_type_id_to_string(ctx, receiver.type);
                yap_build_push_error(src, call->loc,
                    "Receiver type mismatch for method '%s': expected '%s', got '%s'",
                    call->func->method_access.name.value, expected_str, actual_str);
                free(expected_str);
                free(actual_str);
                return (yap_expr){ .kind = yap_expr_error };
            }
        }
        slots[0] = receiver;
        used[0]  = true;
    }

    yap_func_decl* found_decl = NULL;
    bool found_decl_resolved = false;
    unsigned int pos_idx = 0;
    unsigned int total_args = darr_len(call->args) + (is_method_call ? 1 : 0);

    for_darr(pi, arg_node, call->args){
        yap_expr pe = yap_build_expr(src, arg_node.value);
        if (pe.kind == yap_expr_error)
            return (yap_expr){ .kind = yap_expr_error };

        unsigned int target_idx;
        if (arg_node.is_named){
            if (!found_decl_resolved){
                found_decl = yap_find_func_decl_for_call(src, func_expr);
                found_decl_resolved = true;
            }
            if (!found_decl){
                yap_build_push_error(src, call->loc,
                    "Named argument '.%s' requires calling a directly-declared function",
                    arg_node.name.value);
                return (yap_expr){ .kind = yap_expr_error };
            }
            bool found = false;
            for (unsigned int i = 0; i < darr_len(found_decl->args) && i < nexpected; i++){
                if (found_decl->args[i].kind == yap_func_arg_valid && found_decl->args[i].name
                    && strus_eq(found_decl->args[i].name, arg_node.name.value)){
                    if (used[i]){
                        yap_build_push_error(src, call->loc,
                            "Duplicate argument '.%s'", arg_node.name.value);
                        return (yap_expr){ .kind = yap_expr_error };
                    }
                    target_idx = i;
                    found = true;
                    break;
                }
            }
            if (!found){
                yap_build_push_error(src, call->loc,
                    "Function has no parameter named '%s'", arg_node.name.value);
                return (yap_expr){ .kind = yap_expr_error };
            }
        } else {
            while (pos_idx < nexpected && used[pos_idx]) pos_idx++;
            if (pos_idx >= nexpected){
                yap_build_push_error(src, call->loc,
                    "Too many arguments: expected %u, got %u", nexpected, total_args);
                return (yap_expr){ .kind = yap_expr_error };
            }
            target_idx = pos_idx;
            pos_idx++;
        }

        yap_type_id expected = expected_args[target_idx];
        yap_type* pe_type = yap_ctx_get_type(ctx, pe.type);
        if (pe_type && pe_type->kind == yap_type_blob){
            pe = yap_build_blob_cast(src, pe, expected, call->loc);
            if (pe.kind == yap_expr_error)
                return (yap_expr){ .kind = yap_expr_error };
        } else if (!yap_ctx_type_id_compatible(ctx, pe.type, expected)){
            char* expected_str = yap_ctx_type_id_to_string(ctx, expected);
            char* actual_str   = yap_ctx_type_id_to_string(ctx, pe.type);
            yap_build_push_error(src, call->loc,
                "Argument type mismatch: expected '%s', got '%s'",
                expected_str, actual_str);
            free(expected_str);
            free(actual_str);
            return (yap_expr){ .kind = yap_expr_error };
        }

        slots[target_idx] = pe;
        used[target_idx]  = true;
    }

    unsigned int filled = 0;
    for (unsigned int i = 0; i < nexpected; i++) if (used[i]) filled++;

    for (unsigned int i = 0; i < nexpected; i++){
        if (used[i]) continue;
        if (!found_decl_resolved){
            found_decl = yap_find_func_decl_for_call(src, func_expr);
            found_decl_resolved = true;
        }
        if (!found_decl){
            yap_build_push_error(src, call->loc,
                "Too few arguments: expected %u, got %u", nexpected, filled);
            return (yap_expr){ .kind = yap_expr_error };
        }
        if (i < darr_len(found_decl->args)
            && found_decl->args[i].kind == yap_func_arg_valid
            && found_decl->args[i].default_value.kind != yap_expr_error) {
            slots[i] = found_decl->args[i].default_value;
            used[i]  = true;
        } else {
            yap_build_push_error(src, call->loc,
                "Missing argument %u with no default value", i + 1);
            return (yap_expr){ .kind = yap_expr_error };
        }
    }

    darr(yap_expr) params = yap_ctx_darr_new(ctx, yap_expr, .cap = nexpected, .len = 0);
    for (unsigned int i = 0; i < nexpected; i++) darr_push(params, slots[i]);

    return (yap_expr){
        .kind      = yap_expr_func_call,
        .func_call = (yap_func_call){
            .func_expr = yap_ctx_one_cpy(ctx, func_expr),
            .params    = params
        },
        .type        = func_type->func.return_type,
        .is_lvalue   = false,
        .is_comptime = false
    };
}

static yap_expr yap_build_blob_cast(yap_source* src, yap_expr blob_expr, yap_type_id target_type, yap_loc loc){
    yap_ctx* ctx = src->ctx;
    yap_blob blob = blob_expr.literal.blob;
    yap_type* target = yap_ctx_get_type(ctx, target_type);
    if (!target){
        yap_build_push_error(src, loc, "Invalid blob cast target type");
        return (yap_expr){ .kind = yap_expr_error };
    }

    if (target->kind == yap_type_struct){
        if (target->structure.name){
            yap_type_id resolved_id = yap_ctx_get_type_id_by_name(ctx, target->structure.name);
            if (resolved_id) {
                yap_type* resolved = yap_ctx_get_type(ctx, resolved_id);
                if (resolved && resolved->kind == yap_type_struct && resolved->structure.fields)
                    target = resolved;
            }
        }
        darr(yap_struct_field) fields = target->structure.fields;
        if (!fields){
            yap_build_push_error(src, loc, "Struct type has no fields (forward declaration only)");
            return (yap_expr){ .kind = yap_expr_error };
        }
        unsigned int nfields = darr_len(fields);
        if (blob.field_count > nfields){
            yap_build_push_error(src, loc, "Blob has %u elements, struct has %u fields",
                blob.field_count, nfields);
            return (yap_expr){ .kind = yap_expr_error };
        }
        darr(yap_expr) ordered = yap_ctx_darr_new(ctx, yap_expr, .cap = nfields, .len = 0);
        darr(char*) ordered_names = yap_ctx_darr_new(ctx, char*, .cap = nfields, .len = 0);
        bool* used = yap_ctx_malloc(ctx, sizeof(bool) * nfields);
        for (unsigned int i = 0; i < nfields; i++) used[i] = false;

        unsigned int pos_idx = 0;
        for (unsigned int i = 0; i < blob.field_count; i++){
            char* name = blob.names[i];
            yap_expr elem = blob.elements[i];
            if (name){
                bool found = false;
                for (unsigned int fi = 0; fi < nfields; fi++){
                    if (fields[fi].name && strus_eq(fields[fi].name, name)){
                        if (used[fi]){
                            yap_build_push_error(src, loc, "Duplicate field '%s' in blob", name);
                            return (yap_expr){ .kind = yap_expr_error };
                        }
                        used[fi] = true;
                        found = true;
                        break;
                    }
                }
                if (!found){
                    yap_build_push_error(src, loc, "Struct has no field named '%s'", name);
                    return (yap_expr){ .kind = yap_expr_error };
                }
            } else {
                while (pos_idx < nfields && used[pos_idx]) pos_idx++;
                if (pos_idx >= nfields){
                    yap_build_push_error(src, loc, "Too many positional elements in blob");
                    return (yap_expr){ .kind = yap_expr_error };
                }
                used[pos_idx] = true;
                pos_idx++;
            }
            darr_push(ordered, elem);
            darr_push(ordered_names, name);
        }

        for (unsigned int fi = 0; fi < nfields; fi++){
            if (!used[fi]){
                if (fields[fi].default_value){
                    darr_push(ordered, *fields[fi].default_value);
                    darr_push(ordered_names, fields[fi].name);
                } else {
                    yap_build_push_error(src, loc,
                        "Missing value for field '%s' with no default",
                        fields[fi].name ? fields[fi].name : "(unnamed)");
                    return (yap_expr){ .kind = yap_expr_error };
                }
            }
        }

        blob_expr.literal.blob.elements = ordered;
        blob_expr.literal.blob.names = ordered_names;
        blob_expr.literal.blob.field_count = darr_len(ordered);
        blob_expr.type = target_type;
        return blob_expr;
    }

    if (target->kind == yap_type_array){
        if (blob.field_count != target->array.size){
            yap_build_push_error(src, loc, "Blob has %u elements, array has %zu slots",
                blob.field_count, target->array.size);
            return (yap_expr){ .kind = yap_expr_error };
        }
        blob_expr.type = target_type;
        return blob_expr;
    }

    if (target->kind == yap_type_slice){
        blob_expr.type = target_type;
        return blob_expr;
    }

    yap_build_push_error(src, loc, "Blob can only be cast to struct, array, or slice");
    return (yap_expr){ .kind = yap_expr_error };
}

yap_expr yap_build_cast_expr(yap_source* src, yap_cast_node* cast){
    yap_ctx* ctx = src->ctx;

    yap_type_id target_type = yap_build_type_from_type_node(src, cast->type_node);
    if (!target_type){
        yap_build_push_error(src, cast->loc, "Invalid cast target type");
        return (yap_expr){ .kind = yap_expr_error };
    }

    yap_expr expr = yap_build_expr(src, cast->expr);
    if (expr.kind == yap_expr_error)
        return (yap_expr){ .kind = yap_expr_error };

    yap_type* expr_type = yap_ctx_get_type(ctx, expr.type);
    if (expr_type && expr_type->kind == yap_type_blob){
        return yap_build_blob_cast(src, expr, target_type, cast->loc);
    }

    return (yap_expr){
        .kind        = yap_expr_cast,
        .subexpr     = yap_ctx_one_cpy(ctx, expr),
        .type        = target_type,
        .is_lvalue   = expr.is_lvalue,
        .is_comptime = expr.is_comptime
    };
}

yap_expr yap_build_at_op_expr(yap_source* src, yap_at_op_node* at){
    yap_ctx* ctx = src->ctx;

    yap_expr expr = yap_build_expr(src, at->expr);
    if (expr.kind == yap_expr_error)
        return (yap_expr){ .kind = yap_expr_error };

    if (!expr.is_lvalue){
        yap_build_push_error(src, at->loc, "Cannot take address of non-lvalue");
        return (yap_expr){ .kind = yap_expr_error };
    }

    return (yap_expr){
        .kind      = yap_expr_at_op,
        .subexpr   = yap_ctx_one_cpy(ctx, expr),
        .type      = yap_ctx_get_pointer_of_type_id(ctx, expr.type),
        .is_lvalue = false
    };
}

yap_expr yap_build_paren_expr(yap_source* src, yap_paren_node* par){
    yap_ctx* ctx = src->ctx;
    yap_expr expr = yap_build_expr(src, par->expr);
    if (expr.kind == yap_expr_error) return expr;

    yap_expr res = expr;
    res.kind    = yap_expr_paren;
    res.subexpr = yap_ctx_one_cpy(ctx, expr);
    return res;
}

yap_expr yap_build_increment_expr(yap_source* src, yap_increment_node* incr){
    yap_ctx* ctx = src->ctx;
    yap_expr expr = yap_build_expr(src, incr->expr);
    if (expr.kind == yap_expr_error) return (yap_expr){ .kind = yap_expr_error };

    if (!expr.is_lvalue){
        yap_build_push_error(src, incr->expr->loc, "Operand of increment must be an lvalue");
        return (yap_expr){ .kind = yap_expr_error };
    }

    return (yap_expr){
        .kind      = yap_expr_increment,
        .subexpr   = yap_ctx_one_cpy(ctx, expr),
        .type      = expr.type,
        .is_lvalue = false,
        .prefix    = incr->prefix
    };
}

yap_expr yap_build_decrement_expr(yap_source* src, yap_decrement_node* decr){
    yap_ctx* ctx = src->ctx;
    yap_expr expr = yap_build_expr(src, decr->expr);
    if (expr.kind == yap_expr_error) return (yap_expr){ .kind = yap_expr_error };

    if (!expr.is_lvalue){
        yap_build_push_error(src, decr->expr->loc, "Operand of decrement must be an lvalue");
        return (yap_expr){ .kind = yap_expr_error };
    }

    return (yap_expr){
        .kind      = yap_expr_decrement,
        .subexpr   = yap_ctx_one_cpy(ctx, expr),
        .type      = expr.type,
        .is_lvalue = false,
        .prefix    = decr->prefix
    };
}

yap_expr yap_build_ternary_expr(yap_source* src, yap_ternary_node* ter){
    yap_ctx* ctx = src->ctx;

    yap_expr cond = yap_build_expr(src, ter->condition);
    if (cond.kind == yap_expr_error) return (yap_expr){ .kind = yap_expr_error };

    yap_expr then_expr = yap_build_expr(src, ter->then_expr);
    if (then_expr.kind == yap_expr_error) return (yap_expr){ .kind = yap_expr_error };

    yap_expr else_expr = yap_build_expr(src, ter->else_expr);
    if (else_expr.kind == yap_expr_error) return (yap_expr){ .kind = yap_expr_error };

    yap_type_id common = yap_ctx_find_common_type(ctx, then_expr.type, else_expr.type);
    if (!common){
        char* t_s = yap_ctx_type_id_to_string(ctx, then_expr.type);
        char* e_s = yap_ctx_type_id_to_string(ctx, else_expr.type);
        yap_build_push_error(src, ter->loc,
            "Incompatible types in ternary: '%s' and '%s'", t_s, e_s);
        free(t_s); free(e_s);
        return (yap_expr){ .kind = yap_expr_error };
    }

    return (yap_expr){
        .kind    = yap_expr_ternary,
        .ternary = (yap_ternary_expr){
            .condition = yap_ctx_one_cpy(ctx, cond),
            .then_expr = yap_ctx_one_cpy(ctx, then_expr),
            .else_expr = yap_ctx_one_cpy(ctx, else_expr)
        },
        .type        = common,
        .is_lvalue   = false,
        .is_comptime = cond.is_comptime && then_expr.is_comptime && else_expr.is_comptime
    };
}

yap_expr yap_build_member_access_expr(yap_source* src, yap_member_access_node* ma){
    yap_ctx* ctx = src->ctx;

    yap_expr object = yap_build_expr(src, ma->object);
    if (object.kind == yap_expr_error) return (yap_expr){ .kind = yap_expr_error };

    yap_type* obj_type = yap_ctx_get_type(ctx, object.type);

    /* Slices (e.g. string literals, yExprList) codegen to a real
     * 'struct { T* data; unsigned long len; }' (yap_gen_name_type_combo's
     * yap_type_slice case) -- expose those two fields directly rather than
     * requiring a builder/method for something that's already a plain
     * struct at the C level. */
    if (obj_type && obj_type->kind == yap_type_slice){
        if (ma->member.value && strcmp(ma->member.value, "len") == 0){
            yap_type_id u64_id = yap_ctx_get_type_id_by_name(ctx, "u64");
            return (yap_expr){
                .kind          = yap_expr_member_access,
                .member_access = { .object = yap_ctx_one_cpy(ctx, object), .member = ma->member.value },
                .type        = u64_id,
                .is_lvalue   = false,
                .is_comptime = object.is_comptime
            };
        }
        if (ma->member.value && strcmp(ma->member.value, "data") == 0){
            yap_type_id ptr_id = yap_ctx_get_pointer_of_type_id(ctx, obj_type->slice.element_type);
            return (yap_expr){
                .kind          = yap_expr_member_access,
                .member_access = { .object = yap_ctx_one_cpy(ctx, object), .member = ma->member.value },
                .type        = ptr_id,
                .is_lvalue   = object.is_lvalue,
                .is_comptime = object.is_comptime
            };
        }
        yap_build_push_error(src, ma->loc,
            "Slice has no member named '%s' (only 'len' and 'data')", ma->member.value);
        return (yap_expr){ .kind = yap_expr_error };
    }

    if (!obj_type || (obj_type->kind != yap_type_struct && obj_type->kind != yap_type_union)){
        yap_build_push_error(src, ma->loc, "Member access requires struct or union type");
        return (yap_expr){ .kind = yap_expr_error };
    }

    yap_type_id member_type = yap_ctx_find_member_type(ctx, object.type, ma->member.value);
    if (member_type == ctx->internal_error_type_id){
        yap_build_push_error(src, ma->loc,
            "Type has no member named '%s'", ma->member.value);
        return (yap_expr){ .kind = yap_expr_error };
    }

    return (yap_expr){
        .kind          = yap_expr_member_access,
        .member_access = (yap_member_access){
            .object = yap_ctx_one_cpy(ctx, object),
            .member = ma->member.value
        },
        .type        = member_type,
        .is_lvalue   = object.is_lvalue,
        .is_comptime = object.is_comptime
    };
}

// `ptr?.member`: only valid on a pointer to struct/union. When `ptr` is
// null at runtime, evaluates to the zero value of `member`'s type instead
// of dereferencing (there's no Optional<T> in yap to propagate, so a zero
// value is the fallback - chosen so the result keeps the same type as plain
// `.member` access, letting `?.` chains keep type-checking normally).
yap_expr yap_build_optional_member_access_expr(yap_source* src, yap_member_access_node* ma){
    yap_ctx* ctx = src->ctx;

    yap_expr object = yap_build_expr(src, ma->object);
    if (object.kind == yap_expr_error) return (yap_expr){ .kind = yap_expr_error };

    yap_type* obj_type = yap_ctx_get_type(ctx, object.type);
    if (!obj_type || obj_type->kind != yap_type_ptr){
        yap_build_push_error(src, ma->loc, "Optional chaining ('?.') requires a pointer type");
        return (yap_expr){ .kind = yap_expr_error };
    }

    yap_type* pointee_type = yap_ctx_get_type(ctx, obj_type->pointer_type);
    if (!pointee_type || (pointee_type->kind != yap_type_struct && pointee_type->kind != yap_type_union)){
        yap_build_push_error(src, ma->loc, "Optional chaining ('?.') requires a pointer to struct or union");
        return (yap_expr){ .kind = yap_expr_error };
    }

    yap_type_id member_type = yap_ctx_find_member_type(ctx, obj_type->pointer_type, ma->member.value);
    if (member_type == ctx->internal_error_type_id){
        yap_build_push_error(src, ma->loc,
            "Type has no member named '%s'", ma->member.value);
        return (yap_expr){ .kind = yap_expr_error };
    }

    return (yap_expr){
        .kind          = yap_expr_optional_member_access,
        .member_access = (yap_member_access){
            .object = yap_ctx_one_cpy(ctx, object),
            .member = ma->member.value
        },
        .type        = member_type,
        .is_lvalue   = false,
        .is_comptime = false
    };
}

// `expr.`: unchecked dereference, works on any pointer type. Unlike `?.`,
// this is the raw/unsafe deref (matching C's *ptr - UB on null), since `?.`
// already exists as the null-checked alternative for struct field access.
yap_expr yap_build_deref_expr(yap_source* src, yap_deref_node* dn){
    yap_ctx* ctx = src->ctx;

    yap_expr expr = yap_build_expr(src, dn->expr);
    if (expr.kind == yap_expr_error) return (yap_expr){ .kind = yap_expr_error };

    yap_type* obj_type = yap_ctx_get_type(ctx, expr.type);
    if (!obj_type || obj_type->kind != yap_type_ptr){
        yap_build_push_error(src, dn->loc, "Cannot dereference a non-pointer type");
        return (yap_expr){ .kind = yap_expr_error };
    }

    return (yap_expr){
        .kind        = yap_expr_deref,
        .subexpr     = yap_ctx_one_cpy(ctx, expr),
        .type        = obj_type->pointer_type,
        .is_lvalue   = true,
        .is_comptime = false
    };
}

yap_expr yap_build_index_access_expr(yap_source* src, yap_index_access_node* ia){
    yap_ctx* ctx = src->ctx;

    yap_expr object = yap_build_expr(src, ia->object);
    if (object.kind == yap_expr_error) return (yap_expr){ .kind = yap_expr_error };

    yap_expr index = yap_build_expr(src, ia->index);
    if (index.kind == yap_expr_error) return (yap_expr){ .kind = yap_expr_error };

    yap_type* obj_type = yap_ctx_get_type(ctx, object.type);
    if (!obj_type){
        yap_build_push_error(src, ia->loc, "Invalid type in index access");
        return (yap_expr){ .kind = yap_expr_error };
    }

    yap_type_id element_type = 0;
    if (obj_type->kind == yap_type_array)
        element_type = obj_type->array.element_type;
    else if (obj_type->kind == yap_type_slice)
        element_type = obj_type->slice.element_type;
    else if (obj_type->kind == yap_type_ptr)
        element_type = obj_type->pointer_type;
    else {
        yap_build_push_error(src, ia->loc, "Index access requires array, slice, or pointer type");
        return (yap_expr){ .kind = yap_expr_error };
    }

    return (yap_expr){
        .kind          = yap_expr_index_access,
        .index_access  = (yap_index_access){
            .object = yap_ctx_one_cpy(ctx, object),
            .index  = yap_ctx_one_cpy(ctx, index)
        },
        .type        = element_type,
        .is_lvalue   = true,
        .is_comptime = false
    };
}

yap_expr yap_build_block_expr(yap_source* src, yap_block_node* bnode){
    yap_ctx* ctx = src->ctx;

    yap_block block = yap_build_block(src, bnode);
    if (block.kind != yap_block_valid){
        return (yap_expr){ .kind = yap_expr_error };
    }

    if (darr_len(block.statements) == 0){
        yap_build_push_error(src, bnode->loc, "Block expression cannot be empty");
        return (yap_expr){ .kind = yap_expr_error };
    }

    yap_statement last = block.statements[darr_len(block.statements) - 1];
    if (last.kind != yap_statement_expr){
        yap_build_push_error(src, bnode->loc,
            "Block expression's last statement must be an expression");
        return (yap_expr){ .kind = yap_expr_error };
    }

    return (yap_expr){
        .kind        = yap_expr_block,
        .block       = yap_ctx_one_cpy(ctx, block),
        .type        = last.expr.type,
        .is_lvalue   = last.expr.is_lvalue,
        .is_comptime = last.expr.is_comptime
    };
}

yap_expr yap_build_module_access_expr(yap_source* src, yap_module_access_node* ma){
    yap_ctx* ctx = src->ctx;

    if (!ma->module.value || !ma->field.value){
        yap_build_push_error(src, ma->loc, "Invalid module access expression");
        return (yap_expr){ .kind = yap_expr_error };
    }

    yap_module* mod = yap_ctx_get_module(ctx, ma->module.value);
    if (!mod){
        yap_build_push_error(src, ma->loc, "Unknown module '%s'", ma->module.value);
        return (yap_expr){ .kind = yap_expr_error };
    }

    if (!mod->scope){
        yap_build_push_error(src, ma->loc, "Module '%s' has no scope", ma->module.value);
        return (yap_expr){ .kind = yap_expr_error };
    }

    const yap_var* var = yap_scope_get_var(mod->scope, ma->field.value);
    if (!var){
        yap_build_push_error(src, ma->loc, "'%s' is not a member of module '%s'",
            ma->field.value, ma->module.value);
        return (yap_expr){ .kind = yap_expr_error };
    }

    char* emit_name = var->name;
    if (mod->prefix && mod->prefix[0]) {
        emit_name = yap_ctx_strus_newf(ctx, "%s%s", mod->prefix, var->name);
    }

    return (yap_expr){
        .kind        = yap_expr_var,
        .var_name    = emit_name,
        .type        = var->type,
        .is_lvalue   = true,
        .is_comptime = false
    };
}

/* ----------------------------------------------------------------
 *  Macro expansion (comptime execution)
 * ---------------------------------------------------------------- */

static bool yap_is_comptime_type(yap_ctx* ctx, yap_type_id id){
    return id == ctx->yexpr_type_id
        || id == ctx->ytype_type_id
        || id == ctx->ystmt_type_id
        || id == ctx->yfn_type_id
        || id == ctx->yexprblueprint_type_id
        || id == ctx->void_type_id;
}

/* Matches yap_gen_name_type_combo's yap_type_slice codegen exactly
 * ('struct { T* data; unsigned long len; }') -- the runtime shape a real
 * yap-level slice value has once compiled. Macro-call args are marshalled to
 * TCC through a uniform void*-per-slot dispatch (see the switch at the
 * bottom of this function), which can only ever pass one pointer-sized
 * value per slot -- a genuine 2-word by-value slice can't cross that
 * boundary directly, so blob-literal ([a,b,c]) args build one of these on
 * the arena and pass its *address*, and the callee's declared param type
 * must be a pointer to the slice (e.g. 'yExprList@'), not the slice itself. */
typedef struct { void* data; unsigned long len; } yap_yexpr_slice;

static void* yap_exec_macro_call(yap_source* src, yap_macro_call_node* call, yap_type_id* out_ret_type){
    yap_ctx* ctx = src->ctx;
    *out_ret_type = 0;

    if (!call->caller){
        yap_build_push_error(src, call->loc, "Missing macro caller");
        return NULL;
    }

    /* Receiver-dispatched macro call: 'recv:name:(args)' parses 'recv:name'
     * as a method_access caller (macro_caller already allows this in the
     * grammar). A macro-method association is never inferred from the
     * receiver's type/mangled name -- it's looked up directly in
     * ctx->macro_methods, a table populated only by explicit
     * yapi->register_macro_method(owner, name, backing_fn) calls (e.g. one
     * arr(T) instantiation registering "for" for its own concrete `res`,
     * right where it's built). That table lives entirely separately from
     * where real per-instantiation methods (global_scope, mangled
     * "OwnerName_name") and ordinary bare-name macros (their own module's
     * scope) live, so a macro method and a same-named real method can never
     * collide even in principle -- no shape-based guessing needed. The
     * receiver becomes an implicit first argument (an AST-node value,
     * exactly like a '#expr' param), ahead of whatever the call site wrote. */
    yap_expr receiver = {0};
    bool has_receiver = false;
    yap_expr caller;

    if (call->caller->kind == yap_expr_method_access){
        yap_method_access_node* ma = &call->caller->method_access;
        receiver = yap_build_expr(src, ma->caller);
        if (receiver.kind == yap_expr_error) return NULL;
        has_receiver = true;

        yap_macro_method_entry* found = NULL;
        for_darr(mi, entry, ctx->macro_methods){
            if (entry.owner_type == receiver.type && strus_eq(entry.name, ma->name.value)){
                found = &ctx->macro_methods[mi];
                break;
            }
        }
        if (!found){
            char* type_str = yap_ctx_type_id_to_string(ctx, receiver.type);
            yap_build_push_error(src, call->loc,
                "No method-macro '%s' registered for receiver type '%s'",
                ma->name.value, type_str);
            free(type_str);
            return NULL;
        }
        caller = (yap_expr){
            .kind = yap_expr_var, .var_name = found->emit_name, .type = found->func_type,
            .is_lvalue = true, .is_comptime = false
        };
    } else {
        caller = yap_build_expr(src, call->caller);
        if (caller.kind == yap_expr_error) return NULL;
    }

    yap_type* func_type = yap_ctx_get_type(ctx, caller.type);
    if (!func_type || func_type->kind != yap_type_func){
        yap_build_push_error(src, call->loc, "Macro caller is not a function");
        return NULL;
    }

    *out_ret_type = func_type->func.return_type;
    if (*out_ret_type == ctx->yident_type_id){
        yap_build_push_error(src, call->loc,
            "Macro function cannot return yIdent ; identifiers can only come from +ident or yapi->uniq_name()");
        return NULL;
    }
    if (!yap_is_comptime_type(ctx, *out_ret_type)){
        yap_build_push_error(src, call->loc,
            "Macro function must return a comptime type (yExpr, yType, yStmt, yFn)");
        return NULL;
    }

    darr(yap_type_id) expected_args = func_type->func.args;
    unsigned int expected_count = darr_len(expected_args);
    unsigned int receiver_offset = has_receiver ? 1 : 0;
    if (has_receiver && (expected_count < 1 || expected_args[0] != ctx->yexpr_type_id)){
        yap_build_push_error(src, call->loc,
            "Method-macro '%s' must declare a receiver parameter of type yExpr to be called as 'recv:%s:(...)'",
            call->caller->method_access.name.value, call->caller->method_access.name.value);
        return NULL;
    }
    unsigned int provided_count = darr_len(call->params) + receiver_offset;

    /* A trailing yExprList param may be omitted entirely at the call site
     * (e.g. `print:(c"hi")` instead of `print:(c"hi", [])`) ; it then
     * defaults to an empty list, so arg_ptrs must have room for it even
     * when provided_count is one short. */
    unsigned int alloc_count = (provided_count > expected_count) ? provided_count : expected_count;
    void** arg_ptrs = NULL;
    if (alloc_count > 0)
        arg_ptrs = calloc(alloc_count, sizeof(void*));

    if (has_receiver)
        arg_ptrs[0] = yap_ctx_one_cpy(ctx, receiver);

    for (unsigned int i = 0; i < darr_len(call->params); i++){
        yap_macro_param_node* param = &call->params[i];
        unsigned int slot = i + receiver_offset;
        yap_type_id expected_arg_type = (slot < expected_count) ? expected_args[slot] : 0;
        bool arg_is_type = (expected_arg_type == ctx->ytype_type_id);

        if (arg_is_type && param->kind == yap_macro_param_unnamed){
            yap_type_id tid = 0;
            if (param->expr->kind == yap_expr_var && param->expr->var.value){
                tid = yap_ctx_get_type_id_by_name(ctx, param->expr->var.value);
            }
            if (!tid){
                yap_build_push_error(src, param->loc, "Cannot resolve type argument");
                free(arg_ptrs); return NULL;
            }
            arg_ptrs[slot] = (void*)(uintptr_t)tid;
            continue;
        }

        switch (param->kind){
            case yap_macro_param_unnamed: {
                yap_expr built = yap_build_expr(src, param->expr);
                if (built.kind == yap_expr_error){ free(arg_ptrs); return NULL; }
                if (built.kind == yap_expr_literal && built.literal.kind == yap_literal_numerical){
                    if (strchr(built.literal.text, '.'))
                        { double v = atof(built.literal.text); double* p = yap_ctx_one(ctx, double); *p = v; arg_ptrs[slot] = p; }
                    else
                        { long v = atol(built.literal.text); arg_ptrs[slot] = (void*)(uintptr_t)v; }
                } else if (built.kind == yap_expr_literal && built.literal.kind == yap_literal_string){
                    arg_ptrs[slot] = (void*)built.literal.text;
                } else if (built.kind == yap_expr_literal && built.literal.kind == yap_literal_cstring){
                    arg_ptrs[slot] = (void*)built.literal.text;
                } else if (built.kind == yap_expr_literal && built.literal.kind == yap_literal_bool){
                    arg_ptrs[slot] = (void*)(uintptr_t)(strus_eq(built.literal.text, "true") ? 1 : 0);
                } else if (built.kind == yap_expr_literal && built.literal.kind == yap_literal_blob){
                    /* A blob literal `[a, b, c]` is built (yap_build_blob below in
                     * the literal-build path) as a darr(yap_expr) of already
                     * type-checked elements ; a contiguous array of *structs*.
                     * A yExpr value is an 8-byte opaque handle (a pointer to one
                     * such struct, per its "void*" c_name), so the slice's data
                     * array must hold one pointer per element, not the structs
                     * themselves ; build that indirection here. Built as the real
                     * slice ABI shape (see yap_yexpr_slice) and passed by
                     * address, since the generic void*-per-slot dispatch below
                     * can't carry a 2-word by-value struct directly ; the
                     * callee's declared param type must be 'yExprList@', not
                     * bare 'yExprList'. */
                    unsigned int blob_count = built.literal.blob.field_count;
                    yap_expr** elem_ptrs = blob_count
                        ? (yap_expr**)yap_ctx_one_raw(ctx, sizeof(yap_expr*) * blob_count)
                        : NULL;
                    for (unsigned int bi = 0; bi < blob_count; bi++)
                        elem_ptrs[bi] = &built.literal.blob.elements[bi];
                    yap_yexpr_slice* slice = yap_ctx_one(ctx, yap_yexpr_slice);
                    slice->data = elem_ptrs;
                    slice->len  = blob_count;
                    arg_ptrs[slot] = slice;
                } else {
                    yap_build_push_error(src, param->loc,
                        "Comptime call argument must be a literal (use #expr to pass as AST node, or [..] for a yExprList)");
                    free(arg_ptrs); return NULL;
                }
                break;
            }
            case yap_macro_param_ast: {
                yap_expr built = yap_build_expr(src, param->expr);
                if (built.kind == yap_expr_error){ free(arg_ptrs); return NULL; }
                arg_ptrs[slot] = yap_ctx_one_cpy(ctx, built);
                break;
            }
            case yap_macro_param_named: {
                if (!param->named.value){
                    yap_build_push_error(src, param->loc, "Missing value in named macro parameter");
                    free(arg_ptrs); return NULL;
                }
                yap_expr built = yap_build_expr(src, param->named.value);
                if (built.kind == yap_expr_error){ free(arg_ptrs); return NULL; }
                arg_ptrs[slot] = yap_ctx_one_cpy(ctx, built);
                break;
            }
            case yap_macro_param_ident_add: {
                char* name = param->ident_add.value;
                if (!name){ free(arg_ptrs); return NULL; }
                const yap_var* existing = yap_scope_get_var_recursive(
                    yap_ctx_current_scope(ctx), name);
                if (existing){
                    yap_build_push_error(src, param->loc,
                        "Identifier '%s' is already in scope (used with +ident)", name);
                    free(arg_ptrs); return NULL;
                }
                arg_ptrs[slot] = yap_ctx_strus_cpy(ctx, name);
                break;
            }
            case yap_macro_param_mut: {
                yap_expr built = yap_build_expr(src, param->mut_expr);
                if (built.kind == yap_expr_error){ free(arg_ptrs); return NULL; }
                if (!built.is_lvalue){
                    yap_build_push_error(src, param->loc, "Mutable macro parameter must be an lvalue");
                    free(arg_ptrs); return NULL;
                }
                arg_ptrs[slot] = yap_ctx_one_cpy(ctx, built);
                break;
            }
            case yap_macro_param_statement: {
                /* A raw statement/block macro arg (e.g. the '{ }' body of
                 * `a:for:(+i, +v, { ... });`) can't be built now -- it may
                 * reference hygienic idents (+i/+v) the macro itself hasn't
                 * introduced yet (that only happens once the macro's
                 * returned yStmt is spliced back in, see
                 * yap_resolve_deferred_fragments below). Pass it through as
                 * an opaque yStmt sentinel carrying the unbuilt parse node;
                 * the macro can embed it anywhere in its own returned tree
                 * (e.g. via a stmt${ }'s :fill_stmt()) with no special
                 * awareness needed. */
                yap_statement* deferred = yap_ctx_one(ctx, yap_statement);
                *deferred = (yap_statement){
                    .kind = yap_statement_deferred,
                    .deferred_raw = param->statement,
                    .loc = param->loc
                };
                arg_ptrs[slot] = deferred;
                break;
            }
            default:
                yap_build_push_error(src, param->loc, "Unsupported macro parameter kind");
                free(arg_ptrs); return NULL;
        }
    }

    if (provided_count != expected_count){
        yap_type* last_expected = (expected_count > 0) ? yap_ctx_get_type(ctx, expected_args[expected_count - 1]) : NULL;
        // Structural check: 'yExpr[]@' and 'yExprList@' resolve to the same anonymous slice type
        yap_type* last_pointee = (last_expected && last_expected->kind == yap_type_ptr)
            ? yap_ctx_get_type(ctx, last_expected->pointer_type) : NULL;
        bool last_is_yexprlist_ptr = last_pointee && last_pointee->kind == yap_type_slice
            && last_pointee->slice.element_type == ctx->yexpr_type_id;
        bool defaulted_empty_list = expected_count > 0
            && provided_count == expected_count - 1
            && last_is_yexprlist_ptr;
        if (defaulted_empty_list){
            arg_ptrs[expected_count - 1] = yap_ctx_one_cpy(ctx, ((yap_yexpr_slice){0}));
            provided_count = expected_count;
        } else {
            yap_build_push_error(src, call->loc,
                "Macro argument count mismatch: expected %u, got %u",
                expected_count, provided_count);
            free(arg_ptrs); return NULL;
        }
    }

    if (!ctx->ensure_symbol){
        yap_build_push_error(src, call->loc, "No backend symbol resolver available for macro execution");
        free(arg_ptrs); return NULL;
    }

    char* func_name = NULL;
    if (caller.kind == yap_expr_var && caller.var_name)
        func_name = caller.var_name;

    if (!func_name){
        yap_build_push_error(src, call->loc, "Cannot resolve macro function name");
        free(arg_ptrs); return NULL;
    }

    void* sym = ctx->ensure_symbol(ctx, func_name);
    if (!sym){
        yap_build_push_error(src, call->loc,
            "Failed to resolve macro function '%s' in TCC", func_name);
        free(arg_ptrs); return NULL;
    }

    yap_log("Executing macro '%s' with %u args", func_name, provided_count);
    if (ctx->set_macro_name)
        ctx->set_macro_name(func_name);
    if (ctx->set_macro_loc){
        yap_source* ct_src = yap_ctx_one(ctx, yap_source);
        *ct_src = (yap_source){
            .kind = yap_source_comptime,
            .identity = func_name,
            .parent = src,
            .label = func_name,
            .origin = NULL,
            .content = src->content,
            .sz = src->sz,
            .ctx = ctx,
            .import_loc = call->loc,
        };
        ctx->set_macro_loc(ct_src, call->loc);
    }

    void* result = NULL;
    switch (provided_count){
        case 0: { typedef void* (*fn0_t)(void);                             result = ((fn0_t)sym)(); break; }
        case 1: { typedef void* (*fn1_t)(void*);                            result = ((fn1_t)sym)(arg_ptrs[0]); break; }
        case 2: { typedef void* (*fn2_t)(void*, void*);                     result = ((fn2_t)sym)(arg_ptrs[0], arg_ptrs[1]); break; }
        case 3: { typedef void* (*fn3_t)(void*, void*, void*);              result = ((fn3_t)sym)(arg_ptrs[0], arg_ptrs[1], arg_ptrs[2]); break; }
        case 4: { typedef void* (*fn4_t)(void*, void*, void*, void*);       result = ((fn4_t)sym)(arg_ptrs[0], arg_ptrs[1], arg_ptrs[2], arg_ptrs[3]); break; }
        default:
            yap_build_push_error(src, call->loc,
                "Too many macro arguments (max 4 supported currently, got %u)", provided_count);
            free(arg_ptrs); return NULL;
    }

    if (ctx->pop_macro_loc)
        ctx->pop_macro_loc();

    free(arg_ptrs);
    return result;
}

static yap_expr yap_build_macro_expr(yap_source* src, yap_macro_call_node* call){
    yap_ctx* ctx = src->ctx;
    yap_type_id ret_type_id = 0;
    void* result = yap_exec_macro_call(src, call, &ret_type_id);

    if (!result){
        if (darr_len(ctx->errors) == 0)
            yap_build_push_error(src, call->loc, "Macro returned NULL");
        return (yap_expr){ .kind = yap_expr_error };
    }

    if (ret_type_id == ctx->yexpr_type_id){
        yap_expr* expanded = (yap_expr*)result;
        yap_log("Macro expanded to expression (kind=%d)", expanded->kind);
        yap_expr ret = *expanded;
        if (ret.kind == yap_expr_var && ret.var_name && !ret.type){
            const yap_var* var = yap_scope_get_var_recursive(
                yap_ctx_current_scope(ctx), ret.var_name);
            if (var){
                ret.type = var->type;
            } else {
                yap_build_push_error(src, call->loc,
                    "Macro produced reference to undefined variable '%s'", ret.var_name);
                return (yap_expr){ .kind = yap_expr_error };
            }
        }
        return ret;
    }

    if (ret_type_id == ctx->ytype_type_id){
        yap_build_push_error(src, call->loc,
            "Macro returns yType but was used in expression position; use it in type position instead");
        return (yap_expr){ .kind = yap_expr_error };
    }

    if (ret_type_id == ctx->ystmt_type_id){
        yap_build_push_error(src, call->loc,
            "Macro returns yStmt but was used in expression position; use it as a statement instead");
        return (yap_expr){ .kind = yap_expr_error };
    }

    yap_build_push_error(src, call->loc, "Unsupported macro return type in expression position");
    return (yap_expr){ .kind = yap_expr_error };
}

static yap_type_id yap_build_macro_type(yap_source* src, yap_macro_call_node* call){
    yap_ctx* ctx = src->ctx;
    yap_type_id ret_type_id = 0;
    void* result = yap_exec_macro_call(src, call, &ret_type_id);

    if (!result){
        if (darr_len(ctx->errors) == 0)
            yap_build_push_error(src, call->loc, "Macro returned NULL");
        return 0;
    }

    if (ret_type_id == ctx->ytype_type_id){
        yap_type_id tid = (yap_type_id)(uintptr_t)result;
        yap_log("Macro expanded to type id=%u", tid);
        return tid;
    }

    yap_build_push_error(src, call->loc, "Macro in type position must return yType");
    return 0;
}

yap_type_id yap_build_type_from_type_node(yap_source* src, yap_type_node* tnode){
    yap_ctx* ctx = src->ctx;
    if (!tnode) return 0;

    switch (tnode->kind){
        case yap_type_node_error:
            return 0;

        case yap_type_node_identifier: {
            if (!tnode->identifier.value) return 0;
            yap_type_id id = yap_ctx_get_type_id_by_name(ctx, tnode->identifier.value);
            if (!id){
                yap_build_push_error(src, tnode->loc, "Unknown type '%s'", tnode->identifier.value);
                return 0;
            }
            return id;
        }

        case yap_type_node_pointer: {
            yap_type_id subtype = yap_build_type_from_type_node(src, tnode->pointer_subtype);
            if (!subtype) return 0;
            return yap_ctx_get_pointer_of_type_id(ctx, subtype);
        }

        case yap_type_node_const: {
            yap_type_id inner = yap_build_type_from_type_node(src, tnode->const_subtype);
            if (!inner) return 0;
            yap_type* inner_type = yap_ctx_get_type(ctx, inner);
            if (!inner_type) return 0;
            yap_type t = *inner_type;
            t.is_const = true;
            return yap_ctx_insert_type_if_not_exists(ctx, t);
        }

        case yap_type_node_paren: {
            return yap_build_type_from_type_node(src, tnode->paren_subtype);
        }

        case yap_type_node_function: {
            yap_type_id return_type = ctx->void_type_id;
            if (tnode->func_type.return_type){
                return_type = yap_build_type_from_type_node(src, tnode->func_type.return_type);
                if (!return_type) return 0;
            }
            darr(yap_type_id) param_types = yap_ctx_darr_new(ctx, yap_type_id,
                .cap = darr_len(tnode->func_type.params), .len = 0);
            for_darr(pi, pnode, tnode->func_type.params){
                yap_type_id pid = yap_build_type_from_type_node(src, &pnode);
                if (!pid) return 0;
                darr_push(param_types, pid);
            }
            yap_type t = {
                .kind = yap_type_func,
                .func = { .args = param_types, .return_type = return_type },
                .is_const = false
            };
            return yap_ctx_insert_type_if_not_exists(ctx, t);
        }

        case yap_type_node_anon_struct: {
            darr(yap_struct_field) fields = yap_ctx_darr_new(ctx, yap_struct_field,
                .cap = darr_len(tnode->anon_struct.fields), .len = 0);
            for_darr(fi, fv, tnode->anon_struct.fields){
                darr_push(fields, yap_build_struct_field(src, &fv));
            }
            yap_anon_id anon_id = ctx->anon_id++;
            char* c_name = yap_ctx_get_anon_name(ctx, "struct", anon_id);
            yap_type t = {
                .kind = yap_type_struct,
                .structure = { .fields = fields, .c_name = c_name, .name = NULL },
                .is_const = false
            };
            return yap_ctx_insert_type_if_not_exists(ctx, t);
        }

        case yap_type_node_anon_enum: {
            darr(yap_enum_variant) variants = yap_ctx_darr_new(ctx, yap_enum_variant,
                .cap = darr_len(tnode->anon_enum.variants), .len = 0);
            for_darr(vi, ev, tnode->anon_enum.variants){
                darr_push(variants, yap_build_enum_variant(src, &ev));
            }
            yap_anon_id anon_id = ctx->anon_id++;
            char* c_name = yap_ctx_get_anon_name(ctx, "enum", anon_id);
            yap_type t = {
                .kind = yap_type_enum,
                .enumeration = { .variants = variants, .c_name = c_name, .name = NULL },
                .is_const = false
            };
            return yap_ctx_insert_type_if_not_exists(ctx, t);
        }

        case yap_type_node_anon_union: {
            darr(yap_struct_field) variants = yap_ctx_darr_new(ctx, yap_struct_field,
                .cap = darr_len(tnode->anon_union.variants), .len = 0);
            for_darr(vi, uv, tnode->anon_union.variants){
                darr_push(variants, yap_build_struct_field(src, &uv));
            }
            yap_anon_id anon_id = ctx->anon_id++;
            char* c_name = yap_ctx_get_anon_name(ctx, "union", anon_id);
            yap_type t = {
                .kind = yap_type_union,
                .uni = { .variants = variants, .c_name = c_name, .name = NULL },
                .is_const = false
            };
            return yap_ctx_insert_type_if_not_exists(ctx, t);
        }
        case yap_type_node_array: {
            yap_type_id elem = yap_build_type_from_type_node(src, tnode->array_type.element_type);
            if (!elem) return 0;
            size_t size = 0;
            if (tnode->array_type.size_expr &&
                tnode->array_type.size_expr->kind == yap_expr_literal &&
                tnode->array_type.size_expr->literal.kind == yap_literal_numerical) {
                size = (size_t)atol(tnode->array_type.size_expr->literal.numerical);
            } else {
                yap_build_push_error(src, tnode->loc, "Array size must be an integer literal");
                return 0;
            }
            yap_type t = {
                .kind = yap_type_array,
                .array = { .element_type = elem, .size = size },
                .is_const = false
            };
            return yap_ctx_insert_type_if_not_exists(ctx, t);
        }

        case yap_type_node_slice: {
            yap_type_id elem = yap_build_type_from_type_node(src, tnode->slice_subtype);
            if (!elem) return 0;
            yap_type t = {
                .kind = yap_type_slice,
                .slice = { .element_type = elem },
                .is_const = false
            };
            return yap_ctx_insert_type_if_not_exists(ctx, t);
        }

        case yap_type_node_macro: {
            return yap_build_macro_type(src, &tnode->macro_call);
        }
    }
    return 0;
}

/* Backward-compatible shim ; converts identifier node to type_node */
yap_type_id yap_build_type_from_node(yap_source* src, yap_identifier_node* tnode){
    if (!tnode || !tnode->value) return 0;
    yap_ctx* ctx = src->ctx;
    return yap_ctx_get_type_id_by_name(ctx, tnode->value);
}

/* ----------------------------------------------------------------
 *  Helpers
 * ---------------------------------------------------------------- */

yap_func_arg yap_build_func_arg(yap_source* src, yap_func_arg_node* anode){
    yap_ctx* ctx = src->ctx;

    if (!anode->is_valid)
        return (yap_func_arg){ .kind = yap_func_arg_error };

    if (!anode->name.value){
        yap_build_push_error(src, anode->loc, "Missing argument name");
        return (yap_func_arg){ .kind = yap_func_arg_error };
    }

    yap_type_id type = ctx->untyped_int_type_id;
    if (anode->has_type && anode->type_node){
        type = yap_build_type_from_type_node(src, anode->type_node);
        if (!type){
            yap_build_push_error(src, anode->loc,
                "Invalid argument type for '%s'", anode->name.value);
            return (yap_func_arg){ .kind = yap_func_arg_error };
        }
    }

    yap_expr default_val = {0};
    if (anode->has_default){
        default_val = yap_build_expr(src, &anode->default_value);
        if (default_val.kind == yap_expr_error){
            yap_build_push_error(src, anode->loc,
                "Invalid default value for argument '%s'", anode->name.value);
            return (yap_func_arg){ .kind = yap_func_arg_error };
        }
    }

    return (yap_func_arg){
        .kind          = yap_func_arg_valid,
        .name          = anode->name.value,
        .type          = type,
        .default_value = default_val
    };
}

yap_struct_field yap_build_struct_field(yap_source* src, yap_var_decl_node* vn){
    yap_ctx* ctx = src->ctx;

    // Nameless fields: anonymous structs/unions embed their members (C11-style).
    // Nameless enums are illegal ; they declare nothing in C.
    if (!vn->name.value){
        if (!vn->has_type || !vn->type_node){
            yap_build_push_error(src, vn->loc, "Missing field name");
            return (yap_struct_field){ .kind = yap_struct_field_error };
        }
    }

    yap_type_id type = ctx->internal_error_type_id;
    if (vn->has_type && vn->type_node){
        type = yap_build_type_from_type_node(src, vn->type_node);
        if (!type){
            yap_build_push_error(src, vn->loc,
                "Invalid type for field '%s'", vn->name.value);
            return (yap_struct_field){ .kind = yap_struct_field_error };
        }
    }

    yap_expr* default_expr = NULL;
    if (vn->has_init){
        yap_expr e = yap_build_expr(src, &vn->init);
        if (e.kind != yap_expr_error){
            default_expr = yap_ctx_one_cpy(ctx, e);
        }
    }

    return (yap_struct_field){
        .kind          = yap_struct_field_valid,
        .name          = vn->name.value,
        .type          = type,
        .default_value = default_expr
    };
}

yap_enum_variant yap_build_enum_variant(yap_source* src, yap_enum_variant_node* ev){
    yap_ctx* ctx = src->ctx;

    if (!ev->name.value){
        yap_build_push_error(src, ev->loc, "Missing enum variant name");
        return (yap_enum_variant){0};
    }

    yap_enum_variant res = { .name = ev->name.value, .value = NULL };
    if (ev->has_value){
        yap_expr e = yap_build_expr(src, &ev->value);
        if (e.kind != yap_expr_error){
            res.value = yap_ctx_one_cpy(ctx, e);
        }
    }
    return res;
}

yap_block yap_build_block(yap_source* src, yap_block_node* bnode){
    yap_ctx* ctx = src->ctx;

    uint32_t count = darr_len(bnode->statements);
    /* Built into a plain (malloc-backed, growable) darr first, NOT the usual
     * yap_ctx_darr_new/arena-backed one: a macro-call statement can flatten
     * into MORE entries than `count` (see below), and darr_push's growth path
     * (_darr_grow_if_needed -> darr_resize -> realloc) is only safe for a
     * standalone malloc'd block -- an arena-backed darr's storage is a slice
     * of a much larger quake_alloc chunk, so pushing past its initial .cap
     * silently corrupts adjacent arena memory instead of growing (confirmed
     * via a real heap-corruption crash while building this). The final,
     * correctly-sized copy below is what actually becomes yap_block.statements
     * (arena-owned, matching every other use of this struct). */
    darr(yap_statement) building = darr_new(yap_statement, .cap = count, .len = 0);

    yap_ctx_push_new_scope(ctx);

    for_darr(i, stmt_node, bnode->statements){
        yap_statement st = yap_build_statement(src, &stmt_node);
        if (st.kind == yap_statement_error){
            //Commented out because all statement errors are already reported in yap_build_statement
            //yap_build_push_error(src, stmt_node.loc, "Invalid statement in block");
            yap_ctx_pop_scope(ctx);
            darr_free(building);
            return (yap_block){ .kind = yap_block_error };
        }
        /* A bare macro-call statement (e.g. `declare_var:(...);`) that
         * expanded to a yStmt block (a var_decl needing a second statement
         * for its initializer -- see bp_wrap_stmts_in_block, stmt${ }'s
         * var_decl support) must NOT be nested as a literal C block: codegen
         * (yap_gen_block) emits a real '{ ... }' scope for any
         * yap_statement_block, which would make a var_decl introduced inside
         * it invisible the instant that block closes -- defeating the entire
         * point of a macro-introduced variable (a BARE var_decl macro like
         * declare_int already works correctly because its result, with no
         * wrapping block, splices directly into the caller's own statement
         * list with no extra scope). Flatten: splice the block's top-level
         * statements directly into THIS block's list instead of nesting them.
         * A literal source-level '{ }' the user actually wrote is unaffected
         * -- that parses as yap_statement_block, not yap_statement_expr
         * wrapping a yap_expr_macro, so it never reaches this branch. */
        if (stmt_node.kind == yap_statement_expr && stmt_node.expr.kind == yap_expr_macro &&
            st.kind == yap_statement_block && st.block.kind == yap_block_valid){
            for_darr(j, inner, st.block.statements) darr_push(building, inner);
        } else {
            darr_push(building, st);
        }
    }

    yap_ctx_pop_scope(ctx);

    uint32_t final_count = darr_len(building);
    darr(yap_statement) statements = yap_ctx_darr_new(ctx, yap_statement,
        .cap = final_count, .len = final_count, .src = building);
    darr_free(building);

    return (yap_block){ .kind = yap_block_valid, .statements = statements };
}
