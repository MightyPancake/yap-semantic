#ifndef YAP_BUILD_H
#define YAP_BUILD_H

#include "yap/all.h"

// Main entry point – builds the semantic tree from the parsed source node tree
yap_ctx* yap_build(yap_ctx* ctx, yap_args args);

// Pass-1 registration of a top-level declaration (for mutual recursion)
void yap_build_top_level_declaration(yap_source* src, yap_decl_node* node);

// Declarations
yap_decl yap_build_decl(yap_source* src, yap_decl_node* node);
yap_decl yap_build_fn_def(yap_source* src, yap_func_decl_node* fnode);
yap_decl yap_build_fn_declaration(yap_source* src, yap_func_decl_node* fnode);
yap_decl yap_build_named_type_decl(yap_source* src, yap_named_type_decl_node* tnode);

// Statements
yap_statement yap_build_statement(yap_source* src, yap_statement_node* node);
yap_statement yap_build_empty_statement(yap_source* src);
yap_statement yap_build_expr_statement(yap_source* src, yap_expr_node* expr_node);
yap_statement yap_build_var_decl_statement(yap_source* src, yap_var_decl_node* vnode);
yap_statement yap_build_return_statement(yap_source* src, yap_return_statement_node* rnode);
yap_statement yap_build_if_statement(yap_source* src, yap_if_node* inode);
yap_statement yap_build_if_else_statement(yap_source* src, yap_if_else_node* inode);
yap_statement yap_build_while_statement(yap_source* src, yap_while_node* wnode);
yap_statement yap_build_for_statement(yap_source* src, yap_for_node* fnode);
yap_statement yap_build_break_statement(yap_source* src, yap_statement_node* node);
yap_statement yap_build_continue_statement(yap_source* src, yap_statement_node* node);
yap_statement yap_build_block_statement(yap_source* src, yap_block_node* bnode);

// Expressions
yap_expr yap_build_expr(yap_source* src, yap_expr_node* node);
yap_expr yap_build_literal_expr(yap_source* src, yap_literal_node* lit);
yap_expr yap_build_var_access_expr(yap_source* src, yap_identifier_node* ident);
yap_expr yap_build_bin_expr(yap_source* src, yap_bin_op_node* bin);
yap_expr yap_build_assignment_expr(yap_source* src, yap_assignment_node* assign);
yap_expr yap_build_func_call_expr(yap_source* src, yap_func_call_node* call);
yap_expr yap_build_cast_expr(yap_source* src, yap_cast_node* cast);
yap_expr yap_build_at_op_expr(yap_source* src, yap_at_op_node* at);
yap_expr yap_build_paren_expr(yap_source* src, yap_paren_node* par);
yap_expr yap_build_increment_expr(yap_source* src, yap_expr_node* sub);
yap_expr yap_build_decrement_expr(yap_source* src, yap_expr_node* sub);
yap_expr yap_build_ternary_expr(yap_source* src, yap_ternary_node* ter);
yap_expr yap_build_member_access_expr(yap_source* src, yap_member_access_node* ma);
yap_expr yap_build_optional_member_access_expr(yap_source* src, yap_member_access_node* ma);
yap_expr yap_build_deref_expr(yap_source* src, yap_deref_node* dn);
yap_expr yap_build_index_access_expr(yap_source* src, yap_index_access_node* ia);
yap_expr yap_build_module_access_expr(yap_source* src, yap_module_access_node* ma);

// Helpers
yap_func_arg      yap_build_func_arg(yap_source* src, yap_func_arg_node* anode);
yap_struct_field  yap_build_struct_field(yap_source* src, yap_var_decl_node* vn);
yap_enum_variant  yap_build_enum_variant(yap_source* src, yap_enum_variant_node* ev);
yap_block         yap_build_block(yap_source* src, yap_block_node* bnode);

// Types
yap_type_id yap_build_type_from_type_node(yap_source* src, yap_type_node* tnode);
yap_type_id yap_build_type_from_node(yap_source* src, yap_identifier_node* tnode);  // legacy shim

#endif //YAP_BUILD_H
