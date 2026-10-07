#include "parser.h"

#include "decimal.h"
#include "lexer.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct LoopContext {
    size_t start_index;
    size_t *destroy_jumps;
    size_t destroy_count;
} LoopContext;

typedef struct Parser {
    ZSharpLexer lexer;
    ZSharpToken current;
    int failed;
    ZSharpDiagnostic *diagnostic;
    LoopContext loops[64];
    size_t loop_depth;
    size_t named_outcome_depth;
    const ZSharpCustomSyntaxRule *syntax_rules;
    size_t syntax_rule_count;
    ZSharpRoom *member_room;
    const char *source_name;
} Parser;

static void fail_at(Parser *parser, const ZSharpToken *token,
                    const char *format, ...) {
    va_list arguments;
    if (parser->failed) return;
    parser->failed = 1;
    parser->diagnostic->line = token->line;
    parser->diagnostic->column = token->column;
    va_start(arguments, format);
    vsnprintf(parser->diagnostic->message,
              sizeof(parser->diagnostic->message), format, arguments);
    va_end(arguments);
}

static void advance_token(Parser *parser) {
    if (parser->failed) return;
    parser->current = zsharp_lexer_next(&parser->lexer);
    if (parser->current.type == ZTOKEN_ERROR) {
        if (parser->current.length > 0 && parser->current.start[0] == '"') {
            fail_at(parser, &parser->current, "unterminated text value");
        } else {
            fail_at(parser, &parser->current, "unexpected character '%.*s'",
                    (int)parser->current.length, parser->current.start);
        }
    }
}

static int match_type(Parser *parser, ZSharpTokenType type) {
    if (parser->current.type != type) return 0;
    advance_token(parser);
    return 1;
}

static int match_word(Parser *parser, const char *word) {
    if (!zsharp_token_equals(&parser->current, word)) return 0;
    advance_token(parser);
    return 1;
}

static int consume_type(Parser *parser, ZSharpTokenType type,
                        const char *description) {
    if (match_type(parser, type)) return 1;
    fail_at(parser, &parser->current, "expected %s", description);
    return 0;
}

static int consume_word(Parser *parser, const char *word) {
    if (match_word(parser, word)) return 1;
    fail_at(parser, &parser->current, "expected '%s'", word);
    return 0;
}

static char *copy_token(Parser *parser, const ZSharpToken *token) {
    char *copy = zsharp_copy_text(token->start, token->length);
    if (copy == NULL) fail_at(parser, token, "out of memory");
    return copy;
}

static char *consume_name(Parser *parser, const char *description) {
    ZSharpToken token = parser->current;
    char *name;
    if (token.type != ZTOKEN_IDENTIFIER) {
        fail_at(parser, &token, "expected %s", description);
        return NULL;
    }
    name = copy_token(parser, &token);
    advance_token(parser);
    return name;
}

static char *join_path_parts(Parser *parser, char **parts, size_t count) {
    size_t total = count > 0 ? count - 1 : 0;
    size_t index;
    char *path;
    char *output;
    for (index = 0; index < count; index++) {
        size_t length = strlen(parts[index]);
        if (total > SIZE_MAX - length) {
            fail_at(parser, &parser->current, "qualified path is too large");
            return NULL;
        }
        total += length;
    }
    path = (char *)malloc(total + 1);
    if (path == NULL) {
        fail_at(parser, &parser->current, "out of memory");
        return NULL;
    }
    output = path;
    for (index = 0; index < count; index++) {
        size_t length = strlen(parts[index]);
        if (index > 0) *output++ = '.';
        memcpy(output, parts[index], length);
        output += length;
    }
    *output = '\0';
    return path;
}

static char *consume_dotted_path(Parser *parser, const char *description,
                                 size_t maximum_parts,
                                 size_t *part_count_output) {
    char *parts[64] = {0};
    size_t part_count = 0;
    size_t index;
    char *path;
    parts[part_count++] = consume_name(parser, description);
    while (!parser->failed && match_type(parser, ZTOKEN_DOT)) {
        if (part_count == maximum_parts || part_count == 64) {
            fail_at(parser, &parser->current,
                    "this path can contain at most %zu names",
                    maximum_parts);
            break;
        }
        parts[part_count++] = consume_name(parser, "a name after '.'");
    }
    if (parser->failed) {
        for (index = 0; index < part_count; index++) free(parts[index]);
        return NULL;
    }
    path = join_path_parts(parser, parts, part_count);
    for (index = 0; index < part_count; index++) free(parts[index]);
    if (path != NULL) *part_count_output = part_count;
    return path;
}

static int next_token_is_word(const Parser *parser, const char *word) {
    ZSharpLexer lexer = parser->lexer;
    ZSharpToken token = zsharp_lexer_next(&lexer);
    return zsharp_token_equals(&token, word);
}

static char *decode_text(Parser *parser, const ZSharpToken *token) {
    const char *input = token->start + 1;
    const char *end = token->start + token->length - 1;
    char *result = (char *)malloc(token->length);
    char *output = result;
    if (result == NULL) {
        fail_at(parser, token, "out of memory");
        return NULL;
    }
    while (input < end) {
        if (*input == '\\' && input + 1 < end) {
            input++;
            switch (*input) {
                case 'n': *output++ = '\n'; break;
                case 'r': *output++ = '\r'; break;
                case 't': *output++ = '\t'; break;
                case '"': *output++ = '"'; break;
                case '\\': *output++ = '\\'; break;
                default:
                    free(result);
                    fail_at(parser, token, "unsupported text escape '\\%c'",
                            *input);
                    return NULL;
            }
            input++;
        } else {
            *output++ = *input++;
        }
    }
    *output = '\0';
    return result;
}

static int consume_number_text(Parser *parser, char **value) {
    ZSharpToken first = parser->current;
    ZSharpToken last = first;
    char *raw;
    char error[128] = {0};
    if (first.type != ZTOKEN_NUMBER) {
        fail_at(parser, &first, "expected a number");
        return 0;
    }
    advance_token(parser);
    if (parser->current.type == ZTOKEN_DOT &&
        first.start + first.length == parser->current.start) {
        ZSharpLexer lookahead = parser->lexer;
        ZSharpToken fraction = zsharp_lexer_next(&lookahead);
        if (fraction.type == ZTOKEN_NUMBER &&
            parser->current.start + parser->current.length == fraction.start) {
            last = fraction;
            advance_token(parser);
            advance_token(parser);
        }
    }
    raw = zsharp_copy_text(first.start,
                           (size_t)((last.start + last.length) - first.start));
    if (raw == NULL) {
        fail_at(parser, &first, "out of memory");
        return 0;
    }
    if (!zsharp_decimal_normalize(raw, value, error, sizeof(error))) {
        fail_at(parser, &first, "%s", error);
        free(raw);
        return 0;
    }
    free(raw);
    return 1;
}

static int consume_signed_number_text(Parser *parser, char **value) {
    int negative = match_type(parser, ZTOKEN_MINUS);
    char *negated;
    char error[128] = {0};
    if (!consume_number_text(parser, value)) return 0;
    if (!negative) return 1;
    negated = zsharp_decimal_negate(*value, error, sizeof(error));
    if (negated == NULL) {
        fail_at(parser, &parser->current, "%s", error);
        free(*value);
        *value = NULL;
        return 0;
    }
    free(*value);
    *value = negated;
    return 1;
}

static int parse_visibility(Parser *parser, int *is_public) {
    if (match_word(parser, "noticed")) {
        *is_public = 1;
        return 1;
    }
    if (match_word(parser, "silent")) {
        *is_public = 0;
        return 1;
    }
    fail_at(parser, &parser->current, "expected 'noticed' or 'silent'");
    return 0;
}

static ZSharpInstruction *emit(Parser *parser, ZSharpFunction *function,
                               ZSharpOpCode op) {
    ZSharpInstruction *instruction =
        zsharp_function_add_instruction(function);
    if (instruction == NULL) {
        fail_at(parser, &parser->current, "out of memory");
        return NULL;
    }
    instruction->op = op;
    return instruction;
}

static int parse_expression(Parser *parser, ZSharpFunction *function);
static int custom_expression_matches(Parser *parser, const char *pattern);
static int parse_custom_statement(Parser *parser, ZSharpFunction *function,
                                  const ZSharpCustomSyntaxRule *rule);
static int parse_qualified_call(Parser *parser, ZSharpFunction *function,
                                int produces_value);

static int parse_primary(Parser *parser, ZSharpFunction *function) {
    ZSharpToken token = parser->current;
    ZSharpInstruction *instruction;
    size_t syntax_index;
    for (syntax_index = 0; syntax_index < parser->syntax_rule_count; syntax_index++) {
        const ZSharpCustomSyntaxRule *rule = &parser->syntax_rules[syntax_index];
        if (rule->is_expression && custom_expression_matches(parser, rule->pattern))
            return parse_custom_statement(parser, function, rule);
    }
    if (parser->current.type == ZTOKEN_NUMBER) {
        char *number_text = NULL;
        if (!consume_number_text(parser, &number_text)) return 0;
        instruction = emit(parser, function, ZOP_PUSH_NUMBER);
        if (instruction == NULL) {
            free(number_text);
            return 0;
        }
        instruction->operand = number_text;
        return 1;
    }
    if (match_type(parser, ZTOKEN_STRING)) {
        instruction = emit(parser, function, ZOP_PUSH_TEXT);
        if (instruction == NULL) return 0;
        instruction->operand = decode_text(parser, &token);
        return instruction->operand != NULL;
    }
    if (match_type(parser, ZTOKEN_LEFT_PAREN)) {
        if (!parse_expression(parser, function)) return 0;
        return consume_type(parser, ZTOKEN_RIGHT_PAREN,
                            "')' after the expression");
    }
    if (zsharp_token_equals(&token, "alive") ||
        zsharp_token_equals(&token, "dead")) {
        instruction = emit(parser, function, ZOP_PUSH_STATUS);
        if (instruction == NULL) return 0;
        instruction->number_operand = zsharp_token_equals(&token, "alive");
        advance_token(parser);
        return 1;
    }
    if (zsharp_token_equals(&token, "null")) {
        advance_token(parser);
        return emit(parser, function, ZOP_PUSH_NULL) != NULL;
    }
    if (token.type == ZTOKEN_IDENTIFIER) {
        char *name;
        char *parts[5] = {0};
        size_t part_count = 1;
        size_t part_index;
        int qualified = zsharp_token_equals(&token, "number") ||
                        zsharp_token_equals(&token, "var");
        advance_token(parser);
        if (zsharp_token_equals(&token, "JSON") &&
            match_type(parser, ZTOKEN_DOT)) {
            char *schema_name;
            if (!consume_word(parser, "load") ||
                !consume_type(parser, ZTOKEN_LEFT_PAREN,
                              "'(' after JSON.load")) return 0;
            schema_name = consume_name(parser, "a JSON schema name");
            if (schema_name == NULL ||
                !consume_type(parser, ZTOKEN_COMMA,
                              "',' after the JSON schema") ||
                !parse_expression(parser, function) ||
                !consume_type(parser, ZTOKEN_RIGHT_PAREN,
                              "')' after the JSON path")) {
                free(schema_name);
                return 0;
            }
            instruction = emit(parser, function, ZOP_JSON_LOAD);
            if (instruction == NULL) {
                free(schema_name);
                return 0;
            }
            instruction->operand = schema_name;
            return 1;
        }
        if (zsharp_token_equals(&token, "File") &&
            match_type(parser, ZTOKEN_DOT)) {
            char *method = consume_name(parser, "read, exists, or searchExtension");
            ZSharpOpCode operation =
                method != NULL && strcmp(method, "read") == 0 ? ZOP_FILE_READ :
                method != NULL && strcmp(method, "exists") == 0 ? ZOP_FILE_EXISTS :
                method != NULL && strcmp(method, "searchExtension") == 0 ? ZOP_FILE_SEARCH_EXTENSION :
                0;
            free(method);
            if (operation == 0) {
                fail_at(parser, &token,
                        "File expressions support read, exists, or searchExtension");
                return 0;
            }
            if (!consume_type(parser, ZTOKEN_LEFT_PAREN,
                              "'(' after the File operation") ||
                !parse_expression(parser, function) ||
                !consume_type(parser, ZTOKEN_RIGHT_PAREN,
                              "')' after the file path")) return 0;
            if (emit(parser, function, operation) == NULL) return 0;
            if (operation == ZOP_FILE_SEARCH_EXTENSION &&
                match_type(parser, ZTOKEN_LEFT_BRACKET)) {
                if (!parse_expression(parser, function) ||
                    !consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                                  "']' after the search result index") ||
                    emit(parser, function, ZOP_GET_INDEX) == NULL) return 0;
            }
            return 1;
        }
        if (zsharp_token_equals(&token, "Regex") && match_type(parser, ZTOKEN_DOT)) {
            char *method = consume_name(parser, "test, matches, or replace");
            int kind = method && strcmp(method, "test") == 0 ? 1 :
                       method && strcmp(method, "matches") == 0 ? 2 :
                       method && strcmp(method, "replace") == 0 ? 3 : 0;
            uint32_t count = 0, required = kind == 3 ? 3 : 2;
            free(method);
            if (!kind) { fail_at(parser, &token, "Regex supports test, matches, or replace"); return 0; }
            if (!consume_type(parser, ZTOKEN_LEFT_PAREN, "'(' after Regex method")) return 0;
            do {
                if (!parse_expression(parser, function)) return 0;
                count++;
            } while (count <= required && match_type(parser, ZTOKEN_COMMA));
            if (count < required || count > required + 1) {
                fail_at(parser, &token, "Regex method has an incorrect argument count"); return 0;
            }
            if (!consume_type(parser, ZTOKEN_RIGHT_PAREN, "')' after Regex arguments")) return 0;
            instruction = emit(parser, function, ZOP_REGEX);
            if (!instruction) return 0;
            instruction->number_operand = kind;
            instruction->argument_count = count;
            if (kind == 2 && match_type(parser, ZTOKEN_LEFT_BRACKET)) {
                if (!parse_expression(parser, function) || !consume_type(parser, ZTOKEN_RIGHT_BRACKET, "']' after match index") ||
                    !emit(parser, function, ZOP_GET_INDEX)) return 0;
            }
            return 1;
        }
        if (zsharp_token_equals(&token, "Math") &&
            match_type(parser, ZTOKEN_DOT)) {
            char *method = consume_name(parser, "a Math function name");
            int kind = method != NULL && strcmp(method, "sin") == 0 ? 1 :
                       method != NULL && strcmp(method, "cos") == 0 ? 2 :
                       method != NULL && strcmp(method, "tan") == 0 ? 3 :
                       method != NULL && strcmp(method, "sqrt") == 0 ? 4 :
                       method != NULL && strcmp(method, "abs") == 0 ? 5 :
                       method != NULL && strcmp(method, "min") == 0 ? 6 :
                       method != NULL && strcmp(method, "max") == 0 ? 7 :
                       method != NULL && strcmp(method, "radians") == 0 ? 8 :
                       method != NULL && strcmp(method, "degrees") == 0 ? 9 :
                       method != NULL && strcmp(method, "pow") == 0 ? 10 : 0;
            uint32_t arguments = kind == 6 || kind == 7 || kind == 10 ? 2u : 1u;
            free(method);
            if (kind == 0) {
                fail_at(parser, &token,
                        "Math supports sin, cos, tan, sqrt, abs, min, max, radians, degrees, or pow");
                return 0;
            }
            if (!consume_type(parser, ZTOKEN_LEFT_PAREN,
                              "'(' after the Math function") ||
                !parse_expression(parser, function)) return 0;
            if (arguments == 2 &&
                (!consume_type(parser, ZTOKEN_COMMA,
                               "',' between Math arguments") ||
                 !parse_expression(parser, function))) return 0;
            if (!consume_type(parser, ZTOKEN_RIGHT_PAREN,
                              "')' after the Math arguments")) return 0;
            instruction = emit(parser, function, ZOP_MATH);
            if (instruction == NULL) return 0;
            instruction->number_operand = kind;
            instruction->argument_count = arguments;
            return 1;
        }
        if (zsharp_token_equals(&token, "random") &&
            match_type(parser, ZTOKEN_DOT)) {
            char *method = consume_name(parser, "number, decimal, or chance");
            int kind = method != NULL && strcmp(method, "number") == 0 ? 1 :
                       method != NULL && strcmp(method, "decimal") == 0 ? 2 :
                       method != NULL && strcmp(method, "chance") == 0 ? 3 : 0;
            uint32_t arguments = kind == 3 ? 1u : 2u;
            free(method);
            if (kind == 0) {
                fail_at(parser, &token,
                        "random supports number, decimal, or chance");
                return 0;
            }
            if (!consume_type(parser, ZTOKEN_LEFT_PAREN,
                              "'(' after the random function") ||
                !parse_expression(parser, function)) return 0;
            if (arguments == 2 &&
                (!consume_type(parser, ZTOKEN_COMMA,
                               "',' between random bounds") ||
                 !parse_expression(parser, function))) return 0;
            if (!consume_type(parser, ZTOKEN_RIGHT_PAREN,
                              "')' after the random arguments")) return 0;
            instruction = emit(parser, function, ZOP_RANDOM);
            if (instruction == NULL) return 0;
            instruction->number_operand = kind;
            instruction->argument_count = arguments;
            return 1;
        }
        if (zsharp_token_equals(&token, "Function")) {
            return parse_qualified_call(parser, function, 1);
        }
        if (qualified && match_type(parser, ZTOKEN_DOT)) {
            name = consume_name(parser, "a name after the qualifier");
        } else {
            name = copy_token(parser, &token);
        }
        if (name == NULL) return 0;
        if (match_type(parser, ZTOKEN_LEFT_PAREN)) {
            if (qualified) {
                free(name);
                fail_at(parser, &token,
                        "qualified values cannot be called as functions");
                return 0;
            }
            if (strcmp(name, "addition") == 0) {
                int parsed = parse_expression(parser, function) &&
                    consume_type(parser, ZTOKEN_RIGHT_PAREN,
                                 "')' after the addition value");
                free(name);
                return parsed;
            }
            fail_at(parser, &token,
                    "declared functions must be called with 'Function.call'");
            free(name);
            return 0;
        }
        parts[0] = name;
        while (!qualified && match_type(parser, ZTOKEN_DOT)) {
            if (part_count == 5) {
                for (part_index = 0; part_index < part_count; part_index++) {
                    free(parts[part_index]);
                }
                fail_at(parser, &parser->current,
                        "a value path can contain at most five names");
                return 0;
            }
            parts[part_count] =
                consume_name(parser, "a name after '.' in the value path");
            if (parts[part_count] == NULL) {
                for (part_index = 0; part_index < part_count; part_index++) {
                    free(parts[part_index]);
                }
                return 0;
            }
            part_count++;
        }
        {
        int reads_length = part_count > 1 &&
                           strcmp(parts[part_count - 1], "Length") == 0;
        size_t load_count = reads_length ? part_count - 1 : part_count;
        instruction = emit(parser, function,
                           load_count == 1 ? ZOP_LOAD_NAME : ZOP_LOAD_PATH);
        if (instruction == NULL) {
            for (part_index = 0; part_index < part_count; part_index++) {
                free(parts[part_index]);
            }
            return 0;
        }
        if (load_count == 1) {
            instruction->operand = parts[0];
            parts[0] = NULL;
        } else {
            instruction->operand = join_path_parts(parser, parts, load_count);
            instruction->argument_count = (uint32_t)load_count;
            if (instruction->operand == NULL) {
                for (part_index = 0; part_index < part_count; part_index++) {
                    free(parts[part_index]);
                }
                return 0;
            }
        }
        for (part_index = 0; part_index < part_count; part_index++) {
            free(parts[part_index]);
        }
        if (reads_length &&
            emit(parser, function, ZOP_ARRAY_LENGTH) == NULL) return 0;
        }
        if (match_type(parser, ZTOKEN_LEFT_BRACKET)) {
            if (!parse_expression(parser, function) ||
                !consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                              "']' after the array index") ||
                emit(parser, function, ZOP_GET_INDEX) == NULL) {
                return 0;
            }
            if (match_type(parser, ZTOKEN_DOT)) {
                char *member = consume_name(
                    parser, "a member name after the indexed object");
                instruction = emit(parser, function, ZOP_GET_MEMBER);
                if (instruction == NULL) {
                    free(member);
                    return 0;
                }
                instruction->operand = member;
            }
        }
        return 1;
    }
    fail_at(parser, &parser->current, "expected a value or expression");
    return 0;
}

static int parse_unary(Parser *parser, ZSharpFunction *function) {
    if (match_word(parser, "not")) {
        return parse_unary(parser, function) &&
               emit(parser, function, ZOP_NOT) != NULL;
    }
    if (match_type(parser, ZTOKEN_MINUS)) {
        return parse_unary(parser, function) &&
               emit(parser, function, ZOP_NEGATE) != NULL;
    }
    return parse_primary(parser, function);
}

static int parse_multiplication(Parser *parser, ZSharpFunction *function) {
    if (!parse_unary(parser, function)) return 0;
    while (parser->current.type == ZTOKEN_STAR ||
           parser->current.type == ZTOKEN_SLASH ||
           parser->current.type == ZTOKEN_PERCENT) {
        ZSharpOpCode operation = parser->current.type == ZTOKEN_STAR
            ? ZOP_MULTIPLY
            : parser->current.type == ZTOKEN_SLASH ? ZOP_DIVIDE
                                                   : ZOP_REMAINDER;
        advance_token(parser);
        if (!parse_unary(parser, function) ||
            emit(parser, function, operation) == NULL) {
            return 0;
        }
    }
    return 1;
}

static int parse_addition(Parser *parser, ZSharpFunction *function) {
    if (!parse_multiplication(parser, function)) return 0;
    while (parser->current.type == ZTOKEN_PLUS ||
           parser->current.type == ZTOKEN_MINUS) {
        ZSharpOpCode operation = parser->current.type == ZTOKEN_PLUS
            ? ZOP_ADD
            : ZOP_SUBTRACT;
        advance_token(parser);
        if (!parse_multiplication(parser, function) ||
            emit(parser, function, operation) == NULL) {
            return 0;
        }
    }
    return 1;
}

static int parse_comparison(Parser *parser, ZSharpFunction *function) {
    if (!parse_addition(parser, function)) return 0;
    while (parser->current.type == ZTOKEN_GREATER_EQUAL ||
           parser->current.type == ZTOKEN_GREATER ||
           parser->current.type == ZTOKEN_LESS_EQUAL ||
           parser->current.type == ZTOKEN_LESS) {
        ZSharpOpCode operation;
        if (parser->current.type == ZTOKEN_GREATER_EQUAL) {
            operation = ZOP_GREATER_EQUAL;
        } else if (parser->current.type == ZTOKEN_GREATER) {
            operation = ZOP_GREATER;
        } else if (parser->current.type == ZTOKEN_LESS_EQUAL) {
            operation = ZOP_LESS_EQUAL;
        } else {
            operation = ZOP_LESS;
        }
        advance_token(parser);
        if (!parse_addition(parser, function) ||
            emit(parser, function, operation) == NULL) {
            return 0;
        }
    }
    return 1;
}

static int parse_equality(Parser *parser, ZSharpFunction *function) {
    if (!parse_comparison(parser, function)) return 0;
    while (parser->current.type == ZTOKEN_EQUAL_EQUAL ||
           parser->current.type == ZTOKEN_BANG_EQUAL) {
        ZSharpOpCode operation = parser->current.type == ZTOKEN_EQUAL_EQUAL
            ? ZOP_EQUAL
            : ZOP_NOT_EQUAL;
        advance_token(parser);
        if (!parse_comparison(parser, function) ||
            emit(parser, function, operation) == NULL) {
            return 0;
        }
    }
    return 1;
}

static int parse_and(Parser *parser, ZSharpFunction *function) {
    if (!parse_equality(parser, function)) return 0;
    while (match_word(parser, "and")) {
        if (!parse_equality(parser, function) ||
            emit(parser, function, ZOP_AND) == NULL) {
            return 0;
        }
    }
    return 1;
}

static int parse_expression(Parser *parser, ZSharpFunction *function) {
    if (!parse_and(parser, function)) return 0;
    while (match_word(parser, "or")) {
        if (!parse_and(parser, function) ||
            emit(parser, function, ZOP_OR) == NULL) {
            return 0;
        }
    }
    return 1;
}

static int parse_print(Parser *parser, ZSharpFunction *function) {
    int update = 0;
    if (!consume_type(parser, ZTOKEN_LEFT_PAREN, "'(' after 'Print'") ||
        !parse_expression(parser, function) ||
        !consume_type(parser, ZTOKEN_RIGHT_PAREN,
                      "')' after the Print value")) {
        return 0;
    }
    if (match_type(parser, ZTOKEN_DOT)) {
        if (!consume_word(parser, "update")) return 0;
        update = 1;
    }
    if (!consume_type(parser, ZTOKEN_COLON,
                      "':' after the Print statement")) return 0;
    return emit(parser, function, update ? ZOP_PRINT_UPDATE : ZOP_PRINT) != NULL;
}

static int parse_qualified_call(Parser *parser, ZSharpFunction *function,
                                int produces_value) {
    char *parts[66] = {0};
    size_t part_count = 0;
    size_t index;
    uint32_t argument_count = 0;
    char *call_outcome = NULL;
    ZSharpInstruction *instruction;
    if (!consume_type(parser, ZTOKEN_DOT, "'.' after 'Function'") ||
        !consume_word(parser, "call") ||
        !consume_type(parser, ZTOKEN_LEFT_PAREN, "'(' after 'Function.call'")) {
        return 0;
    }
    parts[part_count++] = consume_name(parser, "the first call target name");
    while (!parser->failed &&
           (parser->current.type == ZTOKEN_COLON ||
            parser->current.type == ZTOKEN_DOT)) {
        if (part_count == 66) {
            fail_at(parser, &parser->current,
                    "Function.call target is too long");
            break;
        }
        advance_token(parser);
        parts[part_count++] =
            consume_name(parser, "the next Function.call target name");
    }
    if (!parser->failed && strcmp(parts[0], "py") != 0 &&
        strcmp(parts[0], "js") != 0 &&
        strcmp(parts[0], "lua") != 0 &&
        strcmp(parts[0], "c") != 0 &&
        strcmp(parts[0], "cpp") != 0 &&
        strcmp(parts[0], "kt") != 0 &&
        strcmp(parts[0], "rust") != 0 &&
        part_count != 1 && part_count != 3 && part_count != 4) {
        fail_at(parser, &parser->current,
                "Function.call requires File.Room.Function or "
                "Project.File.Room.Function");
    }
    if (!parser->failed &&
        (strcmp(parts[0], "py") == 0 || strcmp(parts[0], "js") == 0 ||
         strcmp(parts[0], "lua") == 0 || strcmp(parts[0], "c") == 0 ||
         strcmp(parts[0], "cpp") == 0 ||
         strcmp(parts[0], "kt") == 0 ||
         strcmp(parts[0], "rust") == 0) &&
        part_count < 3) {
        fail_at(parser, &parser->current,
                "foreign calls require LANGUAGE:Path.To.File:function");
    }
    if (!parser->failed && match_type(parser, ZTOKEN_LEFT_BRACKET)) {
        if (parser->current.type != ZTOKEN_RIGHT_BRACKET) {
            do {
                if (!parse_expression(parser, function)) break;
                if (argument_count == UINT32_MAX) {
                    fail_at(parser, &parser->current,
                            "Function.call has too many arguments");
                    break;
                }
                argument_count++;
            } while (match_type(parser, ZTOKEN_COMMA));
        }
        consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                     "']' after the Function.call arguments");
    }
    if (!parser->failed) {
        consume_type(parser, ZTOKEN_RIGHT_PAREN,
                     "')' after the Function.call target");
        if (!parser->failed && match_type(parser, ZTOKEN_LEFT_BRACKET)) {
            call_outcome = consume_name(parser, "the named brain outcome");
            consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                         "']' after the named brain outcome");
        }
        if (!produces_value) {
            consume_type(parser, ZTOKEN_COLON,
                         "':' after the Function.call statement");
        }
    }
    if (parser->failed) {
        for (index = 0; index < part_count; index++) free(parts[index]);
        free(call_outcome);
        return 0;
    }
    if (part_count == 1) {
        ZSharpInstruction *load = emit(parser, function, ZOP_LOAD_NAME);
        if (load == NULL) { free(parts[0]); free(call_outcome); return 0; }
        load->operand = zsharp_copy_text(parts[0], strlen(parts[0]));
        if (load->operand == NULL) {
            free(parts[0]); free(call_outcome);
            fail_at(parser, &parser->current, "out of memory"); return 0;
        }
    }
    instruction = emit(parser, function, produces_value
        ? ZOP_CALL_QUALIFIED_VALUE
        : ZOP_CALL_QUALIFIED);
    if (instruction == NULL) {
        for (index = 0; index < part_count; index++) free(parts[index]);
        free(call_outcome);
        return 0;
    }
    if (part_count == 1) {
        instruction->operand = zsharp_copy_text("@callback", 9);
        free(parts[0]);
        if (instruction->operand == NULL) {
            free(call_outcome);
            fail_at(parser, &parser->current, "out of memory");
            return 0;
        }
    } else if (strcmp(parts[0], "py") == 0 || strcmp(parts[0], "js") == 0 ||
        strcmp(parts[0], "lua") == 0 || strcmp(parts[0], "c") == 0 ||
        strcmp(parts[0], "cpp") == 0 ||
        strcmp(parts[0], "kt") == 0 ||
        strcmp(parts[0], "rust") == 0) {
        size_t module_length = 0;
        char *module;
        char *cursor;
        for (index = 1; index + 1 < part_count; index++)
            module_length += strlen(parts[index]) + (index > 1 ? 1u : 0u);
        module = (char *)malloc(module_length + 1);
        instruction->operand = zsharp_copy_text(
            strcmp(parts[0], "py") == 0 ? "@py" :
            strcmp(parts[0], "js") == 0 ? "@js" :
            strcmp(parts[0], "lua") == 0 ? "@lua" :
            strcmp(parts[0], "c") == 0 ? "@c" :
            strcmp(parts[0], "cpp") == 0 ? "@cpp" :
            strcmp(parts[0], "kt") == 0 ? "@kt" : "@rust",
            (strcmp(parts[0], "lua") == 0 ||
             strcmp(parts[0], "cpp") == 0) ? 4 :
             strcmp(parts[0], "rust") == 0 ? 5 :
             strcmp(parts[0], "c") == 0 ? 2 : 3);
        instruction->call_room = zsharp_copy_text("", 0);
        if (module == NULL || instruction->operand == NULL ||
            instruction->call_room == NULL) {
            free(module);
            for (index = 0; index < part_count; index++) free(parts[index]);
            free(call_outcome);
            fail_at(parser, &parser->current, "out of memory");
            return 0;
        }
        cursor = module;
        for (index = 1; index + 1 < part_count; index++) {
            size_t length = strlen(parts[index]);
            if (index > 1) *cursor++ = '.';
            memcpy(cursor, parts[index], length);
            cursor += length;
        }
        *cursor = '\0';
        instruction->call_file = module;
        instruction->call_function = parts[part_count - 1];
        parts[part_count - 1] = NULL;
        for (index = 0; index < part_count; index++) free(parts[index]);
    } else if (part_count == 4) {
        instruction->operand = parts[0];
        instruction->call_file = parts[1];
        instruction->call_room = parts[2];
        instruction->call_function = parts[3];
    } else {
        instruction->call_file = parts[0];
        instruction->call_room = parts[1];
        instruction->call_function = parts[2];
    }
    instruction->argument_count = argument_count;
    instruction->call_outcome = call_outcome;
    return 1;
}

static int parse_number_statement(Parser *parser, ZSharpFunction *function) {
    ZSharpInstruction *instruction;
    char *name;
    size_t path_count = 1;
    if (match_type(parser, ZTOKEN_DOT)) {
        if (!consume_word(parser, "set") ||
            !consume_type(parser, ZTOKEN_COLON, "':' after 'number.set'")) {
            return 0;
        }
        name = consume_dotted_path(parser, "the number variable to set", 4,
                                   &path_count);
        if (!consume_type(parser, ZTOKEN_EQUAL,
                          "'=' after the number variable") ||
            !parse_expression(parser, function) ||
            !consume_type(parser, ZTOKEN_COLON,
                          "':' after the number.set statement")) {
            free(name);
            return 0;
        }
        instruction = emit(parser, function,
                           path_count == 1 ? ZOP_STORE_GLOBAL
                                           : ZOP_STORE_PATH);
    } else {
        name = consume_name(parser, "a local number name");
        if (!consume_type(parser, ZTOKEN_EQUAL,
                          "'=' after the local number name") ||
            !parse_expression(parser, function) ||
            !consume_type(parser, ZTOKEN_COLON,
                          "':' after the local number value")) {
            free(name);
            return 0;
        }
        instruction = emit(parser, function, ZOP_STORE_LOCAL);
    }
    if (instruction == NULL) {
        free(name);
        return 0;
    }
    instruction->operand = name;
    if (path_count > 1) instruction->argument_count = (uint32_t)path_count;
    return 1;
}

static int parse_text_statement(Parser *parser, ZSharpFunction *function) {
    ZSharpInstruction *instruction;
    char *name;
    size_t path_count = 1;
    if (match_type(parser, ZTOKEN_DOT)) {
        if (!consume_word(parser, "set") ||
            !consume_type(parser, ZTOKEN_DOT, "'.' after 'text.set'")) {
            return 0;
        }
        name = consume_dotted_path(parser, "the text value to set", 4,
                                   &path_count);
        if (!consume_type(parser, ZTOKEN_EQUAL, "'=' after the text field") ||
            !parse_expression(parser, function) ||
            !consume_type(parser, ZTOKEN_COLON,
                          "':' after the text.set statement")) {
            free(name);
            return 0;
        }
        instruction = emit(parser, function,
                           path_count == 1 ? ZOP_STORE_FIELD
                                           : ZOP_STORE_PATH);
    } else {
        if (match_type(parser, ZTOKEN_LEFT_PAREN)) {
            if (!consume_type(parser, ZTOKEN_RIGHT_PAREN,"')' in local text() type")) return 0;
            name=consume_name(parser,"a local text array name");
            if(!name || !consume_type(parser,ZTOKEN_EQUAL,"'=' after local text array") ||
               !parse_expression(parser,function) ||
               !consume_type(parser,ZTOKEN_COLON,"':' after local text array")) {free(name);return 0;}
            instruction=emit(parser,function,ZOP_STORE_LOCAL_TEXT_ARRAY);
            if(!instruction){free(name);return 0;}
            instruction->operand=name;
            return 1;
        }
        name = consume_name(parser, "a local text name");
        if (!consume_type(parser, ZTOKEN_EQUAL,
                          "'=' after the local text name") ||
            !parse_expression(parser, function) ||
            !consume_type(parser, ZTOKEN_COLON,
                          "':' after the local text value")) {
            free(name);
            return 0;
        }
        instruction = emit(parser, function, ZOP_STORE_LOCAL_TEXT);
    }
    if (instruction == NULL) {
        free(name);
        return 0;
    }
    instruction->operand = name;
    if (path_count > 1) instruction->argument_count = (uint32_t)path_count;
    return 1;
}

static char *consume_ui_color_text(Parser *parser) {
    ZSharpToken token = parser->current;
    char *color;
    size_t index;
    if (!consume_type(parser, ZTOKEN_COLOR, "a #RRGGBB color value"))
        return NULL;
    if (token.length != 7) {
        fail_at(parser, &token, "UI colors must use exactly #RRGGBB");
        return NULL;
    }
    for (index = 1; index < token.length; index++) {
        if (!isxdigit((unsigned char)token.start[index])) {
            fail_at(parser, &token, "UI colors must use hexadecimal #RRGGBB");
            return NULL;
        }
    }
    color = zsharp_copy_text(token.start, token.length);
    if (color == NULL) {
        fail_at(parser, &token, "out of memory");
        return NULL;
    }
    for (index = 1; index < token.length; index++)
        color[index] = (char)toupper((unsigned char)color[index]);
    return color;
}

static int parse_ui_paint_text(Parser *parser, char **value) {
    const char *kind;
    char *degrees = NULL;
    char **colors = NULL;
    size_t color_count = 0;
    size_t length;
    size_t index;
    char *output;
    char *cursor;
    if (parser->current.type == ZTOKEN_COLOR) {
        *value = consume_ui_color_text(parser);
        return *value != NULL;
    }
    if (zsharp_token_equals(&parser->current, "linear")) {
        kind = "linear";
    } else if (zsharp_token_equals(&parser->current, "radial")) {
        kind = "radial";
    } else {
        fail_at(parser, &parser->current,
                "expected #RRGGBB, linear-gradient, or radial-gradient");
        return 0;
    }
    advance_token(parser);
    if (!consume_type(parser, ZTOKEN_MINUS, "'-' in the gradient name") ||
        !consume_word(parser, "gradient") ||
        !consume_type(parser, ZTOKEN_LEFT_PAREN,
                      "'(' after the gradient name") ||
        !consume_signed_number_text(parser, &degrees)) {
        return 0;
    }
    do {
        char **resized;
        char *color;
        if (!consume_type(parser, ZTOKEN_COLON,
                          "':' before a gradient color")) goto failed;
        color = consume_ui_color_text(parser);
        if (color == NULL) goto failed;
        resized = (char **)realloc(colors,
            (color_count + 1) * sizeof(*colors));
        if (resized == NULL) {
            free(color);
            fail_at(parser, &parser->current, "out of memory");
            goto failed;
        }
        colors = resized;
        colors[color_count++] = color;
    } while (parser->current.type == ZTOKEN_COLON);
    if (color_count < 2) {
        fail_at(parser, &parser->current,
                "a gradient requires at least two colors");
        goto failed;
    }
    if (!consume_type(parser, ZTOKEN_RIGHT_PAREN,
                      "')' after the gradient colors")) goto failed;
    length = strlen(kind) + strlen("-gradient(") + strlen(degrees) + 2;
    for (index = 0; index < color_count; index++)
        length += 1 + strlen(colors[index]);
    output = (char *)malloc(length);
    if (output == NULL) {
        fail_at(parser, &parser->current, "out of memory");
        goto failed;
    }
    cursor = output;
    memcpy(cursor, kind, strlen(kind));
    cursor += strlen(kind);
    memcpy(cursor, "-gradient(", strlen("-gradient("));
    cursor += strlen("-gradient(");
    memcpy(cursor, degrees, strlen(degrees));
    cursor += strlen(degrees);
    for (index = 0; index < color_count; index++) {
        size_t color_length = strlen(colors[index]);
        *cursor++ = ':';
        memcpy(cursor, colors[index], color_length);
        cursor += color_length;
    }
    *cursor++ = ')';
    *cursor = '\0';
    free(degrees);
    for (index = 0; index < color_count; index++) free(colors[index]);
    free(colors);
    *value = output;
    return 1;

failed:
    free(degrees);
    for (index = 0; index < color_count; index++) free(colors[index]);
    free(colors);
    return 0;
}

static int parse_ui_update_value(Parser *parser, int32_t *type,
                                 uint32_t *unit, char **value) {
    ZSharpToken token = parser->current;
    *unit = ZUI_UNIT_NONE;
    if (token.type == ZTOKEN_STRING) {
        advance_token(parser);
        *type = ZUI_PROPERTY_TEXT;
        *value = decode_text(parser, &token);
        return *value != NULL;
    }
    if (token.type == ZTOKEN_COLOR ||
        zsharp_token_equals(&token, "linear") ||
        zsharp_token_equals(&token, "radial")) {
        *type = ZUI_PROPERTY_COLOR;
        return parse_ui_paint_text(parser, value);
    }
    if (zsharp_token_equals(&token, "alive") ||
        zsharp_token_equals(&token, "dead")) {
        *type = ZUI_PROPERTY_STATUS;
        *value = copy_token(parser, &token);
        advance_token(parser);
        return *value != NULL;
    }
    if (token.type == ZTOKEN_NUMBER || token.type == ZTOKEN_MINUS) {
        *type = ZUI_PROPERTY_MEASUREMENT;
        if (!consume_signed_number_text(parser, value)) return 0;
        *unit = ZUI_UNIT_ZU;
        if (match_word(parser, "zu")) *unit = ZUI_UNIT_ZU;
        else if (match_word(parser, "px")) *unit = ZUI_UNIT_PX;
        return 1;
    }
    if (token.type == ZTOKEN_IDENTIFIER) {
        *type = ZUI_PROPERTY_IDENTIFIER;
        *value = copy_token(parser, &token);
        advance_token(parser);
        return *value != NULL;
    }
    fail_at(parser, &token, "expected a window property value");
    return 0;
}

static int parse_delay_statement(Parser *parser, ZSharpFunction *function) {
    char *duration = NULL;
    char *milliseconds = NULL;
    char decimal_error[128] = {0};
    ZSharpInstruction *instruction;
    int seconds = 0;
    if (!consume_type(parser, ZTOKEN_LEFT_PAREN, "'(' after wait/delay") ||
        !consume_number_text(parser, &duration)) {
        free(duration);
        return 0;
    }
    if (match_word(parser, "ms")) {
        seconds = 0;
    } else if (match_word(parser, "s")) {
        seconds = 1;
    } else {
        fail_at(parser, &parser->current,
                "expected 'ms' or 's' after the wait duration");
        free(duration);
        return 0;
    }
    if (!consume_type(parser, ZTOKEN_RIGHT_PAREN,
                      "')' after the wait duration") ||
        !consume_type(parser, ZTOKEN_COLON,
                      "':' after the wait/delay statement")) {
        free(duration);
        return 0;
    }
    if (seconds) {
        milliseconds = zsharp_decimal_multiply(
            duration, "1000", decimal_error, sizeof(decimal_error));
        free(duration);
        if (milliseconds == NULL) {
            fail_at(parser, &parser->current, "%s", decimal_error);
            return 0;
        }
    } else {
        milliseconds = duration;
    }
    instruction = emit(parser, function, ZOP_DELAY);
    if (instruction == NULL) {
        free(milliseconds);
        return 0;
    }
    instruction->operand = milliseconds;
    return 1;
}

static int parse_named_statement(Parser *parser, ZSharpFunction *function) {
    ZSharpToken first_token = parser->current;
    char *parts[5] = {0};
    size_t part_count = 0;
    size_t index;
    ZSharpInstruction *instruction;
    char *path = NULL;
    char *field_name = NULL;
    uint32_t argument_count = 0;
    int is_setter = 0;
    int is_array_setter = 0;
    int is_ui_setter = 0;

    parts[part_count++] = copy_token(parser, &first_token);
    advance_token(parser);
    while (!parser->failed && match_type(parser, ZTOKEN_DOT)) {
        if (match_word(parser, "set")) {
            if (parser->current.type == ZTOKEN_COLON) {
                is_ui_setter = 1;
                break;
            }
            is_setter = 1;
            if (parser->current.type == ZTOKEN_LEFT_BRACKET) {
                is_array_setter = 1;
                break;
            }
            if (!consume_type(parser, ZTOKEN_DOT, "'.' after 'set'")) break;
            field_name = consume_name(parser, "a field name to set");
            break;
        }
        if (part_count == 5) {
            fail_at(parser, &parser->current,
                    "an object method path can contain at most five names");
            break;
        }
        parts[part_count++] = consume_name(parser, "a name after '.'");
    }
    if (parser->failed) goto failed;

    if (part_count == 3 && strcmp(parts[1], "toPoint") == 0 &&
        (strcmp(parts[2], "glide") == 0 ||
         strcmp(parts[2], "teleport") == 0) &&
        parser->current.type == ZTOKEN_LEFT_PAREN) {
        char *target;
        char command[512];
        int glide = strcmp(parts[2], "glide") == 0;
        advance_token(parser);
        if (parser->current.type != ZTOKEN_IDENTIFIER &&
            parser->current.type != ZTOKEN_STRING) {
            fail_at(parser, &parser->current, "a navigation point name");
            goto failed;
        }
        target = copy_token(parser, &parser->current);
        if (target == NULL) goto failed;
        if (snprintf(command, sizeof(command), "%s.toPoint.%s.%s",
                     parts[0], parts[2], target) >= (int)sizeof(command)) {
            free(target);
            fail_at(parser, &parser->current, "navigation command is too long");
            goto failed;
        }
        free(target);
        advance_token(parser);
        if (glide) {
            if (!consume_type(parser, ZTOKEN_COMMA,
                              "',' before AI glide speed") ||
                !parse_expression(parser, function)) goto failed;
        }
        if (!consume_type(parser, ZTOKEN_RIGHT_PAREN,
                          "')' after navigation command") ||
            !consume_type(parser, ZTOKEN_COLON,
                          "':' after navigation command")) goto failed;
        instruction = emit(parser, function,
                           glide ? ZOP_UI_SET_VALUE : ZOP_UI_SET);
        if (instruction == NULL) goto failed;
        instruction->operand = zsharp_copy_text(command, strlen(command));
        if (!glide) {
            instruction->number_operand = ZUI_PROPERTY_TEXT;
            instruction->call_function = zsharp_copy_text("", 0);
        }
        if (instruction->operand == NULL ||
            (!glide && instruction->call_function == NULL)) goto failed;
        for (index = 0; index < part_count; index++) free(parts[index]);
        return 1;
    }

    if (part_count == 4 && strcmp(parts[0], "ZSharp") == 0 &&
        strcmp(parts[1], "Achievement") == 0 &&
        strcmp(parts[2], "Award") == 0 &&
        parser->current.type == ZTOKEN_COLON) {
        if (!consume_type(parser, ZTOKEN_COLON,
                          "':' after the achievement award")) goto failed;
        instruction = emit(parser, function, ZOP_UI_SET);
        if (instruction == NULL) goto failed;
        path = join_path_parts(parser, parts, part_count);
        if (path == NULL) goto failed;
        instruction->operand = path;
        path = NULL;
        instruction->number_operand = ZUI_PROPERTY_STATUS;
        instruction->call_function = zsharp_copy_text("alive", 5);
        if (instruction->call_function == NULL) goto failed;
        for (index = 0; index < part_count; index++) free(parts[index]);
        return 1;
    }

    if (part_count == 3 &&
        (strcmp(parts[1], "playClip") == 0 ||
         strcmp(parts[1], "pauseClip") == 0 ||
         strcmp(parts[1], "stopClip") == 0) &&
        (parser->current.type == ZTOKEN_COLON ||
         parser->current.type == ZTOKEN_LEFT_BRACKET)) {
        char selected[256] = {0};
        size_t used = 0;
        if (match_type(parser, ZTOKEN_LEFT_BRACKET)) {
            do {
                char *index_text;
                char *end = NULL;
                unsigned long instance;
                size_t length;
                if (parser->current.type != ZTOKEN_NUMBER) {
                    fail_at(parser, &parser->current,
                            "animation instance indexes must be positive integers");
                    goto failed;
                }
                index_text = copy_token(parser, &parser->current);
                if (index_text == NULL) goto failed;
                instance = strtoul(index_text, &end, 10);
                if (end == index_text || *end != '\0' || instance == 0 ||
                    instance > 1000000) {
                    free(index_text);
                    fail_at(parser, &parser->current,
                            "animation instance indexes must be positive integers");
                    goto failed;
                }
                length = strlen(index_text);
                if (used + length + 2 >= sizeof(selected)) {
                    free(index_text);
                    fail_at(parser, &parser->current,
                            "too many animation instance indexes");
                    goto failed;
                }
                if (used > 0) selected[used++] = ',';
                memcpy(selected + used, index_text, length);
                used += length;
                selected[used] = '\0';
                free(index_text);
                advance_token(parser);
            } while (match_type(parser, ZTOKEN_COMMA));
            if (!consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                              "']' after animation indexes")) goto failed;
        }
        if (!consume_type(parser, ZTOKEN_COLON,
                          "':' after the animation command")) goto failed;
        instruction = emit(parser, function, ZOP_UI_SET);
        if (instruction == NULL) goto failed;
        instruction->operand = join_path_parts(parser, parts, part_count);
        instruction->call_function = zsharp_copy_text(selected, used);
        instruction->number_operand = ZUI_PROPERTY_TEXT;
        if (instruction->operand == NULL || instruction->call_function == NULL)
            goto failed;
        for (index = 0; index < part_count; index++) free(parts[index]);
        return 1;
    }

    if (is_ui_setter) {
        int32_t value_type = 0;
        uint32_t unit = ZUI_UNIT_NONE;
        char *value_text = NULL;
        if (part_count != 1 && part_count != 2 && part_count != 3) {
            fail_at(parser, &first_token,
                    "window property setters use PathAlias.set, "
                    "Element.property.set, or File.Element.property.set");
            goto failed;
        }
        if (!consume_type(parser, ZTOKEN_COLON,
                          "':' after the window property setter")) {
            goto failed;
        }
        if (parser->current.type == ZTOKEN_LEFT_PAREN ||
            (part_count > 1 && parser->current.type == ZTOKEN_IDENTIFIER &&
             (strcmp(parts[part_count - 1], "content") == 0 ||
              strcmp(parts[part_count - 1], "title") == 0 ||
              strcmp(parts[part_count - 1], "icon") == 0 ||
              strcmp(parts[part_count - 1], "text") == 0 ||
              strcmp(parts[part_count - 1], "file") == 0 ||
              strcmp(parts[part_count - 1], "display") == 0 ||
              strcmp(parts[part_count - 1], "contents") == 0 ||
              strcmp(parts[part_count - 1], "selected") == 0))) {
            if (part_count == 1) {
                fail_at(parser, &first_token,
                        "calculated window setters require an explicit element property path");
                goto failed;
            }
            if (!parse_expression(parser, function) ||
                !consume_type(parser, ZTOKEN_COLON,
                              "':' after the calculated window property value")) {
                goto failed;
            }
            path = join_path_parts(parser, parts, part_count);
            if (path == NULL) goto failed;
            instruction = emit(parser, function, ZOP_UI_SET_VALUE);
            if (instruction == NULL) goto failed;
            instruction->operand = path;
            path = NULL;
            for (index = 0; index < part_count; index++) free(parts[index]);
            return 1;
        }
        if (!parse_ui_update_value(parser, &value_type, &unit, &value_text) ||
            !consume_type(parser, ZTOKEN_COLON,
                          "':' after the window property value")) {
            free(value_text);
            goto failed;
        }
        if (part_count == 1) {
            instruction = emit(parser, function, ZOP_LOAD_NAME);
            if (instruction == NULL) {
                free(value_text);
                goto failed;
            }
            instruction->operand = parts[0];
            parts[0] = NULL;
            instruction = emit(parser, function, ZOP_UI_SET_DYNAMIC);
        } else {
            path = join_path_parts(parser, parts, part_count);
            if (path == NULL) {
                free(value_text);
                goto failed;
            }
            instruction = emit(parser, function, ZOP_UI_SET);
        }
        if (instruction == NULL) {
            free(value_text);
            goto failed;
        }
        if (part_count > 1) instruction->operand = path;
        instruction->number_operand = value_type;
        instruction->index_operand = unit;
        instruction->call_function = value_text;
        path = NULL;
        for (index = 0; index < part_count; index++) free(parts[index]);
        return 1;
    }

    if (!is_setter && part_count >= 2 &&
        strcmp(parts[part_count - 1], "add") == 0 &&
        parser->current.type == ZTOKEN_LEFT_PAREN) {
        size_t base_count = part_count - 1;
        char *constructor_type;
        if (base_count == 1) {
            instruction = emit(parser, function, ZOP_LOAD_NAME);
            if (instruction == NULL) goto failed;
            instruction->operand = parts[0];
            parts[0] = NULL;
        } else {
            path = join_path_parts(parser, parts, base_count);
            if (path == NULL) goto failed;
            instruction = emit(parser, function, ZOP_LOAD_PATH);
            if (instruction == NULL) goto failed;
            instruction->operand = path;
            instruction->argument_count = (uint32_t)base_count;
            path = NULL;
        }
        if (!consume_type(parser, ZTOKEN_LEFT_PAREN,
                          "'(' after array.add") ||
            !consume_word(parser, "new")) goto failed;
        constructor_type = consume_name(parser, "a room name after 'new'");
        if (constructor_type == NULL ||
            !consume_type(parser, ZTOKEN_LEFT_BRACKET,
                          "'[' before the constructor arguments")) {
            free(constructor_type);
            goto failed;
        }
        if (parser->current.type != ZTOKEN_RIGHT_BRACKET) {
            do {
                if (!parse_expression(parser, function)) {
                    free(constructor_type);
                    goto failed;
                }
                if (argument_count == UINT32_MAX) {
                    fail_at(parser, &parser->current,
                            "array.add has too many constructor arguments");
                    free(constructor_type);
                    goto failed;
                }
                argument_count++;
            } while (match_type(parser, ZTOKEN_COMMA));
        }
        if (!consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                          "']' after the constructor arguments") ||
            !consume_type(parser, ZTOKEN_RIGHT_PAREN,
                          "')' after array.add") ||
            !consume_type(parser, ZTOKEN_COLON,
                          "':' after array.add")) {
            free(constructor_type);
            goto failed;
        }
        instruction = emit(parser, function, ZOP_ARRAY_ADD_OBJECT);
        if (instruction == NULL) {
            free(constructor_type);
            goto failed;
        }
        instruction->operand = constructor_type;
        instruction->argument_count = argument_count;
        for (index = 0; index < part_count; index++) free(parts[index]);
        return 1;
    }

    if (is_array_setter) {
        if (part_count == 1) {
            instruction = emit(parser, function, ZOP_LOAD_NAME);
            if (instruction == NULL) goto failed;
            instruction->operand = parts[0];
            parts[0] = NULL;
        } else {
            path = join_path_parts(parser, parts, part_count);
            if (path == NULL) goto failed;
            instruction = emit(parser, function, ZOP_LOAD_PATH);
            if (instruction == NULL) goto failed;
            instruction->operand = path;
            instruction->argument_count = (uint32_t)part_count;
            path = NULL;
        }
        if (!consume_type(parser, ZTOKEN_LEFT_BRACKET,
                          "'[' after array.set") ||
            !parse_expression(parser, function) ||
            !consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                          "']' after the array index") ||
            !consume_type(parser, ZTOKEN_EQUAL,
                          "'=' after the array index") ||
            !parse_expression(parser, function) ||
            !consume_type(parser, ZTOKEN_COLON,
                          "':' after the array assignment") ||
            emit(parser, function, ZOP_SET_INDEX) == NULL) goto failed;
        for (index = 0; index < part_count; index++) free(parts[index]);
        return 1;
    }

    if (is_setter) {
        if (part_count == 1) {
            instruction = emit(parser, function, ZOP_LOAD_NAME);
            if (instruction == NULL) goto failed;
            instruction->operand = parts[0];
            parts[0] = NULL;
        } else {
            path = join_path_parts(parser, parts, part_count);
            if (path == NULL) goto failed;
        }
        if (!consume_type(parser, ZTOKEN_EQUAL, "'=' after the field name") ||
            !parse_expression(parser, function) ||
            !consume_type(parser, ZTOKEN_COLON,
                          "':' after the object field assignment")) {
            goto failed;
        }
        instruction = emit(parser, function,
                           part_count == 1 ? ZOP_SET_MEMBER
                                           : ZOP_SET_MEMBER_PATH);
        if (instruction == NULL) goto failed;
        if (part_count == 1) {
            instruction->operand = field_name;
        } else {
            instruction->operand = path;
            instruction->index_operand = (uint32_t)part_count;
            instruction->call_function = field_name;
            path = NULL;
        }
        field_name = NULL;
        for (index = 0; index < part_count; index++) free(parts[index]);
        return 1;
    }

    if (match_type(parser, ZTOKEN_EQUAL)) {
        if (part_count > 4) {
            fail_at(parser, &first_token,
                    "object fields must be written with '.set.Field'");
            goto failed;
        }
        if (!parse_expression(parser, function) ||
            !consume_type(parser, ZTOKEN_COLON,
                          "':' after the assignment")) {
            goto failed;
        }
        instruction = emit(parser, function,
                           part_count == 1 ? ZOP_STORE_NAME
                                           : ZOP_STORE_PATH);
        if (instruction == NULL) goto failed;
        if (part_count == 1) {
            instruction->operand = parts[0];
            parts[0] = NULL;
        } else {
            instruction->operand = join_path_parts(parser, parts, part_count);
            instruction->argument_count = (uint32_t)part_count;
            if (instruction->operand == NULL) goto failed;
        }
        for (index = 0; index < part_count; index++) free(parts[index]);
        return 1;
    }

    if (part_count < 2) {
        fail_at(parser, &first_token,
                "expected an assignment, object field write, or method call");
        goto failed;
    }
    if (!consume_type(parser, ZTOKEN_LEFT_BRACKET,
                      "'[' after the method name")) {
        goto failed;
    }
    if (part_count == 2) {
        instruction = emit(parser, function, ZOP_LOAD_NAME);
        if (instruction == NULL) goto failed;
        instruction->operand = parts[0];
        parts[0] = NULL;
    } else {
        path = join_path_parts(parser, parts, part_count - 1);
        if (path == NULL) goto failed;
    }
    if (parser->current.type != ZTOKEN_RIGHT_BRACKET) {
        do {
            if (!parse_expression(parser, function)) goto failed;
            argument_count++;
        } while (match_type(parser, ZTOKEN_COMMA));
    }
    if (!consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                      "']' after the method arguments") ||
        !consume_type(parser, ZTOKEN_COLON,
                      "':' after the method call")) {
        goto failed;
    }
    instruction = emit(parser, function,
                       part_count == 2 ? ZOP_CALL_METHOD
                                       : ZOP_CALL_METHOD_PATH);
    if (instruction == NULL) goto failed;
    if (part_count == 2) {
        instruction->operand = parts[1];
        parts[1] = NULL;
    } else {
        instruction->operand = path;
        instruction->index_operand = (uint32_t)(part_count - 1);
        instruction->call_function = parts[part_count - 1];
        parts[part_count - 1] = NULL;
        path = NULL;
    }
    instruction->argument_count = argument_count;
    for (index = 0; index < part_count; index++) free(parts[index]);
    return 1;

failed:
    free(path);
    free(field_name);
    for (index = 0; index < part_count; index++) free(parts[index]);
    return 0;
}

static int parse_feed(Parser *parser, ZSharpFunction *function) {
    if (match_type(parser, ZTOKEN_COLON)) {
        return emit(parser, function, ZOP_RETURN_VOID) != NULL;
    }
    if (function->return_type == ZRETURN_VOID &&
        parser->named_outcome_depth == 0) {
        fail_at(parser, &parser->current,
                "'feed' cannot return a value from a brain");
        return 0;
    }
    return consume_type(parser, ZTOKEN_LEFT_PAREN, "'(' after 'feed'") &&
           parse_expression(parser, function) &&
           consume_type(parser, ZTOKEN_RIGHT_PAREN,
                        "')' after the feed value") &&
           consume_type(parser, ZTOKEN_COLON,
                        "':' after the feed statement") &&
           emit(parser, function, ZOP_RETURN_VALUE) != NULL;
}

static int parse_statement(Parser *parser, ZSharpFunction *function);

static int parse_statement_block(Parser *parser, ZSharpFunction *function) {
    if (!consume_type(parser, ZTOKEN_LEFT_PAREN, "'(' before the block")) {
        return 0;
    }
    while (!parser->failed && parser->current.type != ZTOKEN_RIGHT_PAREN &&
           parser->current.type != ZTOKEN_EOF) {
        if (!parse_statement(parser, function)) return 0;
    }
    return consume_type(parser, ZTOKEN_RIGHT_PAREN, "')' after the block");
}

static int append_outcome_name(Parser *parser, ZSharpFunction *function,
                               char *name) {
    char **resized;
    size_t index;
    for (index = 0; index < function->outcome_count; index++) {
        if (strcmp(function->outcome_names[index], name) == 0) {
            fail_at(parser, &parser->current,
                    "duplicate named if '%s'", name);
            free(name);
            return 0;
        }
    }
    resized = (char **)realloc(
        function->outcome_names,
        (function->outcome_count + 1) * sizeof(*function->outcome_names));
    if (resized == NULL) {
        fail_at(parser, &parser->current, "out of memory");
        free(name);
        return 0;
    }
    function->outcome_names = resized;
    function->outcome_names[function->outcome_count++] = name;
    return 1;
}

static int parse_if(Parser *parser, ZSharpFunction *function) {
    size_t jump_if_false_index;
    size_t jump_over_else_index;
    size_t named_marker_index = SIZE_MAX;
    char *outcome_name = NULL;
    if (match_type(parser, ZTOKEN_LEFT_PAREN)) {
        char *stored_name;
        ZSharpInstruction *marker;
        if (function->return_type != ZRETURN_VOID) {
            fail_at(parser, &parser->current,
                    "only a brain can declare a named if");
            return 0;
        }
        outcome_name = consume_name(parser, "a named if name");
        if (outcome_name == NULL ||
            !consume_type(parser, ZTOKEN_RIGHT_PAREN,
                          "')' after the named if name")) {
            free(outcome_name);
            return 0;
        }
        stored_name = zsharp_copy_text(outcome_name, strlen(outcome_name));
        if (stored_name == NULL ||
            !append_outcome_name(parser, function, stored_name)) {
            free(outcome_name);
            return 0;
        }
        named_marker_index = function->instruction_count;
        marker = emit(parser, function, ZOP_NAMED_IF_START);
        if (marker == NULL) {
            free(outcome_name);
            return 0;
        }
        marker->operand = outcome_name;
        outcome_name = NULL;
    }
    if (!consume_type(parser, ZTOKEN_LEFT_BRACKET, "'[' after 'if'") ||
        !parse_expression(parser, function) ||
        !consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                      "']' after the if condition")) {
        return 0;
    }
    jump_if_false_index = function->instruction_count;
    if (emit(parser, function, ZOP_JUMP_IF_FALSE) == NULL) {
        return 0;
    }
    if (named_marker_index != SIZE_MAX) parser->named_outcome_depth++;
    if (!parse_statement_block(parser, function)) {
        if (named_marker_index != SIZE_MAX) parser->named_outcome_depth--;
        return 0;
    }
    if (match_word(parser, "else")) {
        jump_over_else_index = function->instruction_count;
        if (emit(parser, function, ZOP_JUMP) == NULL) {
            if (named_marker_index != SIZE_MAX) parser->named_outcome_depth--;
            return 0;
        }
        if (function->instruction_count > UINT32_MAX) {
            fail_at(parser, &parser->current, "brain is too large");
            if (named_marker_index != SIZE_MAX) parser->named_outcome_depth--;
            return 0;
        }
        function->instructions[jump_if_false_index].index_operand =
            (uint32_t)function->instruction_count;
        if (!(match_word(parser, "if")
                  ? parse_if(parser, function)
                  : parse_statement_block(parser, function))) {
            if (named_marker_index != SIZE_MAX) parser->named_outcome_depth--;
            return 0;
        }
        if (function->instruction_count > UINT32_MAX) {
            fail_at(parser, &parser->current, "brain is too large");
            if (named_marker_index != SIZE_MAX) parser->named_outcome_depth--;
            return 0;
        }
        function->instructions[jump_over_else_index].index_operand =
            (uint32_t)function->instruction_count;
    } else {
        function->instructions[jump_if_false_index].op =
            ZOP_RETURN_IF_FALSE;
    }
    if (named_marker_index != SIZE_MAX) parser->named_outcome_depth--;
    if (named_marker_index != SIZE_MAX) {
        if (function->instruction_count > UINT32_MAX) {
            fail_at(parser, &parser->current, "brain is too large");
            return 0;
        }
        function->instructions[named_marker_index].index_operand =
            (uint32_t)function->instruction_count;
    }
    return 1;
}

static int parse_loop(Parser *parser, ZSharpFunction *function) {
    LoopContext *context;
    ZSharpInstruction *jump;
    size_t loop_end;
    size_t index;
    int parsed;
    if (parser->loop_depth >= 64) {
        fail_at(parser, &parser->current, "loops are nested too deeply");
        return 0;
    }
    context = &parser->loops[parser->loop_depth++];
    memset(context, 0, sizeof(*context));
    context->start_index = function->instruction_count;
    parsed = parse_statement_block(parser, function);
    if (!parsed) {
        free(context->destroy_jumps);
        parser->loop_depth--;
        return 0;
    }
    if (parser->current.type == ZTOKEN_COLON) {
        fail_at(parser, &parser->current,
                "a loop closes with ')' and does not use ':' after it");
        free(context->destroy_jumps);
        parser->loop_depth--;
        return 0;
    }
    if (context->start_index > UINT32_MAX) {
        fail_at(parser, &parser->current, "brain is too large");
        free(context->destroy_jumps);
        parser->loop_depth--;
        return 0;
    }
    jump = emit(parser, function, ZOP_JUMP);
    if (jump == NULL) {
        free(context->destroy_jumps);
        parser->loop_depth--;
        return 0;
    }
    jump->index_operand = (uint32_t)context->start_index;
    loop_end = function->instruction_count;
    if (loop_end > UINT32_MAX) {
        fail_at(parser, &parser->current, "brain is too large");
        free(context->destroy_jumps);
        parser->loop_depth--;
        return 0;
    }
    for (index = 0; index < context->destroy_count; index++) {
        function->instructions[context->destroy_jumps[index]].index_operand =
            (uint32_t)loop_end;
    }
    free(context->destroy_jumps);
    parser->loop_depth--;
    return 1;
}

static int parse_loop_end(Parser *parser, ZSharpFunction *function) {
    LoopContext *context;
    size_t *resized;
    size_t jump_index;
    if (!consume_type(parser, ZTOKEN_COLON, "':' after 'loop.end'")) return 0;
    if (parser->loop_depth == 0) {
        fail_at(parser, &parser->current,
                "'loop.end' can only be used inside a loop");
        return 0;
    }
    context = &parser->loops[parser->loop_depth - 1];
    jump_index = function->instruction_count;
    if (emit(parser, function, ZOP_JUMP) == NULL) return 0;
    resized = (size_t *)realloc(
        context->destroy_jumps,
        (context->destroy_count + 1) * sizeof(*context->destroy_jumps));
    if (resized == NULL) {
        fail_at(parser, &parser->current, "out of memory");
        return 0;
    }
    context->destroy_jumps = resized;
    context->destroy_jumps[context->destroy_count++] = jump_index;
    return 1;
}

static int parse_continue(Parser *parser, ZSharpFunction *function) {
    ZSharpInstruction *jump;
    LoopContext *context;
    if (!consume_type(parser, ZTOKEN_COLON, "':' after 'continue'")) return 0;
    if (parser->loop_depth == 0) {
        fail_at(parser, &parser->current,
                "'continue' can only be used inside a loop");
        return 0;
    }
    context = &parser->loops[parser->loop_depth - 1];
    if (context->start_index > UINT32_MAX) {
        fail_at(parser, &parser->current, "brain is too large");
        return 0;
    }
    jump = emit(parser, function, ZOP_JUMP);
    if (jump == NULL) return 0;
    jump->index_operand = (uint32_t)context->start_index;
    return 1;
}

static int syntax_token_equals(const ZSharpToken *left,
                               const ZSharpToken *right) {
    return left->type == right->type && left->length == right->length &&
           memcmp(left->start, right->start, left->length) == 0;
}

/* A dry match avoids emitting bytecode for a rule that does not apply. */
static int custom_pattern_matches(Parser *parser, const char *pattern, int expression) {
    ZSharpLexer pattern_lexer;
    ZSharpLexer source_lexer = parser->lexer;
    ZSharpToken expected;
    ZSharpToken actual = parser->current;
    zsharp_lexer_init(&pattern_lexer, pattern);
    expected = zsharp_lexer_next(&pattern_lexer);
    while (expected.type != ZTOKEN_EOF && expected.type != ZTOKEN_ERROR) {
        if (expected.type == ZTOKEN_LEFT_BRACE) {
            ZSharpToken name = zsharp_lexer_next(&pattern_lexer);
            ZSharpToken close = zsharp_lexer_next(&pattern_lexer);
            ZSharpToken following = zsharp_lexer_next(&pattern_lexer);
            ZSharpTokenType previous = ZTOKEN_EOF;
            ZSharpTokenType nesting[256];
            size_t depth = 0, captured = 0;
            if (name.type != ZTOKEN_IDENTIFIER ||
                close.type != ZTOKEN_RIGHT_BRACE) return 0;
            /* Adjacent captures have no expression delimiter; retain their
             * original single-atom interpretation. */
            if (following.type == ZTOKEN_LEFT_BRACE) {
                if (actual.type != ZTOKEN_IDENTIFIER && actual.type != ZTOKEN_NUMBER &&
                    actual.type != ZTOKEN_STRING) return 0;
                actual = zsharp_lexer_next(&source_lexer);
                expected = following;
                continue;
            }
            /* Capture through nested calls/brackets, stopping only at the
             * next literal of this pattern at the outer expression level.
             * This is token-only lookahead: no calls or bytecode side effects. */
            for (;;) {
                if (depth == 0) {
                    if (following.type != ZTOKEN_EOF &&
                        previous != ZTOKEN_DOT && syntax_token_equals(&following, &actual)) break;
                    /* A scalar capture cannot swallow another outer argument.
                     * Otherwise a one-argument overload falsely matches a
                     * two-argument call and consumes its comma as ')'. */
                    if (actual.type == ZTOKEN_COMMA && following.type != ZTOKEN_COMMA) return 0;
                    if (following.type == ZTOKEN_EOF &&
                        (actual.type == ZTOKEN_COLON || actual.type == ZTOKEN_EOF ||
                         (expression && (actual.type == ZTOKEN_COMMA ||
                          actual.type == ZTOKEN_RIGHT_PAREN || actual.type == ZTOKEN_RIGHT_BRACKET)))) break;
                }
                if (actual.type == ZTOKEN_EOF || actual.type == ZTOKEN_ERROR ||
                    (actual.type == ZTOKEN_COLON && depth == 0)) return 0;
                if (actual.type == ZTOKEN_LEFT_PAREN || actual.type == ZTOKEN_LEFT_BRACKET) {
                    if (depth == 256) return 0;
                    nesting[depth++] = actual.type;
                } else if (actual.type == ZTOKEN_RIGHT_PAREN || actual.type == ZTOKEN_RIGHT_BRACKET) {
                    if (depth == 0 || (actual.type == ZTOKEN_RIGHT_PAREN
                        ? nesting[depth - 1] != ZTOKEN_LEFT_PAREN
                        : nesting[depth - 1] != ZTOKEN_LEFT_BRACKET)) return 0;
                    depth--;
                }
                previous = actual.type;
                actual = zsharp_lexer_next(&source_lexer);
                captured++;
            }
            if (!captured) return 0;
            expected = following;
            continue;
        } else if (!syntax_token_equals(&expected, &actual)) {
            return 0;
        }
        actual = zsharp_lexer_next(&source_lexer);
        expected = zsharp_lexer_next(&pattern_lexer);
    }
    /* Do not accept a prefix of a longer property/function path. */
    return expected.type == ZTOKEN_EOF && (expression
        ? (actual.type != ZTOKEN_IDENTIFIER ||
           zsharp_token_equals(&actual, "and") || zsharp_token_equals(&actual, "or")) && actual.type != ZTOKEN_DOT &&
          actual.type != ZTOKEN_LEFT_PAREN
        : actual.type == ZTOKEN_COLON);
}

static int custom_expression_matches(Parser *parser, const char *pattern) {
    return custom_pattern_matches(parser, pattern, 1);
}

static int emit_custom_call(Parser *parser, ZSharpFunction *function,
                            const ZSharpCustomSyntaxRule *rule,
                            uint32_t argument_count) {
    ZSharpInstruction *instruction;
    instruction = emit(parser, function, rule->is_expression
        ? ZOP_CALL_QUALIFIED_VALUE : ZOP_CALL_QUALIFIED);
    if (instruction == NULL) return 0;
    instruction->operand = zsharp_copy_text("@c", 2);
    instruction->call_file = zsharp_copy_text(rule->module,
                                               strlen(rule->module));
    instruction->call_room = zsharp_copy_text("", 0);
    instruction->call_function = zsharp_copy_text(rule->function,
                                                   strlen(rule->function));
    instruction->argument_count = argument_count;
    if (instruction->operand == NULL || instruction->call_file == NULL ||
        instruction->call_room == NULL || instruction->call_function == NULL) {
        fail_at(parser, &parser->current, "out of memory");
        return 0;
    }
    return 1;
}

static int parse_custom_statement(Parser *parser, ZSharpFunction *function,
                                  const ZSharpCustomSyntaxRule *rule) {
    ZSharpLexer pattern_lexer;
    ZSharpToken expected;
    uint32_t argument_count = 0;
    zsharp_lexer_init(&pattern_lexer, rule->pattern);
    expected = zsharp_lexer_next(&pattern_lexer);
    while (expected.type != ZTOKEN_EOF && !parser->failed) {
        if (expected.type == ZTOKEN_LEFT_BRACE) {
            (void)zsharp_lexer_next(&pattern_lexer);
            (void)zsharp_lexer_next(&pattern_lexer);
            /* Operator literals remain pattern separators for compatibility;
             * normal comma/parenthesis/word-delimited captures are expressions. */
            {
                ZSharpLexer remaining = pattern_lexer;
                ZSharpToken next = zsharp_lexer_next(&remaining);
                int operator_separator = next.type == ZTOKEN_LEFT_BRACE ||
                    (next.type >= ZTOKEN_PLUS && next.type <= ZTOKEN_LESS_EQUAL);
                if (!(operator_separator ? parse_primary(parser, function)
                                         : parse_expression(parser, function))) return 0;
            }
            argument_count++;
        } else {
            advance_token(parser);
        }
        expected = zsharp_lexer_next(&pattern_lexer);
    }
    if (!rule->is_expression && !consume_type(parser, ZTOKEN_COLON,
                      "':' after the custom C statement")) return 0;
    return emit_custom_call(parser, function, rule, argument_count);
}

static int parse_custom_block(Parser *parser, ZSharpFunction *function,
                              const ZSharpCustomSyntaxRule *rule) {
    uint32_t argument_count = 0;
    advance_token(parser);
    if (!consume_type(parser, ZTOKEN_LEFT_PAREN,
                      "'(' after the custom C block name")) return 0;
    while (!parser->failed && parser->current.type != ZTOKEN_RIGHT_PAREN &&
           parser->current.type != ZTOKEN_EOF) {
        char *key = consume_name(parser, "a custom C block field name");
        ZSharpInstruction *instruction;
        if (key == NULL ||
            !consume_type(parser, ZTOKEN_COLON,
                          "':' after the custom C block field")) {
            free(key);
            return 0;
        }
        instruction = emit(parser, function, ZOP_PUSH_TEXT);
        if (instruction == NULL) {
            free(key);
            return 0;
        }
        instruction->operand = key;
        if (zsharp_token_equals(&parser->current, "true") ||
            zsharp_token_equals(&parser->current, "false")) {
            instruction = emit(parser, function, ZOP_PUSH_STATUS);
            if (instruction == NULL) return 0;
            instruction->number_operand =
                zsharp_token_equals(&parser->current, "true");
            advance_token(parser);
        } else if (!parse_expression(parser, function)) {
            return 0;
        }
        if (!consume_type(parser, ZTOKEN_COLON,
                          "':' after the custom C block value")) return 0;
        if (argument_count > UINT32_MAX - 2) {
            fail_at(parser, &parser->current,
                    "custom C block has too many fields");
            return 0;
        }
        argument_count += 2;
    }
    if (!consume_type(parser, ZTOKEN_RIGHT_PAREN,
                      "')' after the custom C block")) return 0;
    return emit_custom_call(parser, function, rule, argument_count);
}

/* A function declaration binds a target, rather than executing it. Plain
 * identifiers in local declarations may copy another function reference. */
static char *parse_function_target(Parser *parser, const ZSharpFunction *scope,
                                   int *is_alias) {
    char *parts[66] = {0}, *target = NULL;
    size_t count = 0, i, length = 0;
    int foreign;
    *is_alias = 0;
    parts[count++] = consume_name(parser, "a function target");
    if (parts[0] == NULL) return NULL;
    foreign = strcmp(parts[0], "py") == 0 || strcmp(parts[0], "js") == 0 ||
        strcmp(parts[0], "lua") == 0 || strcmp(parts[0], "cpp") == 0 ||
        strcmp(parts[0], "rust") == 0 || strcmp(parts[0], "c") == 0 ||
        strcmp(parts[0], "kt") == 0;
    if (foreign) {
        if (!consume_type(parser, ZTOKEN_COLON, "':' after the language")) goto done;
        parts[count++] = consume_name(parser, "a foreign module");
        while (!parser->failed && match_type(parser, ZTOKEN_DOT)) {
            if (count == 65) { fail_at(parser, &parser->current, "target is too long"); break; }
            parts[count++] = consume_name(parser, "a module path name");
        }
        if (!consume_type(parser, ZTOKEN_COLON, "':' before the foreign function")) goto done;
        parts[count++] = consume_name(parser, "a foreign function");
    } else {
        int known_alias = 0;
        if (scope != NULL && parser->current.type == ZTOKEN_COLON) {
            for (i = 0; i < scope->parameter_count; i++)
                if (strcmp(scope->parameters[i].name, parts[0]) == 0) known_alias = 1;
            for (i = 0; i < scope->instruction_count; i++) {
                const ZSharpInstruction *prior = &scope->instructions[i];
                if ((prior->op == ZOP_STORE_LOCAL || prior->op == ZOP_STORE_LOCAL_TEXT ||
                     prior->op == ZOP_STORE_LOCAL_FUNCTION || prior->op == ZOP_STORE_LOCAL_VALUE || prior->op == ZOP_STORE_LOCAL_TEXT_ARRAY) &&
                    strcmp(prior->operand, parts[0]) == 0) known_alias = 1;
            }
            if (parser->member_room != NULL)
                for (i = 0; i < parser->member_room->variable_count; i++)
                    if (strcmp(parser->member_room->variables[i].name, parts[0]) == 0) known_alias = 1;
        }
        ZSharpLexer lookahead = parser->lexer;
        ZSharpToken next = zsharp_lexer_next(&lookahead);
        ZSharpToken separator = zsharp_lexer_next(&lookahead);
        ZSharpToken last = zsharp_lexer_next(&lookahead);
        ZSharpToken terminator = zsharp_lexer_next(&lookahead);
        if (known_alias || (parser->current.type == ZTOKEN_COLON &&
            (next.type != ZTOKEN_IDENTIFIER ||
             (separator.type != ZTOKEN_COLON && separator.type != ZTOKEN_DOT) ||
             last.type != ZTOKEN_IDENTIFIER ||
             (terminator.type != ZTOKEN_COLON && terminator.type != ZTOKEN_DOT)))) {
            *is_alias = 1;
        } else {
            for (i = 0; i < 2 && !parser->failed; i++) {
                if (parser->current.type != ZTOKEN_DOT && parser->current.type != ZTOKEN_COLON) {
                    fail_at(parser, &parser->current, "expected File:Room:Function"); break;
                }
                advance_token(parser);
                parts[count++] = consume_name(parser, "a function target name");
            }
            if (!parser->failed && match_type(parser, ZTOKEN_DOT))
                parts[count++] = consume_name(parser, "a project-qualified function");
        }
    }
    if (parser->failed) goto done;
    for (i = 0; i < count; i++) length += strlen(parts[i]) + (i != 0);
    target = (char *)malloc(length + 1);
    if (target == NULL) { fail_at(parser, &parser->current, "out of memory"); goto done; }
    target[0] = 0;
    for (i = 0; i < count; i++) {
        if (i != 0) strcat(target, ":");
        strcat(target, parts[i]);
    }
done:
    for (i = 0; i < count; i++) free(parts[i]);
    return target;
}

static int parse_function_local(Parser *parser, ZSharpFunction *function) {
    char *name = consume_name(parser, "a local function reference name");
    char *target;
    int alias;
    ZSharpInstruction *instruction;
    if (name == NULL || !consume_type(parser, ZTOKEN_EQUAL, "'=' after the function reference")) {
        free(name); return 0;
    }
    target = parse_function_target(parser, function, &alias);
    if (target == NULL || !consume_type(parser, ZTOKEN_COLON, "':' after the function reference")) {
        free(name); free(target); return 0;
    }
    instruction = emit(parser, function, alias ? ZOP_LOAD_NAME : ZOP_PUSH_FUNCTION);
    if (instruction == NULL) { free(name); free(target); return 0; }
    instruction->operand = target;
    instruction = emit(parser, function, ZOP_STORE_LOCAL_FUNCTION);
    if (instruction == NULL) { free(name); return 0; }
    instruction->operand = name;
    return 1;
}

static int parse_statement(Parser *parser, ZSharpFunction *function) {
    size_t syntax_index;
    for (syntax_index = 0; syntax_index < parser->syntax_rule_count;
         syntax_index++) {
        const ZSharpCustomSyntaxRule *rule =
            &parser->syntax_rules[syntax_index];
        if (rule->is_block &&
            zsharp_token_equals(&parser->current, rule->pattern) &&
            parser->current.type == ZTOKEN_IDENTIFIER) {
            ZSharpLexer lookahead = parser->lexer;
            if (zsharp_lexer_next(&lookahead).type == ZTOKEN_LEFT_PAREN)
                return parse_custom_block(parser, function, rule);
        }
        if (!rule->declaration_kind && !rule->is_block && !rule->is_expression &&
            custom_pattern_matches(parser, rule->pattern, 0))
            return parse_custom_statement(parser, function, rule);
    }
    if (match_word(parser, "Print")) return parse_print(parser, function);
    if (match_word(parser, "Application")) {
        if (!consume_type(parser, ZTOKEN_DOT, "'.' after Application") ||
            !consume_word(parser, "Quit") ||
            !consume_type(parser, ZTOKEN_COLON, "':' after Application.Quit")) return 0;
        return emit(parser, function, ZOP_APPLICATION_QUIT) != NULL;
    }
    if (match_word(parser, "Function")) {
        return parse_qualified_call(parser, function, 0);
    }
    if (match_word(parser, "File")) {
        char *method;
        ZSharpOpCode operation;
        if (!consume_type(parser, ZTOKEN_DOT, "'.' after File")) return 0;
        method = consume_name(parser, "write or append");
        operation =
            method != NULL && strcmp(method, "write") == 0 ? ZOP_FILE_WRITE :
            method != NULL && strcmp(method, "append") == 0 ? ZOP_FILE_APPEND :
            0;
        free(method);
        if (operation == 0) {
            fail_at(parser, &parser->current,
                    "File statements support write or append");
            return 0;
        }
        if (!consume_type(parser, ZTOKEN_LEFT_PAREN,
                          "'(' after the File operation") ||
            !parse_expression(parser, function) ||
            !consume_type(parser, ZTOKEN_COMMA,
                          "',' after the file path") ||
            !parse_expression(parser, function) ||
            !consume_type(parser, ZTOKEN_RIGHT_PAREN,
                          "')' after the file contents") ||
            !consume_type(parser, ZTOKEN_COLON,
                          "':' after the File operation")) return 0;
        return emit(parser, function, operation) != NULL;
    }
    if (match_word(parser, "number")) {
        return parse_number_statement(parser, function);
    }
    if (match_word(parser, "text")) {
        return parse_text_statement(parser, function);
    }
    if (match_word(parser, "function")) return parse_function_local(parser, function);
    if (match_word(parser, "JSON")) {
        ZSharpInstruction *instruction;
        char *name = consume_name(parser, "a local JSON variable name");
        if (name == NULL ||
            !consume_type(parser, ZTOKEN_EQUAL,
                          "'=' after the local JSON variable") ||
            !parse_expression(parser, function) ||
            !consume_type(parser, ZTOKEN_COLON,
                          "':' after the local JSON value")) {
            free(name);
            return 0;
        }
        instruction = emit(parser, function, ZOP_STORE_LOCAL_VALUE);
        if (instruction == NULL) {
            free(name);
            return 0;
        }
        instruction->operand = name;
        return 1;
    }
    if (match_word(parser, "feed")) return parse_feed(parser, function);
    if (match_word(parser, "if")) return parse_if(parser, function);
    if (match_word(parser, "loop")) {
        if (match_type(parser, ZTOKEN_DOT)) {
            return consume_word(parser, "end") &&
                   parse_loop_end(parser, function);
        }
        return parse_loop(parser, function);
    }
    if (match_word(parser, "continue")) return parse_continue(parser, function);
    if (match_word(parser, "wait") || match_word(parser, "delay"))
        return parse_delay_statement(parser, function);
    if (parser->current.type == ZTOKEN_IDENTIFIER) {
        return parse_named_statement(parser, function);
    }
    fail_at(parser, &parser->current,
            "expected a confirmed Z# statement");
    return 0;
}

static int parse_parameters(Parser *parser, ZSharpFunction *function,
                            int allow_dr) {
    if (!consume_type(parser, ZTOKEN_LEFT_BRACKET,
                      "'[' after the function name")) {
        return 0;
    }
    if (allow_dr && match_word(parser, "DR")) {
        function->disable_auto_run = 1;
        return consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                            "']' after 'DR'");
    }
    if (parser->current.type != ZTOKEN_RIGHT_BRACKET) {
        do {
            ZSharpParameter *parameter;
            ZSharpToken type_token = parser->current;
            char *object_type = NULL;
            ZSharpValueType type;
            if (match_word(parser, "number")) {
                type = ZVALUE_NUMBER;
            } else if (match_word(parser, "text")) {
                type = ZVALUE_TEXT;
            } else if (match_word(parser, "function")) {
                type = ZVALUE_FUNCTION;
            } else if (match_word(parser, "status")) {
                type = ZVALUE_STATUS;
            } else if (type_token.type == ZTOKEN_IDENTIFIER) {
                object_type = copy_token(parser, &type_token);
                advance_token(parser);
                type = ZVALUE_OBJECT;
            } else {
                fail_at(parser, &parser->current,
                        "expected a parameter type");
                return 0;
            }
            parameter = zsharp_function_add_parameter(function);
            if (parameter == NULL) {
                free(object_type);
                fail_at(parser, &parser->current, "out of memory");
                return 0;
            }
            parameter->type = type;
            parameter->object_type = object_type;
            parameter->name = consume_name(parser, "a parameter name");
        } while (match_type(parser, ZTOKEN_COMMA));
    }
    return consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                        "']' after the parameters");
}

static int parse_function_after_name(Parser *parser, ZSharpRoom *room,
                                     int is_public, char *name,
                                     ZSharpReturnType return_type,
                                     int allow_dr) {
    ZSharpFunction *function = zsharp_room_add_function(room);
    if (function == NULL) {
        free(name);
        fail_at(parser, &parser->current, "out of memory");
        return 0;
    }
    function->is_public = is_public;
    function->return_type = return_type;
    function->name = name;
    if (strncmp(name, "__zsharp_c_init_", 16) == 0) {
        fail_at(parser, &parser->current, "function names beginning __zsharp_c_init_ are reserved"); return 0;
    }
    return parse_parameters(parser, function, allow_dr) &&
           parse_statement_block(parser, function);
}

static int parse_number_array_member(Parser *parser, ZSharpRoom *room,
                                     int is_public);

static int parse_number_member(Parser *parser, ZSharpRoom *room,
                               int is_public) {
    ZSharpVariable *variable;
    char *name;
    if (match_type(parser, ZTOKEN_LEFT_PAREN)) {
        return parse_number_array_member(parser, room, is_public);
    }
    name = consume_name(parser, "a number or function name");
    if (name == NULL) return 0;
    if (parser->current.type == ZTOKEN_LEFT_BRACKET) {
        return parse_function_after_name(parser, room, is_public, name,
                                         ZRETURN_NUMBER, 0);
    }
    {
        variable = zsharp_room_add_variable(room);
        ZSharpToken value_token;
        if (variable == NULL) {
            free(name);
            fail_at(parser, &parser->current, "out of memory");
            return 0;
        }
        variable->is_public = is_public;
        variable->type = ZVALUE_NUMBER;
        variable->name = name;
        if (match_type(parser, ZTOKEN_COLON)) {
            variable->number_text = zsharp_copy_text("0", 1);
            if (variable->number_text == NULL) {
                fail_at(parser, &parser->current, "out of memory");
                return 0;
            }
            return 1;
        }
        if (!consume_type(parser, ZTOKEN_EQUAL,
                          "'=' after the number name")) {
            return 0;
        }
        value_token = parser->current;
        if ((value_token.type != ZTOKEN_NUMBER &&
             value_token.type != ZTOKEN_MINUS) ||
            !consume_signed_number_text(parser, &variable->number_text)) {
            return 0;
        }
        return consume_type(parser, ZTOKEN_COLON,
                            "':' after the number value");
    }
}

static int append_number_item(Parser *parser, ZSharpVariable *variable,
                              char *item) {
    char **resized = (char **)realloc(
        variable->number_items,
        (variable->number_item_count + 1) * sizeof(*variable->number_items));
    if (resized == NULL) {
        free(item);
        fail_at(parser, &parser->current, "out of memory");
        return 0;
    }
    variable->number_items = resized;
    variable->number_items[variable->number_item_count++] = item;
    return 1;
}

static int parse_number_array_member(Parser *parser, ZSharpRoom *room,
                                     int is_public) {
    ZSharpVariable *variable;
    char *name;
    if (!consume_type(parser, ZTOKEN_RIGHT_PAREN,
                      "')' in the number() type")) return 0;
    name = consume_name(parser, "a number array name");
    if (name == NULL) return 0;
    variable = zsharp_room_add_variable(room);
    if (variable == NULL) {
        free(name);
        fail_at(parser, &parser->current, "out of memory");
        return 0;
    }
    variable->is_public = is_public;
    variable->type = ZVALUE_NUMBER_ARRAY;
    variable->name = name;
    if (!consume_type(parser, ZTOKEN_EQUAL,
                      "'=' after the number array name") ||
        !consume_type(parser, ZTOKEN_LEFT_BRACKET,
                      "'[' before the number array values")) return 0;
    if (parser->current.type != ZTOKEN_RIGHT_BRACKET) {
        do {
            char *item = NULL;
            if (!consume_signed_number_text(parser, &item) ||
                !append_number_item(parser, variable, item)) return 0;
        } while (match_type(parser, ZTOKEN_COMMA));
    }
    return consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                        "']' after the number array values") &&
           consume_type(parser, ZTOKEN_COLON,
                        "':' after the number array value");
}

static int append_text_item(Parser *parser, ZSharpVariable *variable,
                            char *item) {
    char **resized = (char **)realloc(
        variable->text_items,
        (variable->text_item_count + 1) * sizeof(*variable->text_items));
    if (resized == NULL) {
        free(item);
        fail_at(parser, &parser->current, "out of memory");
        return 0;
    }
    variable->text_items = resized;
    variable->text_items[variable->text_item_count++] = item;
    return 1;
}

static int parse_text_member(Parser *parser, ZSharpRoom *room, int is_public) {
    int is_array = 0;
    ZSharpVariable *variable;
    char *name;
    if (match_type(parser, ZTOKEN_LEFT_PAREN)) {
        if (!consume_type(parser, ZTOKEN_RIGHT_PAREN,
                          "')' in the text() type")) {
            return 0;
        }
        is_array = 1;
    }
    name = consume_name(parser, is_array ? "a text array name"
                                         : "a text variable or function name");
    if (name == NULL) return 0;
    if (!is_array && parser->current.type == ZTOKEN_LEFT_BRACKET) {
        return parse_function_after_name(parser, room, is_public, name,
                                         ZRETURN_TEXT, 0);
    }
    variable = zsharp_room_add_variable(room);
    if (variable == NULL) {
        free(name);
        fail_at(parser, &parser->current, "out of memory");
        return 0;
    }
    variable->is_public = is_public;
    variable->type = is_array ? ZVALUE_TEXT_ARRAY : ZVALUE_TEXT;
    variable->name = name;
    if (!is_array && match_type(parser, ZTOKEN_COLON)) {
        variable->text_value = zsharp_copy_text("", 0);
        if (variable->text_value == NULL) {
            fail_at(parser, &parser->current, "out of memory");
            return 0;
        }
        return 1;
    }
    if (!consume_type(parser, ZTOKEN_EQUAL, "'=' after the text name")) {
        return 0;
    }
    if (!is_array) {
        ZSharpToken token = parser->current;
        if (!consume_type(parser, ZTOKEN_STRING, "a quoted text value")) {
            return 0;
        }
        variable->text_value = decode_text(parser, &token);
    } else {
        if (!consume_type(parser, ZTOKEN_LEFT_BRACKET,
                          "'[' before the text array values")) {
            return 0;
        }
        if (parser->current.type != ZTOKEN_RIGHT_BRACKET) {
            do {
                ZSharpToken token = parser->current;
                char *item;
                if (!consume_type(parser, ZTOKEN_STRING,
                                  "a quoted text array item")) {
                    return 0;
                }
                item = decode_text(parser, &token);
                if (item == NULL || !append_text_item(parser, variable, item)) {
                    return 0;
                }
            } while (match_type(parser, ZTOKEN_COMMA));
        }
        if (!consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                          "']' after the text array values")) {
            return 0;
        }
    }
    return consume_type(parser, ZTOKEN_COLON, "':' after the text value");
}

static int parse_status_member(Parser *parser, ZSharpRoom *room,
                               int is_public) {
    ZSharpVariable *variable = zsharp_room_add_variable(room);
    if (variable == NULL) {
        fail_at(parser, &parser->current, "out of memory");
        return 0;
    }
    variable->is_public = is_public;
    variable->type = ZVALUE_STATUS;
    variable->name = consume_name(parser, "a status name");
    if (!consume_type(parser, ZTOKEN_EQUAL, "'=' after the status name")) {
        return 0;
    }
    if (match_word(parser, "alive")) {
        variable->number_value = 1;
    } else if (match_word(parser, "dead")) {
        variable->number_value = 0;
    } else {
        fail_at(parser, &parser->current,
                "expected 'alive' or 'dead' for a status value");
        return 0;
    }
    return consume_type(parser, ZTOKEN_COLON, "':' after the status value");
}

static int append_constructor_argument(Parser *parser,
                                       ZSharpVariable *variable,
                                       ZSharpLiteral *literal) {
    ZSharpLiteral *resized = (ZSharpLiteral *)realloc(
        variable->constructor_arguments,
        (variable->constructor_argument_count + 1) *
            sizeof(*variable->constructor_arguments));
    if (resized == NULL) {
        free(literal->number_text);
        free(literal->text_value);
        fail_at(parser, &parser->current, "out of memory");
        return 0;
    }
    variable->constructor_arguments = resized;
    variable->constructor_arguments[variable->constructor_argument_count++] =
        *literal;
    return 1;
}

static int parse_constructor_literal(Parser *parser,
                                     ZSharpLiteral *literal) {
    ZSharpToken token = parser->current;
    memset(literal, 0, sizeof(*literal));
    if (match_type(parser, ZTOKEN_STRING)) {
        literal->type = ZVALUE_TEXT;
        literal->text_value = decode_text(parser, &token);
        return literal->text_value != NULL;
    }
    if (parser->current.type == ZTOKEN_NUMBER ||
        parser->current.type == ZTOKEN_MINUS) {
        literal->type = ZVALUE_NUMBER;
        return consume_signed_number_text(parser, &literal->number_text);
    }
    if (match_word(parser, "alive")) {
        literal->type = ZVALUE_STATUS;
        literal->number_value = 1;
        return 1;
    }
    if (match_word(parser, "dead")) {
        literal->type = ZVALUE_STATUS;
        literal->number_value = 0;
        return 1;
    }
    fail_at(parser, &parser->current,
            "expected a text, number, alive, or dead constructor argument");
    return 0;
}

static int append_object_array_item(Parser *parser,
                                    ZSharpVariable *array,
                                    ZSharpVariable *temporary) {
    ZSharpObjectArrayItem *resized = (ZSharpObjectArrayItem *)realloc(
        array->object_items,
        (array->object_item_count + 1) * sizeof(*array->object_items));
    if (resized == NULL) {
        fail_at(parser, &parser->current, "out of memory");
        return 0;
    }
    array->object_items = resized;
    memset(&array->object_items[array->object_item_count], 0,
           sizeof(*array->object_items));
    array->object_items[array->object_item_count].constructor_arguments =
        temporary->constructor_arguments;
    array->object_items[array->object_item_count]
        .constructor_argument_count = temporary->constructor_argument_count;
    array->object_item_count++;
    temporary->constructor_arguments = NULL;
    temporary->constructor_argument_count = 0;
    return 1;
}

static int parse_object_array_member(Parser *parser, ZSharpRoom *room,
                                     int is_public, char *type_name) {
    ZSharpVariable *array;
    char *name;
    if (!consume_type(parser, ZTOKEN_RIGHT_PAREN,
                      "')' in the object array type")) {
        free(type_name);
        return 0;
    }
    name = consume_name(parser, "an object array name");
    if (name == NULL) {
        free(type_name);
        return 0;
    }
    array = zsharp_room_add_variable(room);
    if (array == NULL) {
        free(type_name);
        free(name);
        fail_at(parser, &parser->current, "out of memory");
        return 0;
    }
    array->is_public = is_public;
    array->type = ZVALUE_OBJECT_ARRAY;
    array->array_object_type = type_name;
    array->name = name;
    if (!consume_type(parser, ZTOKEN_EQUAL,
                      "'=' after the object array name") ||
        !consume_type(parser, ZTOKEN_LEFT_BRACKET,
                      "'[' before the object array values")) return 0;
    if (parser->current.type != ZTOKEN_RIGHT_BRACKET) {
        do {
            ZSharpVariable temporary;
            char *constructor_type;
            memset(&temporary, 0, sizeof(temporary));
            if (!consume_word(parser, "new")) return 0;
            constructor_type = consume_name(parser, "a room name after 'new'");
            if (constructor_type == NULL) return 0;
            if (strcmp(constructor_type, array->array_object_type) != 0) {
                fail_at(parser, &parser->current,
                        "object array type '%s' cannot contain '%s'",
                        array->array_object_type, constructor_type);
                free(constructor_type);
                return 0;
            }
            free(constructor_type);
            if (!consume_type(parser, ZTOKEN_LEFT_BRACKET,
                              "'[' before the constructor arguments")) {
                return 0;
            }
            if (parser->current.type != ZTOKEN_RIGHT_BRACKET) {
                do {
                    ZSharpLiteral literal;
                    if (!parse_constructor_literal(parser, &literal) ||
                        !append_constructor_argument(parser, &temporary,
                                                     &literal)) return 0;
                } while (match_type(parser, ZTOKEN_COMMA));
            }
            if (!consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                              "']' after the constructor arguments") ||
                !append_object_array_item(parser, array, &temporary)) {
                return 0;
            }
        } while (match_type(parser, ZTOKEN_COMMA));
    }
    return consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                        "']' after the object array values") &&
           consume_type(parser, ZTOKEN_COLON,
                        "':' after the object array value");
}

static int parse_custom_member(Parser *parser, ZSharpRoom *room,
                               int is_public, char *type_name) {
    ZSharpVariable *variable;
    char *constructor_type;
    if (match_type(parser, ZTOKEN_LEFT_PAREN)) {
        return parse_object_array_member(parser, room, is_public, type_name);
    }
    if (parser->current.type == ZTOKEN_LEFT_BRACKET) {
        return parse_function_after_name(parser, room, is_public, type_name,
                                         ZRETURN_VOID, 0);
    }
    variable = zsharp_room_add_variable(room);
    if (variable == NULL) {
        free(type_name);
        fail_at(parser, &parser->current, "out of memory");
        return 0;
    }
    variable->is_public = is_public;
    variable->type = ZVALUE_OBJECT;
    variable->object_type = type_name;
    variable->name = consume_name(parser, "an object variable name");
    if (!consume_type(parser, ZTOKEN_EQUAL, "'=' after the object name") ||
        !consume_word(parser, "new")) {
        return 0;
    }
    constructor_type = consume_name(parser, "a room name after 'new'");
    if (constructor_type == NULL) return 0;
    if (strcmp(constructor_type, variable->object_type) != 0) {
        fail_at(parser, &parser->current,
                "object type '%s' cannot be created with '%s'",
                variable->object_type, constructor_type);
        free(constructor_type);
        return 0;
    }
    free(constructor_type);
    if (!consume_type(parser, ZTOKEN_LEFT_BRACKET,
                      "'[' before the constructor arguments")) {
        return 0;
    }
    if (parser->current.type != ZTOKEN_RIGHT_BRACKET) {
        do {
            ZSharpLiteral literal;
            if (!parse_constructor_literal(parser, &literal) ||
                !append_constructor_argument(parser, variable, &literal)) {
                return 0;
            }
        } while (match_type(parser, ZTOKEN_COMMA));
    }
    return consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                        "']' after the constructor arguments") &&
           consume_type(parser, ZTOKEN_COLON,
                        "':' after the object value");
}

static char *consume_json_field_name(Parser *parser) {
    char *parts[32] = {0};
    size_t count = 0;
    char *name;
    parts[count++] = consume_name(parser, "a JSON field name");
    if (parts[0] == NULL) return NULL;
    while (parser->current.type == ZTOKEN_MINUS) {
        advance_token(parser);
        if (count == 32) {
            fail_at(parser, &parser->current, "JSON field name is too long");
            break;
        }
        parts[count++] = consume_name(parser, "a name after '-' ");
        if (parts[count - 1] == NULL) break;
    }
    name = join_path_parts(parser, parts, count);
    if (name != NULL && count > 1) {
        char *cursor = name;
        while ((cursor = strchr(cursor, '.')) != NULL) *cursor++ = '-';
    }
    while (count > 0) free(parts[--count]);
    return name;
}

static int parse_json_schema_member(Parser *parser, ZSharpRoom *room,
                                    int is_public, char *name) {
    ZSharpJsonSchema *schema;
    size_t index;
    for (index = 0; index < room->json_schema_count; index++) {
        if (strcmp(room->json_schemas[index].name, name) == 0) {
            fail_at(parser, &parser->current,
                    "JSON schema '%s' is already defined", name);
            free(name);
            return 0;
        }
    }
    schema = zsharp_room_add_json_schema(room);
    if (schema == NULL) {
        free(name);
        fail_at(parser, &parser->current, "out of memory");
        return 0;
    }
    schema->is_public = is_public;
    schema->name = name;
    if (!consume_type(parser, ZTOKEN_LEFT_BRACKET,
                      "'[' after the JSON schema name") ||
        !consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                      "']' after the JSON schema options") ||
        !consume_type(parser, ZTOKEN_LEFT_PAREN,
                      "'(' before the JSON schema fields")) return 0;
    while (!parser->failed && parser->current.type != ZTOKEN_RIGHT_PAREN &&
           parser->current.type != ZTOKEN_EOF) {
        ZSharpJsonField *field;
        char *field_name = consume_json_field_name(parser);
        ZSharpValueType type;
        if (field_name == NULL ||
            !consume_type(parser, ZTOKEN_COLON,
                          "':' after the JSON field name")) {
            free(field_name);
            return 0;
        }
        if (match_word(parser, "text")) type = ZVALUE_TEXT;
        else if (match_word(parser, "number")) type = ZVALUE_NUMBER;
        else if (match_word(parser, "status")) type = ZVALUE_STATUS;
        else {
            fail_at(parser, &parser->current,
                    "JSON fields must use text, number, or status");
            free(field_name);
            return 0;
        }
        if (!consume_type(parser, ZTOKEN_COLON,
                          "':' after the JSON field type")) {
            free(field_name);
            return 0;
        }
        for (index = 0; index < schema->field_count; index++) {
            if (strcmp(schema->fields[index].name, field_name) == 0) {
                fail_at(parser, &parser->current,
                        "JSON field '%s' is already defined", field_name);
                free(field_name);
                return 0;
            }
        }
        field = zsharp_json_schema_add_field(schema);
        if (field == NULL) {
            free(field_name);
            fail_at(parser, &parser->current, "out of memory");
            return 0;
        }
        field->name = field_name;
        field->type = type;
    }
    return consume_type(parser, ZTOKEN_RIGHT_PAREN,
                        "')' after the JSON schema fields");
}

static int parse_json_member(Parser *parser, ZSharpRoom *room,
                             int is_public) {
    char *name = consume_name(parser, "a JSON schema or variable name");
    if (name == NULL) return 0;
    if (parser->current.type == ZTOKEN_LEFT_BRACKET)
        return parse_json_schema_member(parser, room, is_public, name);
    if (parser->current.type == ZTOKEN_EQUAL) {
        ZSharpVariable *variable = zsharp_room_add_variable(room);
        char *schema_name;
        char *path;
        size_t type_length;
        ZSharpToken path_token;
        if (variable == NULL) {
            free(name);
            fail_at(parser, &parser->current, "out of memory");
            return 0;
        }
        variable->is_public = is_public;
        variable->type = ZVALUE_OBJECT;
        variable->name = name;
        advance_token(parser);
        if (!consume_word(parser, "JSON") ||
            !consume_type(parser, ZTOKEN_DOT, "'.' after JSON") ||
            !consume_word(parser, "load") ||
            !consume_type(parser, ZTOKEN_LEFT_PAREN,
                          "'(' after JSON.load")) return 0;
        schema_name = consume_name(parser, "a JSON schema name");
        if (schema_name == NULL ||
            !consume_type(parser, ZTOKEN_COMMA,
                          "',' after the JSON schema")) {
            free(schema_name);
            return 0;
        }
        path_token = parser->current;
        if (!match_type(parser, ZTOKEN_STRING) ||
            !consume_type(parser, ZTOKEN_RIGHT_PAREN,
                          "')' after the JSON path") ||
            !consume_type(parser, ZTOKEN_COLON,
                          "':' after the JSON value")) {
            free(schema_name);
            return 0;
        }
        path = decode_text(parser, &path_token);
        type_length = strlen(schema_name) + 7;
        variable->object_type = (char *)malloc(type_length);
        variable->constructor_arguments =
            (ZSharpLiteral *)calloc(1, sizeof(ZSharpLiteral));
        if (path == NULL || variable->object_type == NULL ||
            variable->constructor_arguments == NULL) {
            free(path); free(schema_name);
            fail_at(parser, &parser->current, "out of memory");
            return 0;
        }
        snprintf(variable->object_type, type_length, "$json:%s", schema_name);
        variable->constructor_argument_count = 1;
        variable->constructor_arguments[0].type = ZVALUE_TEXT;
        variable->constructor_arguments[0].text_value = path;
        free(schema_name);
        return 1;
    }
    fail_at(parser, &parser->current,
            "expected '[' for a JSON schema or '=' for a JSON variable");
    free(name);
    return 0;
}

static int declaration_text(Parser *parser, ZSharpFunction *function, const char *text) {
    ZSharpInstruction *instruction = emit(parser, function, ZOP_PUSH_TEXT);
    if (instruction == NULL) return 0;
    instruction->operand = zsharp_copy_text(text, strlen(text));
    if (instruction->operand == NULL) { fail_at(parser, &parser->current, "out of memory"); return 0; }
    return 1;
}

static int declaration_fields(Parser *parser, ZSharpFunction *function,
                               const char *prefix, unsigned depth, uint32_t *count) {
    if (depth > 16) { fail_at(parser, &parser->current, "C configuration nesting exceeds 16"); return 0; }
    if (!consume_type(parser, ZTOKEN_LEFT_PAREN, "'(' before configuration fields")) return 0;
    while (!parser->failed && parser->current.type != ZTOKEN_RIGHT_PAREN &&
           parser->current.type != ZTOKEN_EOF) {
        char *key = consume_name(parser, "a configuration field"), path[1024];
        if (key == NULL) return 0;
        if (snprintf(path, sizeof(path), "%s%s%s", prefix, *prefix ? "." : "", key) >= sizeof(path)) {
            free(key); fail_at(parser, &parser->current, "configuration path is too long"); return 0;
        }
        free(key);
        if (parser->current.type == ZTOKEN_LEFT_PAREN) {
            if (!declaration_fields(parser, function, path, depth + 1, count)) return 0;
        } else {
            ZSharpInstruction *instruction;
            if (!consume_type(parser, ZTOKEN_COLON, "':' after the configuration field") ||
                !declaration_text(parser, function, path)) return 0;
            if (parser->current.type == ZTOKEN_COLOR) {
                char *color = copy_token(parser, &parser->current);
                int ok = color != NULL && declaration_text(parser, function, color);
                free(color); if (!ok) return 0; advance_token(parser);
            } else if (match_type(parser, ZTOKEN_LEFT_BRACKET)) {
                /* Configuration lists are scalar JSON, not runtime object arrays. */
                char list[4096] = "[";
                size_t used = 1;
                while (!parser->failed && parser->current.type != ZTOKEN_RIGHT_BRACKET) {
                    ZSharpToken token = parser->current;
                    if (token.type != ZTOKEN_STRING && token.type != ZTOKEN_NUMBER) {
                        fail_at(parser, &token, "configuration list requires text/number literals"); return 0;
                    }
                    if (used + token.length + 3 >= sizeof(list)) { fail_at(parser, &token, "configuration list is too long"); return 0; }
                    if (used > 1) list[used++] = ',';
                    memcpy(list + used, token.start, token.length); used += token.length;
                    advance_token(parser);
                    if (!match_type(parser, ZTOKEN_COMMA)) break;
                }
                if (!consume_type(parser, ZTOKEN_RIGHT_BRACKET, "']' after the configuration list")) return 0;
                list[used++] = ']'; list[used] = 0;
                if (!declaration_text(parser, function, list)) return 0;
            } else if (zsharp_token_equals(&parser->current, "true") || zsharp_token_equals(&parser->current, "false")) {
                instruction = emit(parser, function, ZOP_PUSH_STATUS);
                if (instruction == NULL) return 0;
                instruction->number_operand = zsharp_token_equals(&parser->current, "true");
                advance_token(parser);
            } else if (!parse_expression(parser, function)) return 0;
            if (!consume_type(parser, ZTOKEN_COLON, "':' after the configuration value")) return 0;
            *count += 2;
        }
    }
    return consume_type(parser, ZTOKEN_RIGHT_PAREN, "')' after configuration fields");
}

static int parse_declaration(Parser *parser, ZSharpRoom *room, int is_public,
                              const ZSharpCustomSyntaxRule *rule) {
    size_t index = room->function_count, i;
    char *name, initializer[64];
    ZSharpFunction *function;
    uint32_t count = 1;
    advance_token(parser);
    name = consume_name(parser, "a custom declaration name");
    if (name == NULL) return 0;
    if (rule->declaration_kind == 1 && strncmp(name, "__zsharp_c_init_", 16) == 0) {
        free(name);
        fail_at(parser, &parser->current, "function names beginning __zsharp_c_init_ are reserved"); return 0;
    }
    if (rule->declaration_kind == 1) {
        function = zsharp_room_add_function(room);
        if (!function) { free(name); return 0; }
        function->name = name;
        function->is_public = is_public;
        function->return_type = ZRETURN_VOID;
        function->disable_auto_run = 1;
        if (!parse_parameters(parser, function, 0) ||
            !consume_type(parser, ZTOKEN_LEFT_PAREN, "'(' before declaration body")) return 0;
    }
    snprintf(initializer, sizeof(initializer), "__zsharp_c_init_%zu", room->function_count);
    function = zsharp_room_add_function(room);
    if (function == NULL) { if (rule->declaration_kind != 1) free(name); fail_at(parser, &parser->current, "out of memory"); return 0; }
    function->name = zsharp_copy_text(initializer, strlen(initializer));
    function->disable_auto_run = 1;
    function->return_type = ZRETURN_VOID;
    if (!function->name || !declaration_text(parser, function, name)) return 0;
    if (rule->declaration_kind == 1) {
        char target[1024];
        ZSharpFunction *body = &room->functions[index];
        snprintf(target, sizeof(target), "%s:%s:%s", parser->source_name, room->name, body->name);
        if (!declaration_text(parser, function, target)) return 0;
        count++;
        for (i = 0; i < body->parameter_count; i++) {
            const char *type = body->parameters[i].type == ZVALUE_TEXT ? "text" :
                body->parameters[i].type == ZVALUE_NUMBER ? "number" :
                body->parameters[i].type == ZVALUE_STATUS ? "status" : "object";
            if (!declaration_text(parser, function, body->parameters[i].name) ||
                !declaration_text(parser, function, type)) return 0;
            count += 2;
        }
    } else {
        free(name);
        if (!declaration_fields(parser, function, "", 0, &count) ||
            !consume_type(parser, ZTOKEN_COLON, "':' after the named configuration block")) return 0;
    }
    if (!emit_custom_call(parser, function, rule, count)) return 0;
    if (rule->declaration_kind == 1) {
        int executable = 0;
        while (!parser->failed && parser->current.type != ZTOKEN_RIGHT_PAREN &&
               parser->current.type != ZTOKEN_EOF) {
            const ZSharpCustomSyntaxRule *option = NULL;
            for (i = 0; i < parser->syntax_rule_count; i++) {
                const ZSharpCustomSyntaxRule *candidate = &parser->syntax_rules[i];
                ZSharpLexer pattern, source = parser->lexer;
                ZSharpToken expected, actual = parser->current;
                if (candidate->declaration_kind != 3 && candidate->declaration_kind != 4) continue;
                zsharp_lexer_init(&pattern, candidate->pattern);
                while ((expected = zsharp_lexer_next(&pattern)).type != ZTOKEN_EOF &&
                       syntax_token_equals(&expected, &actual)) actual = zsharp_lexer_next(&source);
                if (expected.type == ZTOKEN_EOF && actual.type == ZTOKEN_LEFT_PAREN) { option = candidate; break; }
            }
            if (option) {
                ZSharpLexer pattern;
                ZSharpToken token;
                uint32_t arguments = 1;
                char target[1024];
                if (executable) { fail_at(parser, &parser->current, "function options must precede executable statements"); return 0; }
                snprintf(target, sizeof(target), "%s:%s:%s", parser->source_name, room->name, name);
                if (!declaration_text(parser, function, target)) return 0;
                zsharp_lexer_init(&pattern, option->pattern);
                while (zsharp_lexer_next(&pattern).type != ZTOKEN_EOF) advance_token(parser);
                advance_token(parser); /* '(' */
                if (option->declaration_kind == 4) {
                    char *duration = NULL, *milliseconds;
                    int seconds;
                    ZSharpInstruction *instruction;
                    if (!consume_number_text(parser, &duration)) return 0;
                    seconds = zsharp_token_equals(&parser->current, "s");
                    if (!seconds && !zsharp_token_equals(&parser->current, "ms")) {
                        free(duration); fail_at(parser, &parser->current, "function duration option requires 'ms' or 's'"); return 0;
                    }
                    advance_token(parser);
                    if (seconds) {
                        char error[256];
                        milliseconds = zsharp_decimal_multiply(duration, "1000", error, sizeof(error));
                        free(duration);
                        if (!milliseconds) { fail_at(parser, &parser->current, "%s", error); return 0; }
                    } else milliseconds = duration;
                    instruction = emit(parser, function, ZOP_PUSH_NUMBER);
                    if (!instruction) { free(milliseconds); return 0; }
                    instruction->operand = milliseconds;
                    arguments++;
                } else do {
                    char *value;
                    token = parser->current;
                    if (token.type != ZTOKEN_STRING) { fail_at(parser, &token, "function option requires one or more literal text values"); return 0; }
                    value = decode_text(parser, &token);
                    advance_token(parser);
                    if (!value) return 0;
                    if (!declaration_text(parser, function, value)) { free(value); return 0; }
                    free(value); arguments++;
                } while (match_type(parser, ZTOKEN_COMMA));
                if (!consume_type(parser, ZTOKEN_RIGHT_PAREN, "')' after function options") ||
                    !consume_type(parser, ZTOKEN_COLON, "':' after function options") ||
                    !emit_custom_call(parser, function, option, arguments)) return 0;
            } else {
                executable = 1;
                if (!parse_statement(parser, &room->functions[index])) return 0;
            }
        }
        return consume_type(parser, ZTOKEN_RIGHT_PAREN, "')' after declaration body");
    }
    return 1;
}

static int parse_member(Parser *parser, ZSharpRoom *room) {
    int is_public;
    int is_horde;
    int parsed = 0;
    size_t variable_count;
    size_t function_count;
    {
        size_t i;
        for (i = 0; i < parser->syntax_rule_count; i++) {
            const ZSharpCustomSyntaxRule *rule = &parser->syntax_rules[i];
            if (rule->declaration_kind == 2 && zsharp_token_equals(&parser->current, rule->pattern)) {
                parser->member_room = room;
                return parse_declaration(parser, room, 1, rule);
            }
        }
    }
    if (!parse_visibility(parser, &is_public)) return 0;
    parser->member_room = room;
    is_horde = match_word(parser, "horde");
    if (room->is_horde && !is_horde) {
        fail_at(parser, &parser->current,
                "every member in a horde room must be horde");
        return 0;
    }
    variable_count = room->variable_count;
    function_count = room->function_count;
    {
        size_t i;
        for (i = 0; i < parser->syntax_rule_count; i++) {
            const ZSharpCustomSyntaxRule *rule = &parser->syntax_rules[i];
            if (rule->declaration_kind && rule->declaration_kind < 3 && zsharp_token_equals(&parser->current, rule->pattern))
                return parse_declaration(parser, room, is_public, rule);
        }
    }
    if (match_word(parser, "JSON")) {
        parsed = parse_json_member(parser, room, is_public);
    } else if (match_word(parser, "function")) {
        int alias;
        ZSharpVariable *variable = zsharp_room_add_variable(room);
        if (variable == NULL) { fail_at(parser, &parser->current, "out of memory"); return 0; }
        variable->is_public = is_public;
        variable->type = ZVALUE_FUNCTION;
        variable->name = consume_name(parser, "a function reference name");
        if (!consume_type(parser, ZTOKEN_EQUAL, "'=' after the function reference")) return 0;
        variable->text_value = parse_function_target(parser, NULL, &alias);
        if (alias) { fail_at(parser, &parser->current, "room function references require a qualified target"); return 0; }
        parsed = variable->text_value != NULL && consume_type(parser, ZTOKEN_COLON, "':' after the function reference");
    } else if (match_word(parser, "text")) {
        parsed = parse_text_member(parser, room, is_public);
    } else if (match_word(parser, "number")) {
        parsed = parse_number_member(parser, room, is_public);
    } else if (match_word(parser, "status")) {
        parsed = parse_status_member(parser, room, is_public);
    } else if (match_word(parser, "brain")) {
        char *name = consume_name(parser, "a brain name");
        if (name == NULL) return 0;
        parsed = parse_function_after_name(parser, room, is_public, name,
                                           ZRETURN_VOID, 1);
    } else if (parser->current.type == ZTOKEN_IDENTIFIER) {
        char *type_name = consume_name(parser, "an object type");
        parsed = parse_custom_member(parser, room, is_public, type_name);
    } else {
        fail_at(parser, &parser->current, "expected a room member type");
        return 0;
    }
    if (!parsed) return 0;
    if (room->variable_count > variable_count) {
        room->variables[room->variable_count - 1].is_horde = is_horde;
    }
    if (room->function_count > function_count) {
        room->functions[room->function_count - 1].is_horde = is_horde;
    }
    return 1;
}

static int parse_import(Parser *parser, ZSharpRoom *room) {
    char *parts[64] = {0};
    size_t part_count = 0;
    size_t index;
    char *path;
    ZSharpImport *import;
    int foreign_language = 0;
    parts[part_count++] = consume_name(parser, "an imported project name");
    if (!parser->failed &&
        (strcmp(parts[0], "py") == 0 || strcmp(parts[0], "js") == 0 ||
         strcmp(parts[0], "lua") == 0 || strcmp(parts[0], "c") == 0 ||
         strcmp(parts[0], "cpp") == 0 ||
         strcmp(parts[0], "kt") == 0 ||
         strcmp(parts[0], "rust") == 0) &&
        match_type(parser, ZTOKEN_COLON)) {
        foreign_language = strcmp(parts[0], "py") == 0 ? 1 :
                           strcmp(parts[0], "js") == 0 ? 2 :
                           strcmp(parts[0], "lua") == 0 ? 3 :
                           strcmp(parts[0], "cpp") == 0 ? 4 :
                           strcmp(parts[0], "rust") == 0 ? 5 :
                           strcmp(parts[0], "c") == 0 ? 6 : 7;
        parts[part_count++] = consume_name(parser,
                                           "the foreign project name");
    }
    while (!parser->failed && match_type(parser, ZTOKEN_DOT)) {
        if (part_count == 64) {
            fail_at(parser, &parser->current,
                    "an import path can contain at most 64 names");
            break;
        }
        if (parser->current.type == ZTOKEN_STAR) {
            ZSharpToken wildcard = parser->current;
            parts[part_count++] = copy_token(parser, &wildcard);
            advance_token(parser);
            if (parser->current.type == ZTOKEN_DOT) {
                fail_at(parser, &wildcard,
                        "'*' must be the final name in an import path");
            }
            break;
        }
        parts[part_count++] =
            consume_name(parser, "a name or '*' in the import path");
    }
    if (!parser->failed && part_count < (foreign_language ? 3u : 2u)) {
        fail_at(parser, &parser->current,
                foreign_language
                    ? "a foreign import requires LANGUAGE:Project.File"
                    : "an import requires at least Project.File");
    }
    if (!parser->failed &&
        (!consume_type(parser, ZTOKEN_LEFT_PAREN,
                       "'(' after the imported file") ||
         !consume_type(parser, ZTOKEN_RIGHT_PAREN,
                       "')' after the imported file") ||
         !consume_type(parser, ZTOKEN_COLON,
                       "':' after the import statement"))) {
        /* The consume helpers record the diagnostic. */
    }
    if (parser->failed) {
        for (index = 0; index < part_count; index++) free(parts[index]);
        return 0;
    }
    path = join_path_parts(parser, parts, part_count);
    for (index = 0; index < part_count; index++) free(parts[index]);
    if (path == NULL) return 0;
    if (foreign_language) {
        const char *prefix = foreign_language == 1 ? "py" :
                             foreign_language == 2 ? "js" :
                             foreign_language == 3 ? "lua" :
                             foreign_language == 4 ? "cpp" :
                             foreign_language == 5 ? "rust" :
                             foreign_language == 6 ? "c" : "kt";
        size_t prefix_length = strlen(prefix);
        char *qualified = (char *)malloc(strlen(path) + 2);
        if (qualified == NULL) {
            free(path);
            fail_at(parser, &parser->current, "out of memory");
            return 0;
        }
        sprintf(qualified, "%s:%s", prefix, path + prefix_length + 1);
        free(path);
        path = qualified;
    }
    for (index = 0; index < room->import_count; index++) {
        if (strcmp(room->imports[index].path, path) == 0) {
            fail_at(parser, &parser->current, "duplicate import '%s'", path);
            free(path);
            return 0;
        }
    }
    import = zsharp_room_add_import(room);
    if (import == NULL) {
        fail_at(parser, &parser->current, "out of memory");
        free(path);
        return 0;
    }
    import->path = path;
    import->part_count = (uint32_t)part_count;
    return 1;
}

static int parse_window_import(Parser *parser, ZSharpWindow *window) {
    char *parts[64] = {0};
    size_t part_count = 0;
    size_t index;
    char *path;
    ZSharpImport *import;
    int foreign_language = 0;
    parts[part_count++] = consume_name(parser, "an imported project name");
    if (!parser->failed &&
        (strcmp(parts[0], "py") == 0 || strcmp(parts[0], "js") == 0 ||
         strcmp(parts[0], "lua") == 0 || strcmp(parts[0], "c") == 0 ||
         strcmp(parts[0], "cpp") == 0 ||
         strcmp(parts[0], "kt") == 0 ||
         strcmp(parts[0], "rust") == 0) &&
        match_type(parser, ZTOKEN_COLON)) {
        foreign_language = strcmp(parts[0], "py") == 0 ? 1 :
                           strcmp(parts[0], "js") == 0 ? 2 :
                           strcmp(parts[0], "lua") == 0 ? 3 :
                           strcmp(parts[0], "cpp") == 0 ? 4 :
                           strcmp(parts[0], "rust") == 0 ? 5 :
                           strcmp(parts[0], "c") == 0 ? 6 : 7;
        parts[part_count++] = consume_name(parser,
                                           "the foreign project name");
    }
    while (!parser->failed && match_type(parser, ZTOKEN_DOT)) {
        if (part_count == 64) {
            fail_at(parser, &parser->current,
                    "an import path can contain at most 64 names");
            break;
        }
        if (parser->current.type == ZTOKEN_STAR) {
            ZSharpToken wildcard = parser->current;
            parts[part_count++] = copy_token(parser, &wildcard);
            advance_token(parser);
            if (parser->current.type == ZTOKEN_DOT) {
                fail_at(parser, &wildcard,
                        "'*' must be the final name in an import path");
            }
            break;
        }
        parts[part_count++] =
            consume_name(parser, "a name or '*' in the import path");
    }
    if (!parser->failed && part_count < (foreign_language ? 3u : 2u)) {
        fail_at(parser, &parser->current,
                foreign_language
                    ? "a foreign import requires LANGUAGE:Project.File"
                    : "an import requires at least Project.File");
    }
    if (!parser->failed &&
        (!consume_type(parser, ZTOKEN_LEFT_PAREN,
                       "'(' after the imported file") ||
         !consume_type(parser, ZTOKEN_RIGHT_PAREN,
                       "')' after the imported file") ||
         !consume_type(parser, ZTOKEN_COLON,
                       "':' after the import statement"))) {
        /* The consume helpers record the diagnostic. */
    }
    if (parser->failed) {
        for (index = 0; index < part_count; index++) free(parts[index]);
        return 0;
    }
    path = join_path_parts(parser, parts, part_count);
    for (index = 0; index < part_count; index++) free(parts[index]);
    if (path == NULL) return 0;
    if (foreign_language) {
        const char *prefix = foreign_language == 1 ? "py" :
                             foreign_language == 2 ? "js" :
                             foreign_language == 3 ? "lua" :
                             foreign_language == 4 ? "cpp" :
                             foreign_language == 5 ? "rust" :
                             foreign_language == 6 ? "c" : "kt";
        size_t prefix_length = strlen(prefix);
        char *qualified = (char *)malloc(strlen(path) + 2);
        if (qualified == NULL) {
            free(path);
            fail_at(parser, &parser->current, "out of memory");
            return 0;
        }
        sprintf(qualified, "%s:%s", prefix, path + prefix_length + 1);
        free(path);
        path = qualified;
    }
    for (index = 0; index < window->import_count; index++) {
        if (strcmp(window->imports[index].path, path) == 0) {
            fail_at(parser, &parser->current, "duplicate import '%s'", path);
            free(path);
            return 0;
        }
    }
    import = zsharp_window_add_import(window);
    if (import == NULL) {
        fail_at(parser, &parser->current, "out of memory");
        free(path);
        return 0;
    }
    import->path = path;
    import->part_count = (uint32_t)part_count;
    return 1;
}

static int token_equals_ignore_case(const ZSharpToken *token,
                                    const char *text) {
    size_t index;
    size_t length = strlen(text);
    if (token->type != ZTOKEN_IDENTIFIER || token->length != length) return 0;
    for (index = 0; index < length; index++) {
        if (tolower((unsigned char)token->start[index]) !=
            tolower((unsigned char)text[index])) return 0;
    }
    return 1;
}

static char *copy_token_lower(Parser *parser, const ZSharpToken *token) {
    size_t index;
    char *copy = copy_token(parser, token);
    if (copy == NULL) return NULL;
    for (index = 0; copy[index] != '\0'; index++) {
        copy[index] = (char)tolower((unsigned char)copy[index]);
    }
    return copy;
}

static ZSharpUIProperty *find_ui_property(ZSharpUIElement *element,
                                          const char *name) {
    size_t index;
    for (index = 0; index < element->property_count; index++) {
        if (strcmp(element->properties[index].name, name) == 0) {
            return &element->properties[index];
        }
    }
    return NULL;
}

static ZSharpUIProperty *add_ui_property(Parser *parser,
                                         ZSharpUIElement *element,
                                         const char *name,
                                         ZSharpUIPropertyType type) {
    ZSharpUIProperty *property;
    if (find_ui_property(element, name) != NULL) {
        fail_at(parser, &parser->current, "duplicate UI field '%s'", name);
        return NULL;
    }
    property = zsharp_ui_element_add_property(element);
    if (property == NULL) {
        fail_at(parser, &parser->current, "out of memory");
        return NULL;
    }
    property->name = zsharp_copy_text(name, strlen(name));
    property->type = type;
    if (property->name == NULL) {
        fail_at(parser, &parser->current, "out of memory");
        return NULL;
    }
    return property;
}

static int append_ui_item(Parser *parser, ZSharpUIProperty *property,
                          char *item) {
    char **resized = (char **)realloc(
        property->items, (property->item_count + 1) * sizeof(*resized));
    if (resized == NULL) {
        free(item);
        fail_at(parser, &parser->current, "out of memory");
        return 0;
    }
    property->items = resized;
    property->items[property->item_count++] = item;
    return 1;
}

static int parse_ui_text_value(Parser *parser, ZSharpUIProperty *property) {
    ZSharpToken token = parser->current;
    if (!consume_type(parser, ZTOKEN_STRING, "a quoted text value")) return 0;
    property->text_value = decode_text(parser, &token);
    return property->text_value != NULL;
}

static int parse_ui_status_value(Parser *parser, ZSharpUIProperty *property) {
    if (match_word(parser, "alive")) {
        property->status_value = 1;
        return 1;
    }
    if (match_word(parser, "dead")) {
        property->status_value = 0;
        return 1;
    }
    fail_at(parser, &parser->current,
            "expected 'alive' or 'dead' for a UI status field");
    return 0;
}

static int parse_ui_color_value(Parser *parser, ZSharpUIProperty *property) {
    return parse_ui_paint_text(parser, &property->text_value);
}

static int parse_ui_measurement_value(Parser *parser,
                                      ZSharpUIProperty *property) {
    if (!consume_signed_number_text(parser, &property->text_value)) return 0;
    property->unit = ZUI_UNIT_ZU;
    if (match_word(parser, "zu")) {
        property->unit = ZUI_UNIT_ZU;
    } else if (match_word(parser, "px")) {
        property->unit = ZUI_UNIT_PX;
    }
    return 1;
}

static int parse_ui_identifier_value(Parser *parser,
                                     ZSharpUIProperty *property) {
    property->text_value = consume_name(parser, "a UI option name");
    return property->text_value != NULL;
}

static int parse_ui_identifier_array(Parser *parser,
                                     ZSharpUIProperty *property) {
    if (!consume_type(parser, ZTOKEN_LEFT_BRACKET,
                      "'[' before the option list")) return 0;
    if (parser->current.type != ZTOKEN_RIGHT_BRACKET) {
        do {
            char *item = consume_name(parser, "an option name");
            if (item == NULL || !append_ui_item(parser, property, item)) {
                return 0;
            }
        } while (match_type(parser, ZTOKEN_COMMA));
    }
    return consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                        "']' after the option list");
}

static size_t utf8_character_bytes(const char *text) {
    unsigned char first = (unsigned char)text[0];
    if (first < 0x80u) return first == 0 ? 0u : 1u;
    if ((first & 0xe0u) == 0xc0u) return 2u;
    if ((first & 0xf0u) == 0xe0u) return 3u;
    if ((first & 0xf8u) == 0xf0u) return 4u;
    return 0u;
}

static int parse_ui_text_array(Parser *parser,
                               ZSharpUIProperty *property) {
    if (!consume_type(parser, ZTOKEN_LEFT_BRACKET,
                      "'[' before the character list")) return 0;
    if (parser->current.type != ZTOKEN_RIGHT_BRACKET) {
        do {
            ZSharpToken token = parser->current;
            char *item;
            size_t bytes;
            if (!consume_type(parser, ZTOKEN_STRING,
                              "a quoted character")) return 0;
            item = decode_text(parser, &token);
            if (item == NULL) return 0;
            bytes = utf8_character_bytes(item);
            if (bytes == 0 || item[bytes] != '\0') {
                free(item);
                fail_at(parser, &token,
                        "each allowedCharacters item must contain exactly one character");
                return 0;
            }
            if (!append_ui_item(parser, property, item)) return 0;
        } while (match_type(parser, ZTOKEN_COMMA));
    }
    return consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                        "']' after the character list");
}

static int parse_ui_option_array(Parser *parser,
                                 ZSharpUIProperty *property) {
    if (!consume_type(parser, ZTOKEN_LEFT_BRACKET,
                      "'[' before the option list")) return 0;
    if (parser->current.type != ZTOKEN_RIGHT_BRACKET) {
        do {
            ZSharpToken token = parser->current;
            char *item;
            if (!consume_type(parser, ZTOKEN_STRING,
                              "a quoted dropdown option")) return 0;
            item = decode_text(parser, &token);
            if (item == NULL || !append_ui_item(parser, property, item))
                return 0;
        } while (match_type(parser, ZTOKEN_COMMA));
    }
    return consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                        "']' after the option list");
}

static int parse_ui_empty_array(Parser *parser) {
    if (!consume_type(parser, ZTOKEN_LEFT_BRACKET,
                      "'[' before the empty runtime value")) return 0;
    if (parser->current.type != ZTOKEN_RIGHT_BRACKET) {
        fail_at(parser, &parser->current,
                "'contents' is runtime-owned and must start as []");
        return 0;
    }
    return consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                        "']' after the empty runtime value");
}

static int parse_callback_path(Parser *parser, ZSharpUIProperty *property) {
    char *parts[4] = {0};
    size_t count = 0;
    size_t length = 0;
    size_t index;
    char *output;
    char *cursor;
    if (!consume_type(parser, ZTOKEN_LEFT_BRACKET,
                      "'[' before the click target")) return 0;
    if (parser->current.type == ZTOKEN_RIGHT_BRACKET) {
        property->text_value = zsharp_copy_text("", 0);
        if (property->text_value == NULL) {
            fail_at(parser, &parser->current, "out of memory");
            return 0;
        }
        return consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                            "']' after the empty click target");
    }
    do {
        if (count == 4) {
            fail_at(parser, &parser->current,
                    "a click target has at most Project:File:Room:Function");
            break;
        }
        parts[count++] = consume_name(parser, "a click target name");
    } while (!parser->failed && match_type(parser, ZTOKEN_COLON));
    if (!parser->failed && count < 3) {
        fail_at(parser, &parser->current,
                "a click target must be File:Room:Function or "
                "Project:File:Room:Function");
    }
    if (!parser->failed) {
        consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                     "']' after the click target");
    }
    if (parser->failed) {
        for (index = 0; index < count; index++) free(parts[index]);
        return 0;
    }
    for (index = 0; index < count; index++) {
        length += strlen(parts[index]) + (index > 0 ? 1u : 0u);
    }
    output = (char *)malloc(length + 1);
    if (output == NULL) {
        for (index = 0; index < count; index++) free(parts[index]);
        fail_at(parser, &parser->current, "out of memory");
        return 0;
    }
    cursor = output;
    for (index = 0; index < count; index++) {
        size_t part_length = strlen(parts[index]);
        if (index > 0) *cursor++ = ':';
        memcpy(cursor, parts[index], part_length);
        cursor += part_length;
        free(parts[index]);
    }
    *cursor = '\0';
    property->text_value = output;
    return 1;
}

static int parse_click_field(Parser *parser, ZSharpUIElement *element) {
    int seen_left = 0;
    int seen_right = 0;
    if (!consume_type(parser, ZTOKEN_LEFT_BRACKET,
                      "'[' after 'Click'")) return 0;
    while (!parser->failed && parser->current.type != ZTOKEN_RIGHT_BRACKET) {
        const char *name;
        ZSharpUIProperty *property;
        if (match_word(parser, "left")) {
            if (seen_left) {
                fail_at(parser, &parser->current,
                        "duplicate left click target");
                return 0;
            }
            seen_left = 1;
            name = "left";
        } else if (match_word(parser, "right")) {
            if (seen_right) {
                fail_at(parser, &parser->current,
                        "duplicate right click target");
                return 0;
            }
            seen_right = 1;
            name = "right";
        } else {
            fail_at(parser, &parser->current,
                    "expected 'left' or 'right' inside Click");
            return 0;
        }
        if (!consume_type(parser, ZTOKEN_COLON,
                          "':' after the mouse button")) return 0;
        property = add_ui_property(parser, element, name,
                                   ZUI_PROPERTY_CALLBACK);
        if (property == NULL || !parse_callback_path(parser, property) ||
            !consume_type(parser, ZTOKEN_COLON,
                          "':' after the click target")) return 0;
    }
    if (!parser->failed && (!seen_left || !seen_right)) {
        fail_at(parser, &parser->current,
                "Click must define both left and right targets; use [] for "
                "an unused target");
        return 0;
    }
    return consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                        "']' after Click") &&
           consume_type(parser, ZTOKEN_COLON, "':' after Click");
}

static int field_is_measurement(const char *name) {
    return strcmp(name, "width") == 0 || strcmp(name, "height") == 0 ||
           strcmp(name, "locationX") == 0 ||
           strcmp(name, "locationY") == 0 ||
           strcmp(name, "fontSize") == 0;
}

static int parse_ui_field(Parser *parser, ZSharpUIElement *element) {
    char *name;
    ZSharpUIProperty *property;
    ZSharpUIPropertyType type = ZUI_PROPERTY_TEXT;
    int valid = 0;
    if (element->type == ZUI_BUTTON && match_word(parser, "Click")) {
        return parse_click_field(parser, element);
    }
    if (element->type == ZUI_DROPDOWN && match_word(parser, "Change")) {
        ZSharpUIProperty *change = add_ui_property(
            parser, element, "change", ZUI_PROPERTY_CALLBACK);
        return change != NULL && parse_callback_path(parser, change) &&
               consume_type(parser, ZTOKEN_COLON,
                            "':' after the change target");
    }
    name = consume_name(parser, "a UI field name");
    if (name == NULL) return 0;
    if (strcmp(name, "heigh") == 0) {
        fail_at(parser, &parser->current,
                "unknown UI field 'heigh'; use 'height'");
        free(name);
        return 0;
    }
    if (element->type != ZUI_DESIGN &&
        (strcmp(name, "anchorX") == 0 || strcmp(name, "anchorY") == 0)) {
        type = ZUI_PROPERTY_IDENTIFIER;
        valid = 1;
    } else if (element->type == ZUI_DESIGN) {
        if (strcmp(name, "title") == 0 || strcmp(name, "icon") == 0) {
            type = ZUI_PROPERTY_TEXT;
            valid = 1;
        } else if (strcmp(name, "scalable") == 0) {
            type = ZUI_PROPERTY_STATUS;
            valid = 1;
        } else if (strcmp(name, "background") == 0) {
            type = ZUI_PROPERTY_COLOR;
            valid = 1;
        } else if (field_is_measurement(name)) {
            type = ZUI_PROPERTY_MEASUREMENT;
            valid = 1;
        }
    } else if (element->type == ZUI_TEXT) {
        if (strcmp(name, "content") == 0) {
            type = ZUI_PROPERTY_TEXT;
            valid = 1;
        } else if (strcmp(name, "color") == 0) {
            type = ZUI_PROPERTY_COLOR;
            valid = 1;
        } else if (strcmp(name, "textAlign") == 0) {
            type = ZUI_PROPERTY_IDENTIFIER;
            valid = 1;
        } else if (field_is_measurement(name)) {
            type = ZUI_PROPERTY_MEASUREMENT;
            valid = 1;
        }
    } else if (element->type == ZUI_BUTTON) {
        if (strcmp(name, "text") == 0) {
            type = ZUI_PROPERTY_TEXT;
            valid = 1;
        } else if (strcmp(name, "textColor") == 0 ||
                   strcmp(name, "buttonColor") == 0) {
            type = ZUI_PROPERTY_COLOR;
            valid = 1;
        } else if (field_is_measurement(name)) {
            type = ZUI_PROPERTY_MEASUREMENT;
            valid = 1;
        }
    } else if (element->type == ZUI_IMAGE) {
        if (strcmp(name, "file") == 0) {
            type = ZUI_PROPERTY_TEXT;
            valid = 1;
        } else if (field_is_measurement(name)) {
            type = ZUI_PROPERTY_MEASUREMENT;
            valid = 1;
        }
    } else if (element->type == ZUI_TEXT_INPUT) {
        if (strcmp(name, "display") == 0) {
            type = ZUI_PROPERTY_TEXT;
            valid = 1;
        } else if (strcmp(name, "type") == 0) {
            type = ZUI_PROPERTY_IDENTIFIER;
            valid = 1;
        } else if (strcmp(name, "multiline") == 0 ||
                   strcmp(name, "wrap") == 0) {
            type = ZUI_PROPERTY_STATUS;
            valid = 1;
        } else if (strcmp(name, "supportedTypes") == 0) {
            type = ZUI_PROPERTY_IDENTIFIER_ARRAY;
            valid = 1;
        } else if (strcmp(name, "allowedCharacters") == 0) {
            type = ZUI_PROPERTY_TEXT_ARRAY;
            valid = 1;
        } else if (strcmp(name, "contents") == 0) {
            type = ZUI_PROPERTY_EMPTY_ARRAY;
            valid = 1;
        } else if (strcmp(name, "textAlign") == 0 ||
                   strcmp(name, "textTransform") == 0) {
            type = ZUI_PROPERTY_IDENTIFIER;
            valid = 1;
        } else if (strcmp(name, "maxLength") == 0) {
            type = ZUI_PROPERTY_MEASUREMENT;
            valid = 1;
        } else if (field_is_measurement(name)) {
            type = ZUI_PROPERTY_MEASUREMENT;
            valid = 1;
        }
    } else if (element->type == ZUI_DROPDOWN) {
        if (strcmp(name, "options") == 0) {
            type = ZUI_PROPERTY_TEXT_ARRAY;
            valid = 1;
        } else if (strcmp(name, "selected") == 0) {
            type = ZUI_PROPERTY_TEXT;
            valid = 1;
        } else if (strcmp(name, "textColor") == 0 ||
                   strcmp(name, "dropdownColor") == 0) {
            type = ZUI_PROPERTY_COLOR;
            valid = 1;
        } else if (field_is_measurement(name)) {
            type = ZUI_PROPERTY_MEASUREMENT;
            valid = 1;
        }
    }
    if (!valid) {
        fail_at(parser, &parser->current, "unknown %s UI field '%s'",
                element->type == ZUI_DESIGN ? "design" :
                element->type == ZUI_TEXT ? "text" :
                element->type == ZUI_BUTTON ? "button" :
                element->type == ZUI_IMAGE ? "image" :
                element->type == ZUI_DROPDOWN ? "dropdown" : "textInput",
                name);
        free(name);
        return 0;
    }
    if (!consume_type(parser, ZTOKEN_COLON, "':' after the UI field name")) {
        free(name);
        return 0;
    }
    property = add_ui_property(parser, element, name, type);
    free(name);
    if (property == NULL) return 0;
    if ((type == ZUI_PROPERTY_TEXT &&
         !parse_ui_text_value(parser, property)) ||
        (type == ZUI_PROPERTY_STATUS &&
         !parse_ui_status_value(parser, property)) ||
        (type == ZUI_PROPERTY_COLOR &&
         !parse_ui_color_value(parser, property)) ||
        (type == ZUI_PROPERTY_MEASUREMENT &&
         !parse_ui_measurement_value(parser, property)) ||
        (type == ZUI_PROPERTY_IDENTIFIER &&
         !parse_ui_identifier_value(parser, property)) ||
        (type == ZUI_PROPERTY_IDENTIFIER_ARRAY &&
         !parse_ui_identifier_array(parser, property)) ||
        (type == ZUI_PROPERTY_TEXT_ARRAY &&
         !(element->type == ZUI_DROPDOWN
               ? parse_ui_option_array(parser, property)
               : parse_ui_text_array(parser, property))) ||
        (type == ZUI_PROPERTY_EMPTY_ARRAY && !parse_ui_empty_array(parser))) {
        return 0;
    }
    if (strcmp(property->name, "maxLength") == 0)
        property->unit = ZUI_UNIT_NONE;
    if (type == ZUI_PROPERTY_COLOR && property->text_value != NULL &&
        (strncmp(property->text_value, "linear-gradient(", 16) == 0 ||
         strncmp(property->text_value, "radial-gradient(", 16) == 0) &&
        !(element->type == ZUI_DESIGN &&
          strcmp(property->name, "background") == 0)) {
        fail_at(parser, &parser->current,
                "gradients are supported by design backgrounds");
        return 0;
    }
    if (type == ZUI_PROPERTY_MEASUREMENT &&
        (strcmp(property->name, "width") == 0 ||
         strcmp(property->name, "height") == 0) &&
        zsharp_decimal_compare(property->text_value, "0") <= 0) {
        fail_at(parser, &parser->current,
                "UI width and height must be greater than zero");
        return 0;
    }
    return consume_type(parser, ZTOKEN_COLON, "':' after the UI field value");
}

static int require_ui_field(Parser *parser, ZSharpUIElement *element,
                            const char *name) {
    if (find_ui_property(element, name) != NULL) return 1;
    fail_at(parser, &parser->current, "%s '%s' requires the '%s' field",
            element->type == ZUI_DESIGN ? "design" :
            element->type == ZUI_TEXT ? "text" :
            element->type == ZUI_BUTTON ? "button" :
            element->type == ZUI_IMAGE ? "image" :
            element->type == ZUI_DROPDOWN ? "dropdown" : "textInput",
            element->name, name);
    return 0;
}

static int finish_ui_element(Parser *parser, ZSharpUIElement *element) {
    ZSharpUIProperty *input_type;
    ZSharpUIProperty *supported;
    ZSharpUIProperty *multiline;
    ZSharpUIProperty *wrap;
    if (element->type != ZUI_DESIGN) {
        ZSharpUIProperty *anchor_x = find_ui_property(element, "anchorX");
        ZSharpUIProperty *anchor_y = find_ui_property(element, "anchorY");
        if (anchor_x != NULL && strcmp(anchor_x->text_value, "left") != 0 &&
            strcmp(anchor_x->text_value, "center") != 0 &&
            strcmp(anchor_x->text_value, "right") != 0) {
            fail_at(parser, &parser->current,
                    "anchorX must be left, center, or right");
            return 0;
        }
        if (anchor_y != NULL && strcmp(anchor_y->text_value, "top") != 0 &&
            strcmp(anchor_y->text_value, "center") != 0 &&
            strcmp(anchor_y->text_value, "bottom") != 0) {
            fail_at(parser, &parser->current,
                    "anchorY must be top, center, or bottom");
            return 0;
        }
    }
    if (element->type == ZUI_DESIGN) {
        return require_ui_field(parser, element, "title");
    }
    if (element->type == ZUI_TEXT) {
        ZSharpUIProperty *alignment = find_ui_property(element, "textAlign");
        if (alignment != NULL &&
            strcmp(alignment->text_value, "left") != 0 &&
            strcmp(alignment->text_value, "center") != 0 &&
            strcmp(alignment->text_value, "right") != 0) {
            fail_at(parser, &parser->current,
                    "textAlign must be left, center, or right");
            return 0;
        }
        return require_ui_field(parser, element, "content");
    }
    if (element->type == ZUI_BUTTON) {
        return require_ui_field(parser, element, "text") &&
               require_ui_field(parser, element, "width") &&
               require_ui_field(parser, element, "height");
    }
    if (element->type == ZUI_IMAGE) {
        return require_ui_field(parser, element, "file") &&
               require_ui_field(parser, element, "width") &&
               require_ui_field(parser, element, "height");
    }
    if (element->type == ZUI_DROPDOWN) {
        ZSharpUIProperty *options = find_ui_property(element, "options");
        ZSharpUIProperty *selected = find_ui_property(element, "selected");
        size_t index;
        int found = 0;
        if (!require_ui_field(parser, element, "options") ||
            !require_ui_field(parser, element, "selected") ||
            !require_ui_field(parser, element, "width") ||
            !require_ui_field(parser, element, "height")) return 0;
        if (options->item_count == 0) {
            fail_at(parser, &parser->current,
                    "dropdown options must contain at least one item");
            return 0;
        }
        for (index = 0; index < options->item_count; index++)
            if (strcmp(options->items[index], selected->text_value) == 0)
                found = 1;
        if (!found) {
            fail_at(parser, &parser->current,
                    "dropdown selected value must appear in options");
            return 0;
        }
        return 1;
    }
    if (!require_ui_field(parser, element, "display") ||
        !require_ui_field(parser, element, "type") ||
        !require_ui_field(parser, element, "width") ||
        !require_ui_field(parser, element, "height")) return 0;
    input_type = find_ui_property(element, "type");
    supported = find_ui_property(element, "supportedTypes");
    multiline = find_ui_property(element, "multiline");
    wrap = find_ui_property(element, "wrap");
    {
        ZSharpUIProperty *alignment = find_ui_property(element, "textAlign");
        ZSharpUIProperty *transform = find_ui_property(element, "textTransform");
        ZSharpUIProperty *maximum = find_ui_property(element, "maxLength");
        ZSharpUIProperty *allowed = find_ui_property(element, "allowedCharacters");
        if (alignment != NULL &&
            strcmp(alignment->text_value, "left") != 0 &&
            strcmp(alignment->text_value, "center") != 0 &&
            strcmp(alignment->text_value, "right") != 0) {
            fail_at(parser, &parser->current,
                    "textAlign must be left, center, or right");
            return 0;
        }
        if (transform != NULL &&
            strcmp(transform->text_value, "none") != 0 &&
            strcmp(transform->text_value, "uppercase") != 0 &&
            strcmp(transform->text_value, "lowercase") != 0) {
            fail_at(parser, &parser->current,
                    "textTransform must be none, uppercase, or lowercase");
            return 0;
        }
        if (maximum != NULL && maximum->unit != ZUI_UNIT_NONE) {
            fail_at(parser, &parser->current,
                    "maxLength is a character count and cannot use px or zu");
            return 0;
        }
        if (allowed != NULL && allowed->item_count == 0) {
            fail_at(parser, &parser->current,
                    "allowedCharacters must contain at least one character");
            return 0;
        }
        if (maximum != NULL &&
            (strchr(maximum->text_value, '.') != NULL ||
             zsharp_decimal_compare(maximum->text_value, "0") <= 0)) {
            fail_at(parser, &parser->current,
                    "maxLength must be a positive whole character count");
            return 0;
        }
    }
    if (strcmp(input_type->text_value, "text") != 0 &&
        strcmp(input_type->text_value, "image") != 0) {
        fail_at(parser, &parser->current,
                "textInput type must be 'text' or 'image'");
        return 0;
    }
    if (strcmp(input_type->text_value, "image") == 0 &&
        (supported == NULL || supported->item_count == 0)) {
        fail_at(parser, &parser->current,
                "image textInput '%s' requires supportedTypes", element->name);
        return 0;
    }
    if (strcmp(input_type->text_value, "text") == 0 && supported != NULL) {
        fail_at(parser, &parser->current,
                "supportedTypes is only valid for an image textInput");
        return 0;
    }
    if (strcmp(input_type->text_value, "text") != 0 &&
        find_ui_property(element, "allowedCharacters") != NULL) {
        fail_at(parser, &parser->current,
                "allowedCharacters is only valid for a text textInput");
        return 0;
    }
    if (strcmp(input_type->text_value, "image") == 0 &&
        (multiline != NULL || wrap != NULL)) {
        fail_at(parser, &parser->current,
                "multiline and wrap are only valid for a text textInput");
        return 0;
    }
    if (wrap != NULL &&
        (multiline == NULL || !multiline->status_value)) {
        fail_at(parser, &parser->current,
                "textInput wrap requires multiline: alive");
        return 0;
    }
    if (find_ui_property(element, "contents") == NULL &&
        add_ui_property(parser, element, "contents",
                        ZUI_PROPERTY_EMPTY_ARRAY) == NULL) return 0;
    return 1;
}

static int parse_ui_element(Parser *parser, ZSharpWindow *window) {
    int is_public;
    ZSharpUIElementType type;
    ZSharpUIElement *element;
    char *variant = NULL;
    size_t index;
    if (!parse_visibility(parser, &is_public)) return 0;
    if (match_word(parser, "design")) {
        type = ZUI_DESIGN;
    } else if (match_word(parser, "text")) {
        type = ZUI_TEXT;
        if (match_type(parser, ZTOKEN_LEFT_BRACKET)) {
            ZSharpToken token = parser->current;
            if (!token_equals_ignore_case(&token, "title") &&
                !token_equals_ignore_case(&token, "subtitle") &&
                !token_equals_ignore_case(&token, "header") &&
                !token_equals_ignore_case(&token, "subheader") &&
                !token_equals_ignore_case(&token, "paragraph")) {
                fail_at(parser, &token,
                        "text type must be title, subtitle, header, "
                        "subheader, or paragraph");
                return 0;
            }
            variant = copy_token_lower(parser, &token);
            advance_token(parser);
            if (!consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                              "']' after the text type")) {
                free(variant);
                return 0;
            }
        } else {
            variant = zsharp_copy_text("paragraph", 9);
            if (variant == NULL) {
                fail_at(parser, &parser->current, "out of memory");
                return 0;
            }
        }
    } else if (match_word(parser, "button")) {
        type = ZUI_BUTTON;
    } else if (match_word(parser, "image")) {
        type = ZUI_IMAGE;
    } else if (match_word(parser, "textInput")) {
        type = ZUI_TEXT_INPUT;
    } else if (match_word(parser, "dropdown") ||
               match_word(parser, "select")) {
        type = ZUI_DROPDOWN;
    } else {
        fail_at(parser, &parser->current,
                "expected design, text, button, image, textInput, or dropdown");
        return 0;
    }
    element = zsharp_window_add_element(window);
    if (element == NULL) {
        free(variant);
        fail_at(parser, &parser->current, "out of memory");
        return 0;
    }
    element->is_public = is_public;
    element->type = type;
    element->variant = variant;
    element->name = consume_name(parser, "a UI element name");
    if (element->name == NULL) return 0;
    for (index = 0; index + 1 < window->element_count; index++) {
        if (strcmp(window->elements[index].name, element->name) == 0) {
            fail_at(parser, &parser->current,
                    "duplicate UI element '%s'", element->name);
            return 0;
        }
        if (type == ZUI_DESIGN &&
            window->elements[index].type == ZUI_DESIGN) {
            fail_at(parser, &parser->current,
                    "a window can contain only one design");
            return 0;
        }
    }
    if (!consume_type(parser, ZTOKEN_LEFT_BRACKET,
                      "'[' after the UI element name") ||
        !consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                      "']' after the UI element options") ||
        !consume_type(parser, ZTOKEN_LEFT_PAREN,
                      "'(' before the UI element body")) return 0;
    while (!parser->failed && parser->current.type != ZTOKEN_RIGHT_PAREN &&
           parser->current.type != ZTOKEN_EOF) {
        if (!parse_ui_field(parser, element)) return 0;
    }
    return consume_type(parser, ZTOKEN_RIGHT_PAREN,
                        "')' after the UI element body") &&
           finish_ui_element(parser, element);
}

static int parse_window(Parser *parser, ZSharpProgram *program) {
    int is_public;
    size_t design_count = 0;
    size_t index;
    ZSharpWindow *window = &program->window;
    if (!parse_visibility(parser, &is_public)) return 0;
    if (!consume_word(parser, "Window")) return 0;
    program->has_window = 1;
    window->is_public = is_public;
    window->name = consume_name(parser, "a window name");
    if (window->name == NULL ||
        !consume_type(parser, ZTOKEN_LEFT_BRACKET,
                      "'[' after the window name") ||
        !consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                      "']' after the window options") ||
        !consume_type(parser, ZTOKEN_LEFT_PAREN,
                      "'(' before the window body")) return 0;
    while (!parser->failed && parser->current.type != ZTOKEN_RIGHT_PAREN &&
           parser->current.type != ZTOKEN_EOF) {
        if (match_word(parser, "import")) {
            if (!parse_window_import(parser, window)) return 0;
        } else if (!parse_ui_element(parser, window)) {
            return 0;
        }
    }
    if (!consume_type(parser, ZTOKEN_RIGHT_PAREN,
                      "')' after the window body")) return 0;
    for (index = 0; index < window->element_count; index++) {
        if (window->elements[index].type == ZUI_DESIGN) design_count++;
    }
    if (design_count != 1) {
        fail_at(parser, &parser->current,
                "a window must contain exactly one design");
        return 0;
    }
    return 1;
}

static int begins_room(const Parser *parser) {
    ZSharpLexer lexer;
    ZSharpToken token;
    if (zsharp_token_equals(&parser->current, "room")) return 1;
    if (zsharp_token_equals(&parser->current, "horde")) {
        return next_token_is_word(parser, "room");
    }
    if (zsharp_token_equals(&parser->current, "noticed") ||
        zsharp_token_equals(&parser->current, "silent")) {
        lexer = parser->lexer;
        token = zsharp_lexer_next(&lexer);
        if (zsharp_token_equals(&token, "room")) return 1;
        if (zsharp_token_equals(&token, "horde")) {
            token = zsharp_lexer_next(&lexer);
            return zsharp_token_equals(&token, "room");
        }
    }
    return 0;
}

static int parse_room(Parser *parser, ZSharpProgram *program,
                      const char *parent_qualified_name) {
    ZSharpVisibility visibility = ZVISIBILITY_FILE;
    int is_horde;
    char *parent_copy = NULL;
    char *room_name;
    char *qualified_name;
    size_t qualified_length;
    size_t room_index;
    ZSharpRoom *room;
    if (parent_qualified_name != NULL) {
        parent_copy = zsharp_copy_text(parent_qualified_name,
                                       strlen(parent_qualified_name));
        if (parent_copy == NULL) {
            fail_at(parser, &parser->current, "out of memory");
            return 0;
        }
    }
    if (match_word(parser, "noticed")) {
        visibility = ZVISIBILITY_NOTICED;
    } else if (match_word(parser, "silent")) {
        visibility = ZVISIBILITY_SILENT;
    }
    is_horde = match_word(parser, "horde");
    if (!consume_word(parser, "room")) {
        free(parent_copy);
        return 0;
    }
    room = zsharp_program_add_room(program);
    if (room == NULL) {
        free(parent_copy);
        fail_at(parser, &parser->current, "out of memory");
        return 0;
    }
    room_index = program->room_count - 1;
    room->visibility = visibility;
    room->is_horde = is_horde;
    room->parent_name = parent_copy;
    room_name = consume_name(parser, "a room name");
    room = &program->rooms[room_index];
    room->name = room_name;
    if (room_name == NULL) return 0;
    qualified_length = strlen(room_name) + 1;
    if (parent_copy != NULL) qualified_length += strlen(parent_copy) + 1;
    qualified_name = (char *)malloc(qualified_length);
    if (qualified_name == NULL) {
        fail_at(parser, &parser->current, "out of memory");
        return 0;
    }
    if (parent_copy == NULL) {
        snprintf(qualified_name, qualified_length, "%s", room_name);
    } else {
        snprintf(qualified_name, qualified_length, "%s.%s", parent_copy,
                 room_name);
    }
    room->qualified_name = qualified_name;
    if (!consume_type(parser, ZTOKEN_LEFT_BRACKET,
                      "'[' after the room name") ||
        !consume_type(parser, ZTOKEN_RIGHT_BRACKET,
                      "']' after the room options") ||
        !consume_type(parser, ZTOKEN_LEFT_PAREN,
                      "'(' before the room body")) {
        return 0;
    }
    while (!parser->failed && parser->current.type != ZTOKEN_RIGHT_PAREN &&
           parser->current.type != ZTOKEN_EOF) {
        room = &program->rooms[room_index];
        if (match_word(parser, "import")) {
            if (!parse_import(parser, room)) return 0;
        } else if (begins_room(parser)) {
            if (!parse_room(parser, program, room->qualified_name)) return 0;
        } else if (zsharp_token_equals(&parser->current, "Print") ||
                   zsharp_token_equals(&parser->current, "Function") ||
                   zsharp_token_equals(&parser->current, "feed") ||
                   zsharp_token_equals(&parser->current, "if") ||
                   zsharp_token_equals(&parser->current, "loop") ||
                   zsharp_token_equals(&parser->current, "continue") ||
                   zsharp_token_equals(&parser->current, "wait") ||
                   zsharp_token_equals(&parser->current, "delay")) {
            fail_at(parser, &parser->current,
                    "executable statements must be inside a brain function; "
                    "for startup code, declare 'noticed brain Start[]'");
            return 0;
        } else if (!parse_member(parser, room)) {
            return 0;
        }
    }
    return consume_type(parser, ZTOKEN_RIGHT_PAREN, "')' after the room body");
}

int zsharp_parse_source_with_syntax(
    const char *source, const char *source_name, ZSharpProgram *program,
    ZSharpDiagnostic *diagnostic, const ZSharpCustomSyntaxRule *rules,
    size_t rule_count) {
    Parser parser;
    memset(&parser, 0, sizeof(parser));
    memset(diagnostic, 0, sizeof(*diagnostic));
    parser.diagnostic = diagnostic;
    parser.syntax_rules = rules;
    parser.syntax_rule_count = rule_count;
    parser.source_name = source_name;
    zsharp_program_init(program);
    program->source_name = zsharp_copy_text(source_name, strlen(source_name));
    if (program->source_name == NULL) {
        diagnostic->line = 1;
        diagnostic->column = 1;
        snprintf(diagnostic->message, sizeof(diagnostic->message),
                 "out of memory");
        return 0;
    }
    zsharp_lexer_init(&parser.lexer, source);
    advance_token(&parser);
    consume_word(&parser, "zsharp");
    consume_type(&parser, ZTOKEN_EQUAL, "'=' after 'zsharp'");
    consume_word(&parser, "type");
    consume_type(&parser, ZTOKEN_DOT, "'.' after 'type'");
    consume_word(&parser, "script");
    if (match_type(&parser, ZTOKEN_COLON)) {
        if (match_word(&parser, "window")) {
            program->script_type = ZSCRIPT_WINDOW;
        } else if (match_word(&parser, "achievement")) {
            program->script_type = ZSCRIPT_ACHIEVEMENT;
        } else {
            fail_at(&parser, &parser.current,
                    "expected 'window' or 'achievement' after the script ':'");
        }
    }
    if (!parser.failed && program->script_type == ZSCRIPT_ACHIEVEMENT) {
        /* Achievement ZSON is validated by the game achievement loader. Keep
         * its tokens out of the normal room grammar. */
        while (!parser.failed && parser.current.type != ZTOKEN_EOF)
            advance_token(&parser);
    } else if (!parser.failed && program->script_type == ZSCRIPT_WINDOW) {
        parse_window(&parser, program);
        if (!parser.failed && parser.current.type != ZTOKEN_EOF) {
            fail_at(&parser, &parser.current,
                    "a window file can contain exactly one Window");
        }
    } else {
        while (!parser.failed && parser.current.type != ZTOKEN_EOF) {
            parse_room(&parser, program, NULL);
        }
        if (!parser.failed && program->room_count == 0) {
            fail_at(&parser, &parser.current,
                    "a script must contain at least one room");
        }
    }
    if (parser.failed) {
        zsharp_program_free(program);
        return 0;
    }
    return 1;
}

int zsharp_parse_source(const char *source, const char *source_name,
                        ZSharpProgram *program, ZSharpDiagnostic *diagnostic) {
    return zsharp_parse_source_with_syntax(source, source_name, program,
                                           diagnostic, NULL, 0);
}
