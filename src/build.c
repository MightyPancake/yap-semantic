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
    if (src->from_module_import) {
        yap_module* src_mod = yap_ctx_get_module(ctx, src->from_module_import);
        if (src_mod && src_mod->prefix && src_mod->prefix[0])
            decl_prefix = src_mod->prefix;
    } else {
        yap_module* cur_mod = yap_ctx_current_module(ctx);
        if (cur_mod && cur_mod->prefix[0])
            decl_prefix = cur_mod->prefix;
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
        if (ret_type_id == ctx->ystatement_type_id){
            yap_statement* expanded = (yap_statement*)result;
            yap_log("Macro expanded to statement (kind=%d)", expanded->kind);
            yap_statement ret = *expanded;
            if (ret.kind == yap_statement_var_decl && ret.var_decl.kind == yap_var_decl_valid){
                yap_ctx_push_var(ctx, ret.var_decl.var);
                yap_log("Macro introduced variable '%s' into scope", ret.var_decl.var.name);
            }
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
    if (expr.kind == yap_expr_unary){
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
        yap_type init_type = *init_t;
        yap_type var_type  = yap_ctx_coerce_type(ctx, init_type);
        var = (yap_var){
            .name = vnode->name.value,
            .type = yap_ctx_insert_type_if_not_exists(ctx, var_type)
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
        case yap_expr_increment:     ret = yap_build_increment_expr(src, node->increment.expr); break;
        case yap_expr_decrement:     ret = yap_build_decrement_expr(src, node->decrement.expr); break;
        case yap_expr_ternary:       ret = yap_build_ternary_expr(src, &node->ternary);         break;
        case yap_expr_member_access: ret = yap_build_member_access_expr(src, &node->member_access); break;
        case yap_expr_optional_member_access: ret = yap_build_optional_member_access_expr(src, &node->member_access); break;
        case yap_expr_deref:         ret = yap_build_deref_expr(src, &node->deref);              break;
        case yap_expr_index_access:  ret = yap_build_index_access_expr(src, &node->index_access);  break;
        case yap_expr_block:         ret = yap_build_block_expr(src, &node->block);                break;
        case yap_expr_module_access: ret = yap_build_module_access_expr(src, &node->module_access); break;
        case yap_expr_macro:         ret = yap_build_macro_expr(src, &node->macro_call); break;
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
    yap_module* cur_mod = yap_ctx_current_module(ctx);
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

yap_expr yap_build_bin_expr(yap_source* src, yap_bin_op_node* bin){
    yap_ctx* ctx = src->ctx;

    yap_expr left  = yap_build_expr(src, bin->left);
    yap_expr right = yap_build_expr(src, bin->right);
    if (left.kind == yap_expr_error || right.kind == yap_expr_error)
        return (yap_expr){ .kind = yap_expr_error };

    bool is_comparison = strchr("<>enlg", bin->op) != NULL;
    if (!strchr("+-*/%<>enlg", bin->op)){
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
    bool is_numeric = operand_type && operand_type->kind == yap_type_primitive
        && !yap_ctx_type_ids_eq(ctx, coerced, ctx->bool_type_id);
    if (!is_numeric){
        yap_build_push_error(src, un->loc, "Operand of unary '-' must be a numeric type");
        return (yap_expr){ .kind = yap_expr_error };
    }

    return (yap_expr){
        .kind        = yap_expr_unary,
        .subexpr     = yap_ctx_one_cpy(ctx, expr),
        .type        = expr.type,
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
    snprintf(a.op, sizeof(a.op), "%s", assign->op);

    return (yap_expr){
        .kind       = yap_expr_assignment,
        .assignment = a,
        .type       = left.type,
        .is_lvalue  = false
    };
}

/* Resolves 'recv:name' to the mangled "TypeName_name" function var and
 * builds the receiver expression that becomes the call's first argument. */
static yap_expr yap_build_method_callee(yap_source* src, yap_method_access_node* ma, yap_expr* out_receiver){
    yap_ctx* ctx = src->ctx;

    yap_expr receiver = yap_build_expr(src, ma->caller);
    if (receiver.kind == yap_expr_error)
        return (yap_expr){ .kind = yap_expr_error };

    yap_type* recv_type = yap_ctx_get_type(ctx, receiver.type);
    const char* owner_name = NULL;
    if (recv_type){
        if (recv_type->kind == yap_type_struct) owner_name = recv_type->structure.name;
        else if (recv_type->kind == yap_type_union) owner_name = recv_type->uni.name;
        else if (recv_type->kind == yap_type_enum) owner_name = recv_type->enumeration.name;
        /* Builtin opaque comptime types (yType, yStructT, yFuncT, ...) have no nominal
         * struct/union/enum name but can still have builtin methods registered under
         * "PrimitiveName_methodname" (see ctx.c) -- fall back to the primitive's own
         * declared name. Harmless for every other primitive since none has methods. */
        else if (recv_type->kind == yap_type_primitive) owner_name = recv_type->primitive.name;
    }
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
    unsigned int params_cap = darr_len(call->args) + (is_method_call ? 1 : 0);
    if (darr_len(expected_args) > params_cap)
        params_cap = darr_len(expected_args);
    darr(yap_expr)    params = yap_ctx_darr_new(ctx, yap_expr,
        .cap = params_cap, .len = 0);

    if (is_method_call){
        if (darr_len(expected_args) > 0 && !yap_ctx_type_id_compatible(ctx, receiver.type, expected_args[0])){
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
        darr_push(params, receiver);
    }

    for_darr(pi, arg_node, call->args){
        yap_expr pe = yap_build_expr(src, &arg_node);
        if (pe.kind == yap_expr_error)
            return (yap_expr){ .kind = yap_expr_error };

        if (darr_len(params) < darr_len(expected_args)){
            yap_type_id expected = expected_args[darr_len(params)];
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
        }
        darr_push(params, pe);
    }

    if (darr_len(params) < darr_len(expected_args)){
        yap_func_decl* found_decl = NULL;
        if (func_expr.kind == yap_expr_var && func_expr.var_name) {
            for (darr_size_t di = 0; di < darr_len(ctx->semantic_decls); di++){
                yap_decl* d = &ctx->semantic_decls[di];
                if ((d->kind == yap_decl_func_def || d->kind == yap_decl_func_decl)
                    && d->func_decl.name) {
                    bool match;
                    if (d->module_prefix && d->module_prefix[0]) {
                        size_t plen = strlen(d->module_prefix);
                        size_t nlen = strlen(d->func_decl.name);
                        size_t vlen = strlen(func_expr.var_name);
                        match = (vlen == plen + nlen)
                             && memcmp(func_expr.var_name, d->module_prefix, plen) == 0
                             && memcmp(func_expr.var_name + plen, d->func_decl.name, nlen) == 0;
                    } else {
                        match = strcmp(d->func_decl.name, func_expr.var_name) == 0;
                    }
                    if (match) {
                        found_decl = &d->func_decl;
                        break;
                    }
                }
            }
        }

        if (found_decl && found_decl->args) {
            unsigned int provided = darr_len(params);
            unsigned int expected = darr_len(expected_args);
            for (unsigned int i = provided; i < expected; i++){
                if (i < darr_len(found_decl->args)
                    && found_decl->args[i].kind == yap_func_arg_valid
                    && found_decl->args[i].default_value.kind != yap_expr_error) {
                    darr_push(params, found_decl->args[i].default_value);
                } else {
                    yap_build_push_error(src, call->loc,
                        "Missing argument %u with no default value", i + 1);
                    return (yap_expr){ .kind = yap_expr_error };
                }
            }
        } else {
            yap_build_push_error(src, call->loc,
                "Too few arguments: expected %u, got %u",
                (unsigned)darr_len(expected_args), (unsigned)darr_len(params));
            return (yap_expr){ .kind = yap_expr_error };
        }
    }

    if (darr_len(params) > darr_len(expected_args)){
        yap_build_push_error(src, call->loc,
            "Too many arguments: expected %u, got %u",
            (unsigned)darr_len(expected_args), (unsigned)darr_len(params));
        return (yap_expr){ .kind = yap_expr_error };
    }

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

yap_expr yap_build_increment_expr(yap_source* src, yap_expr_node* sub){
    yap_ctx* ctx = src->ctx;
    yap_expr expr = yap_build_expr(src, sub);
    if (expr.kind == yap_expr_error) return (yap_expr){ .kind = yap_expr_error };

    if (!expr.is_lvalue){
        yap_build_push_error(src, sub->loc, "Operand of increment must be an lvalue");
        return (yap_expr){ .kind = yap_expr_error };
    }

    return (yap_expr){
        .kind      = yap_expr_increment,
        .subexpr   = yap_ctx_one_cpy(ctx, expr),
        .type      = expr.type,
        .is_lvalue = false
    };
}

yap_expr yap_build_decrement_expr(yap_source* src, yap_expr_node* sub){
    yap_ctx* ctx = src->ctx;
    yap_expr expr = yap_build_expr(src, sub);
    if (expr.kind == yap_expr_error) return (yap_expr){ .kind = yap_expr_error };

    if (!expr.is_lvalue){
        yap_build_push_error(src, sub->loc, "Operand of decrement must be an lvalue");
        return (yap_expr){ .kind = yap_expr_error };
    }

    return (yap_expr){
        .kind      = yap_expr_decrement,
        .subexpr   = yap_ctx_one_cpy(ctx, expr),
        .type      = expr.type,
        .is_lvalue = false
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
        || id == ctx->ystatement_type_id
        || id == ctx->yfunc_type_id
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

    yap_expr caller = yap_build_expr(src, call->caller);
    if (caller.kind == yap_expr_error) return NULL;

    yap_type* func_type = yap_ctx_get_type(ctx, caller.type);
    if (!func_type || func_type->kind != yap_type_func){
        yap_build_push_error(src, call->loc, "Macro caller is not a function");
        return NULL;
    }

    *out_ret_type = func_type->func.return_type;
    if (*out_ret_type == ctx->yident_type_id){
        yap_build_push_error(src, call->loc,
            "Macro function cannot return yIdent — identifiers can only come from +ident or yapi->uniq_name()");
        return NULL;
    }
    if (!yap_is_comptime_type(ctx, *out_ret_type)){
        yap_build_push_error(src, call->loc,
            "Macro function must return a comptime type (yExpr, yType, yStatement, yFunc)");
        return NULL;
    }

    darr(yap_type_id) expected_args = func_type->func.args;
    unsigned int expected_count = darr_len(expected_args);
    unsigned int provided_count = darr_len(call->params);

    /* A trailing yExprList param may be omitted entirely at the call site
     * (e.g. `print:(c"hi")` instead of `print:(c"hi", [])`) — it then
     * defaults to an empty list, so arg_ptrs must have room for it even
     * when provided_count is one short. */
    unsigned int alloc_count = (provided_count > expected_count) ? provided_count : expected_count;
    void** arg_ptrs = NULL;
    if (alloc_count > 0)
        arg_ptrs = calloc(alloc_count, sizeof(void*));

    for (unsigned int i = 0; i < provided_count; i++){
        yap_macro_param_node* param = &call->params[i];
        yap_type_id expected_arg_type = (i < expected_count) ? expected_args[i] : 0;
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
            arg_ptrs[i] = (void*)(uintptr_t)tid;
            continue;
        }

        switch (param->kind){
            case yap_macro_param_unnamed: {
                yap_expr built = yap_build_expr(src, param->expr);
                if (built.kind == yap_expr_error){ free(arg_ptrs); return NULL; }
                if (built.kind == yap_expr_literal && built.literal.kind == yap_literal_numerical){
                    if (strchr(built.literal.text, '.'))
                        { double v = atof(built.literal.text); double* p = yap_ctx_one(ctx, double); *p = v; arg_ptrs[i] = p; }
                    else
                        { long v = atol(built.literal.text); arg_ptrs[i] = (void*)(uintptr_t)v; }
                } else if (built.kind == yap_expr_literal && built.literal.kind == yap_literal_string){
                    arg_ptrs[i] = (void*)built.literal.text;
                } else if (built.kind == yap_expr_literal && built.literal.kind == yap_literal_cstring){
                    arg_ptrs[i] = (void*)built.literal.text;
                } else if (built.kind == yap_expr_literal && built.literal.kind == yap_literal_bool){
                    arg_ptrs[i] = (void*)(uintptr_t)(strus_eq(built.literal.text, "true") ? 1 : 0);
                } else if (built.kind == yap_expr_literal && built.literal.kind == yap_literal_blob){
                    /* A blob literal `[a, b, c]` is built (yap_build_blob below in
                     * the literal-build path) as a darr(yap_expr) of already
                     * type-checked elements — a contiguous array of *structs*.
                     * A yExpr value is an 8-byte opaque handle (a pointer to one
                     * such struct, per its "void*" c_name), so the slice's data
                     * array must hold one pointer per element, not the structs
                     * themselves — build that indirection here. Built as the real
                     * slice ABI shape (see yap_yexpr_slice) and passed by
                     * address, since the generic void*-per-slot dispatch below
                     * can't carry a 2-word by-value struct directly — the
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
                    arg_ptrs[i] = slice;
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
                arg_ptrs[i] = yap_ctx_one_cpy(ctx, built);
                break;
            }
            case yap_macro_param_named: {
                if (!param->named.value){
                    yap_build_push_error(src, param->loc, "Missing value in named macro parameter");
                    free(arg_ptrs); return NULL;
                }
                yap_expr built = yap_build_expr(src, param->named.value);
                if (built.kind == yap_expr_error){ free(arg_ptrs); return NULL; }
                arg_ptrs[i] = yap_ctx_one_cpy(ctx, built);
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
                arg_ptrs[i] = yap_ctx_strus_cpy(ctx, name);
                break;
            }
            case yap_macro_param_mut: {
                yap_expr built = yap_build_expr(src, param->mut_expr);
                if (built.kind == yap_expr_error){ free(arg_ptrs); return NULL; }
                if (!built.is_lvalue){
                    yap_build_push_error(src, param->loc, "Mutable macro parameter must be an lvalue");
                    free(arg_ptrs); return NULL;
                }
                arg_ptrs[i] = yap_ctx_one_cpy(ctx, built);
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

    if (ret_type_id == ctx->ystatement_type_id){
        yap_build_push_error(src, call->loc,
            "Macro returns yStatement but was used in expression position; use it as a statement instead");
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

/* Backward-compatible shim — converts identifier node to type_node */
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
    // Nameless enums are illegal — they declare nothing in C.
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
    darr(yap_statement) statements = yap_ctx_darr_new(ctx, yap_statement,
        .cap = count, .len = 0);

    yap_ctx_push_new_scope(ctx);

    for_darr(i, stmt_node, bnode->statements){
        yap_statement st = yap_build_statement(src, &stmt_node);
        if (st.kind == yap_statement_error){
            //Commented out because all statement errors are already reported in yap_build_statement
            //yap_build_push_error(src, stmt_node.loc, "Invalid statement in block");
            yap_ctx_pop_scope(ctx);
            return (yap_block){ .kind = yap_block_error };
        }
        darr_push(statements, st);
    }

    yap_ctx_pop_scope(ctx);
    return (yap_block){ .kind = yap_block_valid, .statements = statements };
}
