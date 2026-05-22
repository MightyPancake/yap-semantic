#include "semantic.h"
#include "log.h"

void yap_semantic_analyze(yap_ctx* ctx){
    if (!ctx) return;

    yap_log("Semantic pass: starting");

    if (!ctx->current_module){
        yap_log("Semantic pass: no current module set");
        return;
    }

    for_darr(i, decl, ctx->current_module->decls){
        switch (decl.kind){
        case yap_decl_func:
            yap_log("Semantic pass: function '%s'", decl.func_decl.name ? decl.func_decl.name : "<unnamed>");
            break;
        case yap_decl_named_type:
            yap_log("Semantic pass: named type '%s'", decl.named_type_decl.name ? decl.named_type_decl.name : "<unnamed>");
            break;
        default:
            break;
        }
    }

    yap_log("Semantic pass: finished");
}
