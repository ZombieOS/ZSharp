#ifndef ZSHARP_LEXER_H
#define ZSHARP_LEXER_H

#include <stddef.h>

typedef enum ZSharpTokenType {
    ZTOKEN_EOF,
    ZTOKEN_ERROR,
    ZTOKEN_IDENTIFIER,
    ZTOKEN_NUMBER,
    ZTOKEN_STRING,
    ZTOKEN_COLOR,
    ZTOKEN_EQUAL,
    ZTOKEN_EQUAL_EQUAL,
    ZTOKEN_DOT,
    ZTOKEN_LEFT_BRACKET,
    ZTOKEN_RIGHT_BRACKET,
    ZTOKEN_LEFT_PAREN,
    ZTOKEN_RIGHT_PAREN,
    ZTOKEN_COLON,
    ZTOKEN_COMMA,
    ZTOKEN_PLUS,
    ZTOKEN_MINUS,
    ZTOKEN_STAR,
    ZTOKEN_SLASH,
    ZTOKEN_PERCENT,
    ZTOKEN_BANG_EQUAL,
    ZTOKEN_GREATER,
    ZTOKEN_GREATER_EQUAL,
    ZTOKEN_LESS,
    ZTOKEN_LESS_EQUAL
} ZSharpTokenType;

typedef struct ZSharpToken {
    ZSharpTokenType type;
    const char *start;
    size_t length;
    unsigned line;
    unsigned column;
} ZSharpToken;

typedef struct ZSharpLexer {
    const char *source;
    const char *current;
    unsigned line;
    unsigned column;
} ZSharpLexer;

void zsharp_lexer_init(ZSharpLexer *lexer, const char *source);
ZSharpToken zsharp_lexer_next(ZSharpLexer *lexer);
int zsharp_token_equals(const ZSharpToken *token, const char *text);

#endif
