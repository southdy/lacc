#if !AMALGAMATION
# define INTERNAL
# define EXTERNAL extern
#endif
#include "declaration.h"
#include "eval.h"
#include "expression.h"
#include "initializer.h"
#include "parse.h"
#include "statement.h"
#include "symtab.h"
#include "typetree.h"
#include <lacc/context.h>
#include <lacc/token.h>

#include <assert.h>

static const Type *get_typedef(String str)
{
    struct symbol *tag;

    tag = sym_lookup(&ns_ident, str);
    if (tag && tag->symtype == SYM_TYPEDEF) {
        return &tag->type;
    }

    return NULL;
}

static int is_type_placeholder(Type type)
{
    return type.type == -1;
}

static Type get_type_placeholder(void)
{
    Type t = {-1};
    return t;
}

static struct block *parameter_declarator(
    struct definition *def,
    struct block *block,
    Type base,
    Type *type,
    String *name,
    size_t *length);

/*
 * Parse a function parameter list, adding symbols to scope.
 *
 * FOLLOW(parameter-list) = { ')' }, peek to return empty list; even
 * though K&R require at least specifier: (void)
 * Set parameter-type-list = parameter-list, including the , ...
 *
 * As a special case, ignore evaluation when in block scope. This is to
 * avoid VLA code that would be generated in cases like this:
 *
 *     int main(void) {
 *         int foo(int n, int arr[][n + 1]);
 *         return 0;
 *     }
 *
 * The evaluation of n + 1 is done in a throwaway block, and not
 * included in the CFG of main.
 */
static struct block *parameter_list(
    struct definition *def,
    struct block *parent,
    Type base,
    Type *func)
{
    String name;
    size_t length;
    struct block *block;
    struct member *param;

    *func = type_create_function(base);
    block = current_scope_depth(&ns_ident) == 1
        ? parent
        : cfg_block_init(def);

    while (peek().token != ')') {
        name.len = 0;
        length = 0;
        base = declaration_specifiers(NULL, NULL);
        block = parameter_declarator(def, block, base, &base, &name, &length);
        if (is_void(base)) {
            if (nmembers(*func)) {
                error("Incomplete type in parameter list.");
                exit(1);
            }
            break;
        } else if (is_array(base)) {
            base = type_create_pointer(type_next(base));
        }
        param = type_add_member(*func, name, base);
        param->offset = length;
        if (name.len) {
            param->sym =
                sym_add(&ns_ident, name, base, SYM_DEFINITION, LINK_NONE);
        }
        if (peek().token != ',') {
            break;
        }
        consume(',');
        if (peek().token == DOTS) {
            consume(DOTS);
            assert(!is_vararg(*func));
            type_add_member(*func, str_init("..."), basic_type__void);
            assert(is_vararg(*func));
            break;
        }
    }

    return current_scope_depth(&ns_ident) == 1 ? block : parent;
}

/*
 * Old-style function definitions with separate identifiers list and
 * type declarations.
 *
 * Return a function type where all members have placeholder type.
 */
static Type identifier_list(Type base)
{
    struct token t;
    Type type;

    type = type_create_function(base);
    if (peek().token != ')') {
        while (1) {
            t = consume(IDENTIFIER);
            if (get_typedef(t.d.string)) {
                error("Unexpected type '%t' in identifier list.");
                exit(1);
            }
            type_add_member(type, t.d.string, get_type_placeholder());
            if (peek().token == ',') {
                next();
            } else break;
        }
    }

    return type;
}

struct array_param {
    char is_const;
    char is_volatile;
    char is_restrict;
    char is_static;
};

static void array_param_qualifiers(struct array_param *cvrs)
{
    if (!cvrs) {
        return;
    }

    if (peek().token == STATIC) {
        next();
        cvrs->is_static = 1;
    }

    while (1) {
        switch (peek().token) {
        case CONST:
            cvrs->is_const = 1;
            next();
            continue;
        case VOLATILE:
            cvrs->is_volatile = 1;
            next();
            continue;
        case RESTRICT:
            cvrs->is_restrict = 1;
            next();
            continue;
        default:
            break;
        }
        break;
    }

    if (peek().token == STATIC && !cvrs->is_static) {
        next();
        cvrs->is_static = 1;
    }
}

/*
 * Parse expression determining length of array.
 *
 * Function parameters can have type qualifiers and 'static' declared
 * with the array length.
 *
 * Variable length arrays must be ensured to evaluate to a new temporary
 * only associated with this type, such that sizeof always returns the
 * correct value.
 */
static struct block *array_declarator_length(
    struct definition *def,
    struct block *block,
    struct array_param *cvrs)
{
    struct var val;

    if (!def) {
        val = constant_expression();
        block = cfg_block_init(NULL);
    } else {
        if (cvrs) {
            array_param_qualifiers(cvrs);
        }
        block = assignment_expression(def, block);
        val = eval(def, block, block->expr);
    }

    if (!is_integer(val.type)) {
        error("Array dimension must be of integer type.");
        exit(1);
    }

    if (val.kind == IMMEDIATE && is_signed(val.type) && val.imm.i < 0) {
        error("Array dimension must be a positive number.");
        exit(1);
    }

    if (!type_equal(val.type, basic_type__unsigned_long)) {
        val = eval(def, block,
            eval_expr(def, block, IR_OP_CAST, val, basic_type__unsigned_long));
    } else if (val.kind == DIRECT && !is_temporary(val.symbol)) {
        val = eval_copy(def, block, val);
    }

    assert(is_unsigned(val.type));
    block->expr = as_expr(val);
    return block;
}

/*
 * Parse array declarations of the form [s0][s1]..[sn], resulting in
 * type [s0] [s1] .. [sn] (base).
 *
 * Only the first dimension s0 can be unspecified, yielding an
 * incomplete type. Incomplete types are represented by having size of
 * zero.
 *
 * VLA require evaluating an expression, and storing it in a separate
 * stack allocated variable.
 */
static struct block *array_declarator(
    struct definition *def,
    struct block *block,
    Type base,
    Type *type,
    size_t *static_length)
{
    size_t length = 0;
    struct var val;
    struct array_param cvrs = {0};
    int is_incomplete = 0;
    const struct symbol *sym = NULL;

    consume('[');
    if (peek().token == ']') {
        is_incomplete = 1;
    } else {
        if (static_length) {
            block = array_declarator_length(def, block, &cvrs);
        } else {
            block = array_declarator_length(def, block, NULL);
        }
        val = eval(def, block, block->expr);
        assert(type_equal(val.type, basic_type__unsigned_long));
        if (val.kind == IMMEDIATE) {
            length = val.imm.u;
        } else {
            assert(val.kind == DIRECT);
            assert(val.symbol);
            sym = val.symbol;
        }
    }

    consume(']');
    if (peek().token == '[') {
        block = array_declarator(def, block, base, &base, NULL);
    }

    if (!is_complete(base)) {
        error("Array has incomplete element type.");
        exit(1);
    }

    if (static_length) {
        *static_length = length;
        *type = type_create_pointer(base);
        if (cvrs.is_const) type_set_const(*type);
        if (cvrs.is_volatile) type_set_volatile(*type);
        if (cvrs.is_restrict) type_set_restrict(*type);
    } else if (is_incomplete) {
        *type = type_create_incomplete(base);
    } else if (sym) {
        *type = type_create_vla(base, sym);
    } else {
        *type = type_create_array(base, length);
    }

    return block;
}

/*
 * Parse function and array declarators.
 *
 * Example:
 *
 *    void (*foo)(int)
 *
 * Traverse (*foo) first, and prepended on the outer `(int) -> void`,
 * making it `* (int) -> void`. Void is used as a sentinel, the inner
 * declarator can only produce pointer, function or array.
 */
static struct block *direct_declarator(
    struct definition *def,
    struct block *block,
    Type base,
    Type *type,
    String *name,
    size_t *length)
{
    struct token t;
    Type head = basic_type__void;

    switch (peek().token) {
    case IDENTIFIER:
        t = next();
        if (!name) {
            error("Unexpected identifier in abstract declarator.");
            exit(1);
        }
        *name = t.d.string;
        break;
    case '(':
        next();
        block = declarator(def, block, head, &head, name);
        consume(')');
        if (!is_void(head)) {
            length = NULL;
        }
        break;
    default:
        break;
    }

    switch (peek().token) {
    case '[':
        block = array_declarator(def, block, base, type, length);
        break;
    case '(':
        next();
        t = peek();
        push_scope(&ns_tag);
        push_scope(&ns_ident);
        if (t.token == IDENTIFIER && !get_typedef(t.d.string)) {
            *type = identifier_list(base);
        } else {
            block = parameter_list(def, block, base, type);
        }
        pop_scope(&ns_ident);
        pop_scope(&ns_tag);
        consume(')');
        break;
    default:
        *type = base;
        break;
    }

    if (!is_void(head)) {
        *type = type_patch_declarator(head, *type);
    }

    return block;
}

static Type pointer(Type type)
{
    type = type_create_pointer(type);
    while (1) {
        next();
        switch (peek().token) {
        case CONST:
            type = type_set_const(type);
            break;
        case VOLATILE:
            type = type_set_volatile(type);
            break;
        case RESTRICT:
            type = type_set_restrict(type);
            break;
        default:
            return type;
        }
    }
}

static struct block *parameter_declarator(
    struct definition *def,
    struct block *block,
    Type base,
    Type *type,
    String *name,
    size_t *length)
{
    assert(type);
    while (peek().token == '*') {
        base = pointer(base);
    }

    return direct_declarator(def, block, base, type, name, length);
}

INTERNAL struct block *declarator(
    struct definition *def,
    struct block *block,
    Type base,
    Type *type,
    String *name)
{
    return parameter_declarator(def, block, base, type, name, NULL);
}

static void member_declaration_list(Type type)
{
    String name;
    struct var expr;
    Type decl_base, decl_type;

    do {
        decl_base = declaration_specifiers(NULL, NULL);
        do {
            name.len = 0;
            declarator(NULL, NULL, decl_base, &decl_type, &name);
            if (is_struct_or_union(type) && peek().token == ':') {
                if (!is_integer(decl_type)) {
                    error("Unsupported type '%t' for bit-field.", decl_type);
                    exit(1);
                }
                consume(':');
                expr = constant_expression();
                if (is_signed(expr.type) && expr.imm.i < 0) {
                    error("Negative width in bit-field.");
                    exit(1);
                }
                type_add_field(type, name, decl_type, expr.imm.u);
            } else if (!name.len) {
                if (is_struct_or_union(decl_type)) {
                    type_add_anonymous_member(type, decl_type);
                } else {
                    error("Missing name in member declarator.");
                    exit(1);
                }
            } else {
                type_add_member(type, name, decl_type);
            }
            if (peek().token == ',') {
                consume(',');
                continue;
            }
        } while (peek().token != ';');
        consume(';');
    } while (peek().token != '}');
    type_seal(type);
}

/*
 * Parse and declare a new struct or union type, or retrieve type from
 * existing symbol; possibly providing a complete definition that will
 * be available for later declarations.
 */
static Type struct_or_union_declaration(void)
{
    struct symbol *sym = NULL;
    Type type = {0};
    String name;
    enum type kind;

    kind = (next().token == STRUCT) ? T_STRUCT : T_UNION;
    if (peek().token == IDENTIFIER) {
        name = consume(IDENTIFIER).d.string;
        sym = sym_lookup(&ns_tag, name);
        if (!sym) {
            type = type_create(kind);
            sym = sym_add(&ns_tag, name, type, SYM_TAG, LINK_NONE);
        } else if (is_integer(sym->type)) {
            error("Tag '%s' was previously declared as enum.",
                str_raw(sym->name));
            exit(1);
        } else if (type_of(sym->type) != kind) {
            error("Tag '%s' was previously declared as %s.",
                str_raw(sym->name),
                (is_struct(sym->type)) ? "struct" : "union");
            exit(1);
        }
        type = sym->type;
        if (peek().token == '{' && size_of(type)) {
            error("Redefiniton of '%s'.", str_raw(sym->name));
            exit(1);
        }
    }

    if (peek().token == '{') {
        if (!sym) {
            type = type_create(kind);
        }
        next();
        member_declaration_list(type);
        assert(size_of(type));
        consume('}');
    }

    return type;
}

static void enumerator_list(void)
{
    String name;
    struct var val;
    struct symbol *sym;
    int count = 0;

    consume('{');
    do {
        name = consume(IDENTIFIER).d.string;
        if (peek().token == '=') {
            consume('=');
            val = constant_expression();
            if (!is_integer(val.type)) {
                error("Implicit conversion from non-integer type in enum.");
            }
            count = val.imm.i;
        }
        sym = sym_add(
            &ns_ident,
            name,
            basic_type__int,
            SYM_CONSTANT,
            LINK_NONE);
        sym->value.constant.i = count++;
        if (peek().token != ',')
            break;
        consume(',');
    } while (peek().token != '}');
    consume('}');
}

/*
 * Consume enum definition, which represents an int type.
 *
 * Use value.constant as a sentinel to represent definition, checked on
 * lookup to detect duplicate definitions.
 */
static void enum_declaration(void)
{
    String name;
    struct token t;
    struct symbol *tag;

    consume(ENUM);
    t = peek();
    if (t.token == IDENTIFIER) {
        next();
        name = t.d.string;
        tag = sym_lookup(&ns_tag, name);
        if (!tag || tag->depth < current_scope_depth(&ns_tag)) {
            tag = sym_add(
                &ns_tag,
                name,
                basic_type__int,
                SYM_TAG,
                LINK_NONE);
        } else if (!is_integer(tag->type)) {
            error("Tag '%s' was previously defined as aggregate type.",
                str_raw(tag->name));
            exit(1);
        }
        if (peek().token == '{') {
            if (tag->value.constant.i) {
                error("Redefiniton of enum '%s'.", str_raw(tag->name));
                exit(1);
            }
            enumerator_list();
            tag->value.constant.i = 1;
        }
    } else {
        enumerator_list();
    }
}

/*
 * Parse type, qualifiers and storage class. Do not assume int by
 * default, but require at least one type specifier. Storage class is
 * returned as token value, unless the provided pointer is NULL, in
 * which case the input is parsed as specifier-qualifier-list.
 *
 * Use a compact bit representation to hold state about declaration 
 * specifiers. Initialize storage class to sentinel value.
 */
INTERNAL Type declaration_specifiers(int *storage_class, int *is_inline)
{
    Type type = {-1}, other;
    const Type *tagged;
    struct token tok;

    if (storage_class) {
        *storage_class = '$';
    }

    if (is_inline) {
        *is_inline = 0;
    }

    while (1) {
        switch ((tok = peek()).token) {
        case VOID:
            next();
            type.type = T_VOID;
            break;
        case BOOL:
            next();
            type.type = T_BOOL;
            break;
        case CHAR:
            next();
            type.type = T_CHAR;
            break;
        case SHORT:
            next();
            type.type = T_SHORT;
            break;
        case INT:
            next();
            if (type.type != T_LONG && type.type != T_SHORT) {
                type.type = T_INT;
            }
            break;
        case SIGNED:
            next();
            if (type.type == -1) {
                type.type = T_INT;
            }
            if (is_unsigned(type)) {
                error("Conflicting 'signed' and 'unsigned' specifiers.");
            }
            break;
        case UNSIGNED:
            next();
            if (type.type == -1) {
                type.type = T_INT;
            }
            if (is_unsigned(type)) {
                error("Duplicate 'unsigned' specifier.");
            }
            type.is_unsigned = 1;
            break;
        case LONG:
            next();
            if (type.type == T_DOUBLE) {
                type.type = T_LDOUBLE;
            } else {
                type.type = T_LONG;
            }
            break;
        case FLOAT:
            next();
            type.type = T_FLOAT;
            break;
        case DOUBLE:
            next();
            if (type.type == T_LONG) {
                type.type = T_LDOUBLE;
            } else {
                type.type = T_DOUBLE;
            }
            break;
        case CONST:
            next();
            type = type_set_const(type);
            break;
        case VOLATILE:
            next();
            type = type_set_volatile(type);
            break;
        case IDENTIFIER:
            tagged = get_typedef(tok.d.string);
            if (tagged) {
                next();
                type = type_apply_qualifiers(*tagged, type);
                break;
            }
            goto done;
        case UNION:
        case STRUCT:
            other = struct_or_union_declaration();
            type = type_apply_qualifiers(other, type);
            break;
        case ENUM:
            enum_declaration();
            type.type = T_INT;
            break;
        case INLINE:
            next();
            if (!is_inline) {
                error("Unexpected 'inline' specifier.");
            } else if (*is_inline) {
                error("Multiple 'inline' specifiers.");
            } else {
                *is_inline = 1;
            }
            break;
        case AUTO:
        case REGISTER:
        case STATIC:
        case EXTERN:
        case TYPEDEF:
            next();
            if (!storage_class) {
                error("Unexpected storage class in qualifier list.");
            } else if (*storage_class != '$') {
                error("Multiple storage class specifiers.");
            } else {
                *storage_class = tok.token;
            }
            break;
        default:
            goto done;
        }
    }

done:
    if (type.type == -1) {
        type.type = T_INT;
    }

    return type;
}

/* Define __func__ as static const char __func__[] = sym->name; */
static void define_builtin__func__(String name)
{
    Type type;
    struct symbol *sym;
    assert(current_scope_depth(&ns_ident) == 1);
    assert(context.standard >= STD_C99);

    /*
     * Just add the symbol directly as a special string value. No
     * explicit assignment reflected in the IR.
     */
    type = type_create_array(basic_type__char, (size_t) name.len + 1);
    sym = sym_add(
        &ns_ident,
        str_init("__func__"),
        type,
        SYM_STRING_VALUE,
        LINK_INTERN);
    sym->value.string = name;
}

/*
 * Parse old-style function definition parameter declarations if present
 * before opening bracket.
 *
 * Verify in the end that all variables have been declared, and add to
 * symbol table parameters that have not been declared old-style.
 * Default to int for parameters that are given without type in the
 * function signature.
 */
static struct block *parameter_declaration_list(
    struct definition *def,
    struct block *block,
    Type type)
{
    int i;
    struct member *param;

    assert(is_function(type));
    assert(current_scope_depth(&ns_ident) == 1);
    while (peek().token != '{') {
        block = declaration(def, block);
    }

    for (i = 0; i < nmembers(type); ++i) {
        param = get_member(type, i);
        if (!param->name.len) {
            error("Missing parameter name at position %d.", i + 1);
            exit(1);
        }
        if (is_type_placeholder(param->type)) {
            param->type = basic_type__int;
        }
        assert(!is_array(param->type));
        if (!param->sym) {
            param->sym = sym_lookup(&ns_ident, param->name);
            if (!param->sym || param->sym->depth != 1) {
                param->sym = sym_add(&ns_ident,
                    param->name,
                    param->type,
                    SYM_DEFINITION,
                    LINK_NONE);
            }
        } else {
            assert(param->sym->depth == current_scope_depth(&ns_ident));
            sym_make_visible(&ns_ident, param->sym);
        }
        array_push_back(&def->params, param->sym);
    }

    return block;
}

static struct block *declare_vla(
    struct definition *def,
    struct block *block,
    struct symbol *sym)
{
    struct symbol *addr;

    assert(is_vla(sym->type));
    addr = sym_create_temporary(type_create_pointer(type_next(sym->type)));
    array_push_back(&def->locals, addr);
    sym->value.vla_address = addr;
    eval_vla_alloc(def, block, sym);
    return block;
}

/*
 * Parse declaration, possibly with initializer. New symbols are added
 * to the symbol table.
 *
 * Cover external declarations, functions, and local declarations
 * (with optional initialization code) inside functions.
 */
INTERNAL struct block *init_declarator(
    struct definition *def,
    struct block *parent,
    Type base,
    enum symtype symtype,
    enum linkage linkage)
{
    Type type;
    String name = {0};
    struct symbol *sym;
    const struct member *param;

    if (linkage == LINK_INTERN && current_scope_depth(&ns_ident) != 0) {
        declarator(def, cfg_block_init(def), base, &type, &name);
    } else {
        parent = declarator(def, parent, base, &type, &name);
    }

    if (!name.len) {
        return parent;
    }

    if (symtype == SYM_TYPEDEF) {
        /* */
    } else if (is_function(type)) {
        symtype = SYM_DECLARATION;
        linkage = (linkage == LINK_NONE) ? LINK_EXTERN : linkage;
        if (linkage == LINK_INTERN && current_scope_depth(&ns_ident)) {
            error("Cannot declare static function in block scope.");
            exit(1);
        }
    } else if (is_variably_modified(type)) {
        if (current_scope_depth(&ns_ident) == 0) {
            error("Invalid variably modified type at file scope.");
            exit(1);
        } else if (linkage != LINK_NONE
            && !(is_pointer(type) && linkage == LINK_INTERN))
        {
            error("Invalid linkage for block scoped variably modified type.");
            exit(1);
        }
    }

    sym = sym_add(&ns_ident, name, type, symtype, linkage);
    switch (current_scope_depth(&ns_ident)) {
    case 0: break;
    case 1: /* Parameters from old-style function definitions. */
        assert(def->symbol);
        param = find_type_member(def->symbol->type, name, NULL);
        if (is_array(type)) {
            sym->type = type_create_pointer(type_next(type));
        }
        if (param && is_type_placeholder(param->type)) {
            ((struct member *) param)->type = sym->type;
        } else {
            error("Invalid parameter declaration of %s.", str_raw(name));
            exit(1);
        }
        break;
    default:
        if (symtype == SYM_DEFINITION) {
            assert(linkage == LINK_NONE);
            array_push_back(&def->locals, sym);
            if (is_vla(type)) {
                parent = declare_vla(def, parent, sym);
            }
        }
        break;
    }

    switch (peek().token) {
    case '=':
        if (sym->symtype == SYM_DECLARATION) {
            error("Extern symbol '%s' cannot be initialized.",
                str_raw(sym->name));
            exit(1);
        }
        if (!sym->depth && sym->symtype == SYM_DEFINITION) {
            error("Symbol '%s' was already defined.", str_raw(sym->name));
            exit(1);
        }
        if (is_vla(sym->type)) {
            error("Variable length array cannot be initialized.");
            exit(1);
        }
        consume('=');
        sym->symtype = SYM_DEFINITION;
        parent = initializer(def, parent, sym);
        assert(size_of(sym->type) > 0);
        if (sym->linkage != LINK_NONE) {
            cfg_define(def, sym);
        }
        break;
    case IDENTIFIER:
    case FIRST(type_specifier):
    case FIRST(type_qualifier):
    case REGISTER:
    case '{':
        assert(sym->linkage != LINK_NONE);
        if (is_function(sym->type)) {
            sym->symtype = SYM_DEFINITION;
            cfg_define(def, sym);
            push_scope(&ns_label);
            push_scope(&ns_ident);
            parent = parameter_declaration_list(def, parent, type);
            if (context.standard >= STD_C99) {
                define_builtin__func__(sym->name);
            }
            parent = block(def, parent);
            pop_scope(&ns_label);
            pop_scope(&ns_ident);
            return parent;
        }
    default:
        break;
    }

    if (linkage == LINK_INTERN
        || (is_function(sym->type) && sym->symtype != SYM_DEFINITION))
    {
        type_clean_prototype(sym->type);
    }

    return parent;
}

static void static_assertion(void)
{
    struct var val;
    String message;

    consume(STATIC_ASSERT);
    consume('(');

    val = constant_expression();
    consume(',');
    message = consume(STRING).d.string;

    if (val.kind != IMMEDIATE || !is_integer(val.type)) {
        error("Expression in static assertion must be an integer constant.");
        exit(1);
    }

    if (val.imm.i == 0) {
        error(str_raw(message));
        exit(1);
    }

    consume(')');
}

/*
 * Parse a declaration list, beginning with a base set of specifiers,
 * followed by a list of declarators.
 *
 * Each new global declaration is assigned a clean 'struct definition'
 * object, which might get filled with initialization code, or the body
 * of a function.
 *
 * Terminate on hitting a function definition, otherwise read until the
 * end of statement.
 */
INTERNAL struct block *declaration(
    struct definition *def,
    struct block *parent)
{
    Type base;
    enum symtype symtype;
    enum linkage linkage;
    struct definition *decl;
    int storage_class, is_inline;

    if (peek().token == STATIC_ASSERT) {
        static_assertion();
        consume(';');
        return parent;
    }

    base = declaration_specifiers(&storage_class, &is_inline);
    switch (storage_class) {
    case EXTERN:
        symtype = SYM_DECLARATION;
        linkage = LINK_EXTERN;
        break;
    case STATIC:
        symtype = SYM_TENTATIVE;
        linkage = LINK_INTERN;
        break;
    case TYPEDEF:
        symtype = SYM_TYPEDEF;
        linkage = LINK_NONE;
        break;
    default:
        if (!current_scope_depth(&ns_ident)) {
            symtype = SYM_TENTATIVE;
            linkage = LINK_EXTERN;
        } else {
            symtype = SYM_DEFINITION;
            linkage = LINK_NONE;
        }
        break;
    }

    while (1) {
        if (linkage == LINK_INTERN || linkage == LINK_EXTERN) {
            decl = cfg_init();
            init_declarator(decl, decl->body, base, symtype, linkage);
            if (!decl->symbol) {
                cfg_discard(decl);
            } else if (is_function(decl->symbol->type)) {
                return parent;
            }
        } else {
            parent = init_declarator(def, parent, base, symtype, linkage);
        }

        if (peek().token == ',') {
            next();
        } else break;
    }

    consume(';');
    return parent;
}
