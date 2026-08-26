#include "lexer.h"

#include <ctype.h>
#include <string.h>

static int is_at_end(const ZSharpLexer *lexer) {
    return *lexer->current == '\0';
}

static char peek(const ZSharpLexer *lexer) {
    return *lexer->current;
}

static char peek_next(const ZSharpLexer *lexer) {
    if (is_at_end(lexer)) {
        return '\0';
    }
    return lexer->current[1];
}

static char advance(ZSharpLexer *lexer) {
    char character = *lexer->current++;
    if (character == '\n') {
        lexer->line++;
        lexer->column = 1;
    } else {
        lexer->column++;
    }
    return character;
}

static ZSharpToken make_token(ZSharpTokenType type, const char *start,
                              const ZSharpLexer *lexer, unsigned line,
                              unsigned column) {
    ZSharpToken token;
    token.type = type;
    token.start = start;
    token.length = (size_t)(lexer->current - start);
    token.line = line;
    token.column = column;
    return token;
}

static void skip_whitespace_and_comments(ZSharpLexer *lexer) {
    for (;;) {
        char character = peek(lexer);
        if (character == ' ' || character == '\r' || character == '\t' ||
            character == '\n') {
            advance(lexer);
        } else if (character == '/' && peek_next(lexer) == '/') {
            while (peek(lexer) != '\n' && !is_at_end(lexer)) {
                advance(lexer);
            }
        } else {
            return;
        }
    }
}

void zsharp_lexer_init(ZSharpLexer *lexer, const char *source) {
    lexer->source = source;
    lexer->current = source;
    lexer->line = 1;
    lexer->column = 1;
}

ZSharpToken zsharp_lexer_next(ZSharpLexer *lexer) {
    const char *start;
    unsigned line;
    unsigned column;
    char character;

    skip_whitespace_and_comments(lexer);
    start = lexer->current;
    line = lexer->line;
    column = lexer->column;

    if (is_at_end(lexer)) {
        return make_token(ZTOKEN_EOF, start, lexer, line, column);
    }

    character = advance(lexer);
    if (isalpha((unsigned char)character) || character == '_') {
        while (isalnum((unsigned char)peek(lexer)) || peek(lexer) == '_') {
            advance(lexer);
        }
        return make_token(ZTOKEN_IDENTIFIER, start, lexer, line, column);
    }

    if (isdigit((unsigned char)character)) {
        while (isdigit((unsigned char)peek(lexer))) {
            advance(lexer);
        }
        return make_token(ZTOKEN_NUMBER, start, lexer, line, column);
    }

    if (character == '"') {
        while (peek(lexer) != '"' && !is_at_end(lexer)) {
            if (peek(lexer) == '\\' && peek_next(lexer) != '\0') {
                advance(lexer);
            }
            advance(lexer);
        }
        if (is_at_end(lexer)) {
            return make_token(ZTOKEN_ERROR, start, lexer, line, column);
        }
        advance(lexer);
        return make_token(ZTOKEN_STRING, start, lexer, line, column);
    }

    if (character == '#') {
        while (isalnum((unsigned char)peek(lexer))) {
            advance(lexer);
        }
        return make_token(ZTOKEN_COLOR, start, lexer, line, column);
    }

    switch (character) {
        case '=':
            if (peek(lexer) == '=') {
                advance(lexer);
                return make_token(ZTOKEN_EQUAL_EQUAL, start, lexer, line,
                                  column);
            }
            return make_token(ZTOKEN_EQUAL, start, lexer, line, column);
        case '.': return make_token(ZTOKEN_DOT, start, lexer, line, column);
        case '[': return make_token(ZTOKEN_LEFT_BRACKET, start, lexer, line, column);
        case ']': return make_token(ZTOKEN_RIGHT_BRACKET, start, lexer, line, column);
        case '(': return make_token(ZTOKEN_LEFT_PAREN, start, lexer, line, column);
        case ')': return make_token(ZTOKEN_RIGHT_PAREN, start, lexer, line, column);
        case ':': return make_token(ZTOKEN_COLON, start, lexer, line, column);
        case ',': return make_token(ZTOKEN_COMMA, start, lexer, line, column);
        case '+': return make_token(ZTOKEN_PLUS, start, lexer, line, column);
        case '-': return make_token(ZTOKEN_MINUS, start, lexer, line, column);
        case '*': return make_token(ZTOKEN_STAR, start, lexer, line, column);
        case '/': return make_token(ZTOKEN_SLASH, start, lexer, line, column);
        case '%': return make_token(ZTOKEN_PERCENT, start, lexer, line, column);
        case '!':
            if (peek(lexer) == '=') {
                advance(lexer);
                return make_token(ZTOKEN_BANG_EQUAL, start, lexer, line,
                                  column);
            }
            return make_token(ZTOKEN_ERROR, start, lexer, line, column);
        case '>':
            if (peek(lexer) == '=') {
                advance(lexer);
                return make_token(ZTOKEN_GREATER_EQUAL, start, lexer, line,
                                  column);
            }
            return make_token(ZTOKEN_GREATER, start, lexer, line, column);
        case '<':
            if (peek(lexer) == '=') {
                advance(lexer);
                return make_token(ZTOKEN_LESS_EQUAL, start, lexer, line,
                                  column);
            }
            return make_token(ZTOKEN_LESS, start, lexer, line, column);
        default: return make_token(ZTOKEN_ERROR, start, lexer, line, column);
    }
}

int zsharp_token_equals(const ZSharpToken *token, const char *text) {
    size_t length = strlen(text);
    return token->type == ZTOKEN_IDENTIFIER && token->length == length &&
           memcmp(token->start, text, length) == 0;
}
