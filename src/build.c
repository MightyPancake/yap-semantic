#include "yap_semantic.h"
#include "build.h"

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
 */
static void yap_build_source_postorder(yap_ctx* ctx, yap_source* src, darr(char*) visited_origins){
    if (!src || !src->source_node) return;

    // Skip if already visited (by origin)
    if (src->origin){
        for_darr(i, vo, visited_origins){
            if (strcmp(vo, src->origin) == 0) return;
        }
        darr_push(visited_origins, src->origin);
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
    yap_log("Building source: %s (%d declarations)", src->identity, darr_len(snode->declarations));

    /* Pass 1 – register top-level signatures for mutual recursion */
    for_darr(j, dnode, snode->declarations){
        yap_build_top_level_declaration(src, &dnode);
    }

    /* Pass 2 – build full declarations */
    for_darr(j, dnode, snode->declarations){
        yap_decl decl = yap_build_decl(src, &dnode);
        yap_log("Pass 2: built declaration kind=%d", decl.kind);
        darr_push(ctx->semantic_decls, decl);
    }
}

/*
 * Top-level entry point.
 */
yap_ctx* yap_build(yap_ctx* ctx, yap_args args){
    (void)args;
    yap_log("\n\nPhase X: Semantic analysis (build)\n");

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
        if (imp.kind != yap_import_file) continue;
        yap_source* src = find_source_by_identity(ctx, imp.identity);
        if (!src){
            yap_log("Failed to find source for import '%s'", imp.identity);
            continue;
        }
        yap_build_source_postorder(ctx, src, visited_origins);
    }
    darr_free(visited_origins);

    return ctx;
}

void yap_build_top_level_declaration(yap_source* src, yap_decl_node* node){
    yap_ctx* ctx = src->ctx;

    switch (node->kind){
        case yap_decl_func: {
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
                .cap = darr_len(f->args), .len = 0);
            darr(char*) arg_names = yap_ctx_darr_new(ctx, char*,
                .cap = darr_len(f->args), .len = 0);

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

            yap_var func_var = { .name = f->name.value, .type = func_type_id };
            yap_ctx_push_var(ctx, func_var);
            yap_log("Pass 1: registered function '%s'", f->name.value);
            break;
        }
        case yap_decl_named_type:
            /* Named types are fully built in pass 2 */
            break;
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
        case yap_decl_func:
            res = yap_build_fn_decl(src, &node->func_decl);
            break;
        case yap_decl_named_type:
            res = yap_build_named_type_decl(src, &node->named_type_decl);
            break;
        case yap_decl_module_import:
        case yap_decl_file_import:
        case yap_decl_module_decl:
            break;
        default:
            yap_build_push_error(src, node->loc, "Unhandled declaration kind");
            break;
    }
    res.loc   = node->loc;
    res.range = node->loc.range;
    return res;
}

yap_decl yap_build_fn_decl(yap_source* src, yap_func_decl_node* fnode){
    yap_ctx* ctx = src->ctx;

    if (!fnode->name.value){
        yap_build_push_error(src, fnode->loc, "Missing function name");
        return (yap_decl){ .kind = yap_decl_error };
    }

    yap_log("Building function '%s'", fnode->name.value);

    const yap_var* func_var = yap_scope_get_var_recursive(
        yap_ctx_current_scope(ctx), fnode->name.value);

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
        .cap = darr_len(fnode->args), .len = 0);
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
        .kind      = yap_decl_func,
        .func_decl = (yap_func_decl){
            .name    = fnode->name.value,
            .args    = args,
            .ret_typ = fn_type.return_type,
            .body    = body
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
                    .kind    = yap_named_type_decl_union,
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
    yap_expr e = yap_build_expr(src, expr_node);
    if (e.kind == yap_expr_error)
        return (yap_statement){ .kind = yap_statement_error };
    return (yap_statement){ .kind = yap_statement_expr, .expr = e };
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
            // Check that the initializer is assignable to the declared type
            if (!yap_ctx_type_id_assignable(ctx, declared_type, init.type)){
                char* rhs_str = yap_ctx_type_id_to_string(ctx, init.type);
                char* lhs_str = yap_ctx_type_id_to_string(ctx, declared_type);
                yap_build_push_error(src, vnode->loc,
                    "Cannot initialize variable of type '%s' with value of type '%s'",
                    lhs_str, rhs_str);
                free(rhs_str);
                free(lhs_str);
                return (yap_statement){ .kind = yap_statement_error };
            }
        }
    } else if (vnode->has_init){
        init = yap_build_expr(src, &vnode->init);
        if (init.kind == yap_expr_error)
            return (yap_statement){ .kind = yap_statement_error };
        yap_type init_type = *yap_ctx_get_type(ctx, init.type);
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
        case yap_expr_assignment:    ret = yap_build_assignment_expr(src, &node->assignment);   break;
        case yap_expr_func_call:     ret = yap_build_func_call_expr(src, &node->func_call);     break;
        case yap_expr_cast:          ret = yap_build_cast_expr(src, &node->cast);              break;
        case yap_expr_at_op:         ret = yap_build_at_op_expr(src, &node->at_op);             break;
        case yap_expr_paren:         ret = yap_build_paren_expr(src, &node->paren);            break;
        case yap_expr_increment:     ret = yap_build_increment_expr(src, node->increment.expr); break;
        case yap_expr_decrement:     ret = yap_build_decrement_expr(src, node->decrement.expr); break;
        case yap_expr_ternary:       ret = yap_build_ternary_expr(src, &node->ternary);         break;
        case yap_expr_member_access: ret = yap_build_member_access_expr(src, &node->member_access); break;
        default:
            yap_build_push_error(src, node->loc, "Unhandled expression kind");
            break;
    }
    ret.loc   = node->loc;
    ret.range = node->loc.range;
    return ret;
}

yap_expr yap_build_literal_expr(yap_source* src, yap_literal_node* lit){
    yap_ctx* ctx = src->ctx;
    yap_expr res = { .kind = yap_expr_literal, .is_comptime = true, .is_lvalue = false };

    switch (lit->kind){
        case yap_literal_numerical: {
            // Detect float literals (contain a '.') vs integer literals
            bool is_float = strchr(lit->numerical, '.') != NULL;
            res.type = is_float ? ctx->untyped_float_type_id : ctx->untyped_int_type_id;
            res.literal = (yap_literal){ .kind = yap_literal_numerical, .text = lit->numerical };
            break;
        }
        case yap_literal_string:
            res.type = ctx->blob_type_id;
            res.literal = (yap_literal){ .kind = yap_literal_string, .text = lit->string.value };
            break;
        case yap_literal_bool:
            res.type = ctx->bool_type_id;
            res.literal = (yap_literal){ .kind = yap_literal_bool, .text = lit->numerical };
            break;
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

    return (yap_expr){
        .kind        = yap_expr_var,
        .var_name    = var->name,
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

    if (!strchr("+-*/%", bin->op)){
        yap_build_push_error(src, bin->loc, "Unsupported binary operator '%c'", bin->op);
        return (yap_expr){ .kind = yap_expr_error };
    }

    if (!yap_ctx_type_ids_eq(ctx, left.type, right.type)){
        yap_build_push_error(src, bin->loc, "Incompatible types in binary expression");
        return (yap_expr){ .kind = yap_expr_error };
    }

    yap_type_id result_type = yap_ctx_coerce_type_id_to_id(ctx, left.type);
    return (yap_expr){
        .kind     = yap_expr_bin,
        .bin_expr = (yap_bin_expr){
            .op    = bin->op,
            .left  = yap_ctx_one_cpy(ctx, left),
            .right = yap_ctx_one_cpy(ctx, right)
        },
        .type        = result_type,
        .is_comptime = left.is_comptime && right.is_comptime,
        .is_lvalue   = false
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

yap_expr yap_build_func_call_expr(yap_source* src, yap_func_call_node* call){
    yap_ctx* ctx = src->ctx;

    yap_expr func_expr = yap_build_expr(src, call->func);
    if (func_expr.kind == yap_expr_error)
        return (yap_expr){ .kind = yap_expr_error };

    yap_type* func_type = yap_ctx_get_type(ctx, func_expr.type);
    if (!func_type || func_type->kind != yap_type_func){
        yap_build_push_error(src, call->loc, "Cannot call a non-function type");
        return (yap_expr){ .kind = yap_expr_error };
    }

    darr(yap_type_id) expected_args = func_type->func.args;
    darr(yap_expr)    params = yap_ctx_darr_new(ctx, yap_expr,
        .cap = darr_len(call->args), .len = 0);

    for_darr(pi, arg_node, call->args){
        yap_expr pe = yap_build_expr(src, &arg_node);
        if (pe.kind == yap_expr_error)
            return (yap_expr){ .kind = yap_expr_error };

        if (darr_len(params) < darr_len(expected_args)){
            yap_type_id expected = expected_args[darr_len(params)];
            if (!yap_ctx_type_id_compatible(ctx, pe.type, expected)){
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

/* ----------------------------------------------------------------
 *  Type building from node — recursive using yap_type_node
 * ---------------------------------------------------------------- */

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
            yap_anon_id anon_id = src->anon_id++;
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
            yap_anon_id anon_id = src->anon_id++;
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
            yap_anon_id anon_id = src->anon_id++;
            char* c_name = yap_ctx_get_anon_name(ctx, "union", anon_id);
            yap_type t = {
                .kind = yap_type_union,
                .uni = { .variants = variants, .c_name = c_name, .name = NULL },
                .is_const = false
            };
            return yap_ctx_insert_type_if_not_exists(ctx, t);
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

    if (!vn->name.value){
        yap_build_push_error(src, vn->loc, "Missing field name");
        return (yap_struct_field){ .kind = yap_struct_field_error };
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
            yap_build_push_error(src, stmt_node.loc, "Invalid statement in block");
            yap_ctx_pop_scope(ctx);
            return (yap_block){ .kind = yap_block_error };
        }
        darr_push(statements, st);
    }

    yap_ctx_pop_scope(ctx);
    return (yap_block){ .kind = yap_block_valid, .statements = statements };
}
