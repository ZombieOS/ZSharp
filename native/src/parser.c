#include "parser.h"

#include "decimal.h"
#include "lexer.h"

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
static int parse_qualified_call(Parser *parser, ZSharpFunction *function,
                                int produces_value);

static int parse_primary(Parser *parser, ZSharpFunction *function) {
    ZSharpToken token = parser->current;
    ZSharpInstruction *instruction;
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
    if (!consume_type(parser, ZTOKEN_LEFT_PAREN, "'(' after 'Print'") ||
        !parse_expression(parser, function) ||
        !consume_type(parser, ZTOKEN_RIGHT_PAREN,
                      "')' after the Print value") ||
        !consume_type(parser, ZTOKEN_COLON,
                      "':' after the Print statement")) {
        return 0;
    }
    return emit(parser, function, ZOP_PRINT) != NULL;
}

static int parse_qualified_call(Parser *parser, ZSharpFunction *function,
                                int produces_value) {
    char *parts[4] = {0};
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
        if (part_count == 4) {
            fail_at(parser, &parser->current,
                    "Function.call accepts File.Room.Function or "
                    "Project.File.Room.Function");
            break;
        }
        advance_token(parser);
        parts[part_count++] =
            consume_name(parser, "the next Function.call target name");
    }
    if (!parser->failed && part_count != 3 && part_count != 4) {
        fail_at(parser, &parser->current,
                "Function.call requires File.Room.Function or "
                "Project.File.Room.Function");
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
    instruction = emit(parser, function, produces_value
        ? ZOP_CALL_QUALIFIED_VALUE
        : ZOP_CALL_QUALIFIED);
    if (instruction == NULL) {
        for (index = 0; index < part_count; index++) free(parts[index]);
        free(call_outcome);
        return 0;
    }
    if (part_count == 4) {
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

    parts[part_count++] = copy_token(parser, &first_token);
    advance_token(parser);
    while (!parser->failed && match_type(parser, ZTOKEN_DOT)) {
        if (match_word(parser, "set")) {
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
        if (!parse_statement_block(parser, function)) {
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

static int parse_statement(Parser *parser, ZSharpFunction *function) {
    if (match_word(parser, "Print")) return parse_print(parser, function);
    if (match_word(parser, "Function")) {
        return parse_qualified_call(parser, function, 0);
    }
    if (match_word(parser, "number")) {
        return parse_number_statement(parser, function);
    }
    if (match_word(parser, "text")) {
        return parse_text_statement(parser, function);
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

static int parse_member(Parser *parser, ZSharpRoom *room) {
    int is_public;
    int is_horde;
    int parsed = 0;
    size_t variable_count;
    size_t function_count;
    if (!parse_visibility(parser, &is_public)) return 0;
    is_horde = match_word(parser, "horde");
    if (room->is_horde && !is_horde) {
        fail_at(parser, &parser->current,
                "every member in a horde room must be horde");
        return 0;
    }
    variable_count = room->variable_count;
    function_count = room->function_count;
    if (match_word(parser, "text")) {
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
    parts[part_count++] = consume_name(parser, "an imported project name");
    while (!parser->failed && match_type(parser, ZTOKEN_DOT)) {
        if (part_count == 64) {
            fail_at(parser, &parser->current,
                    "an import path can contain at most 64 names");
            break;
        }
        parts[part_count++] =
            consume_name(parser, "a name in the import path");
    }
    if (!parser->failed && part_count < 2) {
        fail_at(parser, &parser->current,
                "an import requires at least Project.File");
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
        } else if (!parse_member(parser, room)) {
            return 0;
        }
    }
    return consume_type(parser, ZTOKEN_RIGHT_PAREN, "')' after the room body");
}

int zsharp_parse_source(const char *source, const char *source_name,
                        ZSharpProgram *program, ZSharpDiagnostic *diagnostic) {
    Parser parser;
    memset(&parser, 0, sizeof(parser));
    memset(diagnostic, 0, sizeof(*diagnostic));
    parser.diagnostic = diagnostic;
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
    while (!parser.failed && parser.current.type != ZTOKEN_EOF) {
        parse_room(&parser, program, NULL);
    }
    if (!parser.failed && program->room_count == 0) {
        fail_at(&parser, &parser.current,
                "a script must contain at least one room");
    }
    if (parser.failed) {
        zsharp_program_free(program);
        return 0;
    }
    return 1;
}
