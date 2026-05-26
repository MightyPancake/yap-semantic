#ifndef YAP_BUILD_H
#define YAP_BUILD_H

#include "yap/all.h"
#include "yap/types.h"

// Top level parsing functions
void yap_parse_top_level_declaration(yap_source* src, TSNode node);
void yap_parse_top_level_func_decl(yap_source *src, TSNode node);

// Main parsing functions
yap_ctx* yap_build(yap_ctx* ctx, yap_args args);
void yap_parse_source_file(yap_source* src, TSNode node);
// Declarations
yap_decl yap_build_decl(yap_source* src, TSNode node);
yap_decl yap_build_fn_decl(yap_source* src, TSNode node);
yap_decl yap_build_type_declaration(yap_source* src, TSNode node);
yap_decl yap_build_struct_declaration(yap_source* src, TSNode node);
yap_decl yap_build_enum_declaration(yap_source* src, TSNode node);
yap_decl yap_build_union_declaration(yap_source* src, TSNode node);

// Block
yap_block yap_build_block(yap_source* src, TSNode node);

yap_assignment yap_build_assignment(yap_source* src, TSNode node);
//statement
yap_statement yap_build_statement(yap_source* src, TSNode node);

// yap_statement
//yap_parse_macro_statement(yap_source* src, TSNode node);
yap_statement yap_build_empty_statement(yap_source* src, TSNode node);
yap_statement yap_build_expr_statement(yap_source* src, TSNode node);
yap_statement yap_build_if_statement(yap_source* src, TSNode node);
yap_statement yap_build_if_else_statement(yap_source* src, TSNode node);
yap_statement yap_build_var_decl(yap_source* src, TSNode node);
yap_statement yap_build_return_statement(yap_source* src, TSNode node);
yap_statement yap_build_while_loop(yap_source* src, TSNode node);
yap_statement yap_build_for_loop(yap_source* src, TSNode node);
yap_statement yap_build_break_statement(yap_source* src, TSNode node);
yap_statement yap_build_continue_statement(yap_source* src, TSNode node);
yap_statement yap_build_block_statement(yap_source* src, TSNode node);

//expr
yap_expr yap_build_expr(yap_source* src, TSNode node);
yap_expr yap_build_literal(yap_source* src, TSNode node);
yap_expr yap_build_bin_expr(yap_source* src, TSNode node);
yap_expr yap_build_var_access(yap_source* src, TSNode node);
yap_expr yap_build_func_call(yap_source* src, TSNode node);
yap_expr yap_build_cast_expr(yap_source* src, TSNode node);
yap_expr yap_build_at_op(yap_source* src, TSNode node);
yap_expr yap_build_paren_expr(yap_source* src, TSNode node);
yap_expr yap_build_incr_expr(yap_source* src, TSNode node);
yap_expr yap_build_ternary_expr(yap_source* src, TSNode node);
yap_expr yap_build_member_access(yap_source* src, TSNode node);

darr(yap_func_arg) yap_build_fn_args(yap_source* src, TSNode node);
yap_func_arg yap_build_fn_arg(yap_source* src, TSNode node);
yap_func_arg yap_build_fn_arg_from_var_decl(yap_source* src, TSNode node);

//Types
yap_type_id yap_build_type(yap_source* src, TSNode node);
yap_type_id yap_build_const_type(yap_source* src, TSNode node);
yap_type_id yap_build_paren_type(yap_source* src, TSNode node);
yap_type_id yap_build_pointer_type(yap_source* src, TSNode node);
yap_type_id yap_build_function_type(yap_source* src, TSNode node);
// Anonymous types
yap_type_id yap_build_anon_struct_type(yap_source* src, TSNode node);
yap_type_id yap_build_anon_union_type(yap_source* src, TSNode node);
yap_type_id yap_build_anon_enum_type(yap_source* src, TSNode node);

//Other
yap_struct_field yap_build_struct_field(yap_source* src, TSNode node);
yap_enum_variant yap_build_enum_variant(yap_source* src, TSNode node);
yap_struct_field yap_build_union_variant(yap_source* src, TSNode node);
yap_type_id yap_build_type_annotation(yap_source* src, TSNode node);
darr(yap_struct_field) yap_build_struct_fields(yap_source* src, TSNode fields_node);
darr(yap_struct_field) yap_build_union_variants(yap_source* src, TSNode variants_node);
darr(yap_enum_variant) yap_build_enum_variants(yap_source* src, TSNode variants_node);

//Misc
yap_type yap_empty_type(yap_type_kind kind);

#endif //YAP_BUILD_H
