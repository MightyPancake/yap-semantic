#include "parse.h"
#include "error.h"
#include "node.h"
#include "tree_sitter/api.h"
#include "ts_yap.h"
#include "utils/utils.h"
#include <stdint.h>
#include <string.h>

static void yap_push_parse_error(yap_source* src, TSNode node, const char* fmt, ...){
    if (!src || !src->ctx || !fmt) return;

    va_list ap;
    va_start(ap, fmt);
    char* msg = NULL;
    int fmt_res = vasprintf(&msg, fmt, ap);
    va_end(ap);

    if (fmt_res < 0 || !msg){
        msg = strus_copy("(failed to format parse error)");
    }

    yap_log("%s [type=%s%s]", msg, ts_node_type(node), ts_node_has_error(node) ? ", has_error" : "");
    yap_ctx_push_error(src->ctx, yap_node_error(src, node, msg));
    free(msg);
}

static void yap_log_node(yap_source* src, const char* prefix, TSNode node){
    char* node_val = yap_node_get_val_ctx(src, node);
    yap_log("%s type='%s' value='%s'%s", prefix, ts_node_type(node), node_val, ts_node_has_error(node) ? ", has error" : "");
}

static uint32_t yap_collect_ts_syntax_errors(yap_source* src, TSNode node){
    uint32_t errors_found = 0;

    if (ts_node_is_missing(node)){
        const char* expected = ts_node_type(node);
        char* msg = strus_newf("Syntax error: expected '%s'", expected);
        yap_ctx_push_error(src->ctx, yap_node_error(src, node, msg));
        free(msg);
        errors_found++;
    }else if (ts_node_is_error(node)){
        char* found = yap_node_get_val_ctx(src, node);
        if (found && found[0] != '\0'){
            char* msg = strus_newf("Syntax error near '%s'", found);
            yap_ctx_push_error(src->ctx, yap_node_error(src, node, msg));
            free(msg);
        }else{
            yap_ctx_push_error(src->ctx, yap_node_error(src, node, "Syntax error"));
        }
        errors_found++;
    }

    for_ts_children(node, child){
        errors_found += yap_collect_ts_syntax_errors(src, child);
    }

    return errors_found;
}

yap_ctx* yap_build_DELETE(yap_ctx* ctx, yap_args args){
    yap_parser* parser = yap_new_parser(ctx);
    if (darr_len(args.extra) == 0){
        printf("No source file provided\n");
        exit(1);
    }
    yap_log("Parsing entry file '%s'", darr_first(args.extra));
    yap_parser_parse_file(parser, darr_first(args.extra));
    yap_ctx* ret = parser->ctx;
    yap_free_parser(parser);
    return ret;
}

void yap_build_source_file(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    uint32_t syntax_errors = yap_collect_ts_syntax_errors(src, node);
    if (syntax_errors > 0){
        yap_log("Collected %u tree-sitter syntax error(s)", syntax_errors);
    }
    if (ts_node_null_or_error(node)){
        yap_push_parse_error(src, node, "Expected source file root");
        return;
    }
    const char* typ = ts_node_type(node);
    yap_log("Parsing source root of type '%s'", typ);
    if (strus_eq(typ, "source")){
        //Pass 1: Collect all declarations and register them in the context, this allows for recursive declarations and mutual recursion. We only collect function declarations, other declarations are not registered in the context and can not be mutually recursive.
        for_ts_named_children(node, n){
            yap_build_top_level_declaration(src, n);
        }
        //Pass 2: Do the actual parsing
        for_ts_named_children(node, n){
            yap_decl decl = yap_build_decl(src, n);
            if (ctx->current_module){
                darr_push(ctx->current_module->decls, decl);
            }
        }
    }else{
        yap_push_parse_error(src, node, "Expected source file root");
        return;
    }
}

void yap_build_top_level_declaration(yap_source* src, TSNode node){
    const char* typ = ts_node_type(node);
    yap_log("Parsing top-level declaration of type '%s'", typ);
    strus_switch(typ, "function_declaration"){
        yap_build_top_level_func_decl(src, node);
    }
}

void yap_build_top_level_func_decl(yap_source *src, TSNode node){
    yap_ctx* ctx = src->ctx;
    yap_node_untyped_guard(node, "Invalid function declaration", src);
    yap_node_field_by_name_var_untyped_check(node, name, "Missing function name", src);
    yap_node_field_by_name_var_untyped_check(node, body, "Missing function body", src);
    yap_node_field_var(return_type_node, node, "return_type");
    yap_node_field_var(return_var_decl_node, node, "return_var_decl");
    yap_node_field_var(args_node, node, "args");
    // yap_node_field_by_name_var_untyped_check(node, args, "Missing function arguments", src);
    // LOGGING
    yap_node_val_ctx(name_node);
    yap_node_start_point(node);
    char* pos_str = yap_pos_string(*src, node_start_point.row, node_start_point.column);
    yap_log("Parsing function named %s at %s", name_node_val, pos_str);
    free(pos_str);
    // END LOGGING
    yap_type_id return_type = ctx->void_type_id;
    if (!ts_node_null_or_error(return_type_node)){
        return_type = yap_build_type(src, return_type_node);
        if (return_type == 0){
            yap_push_parse_error(src, return_type_node, "Invalid return type in function declaration");
            return;
        }
    }else if (!ts_node_null_or_error(return_var_decl_node)){
        yap_func_arg return_arg = yap_build_fn_arg_from_var_decl(src, return_var_decl_node);
        if (return_arg.kind != yap_func_arg_valid){
            yap_push_parse_error(src, return_var_decl_node, "Invalid named return declaration in function declaration");
            return;
        }
        return_type = return_arg.type;
    }
    darr(yap_func_arg) args = yap_build_fn_args(src, args_node);
    //TODO: Check if arguments are valid
    //Register a new scope for the function body
    darr(yap_type_id) arg_type_ids = yap_ctx_darr_new(ctx, yap_type_id, .cap=darr_len(args), .len=0);
    darr(char*) arg_names = yap_ctx_darr_new(ctx, char*, .cap=darr_len(args), .len=0);
    for_darr(i, arg, args){
        darr_push(arg_type_ids, arg.type);
        darr_push(arg_names, arg.name);
    }
    yap_type func_type = {
        .kind=yap_type_func,
        .func=(yap_fn_type){
            .args=arg_type_ids,
            .return_type=return_type
        },
        .is_const=false
    };
    //TODO: Register arg names/defaults in the global context!
    //Register the function variable in the current scope
    yap_log("Registering function '%s' in global scope", name_node_val);
    yap_var func_var = {
        .name=name_node_val,
        .type=yap_ctx_insert_type_if_not_exists(ctx, func_type)
    };
    yap_ctx_push_var(ctx, func_var);
}

yap_decl yap_build_decl(yap_source *src, TSNode node){
    yap_decl res = {.kind=yap_decl_error};
    yap_node_guard(node, yap_decl, "Invalid declaration", src);
    const char* typ = ts_node_type(node);
    yap_log("Parsing declaration: %s", typ);
    strus_switch(typ, "function_declaration"){
        res = yap_build_fn_decl(src, node);
    }strus_case(typ, "type_declaration"){
        res = yap_build_type_declaration(src, node);
    }else{
        yap_log_node(src, "Unhandled declaration", node);
        yap_push_parse_error(src, node, "Unhandled declaration");
        res = yap_error_result(yap_decl, "Unhandled declaration");
    }
    res.loc.src = src;
    res.loc.range = yap_node_get_range(node);
    res.range = res.loc.range;
    return res;
}

yap_decl yap_build_type_declaration(yap_source* src, TSNode p_node){
    yap_node_guard(p_node, yap_decl, "Invalid type declaration", src);
    yap_log("Parsing type declaration of type '%s'", ts_node_type(p_node));
    TSNode node = ts_node_child(p_node, 0);
    const char* typ = ts_node_type(node);
    yap_decl res = yap_error_result(yap_decl, "Invalid type declaration");
    strus_switch(typ, "struct_declaration"){
        res = yap_build_struct_declaration(src, node);
    }strus_case(typ, "enum_declaration"){
        res = yap_build_enum_declaration(src, node);
    }strus_case(typ, "union_declaration"){
        res = yap_build_union_declaration(src, node);
    }else{
        yap_log_node(src, "Unhandled type declaration", node);
        yap_push_parse_error(src, node, "Unhandled type declaration");
    }
    return res;
}

yap_decl yap_build_struct_declaration(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    yap_node_guard(node, yap_decl, "Invalid named struct declaration", src);
    yap_node_field_by_name_var_check_push(node, name, yap_decl, "Missing name in named struct declaration", src);
    yap_node_field_by_name_var_check_push(node, fields, yap_decl, "Missing fields in named struct declaration", src);
    yap_node_val_ctx(name_node);
    yap_log("Parsing named struct declaration named '%s'", name_node_val);
    //Get field count and preallocate fields array
    darr(yap_struct_field) fields = yap_build_struct_fields(src, fields_node);
    //Build the type and push it to the context to get its id
    yap_struct_type struct_type = {
        .fields=fields,
        .c_name=name_node_val,
        .name=name_node_val
    };
    yap_type t = yap_empty_type(yap_type_struct);
    t.structure = struct_type;
    yap_type_id id = yap_ctx_push_named_type(ctx, name_node_val, name_node_val, t);
    return (yap_decl){
        .kind=yap_decl_named_type,
        .named_type_decl=(yap_named_type_decl){
            .name=name_node_val,
            .kind=yap_named_type_decl_struct,
            .type_id=id
        }
    };
}

darr(yap_struct_field) yap_build_struct_fields(yap_source* src, TSNode fields_node){
    yap_ctx* ctx = src->ctx;
    uint32_t field_count = ts_node_named_child_count(fields_node);
    darr(yap_struct_field) fields = yap_ctx_darr_new(ctx, yap_struct_field, .cap=field_count, .len=0);
    //Parse fields
    for_ts_named_children(fields_node, field_node){
        yap_struct_field field = yap_build_struct_field(src, field_node);
        if (field.kind == yap_struct_field_error){
            yap_push_parse_error(src, field_node, "Invalid struct field in struct declaration");
            return NULL;
        }
        darr_push(fields, field);
    }
    return fields;
}

darr(yap_struct_field) yap_build_union_variants(yap_source* src, TSNode variants_node){
    yap_ctx* ctx = src->ctx;
    uint32_t variant_count = ts_node_named_child_count(variants_node);
    darr(yap_struct_field) variants = yap_ctx_darr_new(ctx, yap_struct_field, .cap=variant_count, .len=0);
    for_ts_named_children(variants_node, variant_node){
        yap_struct_field variant = yap_build_union_variant(src, variant_node);
        if (variant.kind == yap_struct_field_error){
            yap_push_parse_error(src, variant_node, "Invalid union variant in union declaration");
            return NULL;
        }
        darr_push(variants, variant);
    }
    return variants;
}

darr(yap_enum_variant) yap_build_enum_variants(yap_source* src, TSNode variants_node){
    yap_ctx* ctx = src->ctx;
    uint32_t variant_count = ts_node_named_child_count(variants_node);
    darr(yap_enum_variant) variants = yap_ctx_darr_new(ctx, yap_enum_variant, .cap=variant_count, .len=0);
    for_ts_named_children(variants_node, variant_node){
        yap_enum_variant variant = yap_build_enum_variant(src, variant_node);
        if (!variant.name){
            yap_push_parse_error(src, variant_node, "Invalid enum variant in enum declaration");
            return NULL;
        }
        darr_push(variants, variant);
    }
    return variants;
}

yap_struct_field yap_build_struct_field(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    yap_log("Parsing struct field of type '%s'", ts_node_type(node));
    yap_struct_field res = {.kind=yap_struct_field_error};
    yap_node_field_var(type_node, node, "type");
    if (ts_node_null_or_error(type_node)){
        yap_push_parse_error(src, node, "Missing type in struct field");
        return res;
    }
    yap_node_field_var(name_node, node, "name");
    if (ts_node_null_or_error(name_node)){
        yap_push_parse_error(src, node, "Missing name in struct field");
        return res;
    }
    yap_node_field_var(default_value_node, node, "default_value");
    yap_expr* default_expr = NULL;
    if (!ts_node_null_or_error(default_value_node)){
        yap_expr e = yap_build_expr(src, default_value_node);
        default_expr = yap_ctx_one_cpy(ctx, e);
    }
    yap_node_val_ctx(name_node);
    yap_type_id type_id = yap_build_type_annotation(src, type_node);
    return (yap_struct_field){
        .kind=yap_struct_field_valid,
        .type=type_id,
        .name=name_node_val,
        .default_value=default_expr
    };
}

yap_struct_field yap_build_union_variant(yap_source* src, TSNode node){
    return yap_build_struct_field(src, node);
}

yap_enum_variant yap_build_enum_variant(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    yap_enum_variant res = {0};
    yap_node_field_var(name_node, node, "name");
    if (ts_node_null_or_error(name_node)){
        yap_push_parse_error(src, node, "Missing name in enum variant");
        return res;
    }
    yap_node_val_ctx(name_node);
    res.name = yap_ctx_strus_cpy(ctx, name_node_val);
    yap_node_field_var(value_node_node, node, "value_node");
    if (!ts_node_null_or_error(value_node_node) && ts_node_named_child_count(value_node_node) > 0){
        TSNode value_expr_node = ts_node_named_child(value_node_node, 0);
        yap_expr value_expr = yap_build_expr(src, value_expr_node);
        if (value_expr.kind == yap_expr_error){
            yap_push_parse_error(src, value_expr_node, "Invalid enum variant value expression");
            return res;
        }
        res.value = yap_ctx_one_cpy(ctx, value_expr);
    }
    return res;
}

yap_type_id yap_build_type_annotation(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    const char* typ = ts_node_type(node);
    yap_log("Parsing type annotation of type '%s'", typ);
    strus_switch(typ, "typ"){
        return yap_build_type(src, node);
    }strus_case(typ, "anon_struct_type"){
        return yap_build_anon_struct_type(src, node);
    }strus_case(typ, "anon_enum_type"){
        return yap_build_anon_enum_type(src, node);
    }strus_case(typ, "anon_union_type"){
        return yap_build_anon_union_type(src, node);
    }
    return ctx->internal_error_type_id;
}

yap_type_id yap_build_anon_struct_type(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    yap_node_field_var(fields_node, node, "fields");
    darr(yap_struct_field) fields = yap_build_struct_fields(src, fields_node);
    yap_anon_id anon_id = src->anon_id++;
    char* c_name = yap_ctx_get_anon_name(ctx, "struct", anon_id);
    yap_type t = yap_empty_type(yap_type_struct);
    t.structure = (yap_struct_type){
        .fields=fields,
        .c_name=c_name,
        .name=NULL
    };
    yap_type_id type_id = yap_ctx_insert_type_if_not_exists(ctx, t);
    return type_id;
}

yap_type_id yap_build_anon_union_type(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    yap_node_field_var(variants_node, node, "variants");
    darr(yap_struct_field) variants = yap_build_union_variants(src, variants_node);
    yap_anon_id anon_id = src->anon_id++;
    char* c_name = yap_ctx_get_anon_name(ctx, "union", anon_id);
    yap_type t = yap_empty_type(yap_type_union);
    t.uni = (yap_union_type){
        .variants=variants,
        .c_name=c_name,
        .name=NULL
    };
    return yap_ctx_insert_type_if_not_exists(ctx, t);
}

yap_type_id yap_build_anon_enum_type(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    yap_node_field_var(variants_node, node, "variants");
    darr(yap_enum_variant) variants = yap_build_enum_variants(src, variants_node);
    yap_anon_id anon_id = src->anon_id++;
    char* c_name = yap_ctx_get_anon_name(ctx, "enum", anon_id);
    yap_type t = yap_empty_type(yap_type_enum);
    t.enumeration = (yap_enum_type){
        .variants=variants,
        .c_name=c_name,
        .name=NULL
    };
    return yap_ctx_insert_type_if_not_exists(ctx, t);
}

yap_decl yap_build_enum_declaration(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    yap_node_guard(node, yap_decl, "Invalid named enum declaration", src);
    yap_node_field_by_name_var_check_push(node, name, yap_decl, "Missing name in named enum declaration", src);
    yap_node_field_by_name_var_check_push(node, variants, yap_decl, "Missing variants in named enum declaration", src);
    yap_node_val_ctx(name_node);
    darr(yap_enum_variant) variants = yap_build_enum_variants(src, variants_node);
    yap_type t = yap_empty_type(yap_type_enum);
    t.enumeration = (yap_enum_type){
        .variants=variants,
        .c_name=name_node_val,
        .name=name_node_val
    };
    yap_type_id id = yap_ctx_push_named_type(ctx, name_node_val, name_node_val, t);
    return (yap_decl){
        .kind=yap_decl_named_type,
        .named_type_decl=(yap_named_type_decl){
            .name=name_node_val,
            .kind=yap_named_type_decl_enum,
            .type_id=id
        }
    };
}

yap_decl yap_build_union_declaration(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    yap_node_guard(node, yap_decl, "Invalid named union declaration", src);
    yap_node_field_by_name_var_check_push(node, name, yap_decl, "Missing name in named union declaration", src);
    yap_node_field_by_name_var_check_push(node, variants, yap_decl, "Missing variants in named union declaration", src);
    yap_node_val_ctx(name_node);
    darr(yap_struct_field) variants = yap_build_union_variants(src, variants_node);
    yap_type t = yap_empty_type(yap_type_union);
    t.uni = (yap_union_type){
        .variants=variants,
        .c_name=name_node_val,
        .name=name_node_val
    };
    yap_type_id id = yap_ctx_push_named_type(ctx, name_node_val, name_node_val, t);
    return (yap_decl){
        .kind=yap_decl_named_type,
        .named_type_decl=(yap_named_type_decl){
            .name=name_node_val,
            .kind=yap_named_type_decl_union,
            .type_id=id
        }
    };
}

yap_decl yap_build_fn_decl(yap_source *src, TSNode node){
    yap_ctx* ctx = src->ctx;
    yap_node_guard(node, yap_decl, "Invalid function declaration", src);
    yap_node_field_by_name_var_check_push(node, name, yap_decl, "Missing function name", src);
    // yap_node_field_by_name_var_check_push(node, args, yap_decl, "Missing function arguments", src);
    yap_node_field_var(args_node, node, "args");
    yap_node_field_by_name_var_check_push(node, body, yap_decl, "Missing function body", src);
    yap_node_field_var(return_var_decl_node, node, "return_var_decl");
    
    yap_node_val_ctx(name_node);
    yap_log("Parsing function body for '%s'", name_node_val);
    
    // Get function var from global scope (signature already registered in pass 1)
    const yap_var *func_var = yap_scope_get_var_recursive(yap_ctx_current_scope(ctx), name_node_val);
    if (!func_var){
        if (darr_len(ctx->errors) == 0){
            yap_push_parse_error(src, node, "Internal parser error: function '%s' was not registered in pass 1", name_node_val);
        }else{
            yap_log("Skipping function '%s' in pass 2 because declaration pass already reported errors", name_node_val);
        }
        return yap_error_result(yap_decl, "Skipped function declaration after previous errors");
    }
    
    // Get function type (already parsed in pass 1)
    yap_type* func_type_ptr = yap_ctx_get_type(ctx, func_var->type);
    
    yap_fn_type fn_type = func_type_ptr->func;
    
    // Parse arguments (needed for populating local function scope aka body)
    darr(yap_func_arg) args = yap_build_fn_args(src, args_node);
    
    // Push a new function scope and populate it with arguments
    yap_scope* func_scope = yap_ctx_push_new_scope(ctx);
    for_darr(i, arg, args){
        yap_var var = (yap_var){
            .name=arg.name,
            .type=arg.type
        };
        yap_scope_set_var(func_scope, var);
    }
    if (!ts_node_null_or_error(return_var_decl_node)){
        yap_func_arg return_arg = yap_build_fn_arg_from_var_decl(src, return_var_decl_node);
        if (return_arg.kind == yap_func_arg_valid){
            yap_scope_set_var(func_scope, (yap_var){
                .name=return_arg.name,
                .type=return_arg.type
            });
        }
    }
    
    // Parse body
    yap_block body = yap_build_block(src, body_node);
    
    // Pop function scope
    yap_ctx_pop_scope(ctx);
    
    return (yap_decl){
        .kind=yap_decl_func,
        .func_decl=(yap_func_decl){
            .name=name_node_val,
            .args=args,
            .ret_typ=fn_type.return_type,
            .body=body
        }
    };
}

darr(yap_func_arg) yap_build_fn_args(yap_source* src, TSNode node);

// Forward declaration for var_decl -> func arg converter
yap_func_arg yap_build_fn_arg_from_var_decl(yap_source* src, TSNode node);

darr(yap_func_arg) yap_build_fn_args(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    if (ts_node_is_null(node)){
        return yap_ctx_darr_new(ctx, yap_func_arg, .cap=0, .len=0);
    }
    uint32_t param_count = ts_node_named_child_count(node);
    yap_log("Parsing %u function arguments", param_count);
    darr(yap_func_arg) args = yap_ctx_darr_new(ctx, yap_func_arg, .cap=param_count, .len=0);
    for_ts_named_children(node, n){
        const char* nt = ts_node_type(n);
        if (strus_eq(nt, "func_decl_arg")){
            darr_push(args, yap_build_fn_arg(src, n));
            continue;
        }
        if (strus_eq(nt, "var_decl")){
            darr_push(args, yap_build_fn_arg_from_var_decl(src, n));
            continue;
        }
        yap_log_node(src, "Expected parameter in function parameter list", n);
        yap_push_parse_error(src, n, "Expected parameter in function parameter list");
    }
    return args;
}

yap_func_arg yap_build_fn_arg(yap_source* src, TSNode node){
    yap_node_guard(node, yap_func_arg, "Invalid function argument", src);
    yap_node_field_by_name_var_check_push(node, name, yap_func_arg, "Missing argument name", src);
    yap_node_field_by_name_var_check_push(node, type, yap_func_arg, "Missing argument type", src);
    yap_node_val_ctx(name_node);
    char* type_str = yap_node_get_val_ctx(src, type_node);
    yap_log("Parsing function argument: %s of type %s", name_node_val, type_str);
    return (yap_func_arg){
        .kind=yap_func_arg_valid,
        .name=name_node_val,
        .type=yap_build_type(src, type_node),
        .default_value=(yap_expr){0}
    };
}

yap_func_arg yap_build_fn_arg_from_var_decl(yap_source* src, TSNode node){
    yap_node_guard(node, yap_func_arg, "Invalid function argument (var_decl)", src);
    TSNode inner = ts_node_child(node, 0);
    if (ts_node_null_or_error(inner)){
        yap_push_parse_error(src, node, "Empty var_decl in function parameter list");
        return yap_error_result(yap_func_arg, "Empty var_decl");
    }
    const char* it = ts_node_type(inner);
    yap_ctx* ctx = src->ctx;
    if (strus_eq(it, "explicit_type_var_decl")){
        yap_node_field_by_name_var_check_push(inner, type, yap_func_arg, "Missing argument type", src);
        yap_node_field_by_name_var_check_push(inner, name, yap_func_arg, "Missing argument name", src);
        yap_node_val_ctx(name_node);
        yap_type_id tid = yap_build_type(src, type_node);
        if (!tid) return yap_error_result(yap_func_arg, "Invalid argument type");
        // optional default
        yap_node_field_by_name_var(inner, value);
        yap_expr def = (yap_expr){0};
        if (!ts_node_null_or_error(value_node)){
            def = yap_build_expr(src, value_node);
            yap_return_if_error_kind(yap_func_arg, yap_expr, def, "Invalid default value expression for argument");
        }
        return (yap_func_arg){
            .kind=yap_func_arg_valid,
            .name=name_node_val,
            .type=tid,
            .default_value=def
        };
    }else if (strus_eq(it, "infered_type_var_decl")){
        yap_node_field_by_name_var_check_push(inner, name, yap_func_arg, "Missing argument name", src);
        yap_node_field_by_name_var_check_push(inner, value, yap_func_arg, "Missing argument default value", src);
        yap_node_val_ctx(name_node);
        yap_expr def = yap_build_expr(src, value_node);
        yap_return_if_error_kind(yap_func_arg, yap_expr, def, "Invalid default value expression for argument");
        // Use an untyped placeholder for inferred param types; will be resolved later
        yap_type_id tid = ctx->untyped_int_type_id ? ctx->untyped_int_type_id : ctx->int_type_id;
        return (yap_func_arg){
            .kind=yap_func_arg_valid,
            .name=name_node_val,
            .type=tid,
            .default_value=def
        };
    }
    yap_push_parse_error(src, node, "Unsupported var_decl form in function parameter");
    return yap_error_result(yap_func_arg, "Unsupported var_decl form");
}

yap_type_id yap_build_type(yap_source* src, TSNode p_node){
    yap_type_id res = -1;
    TSNode node = ts_node_child(p_node, 0);
    if (ts_node_null_or_error(node)){
        yap_push_parse_error(src, p_node, "Missing type");
        return 0;
    }
    yap_ctx* ctx = src->ctx;
    (void)ctx;
    const char* typ = ts_node_type(node);
    // yap_node_guard(node, yap_type, "Invalid type", src);
    yap_node_val_ctx(node);
    //TODO: We can not allocate this probably, here for dev purposes only
    char* type_str = node_val;
    yap_log("Parsing type: %s of type %s", type_str, typ);
    //TODO: Implement type parsing logic
    strus_switch(typ, "identifier"){
        res = yap_ctx_get_type_id_by_name(ctx, node_val);
        if (!res){
            yap_log("Unknown type '%s'", node_val);
            yap_push_parse_error(src, node, "Invalid type");
            return 0;
        }
        yap_log("Parsing identifier as type '%s', got id %u", node_val, res);
        return res;
    }strus_case(typ, "pointer_type"){
        yap_log("Parsing pointer type");
        return yap_build_pointer_type(src, node);
    }strus_case(typ, "function_type"){
        yap_log("Parsing function type");
        return yap_build_function_type(src, node);
    }strus_case(typ, "const_type"){
        yap_log("Parsing const type");
        return yap_build_const_type(src, node);
    }strus_case(typ, "paren_type"){
        yap_log("Parsing parenthesized type");
        return yap_build_paren_type(src, node);
    }else{
        yap_log_node(src, "Unhandled type", node);
        yap_push_parse_error(src, node, "Unhandled type");
    }
    return 0; //Invalid type
}

yap_type_id yap_build_const_type(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    yap_node_field_by_name_var(node, inner);
    if (ts_node_null_or_error(inner_node)){
        yap_push_parse_error(src, node, "Missing inner type in const type");
        return 0;
    }
    yap_type_id inner_type_id = yap_build_type(src, inner_node);
    if (!inner_type_id){
        yap_push_parse_error(src, inner_node, "Invalid inner type in const type");
        return 0;
    }
    yap_type* inner_type = yap_ctx_get_type(ctx, inner_type_id);
    if (!inner_type){
        yap_push_parse_error(src, inner_node, "Invalid inner type id in const type");
        return 0;
    }
    yap_type const_type = *inner_type;
    const_type.is_const = true;
    return yap_ctx_insert_type_if_not_exists(ctx, const_type);
}

yap_type_id yap_build_paren_type(yap_source* src, TSNode node){
    yap_node_field_by_name_var(node, inner);
    if (ts_node_null_or_error(inner_node)){
        yap_push_parse_error(src, node, "Missing inner type in parenthesized type");
        return 0;
    }
    return yap_build_type(src, inner_node);
}

yap_type_id yap_build_function_type(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    yap_type_id return_type_id = ctx->void_type_id; //Default return type is void
    yap_node_field_by_name_var(node, return_type);
    darr(yap_type_id) arg_types = NULL;
    if (ts_node_null_or_error(return_type_node)){
        yap_log_node(src, "Function is missing return type annotation, defaulting to void", node);
    }else{
        return_type_id = yap_build_type(src, return_type_node);
        if (!return_type_id){
            yap_log("Invalid return type in function type");
            yap_push_parse_error(src, return_type_node, "Invalid return type in function type");
            return 0;
        }
    }
    yap_node_field_by_name_var(node, func_type_params);
    if (ts_node_null_or_error(func_type_params_node)){
        yap_log_node(src, "Function is missing parameters, defaulting to none", node);
        arg_types = yap_ctx_darr_new(ctx, yap_type_id, .cap=0, .len=0);
    }else{
        uint32_t param_count = ts_node_named_child_count(func_type_params_node);
        yap_log("Parsing function type with %u parameters", param_count);
        arg_types = yap_ctx_darr_new(ctx, yap_type_id, .cap=param_count, .len=0);
        for_ts_named_children(func_type_params_node, n){
            yap_log_node(src, "Function type parameter", n);
            yap_type_id arg_type_id = yap_build_type(src, n);
            if (!arg_type_id){
                yap_log("Invalid parameter type in function type");
                yap_push_parse_error(src, n, "Invalid parameter type in function type");
                return 0;
            }
            darr_push(arg_types, arg_type_id);
        }
    }
    yap_type_id res_type_id = yap_ctx_insert_type_if_not_exists(ctx, (yap_type){
        .kind=yap_type_func,
        .func=(yap_fn_type){
            .args=arg_types,
            .return_type=return_type_id
        },
        .is_const=false
    });
    if (!res_type_id) yap_push_parse_error(src, node, "Failed to create function type");
    return res_type_id;
}

yap_type_id yap_build_pointer_type(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    TSNode subtype_node = ts_node_child(node, 0);
    yap_type_id subtype_id = yap_build_type(src, subtype_node);
    yap_type_id res_type_id = yap_ctx_insert_type_if_not_exists(ctx, (yap_type){
        .kind=yap_type_ptr,
        .pointer_type=subtype_id,
        .is_const=false
    });
    if (!res_type_id) yap_push_parse_error(src, node, "Failed to create pointer type");
    return res_type_id;
}

yap_block yap_build_block(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    yap_node_guard(node, yap_block, "Invalid block", src);
    yap_node_field_by_name_var_check_push(node, opening_bracket, yap_block, "Missing opening bracket in block", src);
    yap_node_field_by_name_var_check_push(node, closing_bracket, yap_block, "Missing closing bracket in block", src);
    int i = 0;
    uint32_t statement_count = ts_node_named_child_count(node); //Only count named children as statements
    yap_log("Parsing block: %u statements, type: %s, errors: %s", statement_count, ts_node_type(node), ts_node_null_or_error(node) ? "yes" : "no");
    darr(yap_statement) statements = yap_ctx_darr_new(ctx, yap_statement, .cap=statement_count, .len=0);
    yap_ctx_push_new_scope(ctx); //Push a new scope for the block, this allows for variable declarations inside the block that do not affect the outer scope
    // Parse statements
    for_ts_named_children(node, n){
        yap_log_node(src, "Statement", n);
        yap_statement st = yap_build_statement(src, n);
        if (st.kind == yap_statement_error){
            yap_push_parse_error(src, n, "Invalid statement in block");
            return (yap_block){
                .kind=yap_block_error,
                .statements=NULL
            };
        }
        darr_push(statements, st);
        i++;
    }
    yap_ctx_pop_scope(ctx); //Pop the block scope after parsing
    yap_log("Parsed %d statements in block", i);
    return (yap_block){
        .kind=yap_block_valid,
        .statements=statements
    };
}

yap_statement yap_build_statement(yap_source* src, TSNode node){
    yap_node_guard(node, yap_statement, "Invalid statement", src);
    yap_statement ret = yap_error_result(yap_statement, "Invalid statement");
    const char* typ = ts_node_type(node);
    yap_log("Parsing statement of type '%s'", typ);
    strus_switch(typ, "expr_statement"){
        ret = yap_build_expr_statement(src, node);
    }strus_case(typ, "empty_statement"){
        ret = yap_build_empty_statement(src, node);
    }strus_case(typ, "var_decl"){
        ret = yap_build_var_decl(src, node);
    }strus_case(typ, "return_statement"){
        ret = yap_build_return_statement(src, node);
    }strus_case(typ, "if_statement"){
        ret = yap_build_if_statement(src, node);
    }strus_case(typ, "if_else_statement"){
        ret = yap_build_if_else_statement(src, node);
    }strus_case(typ, "while_loop"){
        ret = yap_build_while_loop(src, node);
    }strus_case(typ, "for_loop"){
        ret = yap_build_for_loop(src, node);
    }strus_case(typ, "block"){
        ret = yap_build_block_statement(src, node);
    }strus_case(typ, "break_statement"){
        ret = yap_build_break_statement(src, node);
    }strus_case(typ, "continue_statement"){
        ret = yap_build_continue_statement(src, node);
    }else{
        yap_log_node(src, "Unhandled statement", node);
        yap_push_parse_error(src, node, "Unhandled statement");
        ret = yap_ts_error_result_node(yap_statement, "Unhandled statement", src, node);
    }
    ret.loc.src = src;
    ret.loc.range = yap_node_get_range(node);
    ret.range = ret.loc.range;
    return ret;
}

yap_statement yap_build_continue_statement(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    if (!yap_scope_in_loop(yap_ctx_current_scope(ctx))){
        yap_push_parse_error(src, node, "Continue statement not inside a loop");
        return yap_error_result(yap_statement, "Continue statement not inside a loop");
    }
    return (yap_statement){
        .kind=yap_statement_continue
    };
}

yap_statement yap_build_break_statement(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    if (!yap_scope_in_loop(yap_ctx_current_scope(ctx))){
        yap_push_parse_error(src, node, "Break statement not inside a loop");
        return yap_error_result(yap_statement, "Break statement not inside a loop");
    }
    return (yap_statement){
        .kind=yap_statement_break
    };
}

yap_statement yap_build_block_statement(yap_source* src, TSNode node){
    yap_block block = yap_build_block(src, node);
    if (block.kind == yap_block_error){
        return yap_error_result(yap_statement, "Invalid block statement");
    }
    return (yap_statement){
        .kind=yap_statement_block,
        .block=block
    };
}

yap_statement yap_build_for_loop(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    yap_log("Parsing for loop");
    yap_node_guard(node, yap_statement, "Invalid for loop", src);
    yap_node_field_by_name_var_check_push(node, init, yap_statement, "Missing initializer in for loop", src);
    yap_node_field_by_name_var_check_push(node, condition, yap_statement, "Missing condition in for loop", src);
    yap_node_field_by_name_var_check_push(node, update, yap_statement, "Missing update in for loop", src);
    yap_node_field_by_name_var_check_push(node, body, yap_statement, "Missing body in for loop", src);
    yap_ctx_push_new_loop_scope(ctx); //Push a new loop scope for the for loop, this allows for break/continue statements inside the loop body to work correctly
    yap_statement init = yap_build_statement(src, init_node);
    yap_return_if_error_kind(yap_statement, yap_statement, init, "Invalid initializer statement in for loop");
    yap_expr condition = yap_build_expr(src, condition_node);
    yap_return_if_error_kind(yap_statement, yap_expr, condition, "Invalid condition expression in for loop");
    yap_expr update = yap_build_expr(src, update_node);
    yap_return_if_error_kind(yap_statement, yap_expr, update, "Invalid update expression in for loop");
    yap_statement body = yap_build_statement(src, body_node);
    yap_return_if_error_kind(yap_statement, yap_statement, body, "Invalid body statement in for loop");
    yap_ctx_pop_scope(ctx); //Pop the loop scope after parsing the for loop
    return (yap_statement){
        .kind=yap_statement_for,
        .for_stmt=(yap_for){
            .init=yap_ctx_one_cpy(ctx, init),
            .condition=condition,
            .update=update,
            .body=yap_ctx_one_cpy(ctx, body)
        }
    };
}

yap_statement yap_build_while_loop(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    yap_log("Parsing while loop");
    yap_node_guard(node, yap_statement, "Invalid while loop", src);
    yap_node_field_by_name_var_check_push(node, condition, yap_statement, "Missing condition in while loop", src);
    yap_node_field_by_name_var_check_push(node, body, yap_statement, "Missing body in while loop", src);
    yap_ctx_push_new_loop_scope(ctx);
    yap_expr condition = yap_build_expr(src, condition_node);
    yap_return_if_error_kind(yap_statement, yap_expr, condition, "Invalid condition expression in while loop");
    yap_statement body = yap_build_statement(src, body_node);
    yap_return_if_error_kind(yap_statement, yap_statement, body, "Invalid body statement in while loop");
    yap_ctx_pop_scope(ctx);
    return (yap_statement){
        .kind=yap_statement_while,
        .while_stmt=(yap_while){
            .condition=condition,
            .body=yap_ctx_one_cpy(ctx, body)
        }
    };
}

yap_statement yap_build_if_statement(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    yap_node_guard(node, yap_statement, "Invalid if statement", src);
    yap_node_field_by_name_var_check_push(node, condition, yap_statement, "Missing condition in if statement", src);
    yap_node_field_by_name_var_check_push(node, then_branch, yap_statement, "Missing then branch in if statement", src);
    yap_expr condition = yap_build_expr(src, condition_node);
    yap_return_if_error_kind(yap_statement, yap_expr, condition, "Invalid condition expression in if statement");
    yap_statement then_branch = yap_build_statement(src, then_branch_node);
    yap_return_if_error_kind(yap_statement, yap_statement, then_branch, "Invalid then branch statement in if statement");
    return (yap_statement){
        .kind=yap_statement_if,
        .if_stmt=(yap_if){
            .condition=condition,
            .then_branch=yap_ctx_one_cpy(ctx, then_branch)
        }
    };
}

yap_statement yap_build_if_else_statement(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    yap_node_guard(node, yap_statement, "Invalid if-else statement", src);
    yap_node_field_by_name_var_check_push(node, condition, yap_statement, "Missing condition in if-else statement", src);
    yap_node_field_by_name_var_check_push(node, then_branch, yap_statement, "Missing then branch in if-else statement", src);
    yap_node_field_by_name_var_check_push(node, else_branch, yap_statement, "Missing else branch in if-else statement", src);
    yap_expr condition = yap_build_expr(src, condition_node);
    yap_return_if_error_kind(yap_statement, yap_expr, condition, "Invalid condition expression in if-else statement");
    yap_statement then_branch = yap_build_statement(src, then_branch_node);
    yap_return_if_error_kind(yap_statement, yap_statement, then_branch, "Invalid then branch statement in if-else statement");
    yap_statement else_branch = yap_build_statement(src, else_branch_node);
    yap_return_if_error_kind(yap_statement, yap_statement, else_branch, "Invalid else branch statement in if-else statement");
    return (yap_statement){
        .kind=yap_statement_if_else,
        .if_else_stmt=(yap_if_else){
            .condition=condition,
            .then_branch=yap_ctx_one_cpy(ctx, then_branch),
            .else_branch=yap_ctx_one_cpy(ctx, else_branch)
        }
    };
}

yap_statement yap_build_empty_statement(yap_source* src, TSNode node){
    yap_log("Parsing empty statement");
    return (yap_statement){
        .kind=yap_statement_empty
    };
}

yap_statement yap_build_return_statement(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    yap_node_guard(node, yap_statement, "Invalid return statement", src);
    yap_node_field_var(return_value_node, node, "value");
    if (ts_node_null_or_error(return_value_node)){
        yap_log("Parsing return statement with no return value");
        return (yap_statement){
            .kind=yap_statement_return,
            .return_stmt=(yap_return_statement){
                .value=(yap_expr){
                    .type=ctx->void_type_id
                }
            }
        };
    }
    yap_log("Parsing return statement with return value");
    yap_expr return_value = yap_build_expr(src, return_value_node);
    yap_return_if_error_kind(yap_statement, yap_expr, return_value, "Invalid return value expression in return statement");
    return (yap_statement){
        .kind=yap_statement_return,
        .return_stmt=(yap_return_statement){
            .value=return_value
        }
    };
}

yap_var_declarator yap_build_var_declarator(yap_source* src, TSNode p_node){
    TSNode node = ts_node_child(p_node, 0);
    // yap_ctx* ctx = src->ctx;
    const char* typ = ts_node_type(node);
    strus_switch(typ, "identifier"){
        yap_node_val_ctx(node);
        return (yap_var_declarator){
            .name=node_val,
            .is_const=false,
        };
    }strus_case(typ, "const_var_declarator"){
        yap_node_field_var(var_declarator_node, node, "var_declarator");
        yap_var_declarator vd = yap_build_var_declarator(src, var_declarator_node);
        return (yap_var_declarator){
            .name=vd.name,
            .is_const=true,
        };
    }
    yap_push_parse_error(src, p_node, "Invalid var declarator");
    return (yap_var_declarator){
        .name=NULL,
        .is_const=false,
    };
}

yap_statement yap_build_var_decl(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    yap_node_guard(node, yap_statement, "Invalid variable declaration", src);
    TSNode decl_node = ts_node_child(node, 0);
    if (ts_node_null_or_error(decl_node)){
        yap_push_parse_error(src, node, "Missing variable declaration payload");
        return yap_error_result(yap_statement, "Missing variable declaration payload");
    }
    const char* decl_typ = ts_node_type(decl_node);
    yap_node_val_ctx(decl_node);
    yap_log("Parsing variable declaration node type '%s' value '%s'", decl_typ, decl_node_val);
    yap_var var = {0};
    yap_expr value_expr = {0};
    bool has_init = false;
    if (strus_eq(decl_typ, "infered_type_var_decl")){
        yap_node_field_by_name_var_check_push(decl_node, name, yap_statement, "Missing declared variable in variable declaration", src);
        yap_node_field_by_name_var_check_push(decl_node, value, yap_statement, "Missing variable value in declaration", src);
        yap_node_val_ctx(name_node);
        yap_node_val_ctx(value_node);
        yap_log("Parsing variable declaration: %s := %s", name_node_val, value_node_val);
        value_expr = yap_build_expr(src, value_node);
        yap_return_if_error_kind(yap_statement, yap_expr, value_expr, "Invalid variable initializer expression");
        yap_type expr_type = *yap_ctx_get_type(ctx, value_expr.type);
        yap_type var_type = yap_ctx_coerce_type(ctx, expr_type);
        var = (yap_var){
            .name=name_node_val,
            .type=yap_ctx_insert_type_if_not_exists(ctx, var_type),
        };
        has_init = true;
    }else if (strus_eq(decl_typ, "explicit_type_var_decl")){
        yap_node_field_by_name_var_check_push(decl_node, type, yap_statement, "Missing declared variable type in variable declaration", src);
        yap_node_field_by_name_var_check_push(decl_node, name, yap_statement, "Missing declared variable in variable declaration", src);
        yap_node_val_ctx(name_node);
        yap_type_id declared_type = yap_build_type(src, type_node);
        if (!declared_type){
            yap_push_parse_error(src, type_node, "Invalid declared variable type in variable declaration");
            return yap_error_result(yap_statement, "Invalid declared variable type");
        }
        yap_node_field_var(value_node, decl_node, "value");
        var = (yap_var){
            .name=name_node_val,
            .type=declared_type,
        };
        if (!ts_node_null_or_error(value_node)){
            yap_node_val_ctx(value_node);
            yap_log("Parsing variable declaration: %s : %s := %s", yap_node_get_val_ctx(src, type_node), name_node_val, value_node_val);
            value_expr = yap_build_expr(src, value_node);
            yap_return_if_error_kind(yap_statement, yap_expr, value_expr, "Invalid variable initializer expression");
            has_init = true;
        }else{
            yap_log("Parsing uninitialized variable declaration: %s : %s", yap_node_get_val_ctx(src, type_node), name_node_val);
        }
    }else{
        yap_push_parse_error(src, decl_node, "Unsupported variable declaration form");
        return yap_error_result(yap_statement, "Unsupported variable declaration form");
    }
    yap_statement res = (yap_statement){
        .kind=yap_statement_var_decl,
        .var_decl=(yap_var_decl){
            .var=var,
            .has_init=has_init,
            .init=value_expr
        }
    };
    yap_log("Declared variable '%s' of type id %d", var.name, var.type);
    yap_ctx_push_var(ctx, var);
    return res;
}

yap_statement yap_build_expr_statement(yap_source* src, TSNode node){
    yap_node_guard(node, yap_statement, "Invalid expression statement", src);
    TSNode expr_node = ts_node_child(node, 0);
    yap_log("Parsing expression statement");
    if (ts_node_null_or_error(expr_node)){
        yap_push_parse_error(src, node, "Missing expression in statement");
        return yap_error_result(yap_statement, "Missing expression");
    }
    if (strus_eq(ts_node_type(expr_node), "method_access")){
        yap_node_field_var(caller_node, expr_node, "caller");
        yap_node_field_var(name_node, expr_node, "name");
        if (!ts_node_null_or_error(caller_node) && !ts_node_null_or_error(name_node) && strus_eq(ts_node_type(caller_node), "identifier")){
            yap_node_val_ctx(caller_node);
            yap_type_id type_id = yap_ctx_get_type_id_by_name(src->ctx, caller_node_val);
            if (type_id){
                yap_node_val_ctx(name_node);
                yap_log("Reinterpreting expression statement as uninitialized variable declaration: %s:%s", caller_node_val, name_node_val);
                return (yap_statement){
                    .kind=yap_statement_var_decl,
                    .var_decl=(yap_var_decl){
                        .var=(yap_var){
                            .name=name_node_val,
                            .type=type_id,
                        },
                        .has_init=false,
                        .init=(yap_expr){0},
                    }
                };
            }
        }
    }
    yap_expr expr = yap_build_expr(src, expr_node);
    yap_return_if_error_kind(yap_statement, yap_expr, expr, "Invalid expression statement");
    return (yap_statement){
        .kind=yap_statement_expr,
        .expr=expr
    };
}

yap_expr yap_build_expr(yap_source* src, TSNode node){
    yap_node_guard(node, yap_expr, "Invalid expression", src);
    const char* typ = ts_node_type(node);
    yap_log("Parsing expression of type '%s'", typ);
    // yap_node_val_ctx(node);
    yap_expr ret;
    strus_switch(typ, "literal"){
        ret = yap_build_literal(src, node);
    }strus_case(typ, "bin_expr"){
        ret = yap_build_bin_expr(src, node);
    }strus_case(typ, "assignment"){
        ret = (yap_expr){
            .kind=yap_expr_assignment,
            .assignment=yap_build_assignment(src, node)
        };
    }strus_case(typ, "identifier"){ //variable access
        ret = yap_build_var_access(src, node);
    }strus_case(typ, "func_call"){
        ret = yap_build_func_call(src, node);
    }strus_case(typ, "cast_expr"){
        ret = yap_build_cast_expr(src, node);
    }strus_case(typ, "at_op"){
        ret = yap_build_at_op(src, node);
    }strus_case(typ, "paren_expr"){
        ret = yap_build_paren_expr(src, node);
    }strus_case(typ, "incr_expr"){
        ret = yap_build_incr_expr(src, node);
    }strus_case(typ, "ternary_expr"){
        ret = yap_build_ternary_expr(src, node);
    }strus_case(typ, "member_access"){
        ret = yap_build_member_access(src, node);
    }else{
        yap_log_node(src, "Unhandled expression", node);
        yap_push_parse_error(src, node, "Unhandled expression");
        ret = yap_ts_error_result_node(yap_expr, "Unhandled expression", src, node);
    }
    // Do additional checks
    if (ret.kind == yap_expr_assignment && ret.assignment.kind == yap_assignment_error){
        ret = yap_error_result(yap_expr, "Invalid assignment expression");
    }
    ret.loc.src = src;
    ret.loc.range = yap_node_get_range(node);
    ret.range = ret.loc.range;
    return ret;
}

yap_expr yap_build_member_access(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    yap_log("Parsing member access expression");
    yap_node_guard(node, yap_expr, "Invalid member access expression", src);
    yap_node_field_by_name_var_check_push(node, object, yap_expr, "Missing object in member access expression", src);
    yap_node_field_by_name_var_check_push(node, member, yap_expr, "Missing member name in member access expression", src);
    yap_expr object = yap_build_expr(src, object_node);
    if (object.kind == yap_expr_error){
        return yap_error_result(yap_expr, "Invalid object expression in member access");
    }
    yap_type *object_type = yap_ctx_get_type(src->ctx, object.type);
    if (!object_type){
        yap_push_parse_error(src, object_node, "Unknown type of object in member access expression");
        return yap_error_result(yap_expr, "Unknown type of object in member access expression");
    }
    if (object_type->kind != yap_type_struct && object_type->kind != yap_type_union){
            yap_push_parse_error(src, object_node, "Type of object in member access expression must be a struct or union");
            return yap_error_result(yap_expr, "Type of object in member access expression must be a struct or union");
    }
    yap_node_val_ctx(member_node);
    yap_type_id member_type_id = yap_ctx_find_member_type(src->ctx, object.type, member_node_val);
    if (member_type_id == ctx->internal_error_type_id){ //Member not found
        yap_push_parse_error(src, member_node, "Type '%s' does not have a member named '%s'", yap_ctx_type_id_to_string(src->ctx, object.type), member_node_val);
        return yap_error_result(yap_expr, "Type does not have a member with the given name in member access expression");
    }
    return (yap_expr){
        .kind=yap_expr_member_access,
        .member_access=(yap_member_access){
            .object=yap_ctx_one_cpy(ctx, object),
            .member=member_node_val
        },
        .type=member_type_id,
        .is_lvalue=object.is_lvalue,
        .is_comptime=object.is_comptime
    };
}

yap_expr yap_build_ternary_expr(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    yap_log("Parsing ternary expression");
    yap_node_guard(node, yap_expr, "Invalid ternary expression", src);
    yap_node_field_by_name_var_check_push(node, condition, yap_expr, "Missing condition in ternary expression", src);
    yap_node_field_by_name_var_check_push(node, then_expr, yap_expr, "Missing then branch in ternary expression", src);
    yap_node_field_by_name_var_check_push(node, else_expr, yap_expr, "Missing else branch in ternary expression", src);
    yap_expr condition = yap_build_expr(src, condition_node);
    if (condition.kind == yap_expr_error){
        return yap_error_result(yap_expr, "Invalid condition expression in ternary expression");
    }
    // if (!yap_ctx_type_is_bool(src->ctx, condition.type)){
    //     yap_push_parse_error(src, condition_node, "Condition expression in ternary operator must be of type bool");
    //     return yap_error_result(yap_expr, "Condition expression in ternary operator must be of type bool");
    // }
    yap_expr then_expr = yap_build_expr(src, then_expr_node);
    if (then_expr.kind == yap_expr_error){
        return yap_error_result(yap_expr, "Invalid then branch expression in ternary expression");
    }
    yap_expr else_expr = yap_build_expr(src, else_expr_node);
    if (else_expr.kind == yap_expr_error){
        return yap_error_result(yap_expr, "Invalid else branch expression in ternary expression");
    }
    //Coerce then and else branches to a common type
    //TODO: Implement yap_ctx_find_common_type and use it here to find the common type of then and else branches, emit error if they are incompatible
    yap_type_id common_type = yap_ctx_find_common_type(src->ctx, then_expr.type, else_expr.type);
    if (!common_type){
        char* then_type_str = yap_ctx_type_id_to_string(src->ctx, then_expr.type);
        char* else_type_str = yap_ctx_type_id_to_string(src->ctx, else_expr.type);
        yap_push_parse_error(src, node, "Then and else branches of ternary operator have incompatible types: '%s' and '%s'", then_type_str, else_type_str);
        free(then_type_str);
        free(else_type_str);
        return yap_error_result(yap_expr, "Then and else branches of ternary operator have incompatible types");
    }
    return (yap_expr){
        .kind=yap_expr_ternary,
        .ternary=(yap_ternary_expr){
            .condition=yap_ctx_one_cpy(ctx, condition),
            .then_expr=yap_ctx_one_cpy(ctx, then_expr),
            .else_expr=yap_ctx_one_cpy(ctx, else_expr)
        },
        .type=common_type,
        .is_lvalue=false,
        .is_comptime=condition.is_comptime && then_expr.is_comptime && else_expr.is_comptime
    };
}

yap_expr yap_build_incr_expr(yap_source* src, TSNode node){
    yap_log("Parsing increment/decrement expression");
    yap_node_guard(node, yap_expr, "Invalid increment/decrement expression", src);
    yap_node_field_by_name_var_check_push(node, expr, yap_expr, "Missing operand in increment/decrement expression", src);
    yap_node_field_by_name_var_check_push(node, op, yap_expr, "Missing operator in increment/decrement expression", src);
    const char* operator_str = yap_node_get_val_ctx(src, op_node);
    yap_log("Operator in increment/decrement expression: %s", operator_str);
    bool is_increment = strus_eq(operator_str, "++");
    yap_expr expr = yap_build_expr(src, expr_node);
    if (expr.kind == yap_expr_error){
        return yap_error_result(yap_expr, "Invalid operand expression in increment/decrement expression");
    }
    if (!expr.is_lvalue){
        yap_push_parse_error(src, expr_node, "Operand of increment/decrement expression must be an lvalue");
        return yap_error_result(yap_expr, "Operand of increment/decrement expression must be an lvalue");
    }
    return (yap_expr){
        .kind=is_increment ? yap_expr_increment : yap_expr_decrement,
        .subexpr=yap_ctx_one_cpy(src->ctx, expr),
        .type=expr.type,
        .is_lvalue=false
    };
}

yap_expr yap_build_paren_expr(yap_source* src, TSNode node){
    yap_log("Parsing paren expression");
    yap_ctx* ctx = src->ctx;
    yap_node_field_by_name_var_check_push(node, expr, yap_expr, "Missing expression", src);
    yap_expr expr = yap_build_expr(src, expr_node);
    yap_expr res = expr;
    res.kind = yap_expr_paren;
    res.subexpr = yap_ctx_one_cpy(ctx, expr);
    return res;
}

yap_expr yap_build_at_op(yap_source* src, TSNode node){
    yap_log("Parsing 'at' operator");
    yap_ctx* ctx = src->ctx;
    yap_node_field_by_name_var_check_push(node, expr, yap_expr, "Missing expression to take address of", src);
    yap_expr expr = yap_build_expr(src, expr_node);
    if (!expr.is_lvalue){
        yap_push_parse_error(src, expr_node, "Cannot take address of non-lvalue");
        return yap_error_result(yap_expr, "Cannot take address of non-lvalue");
    }
    //Construct pointer type
    
    return (yap_expr){
        .kind=yap_expr_at_op,
        .subexpr=yap_ctx_one_cpy(ctx, expr),
        .type = yap_ctx_get_pointer_of_type_id(ctx, expr.type)
    };
}


yap_expr yap_build_cast_expr(yap_source* src, TSNode node){
    yap_log("Parsing cast expression");
    yap_ctx* ctx = src->ctx;
    yap_node_field_by_name_var_check_push(node, expr, yap_expr, "Missing expression in type cast", src);
    yap_node_field_by_name_var_check_push(node, type, yap_expr, "Missing type in type cast", src);
    yap_type_id type = yap_build_type(src, type_node);
    yap_expr expr = yap_build_expr(src, expr_node);
    yap_expr res = (yap_expr){
        .kind=yap_expr_cast,
        .subexpr=yap_ctx_one_cpy(ctx, expr),
        .type = type,
        .is_lvalue=expr.is_lvalue,
        .is_comptime=expr.is_comptime
    };
    return res;
}

yap_expr yap_build_func_call(yap_source* src, TSNode node){
    //TODO: Rework this parsing; gather params first, then decide what to do with them and emit errors!
    yap_ctx* ctx = src->ctx;
    yap_node_guard(node, yap_expr, "Invalid function call", src);
    // TODO: Parse type of function call expression
    // This *has* to be done to get default and named parameters to work.
    // Right now the compiler just rejects them!
    yap_node_field_by_name_var_check_push(node, func, yap_expr, "Missing function to call", src);
    // yap_node_field_by_name_var_check_push(node, params, yap_expr, "Missing arguments in function call", src);
    yap_node_field_var(params_node, node, "params");
    yap_expr func_expr = yap_build_expr(src, func_node);
    yap_type* func_type = yap_ctx_get_type(ctx, func_expr.type);
    yap_type_id return_type_id = 0;
    if (func_type && func_type->kind == yap_type_func){
        return_type_id = func_type->func.return_type;
    }else{
        yap_log("Trying to call a non-function type");
        char* func_type_str = yap_ctx_type_id_to_string(ctx, func_expr.type);
        yap_push_parse_error(src, func_node, "Cannot call a non-function type "aesc_reverse("%s"), func_type_str);
        free(func_type_str);
        return yap_error_result(yap_expr, "Cannot call a non-function type");
    }
    darr(yap_type_id) args = func_type->func.args;
    darr(yap_expr) params = NULL;
    if (ts_node_null_or_error(params_node)){
        yap_log_node(src, "Function call is missing arguments, defaulting to none", node);
        params = yap_ctx_darr_new(ctx, yap_expr, .cap=0, .len=0);
    }else{
        uint32_t params_count = ts_node_named_child_count(params_node);
        yap_log("Parsing function call with %u arguments", params_count);
        params = yap_ctx_darr_new(ctx, yap_expr, .cap=darr_len(args), .len=0);
        bool too_many_args = false;
        uint32_t unnamed_params = 0;
        for_ts_named_children(params_node, n){
            yap_log_node(src, "Function call argument", n);
            const char* param_kind = ts_node_type(n);
            strus_switch(param_kind, "unnamed_param"){
                unnamed_params++;
                if (too_many_args) continue;
                TSNode param_expr_node = ts_node_child(n, 0);
                yap_expr param_expr = yap_build_expr(src, param_expr_node);
                yap_return_if_error_kind(yap_expr, yap_expr, param_expr, "Invalid argument expression in function call");
                //Check if original function type has enough parameters
                if (darr_len(args) <= darr_len(params)){
                    too_many_args = true;
                    continue;
                }
                //Check type missmatch between argument and parameter
                if (!yap_ctx_type_id_compatible(ctx, param_expr.type, args[darr_len(params)])){
                    char* expected_type_str = yap_ctx_type_id_to_string(ctx, args[darr_len(params)]);
                    char* actual_type_str = yap_ctx_type_id_to_string(ctx, param_expr.type);
                    yap_log("Type mismatch in function call argument: expected %s but got %s", expected_type_str, actual_type_str);
                    yap_push_parse_error(src, param_expr_node, "Expected parameter of type "aesc_reverse("%s")" but got "aesc_reverse("%s"), expected_type_str, actual_type_str);
                    free(expected_type_str);
                    free(actual_type_str);
                    return yap_error_result(yap_expr, "Type mismatch in function call argument");
                }
                darr_push(params, param_expr);
            }strus_case(param_kind, "named_param"){
                //TODO: Support named params
                return yap_error_result(yap_expr, "Named parameters are not supported yet");
            }
        }
        if (too_many_args){
            yap_log("Too many arguments in function call, expected at most %u but got %u", darr_len(args), darr_len(params) + 1);
            yap_push_parse_error(src, node, "Expected at most "aesc_reverse("%u")" parameters, but got "aesc_reverse("%u"), darr_len(args), unnamed_params);
            return yap_error_result(yap_expr, "Too many arguments in function call");
        }
    }
    return (yap_expr){
        .kind=yap_expr_func_call,
        .func_call=(yap_func_call){
            .func_expr=yap_ctx_one_cpy(ctx, func_expr),
            .params=params
        },
        .type = return_type_id,
        .is_comptime=false,
        .is_lvalue=false
    };
}

yap_expr yap_build_var_access(yap_source* src, TSNode node){
    yap_ctx *ctx = src->ctx;
    yap_node_guard(node, yap_expr, "Invalid variable access", src);
    yap_node_val_ctx(node);
    const char* typ = ts_node_type(node);
    yap_log("Parsing variable access of type '%s'", typ);
    if (!strus_eq(typ, "identifier")){
        yap_log_node(src, "Expected identifier for variable access", node);
        yap_push_parse_error(src, node, "Expected identifier for variable access");
        return yap_error_result(yap_expr, "Expected identifier for variable access");
    }
    yap_scope* scope = darr_last(ctx->scopes);
    const yap_var* var = yap_scope_get_var_recursive(scope, node_val);
    yap_log("Variable '%s': %s", node_val, var ? var->name : "not found");
    if (!var){
        yap_push_parse_error(src, node, "Undefined variable");
        return yap_error_result(yap_expr, "Undefined variable");
    }
    return (yap_expr){
        .var_name=var->name,
        .kind=yap_expr_var,
        .type=var->type,
        .is_lvalue=true,
        //TODO: Determine if variable is comptime or not, currently all variables are non-comptime
        .is_comptime=false,
    };
}

yap_expr yap_build_literal(yap_source* src, TSNode p_node){
    //TODO: Handle different literal types, currently only numerical literals are supported and treated as untyped ints
    yap_node_guard(p_node, yap_expr, "Invalid literal", src);
    TSNode node = ts_node_child(p_node, 0);
    if (ts_node_null_or_error(node)){
        yap_push_parse_error(src, p_node, "Missing literal value");
        return yap_error_result(yap_expr, "Missing literal value");
    }
    const char* typ = ts_node_type(node);
    yap_node_val_ctx(node);
    yap_log("Parsing literal of type '%s'", typ);

    int kind = yap_literal_error;
    yap_expr res = (yap_expr){.kind = yap_expr_error};
    yap_ctx* ctx = src->ctx;
    
    strus_switch(typ, "num_literal"){
        kind = yap_literal_numerical;
        res.type = ctx->untyped_int_type_id;
    // }strus_case(typ, "blob_literal"){
    //     kind = yap_literal_blob;
    //     res.type = ctx->blob_type_id; //Blobs have to be cast!
    }else{
        yap_log_node(src, "Unhandled literal", node);
        yap_push_parse_error(src, node, "Unhandled literal type");
        return yap_error_result(yap_expr, "Unhandled literal type");
    }
    yap_literal lit = (yap_literal){
      .kind = kind,
      .text = yap_ctx_strus_cpy(ctx, node_val)
    };
    return (yap_expr){
        .kind=yap_expr_literal,
        .literal=lit,
        .type=res.type,
        .is_comptime=true,
        .is_lvalue=false
    };
}

yap_expr yap_build_bin_expr(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    yap_node_guard(node, yap_expr, "Invalid binary expression", src);
    yap_log("Parsing binary expression");
    yap_node_field_by_name_var_check_push(node, left, yap_expr, "Missing left side expression of a binary operation", src);
    yap_node_field_by_name_var_check_push(node, right, yap_expr, "Missing right side expression of a binary operation", src);
    yap_node_field_by_name_var_check_push(node, operator, yap_expr, "Missing binary operator", src);
    char op = *yap_node_val_start(src, operator_node);
    yap_expr left_expr = yap_build_expr(src, left_node);
    yap_expr right_expr = yap_build_expr(src, right_node);
    if (left_expr.kind == yap_expr_error || right_expr.kind == yap_expr_error){
        yap_log("Binary expression contains invalid operand(s)");
        return yap_error_result(yap_expr, "Invalid binary expression");
    }
    if (!strchr("+-*/%", op)){
        yap_push_parse_error(src, operator_node, "Unsupported binary operator");
        return yap_error_result(yap_expr, "Unsupported binary operator");
    }
    //TODO: Check if both types are compatible and figure out the resulting type
    bool types_compatible = yap_ctx_type_ids_eq(src->ctx, left_expr.type, right_expr.type);
    if (!types_compatible){
        yap_push_parse_error(src, node, "Incompatible types in binary expression");
        return yap_error_result(yap_expr, "Incompatible types in binary expression");
    }
    yap_type_id result_type = yap_ctx_coerce_type_id_to_id(src->ctx, left_expr.type);
    return (yap_expr){
        .kind=yap_expr_bin,
        .bin_expr=(yap_bin_expr){
            .op=op,
            .left=yap_ctx_one_cpy(ctx, left_expr),
            .right=yap_ctx_one_cpy(ctx, right_expr)
        },
        .is_comptime=left_expr.is_comptime && right_expr.is_comptime,
        .is_lvalue=false,
        .type=result_type
    };
}

yap_assignment yap_build_assignment(yap_source* src, TSNode node){
    yap_ctx* ctx = src->ctx;
    yap_node_guard(node, yap_assignment, "Invalid assignment", src);
    yap_log("Parsing assignment expression");
    yap_node_field_by_name_var_check_push(node, left, yap_assignment, "Missing left side of assignment", src);
    yap_node_field_by_name_var_check_push(node, operator, yap_assignment, "Missing operator in assignment", src);
    yap_node_field_by_name_var_check_push(node, right, yap_assignment, "Missing expression in assignment", src);
    yap_expr left = yap_build_expr(src, left_node);
    yap_expr right = yap_build_expr(src, right_node);
    if (left.kind == yap_expr_error || right.kind == yap_expr_error){
        yap_log("Assignment expression contains invalid side(s)");
        return yap_error_result(yap_assignment, "Invalid assignment");
    }
    //Check for lvalue on the left side of the assignment
    if (!left.is_lvalue){
        yap_push_parse_error(src, left_node, "Left side of assignment must be an lvalue");
        return yap_error_result(yap_assignment, "Left side of assignment must be an lvalue");
    }
    //Check for mutability on the left side of the assignment
    yap_type *left_type = yap_ctx_get_type(src->ctx, left.type);
    if (left_type->is_const){
        yap_push_parse_error(src, left_node, "Cannot assign to an immutable value");
        return yap_error_result(yap_assignment, "Cannot assign to an immutable value");
    }
    //TODO: Check if operator is supported and figure out the resulting type (e.g. for += operator)
    //TODO: Check if types are compatible for the assignment
    char* op_str = yap_node_get_val_ctx(src, operator_node);
    yap_assignment res = (yap_assignment){
        .kind=yap_assignment_valid,
        .left=yap_ctx_one_cpy(ctx, left),
        .right=yap_ctx_one_cpy(ctx, right),
    };
    snprintf(res.op, sizeof(res.op), "%s", op_str ? op_str : "");
    yap_log("Assignment operator: %s", res.op);
    return res;
}
