#include <tree_sitter/parser.h>

/* Set this to #define instead to enable debug printing */
#undef DEBUGGING

/* for debug */
#ifdef DEBUGGING
#  include <stdio.h>
#  define DEBUG(fmt,...)  fprintf(stderr, "scanner.c DEBUG: " fmt, __VA_ARGS__)
#else
#  define DEBUG(fmt,...)
#endif

#include <string.h>
#define streq(a,b)  (strcmp(a,b)==0)

#include <wctype.h>

enum TokenType {
  /* ident-alikes */
  TOKEN_Q_STRING_BEGIN,
  TOKEN_QQ_STRING_BEGIN,
  TOKEN_QW_LIST_BEGIN,
  /* non-ident tokens */
  PERLY_SEMICOLON,
  /* immediates */
  TOKEN_QUOTELIKE_END,
  TOKEN_Q_STRING_CONTENT,
  TOKEN_QQ_STRING_CONTENT,
  TOKEN_QW_LIST_CONTENT,
  TOKEN_ESCAPE_SEQUENCE,
};

struct LexerState {
  int delim_open, delim_close;  /* codepoints */
  int delim_count;
};

#define TOKEN(type) \
  do {                            \
    DEBUG("token(%s)\n", #type);  \
    lexer->result_symbol = type;  \
    return true;                  \
  } while(0)

static void skip_whitespace(TSLexer *lexer)
{
  while(1) {
    int c = lexer->lookahead;
    if(!c)
      return;
    if(iswspace(c))
      lexer->advance(lexer, true);
      /* continue */
    else
      return;
  }
}

static void _skip_chars(TSLexer *lexer, int maxlen, const char *allow)
{
  while(maxlen)
    if(!lexer->lookahead)
      return;
    else if(strchr(allow, lexer->lookahead)) {
      lexer->advance(lexer, false);
      if(maxlen > 0)
        maxlen--;
    }
    else
      break;
}
#define skip_hexdigits(lexer, maxlen)  _skip_chars(lexer, maxlen, "0123456789ABCDEFabcdef")
#define skip_digits(lexer, maxlen)     _skip_chars(lexer, maxlen, "0123456789")
#define skip_octdigits(lexer, maxlen)  _skip_chars(lexer, maxlen, "01234567")

static void skip_braced(TSLexer *lexer)
{
  if(lexer->lookahead != '{')
    return;

  lexer->advance(lexer, false);
  while(lexer->lookahead && lexer->lookahead != '}')
    lexer->advance(lexer, false);

  lexer->advance(lexer, false);
}

static int close_for_open(int c)
{
  switch(c) {
    case '(': return ')';
    case '[': return ']';
    case '{': return '}';
    case '<': return '>';
    /* TODO: Add aaaaalll the Unicode ones */
    default:
      return 0;
  }
}

static bool isidfirst(int c)
{
  // TODO: More Unicode in here
  return c == '_' || iswalpha(c);
}

static bool isidcont(int c)
{
  // TODO: More Unicode in here
  return isidfirst(c) || iswdigit(c);
}

void *tree_sitter_perl_external_scanner_create()
{
  return malloc(sizeof(struct LexerState));
}

void tree_sitter_perl_external_scanner_destroy(void *payload)
{
  free(payload);
}

void tree_sitter_perl_external_scanner_reset(void *payload) {}

unsigned int tree_sitter_perl_external_scanner_serialize(void *payload, char *buffer)
{
  struct LexerState *state = payload;

  unsigned int n = sizeof(struct LexerState);
  memcpy(buffer, state, n);
  return n;
}

void tree_sitter_perl_external_scanner_deserialize(void *payload, const char *buffer, unsigned int n)
{
  struct LexerState *state = payload;

  memcpy(state, buffer, n);
}

/* Longest identifier name we ever care to look specifically for (excluding
 * terminating NUL)
 */
#define MAX_IDENT_LEN 2

bool tree_sitter_perl_external_scanner_scan(
  void *payload,
  TSLexer *lexer,
  const bool *valid_symbols
) {
  struct LexerState *state = payload;

  // The only time we'd ever be looking for both BEGIN and END is during an error
  // condition. Abort in that case
  if(valid_symbols[TOKEN_Q_STRING_BEGIN] && valid_symbols[TOKEN_QUOTELIKE_END])
    return false;

  bool allow_identalike = false;
  for(int sym = 0; sym <= TOKEN_Q_STRING_BEGIN; sym++)
    if(valid_symbols[sym]) {
      allow_identalike = true;
      break;
    }

  if(allow_identalike || valid_symbols[PERLY_SEMICOLON]);
    skip_whitespace(lexer);

  int c = lexer->lookahead;

  if(valid_symbols[PERLY_SEMICOLON]) {
    if(c == ';') {
      lexer->advance(lexer, false);

      TOKEN(PERLY_SEMICOLON);
    }
    if(!c || c == '}') {
      DEBUG("Fake PERLY_SEMICOLON at end-of-scope\n", 0);
      // no advance

      TOKEN(PERLY_SEMICOLON);
    }
  }

  int ident_len = 0;
  char ident[MAX_IDENT_LEN+1];
  if(allow_identalike && isidfirst(c)) {
    /* All the identifiers we care about are US-ASCII */
    ident[0] = c;
    ident[1] = 0;
    ident_len++;
    lexer->advance(lexer, false);

    while((c = lexer->lookahead) && isidcont(c)) {
      if(ident_len < MAX_IDENT_LEN) {
        ident[ident_len] = c;
        ident[ident_len+1] = 0;
      }

      lexer->advance(lexer, false);
      ident_len++;
    }
    if(ident_len) {
      DEBUG("IDENT \"%.*s\"\n", ident_len, ident);
    }
  }

  if(valid_symbols[TOKEN_Q_STRING_BEGIN]) {
    /* Always expecting TOKEN_QQ_STRING_BEGIN as well */
    if(ident_len == 1 && streq(ident, "q") ||
        ident_len == 2 && streq(ident, "qq")) {
      skip_whitespace(lexer);

      int delim_close = close_for_open(lexer->lookahead);
      if(delim_close) {
        state->delim_open  = lexer->lookahead;
        state->delim_close = delim_close;
      }
      else {
        state->delim_open  = 0;
        state->delim_close = lexer->lookahead;
      }
      state->delim_count = 0;

      lexer->advance(lexer, false);

      DEBUG("Generic QSTRING open='%c' close='%c'\n", state->delim_open, state->delim_close);

      if(ident_len == 1)
        TOKEN(TOKEN_Q_STRING_BEGIN);
      else
        TOKEN(TOKEN_QQ_STRING_BEGIN);
    }
    if(lexer->lookahead == '\'') {
      lexer->advance(lexer, false);

      state->delim_open = 0;
      state->delim_close = '\'';
      state->delim_count = 0;

      TOKEN(TOKEN_Q_STRING_BEGIN);
    }
    if(lexer->lookahead == '"') {
      lexer->advance(lexer, false);

      state->delim_open = 0;
      state->delim_close = '"';
      state->delim_count = 0;

      TOKEN(TOKEN_QQ_STRING_BEGIN);
    }
  }
  if(valid_symbols[TOKEN_QW_LIST_BEGIN]) {
    if(ident_len == 2 && streq(ident, "qw")) {
      skip_whitespace(lexer);

      int delim_close = close_for_open(lexer->lookahead);
      if(delim_close) {
        state->delim_open  = lexer->lookahead;
        state->delim_close = delim_close;
      }
      else {
        state->delim_open  = 0;
        state->delim_close = lexer->lookahead;
      }
      state->delim_count = 0;

      lexer->advance(lexer, false);

      DEBUG("QW LIST open='%c' close='%c'\n", state->delim_open, state->delim_close);

      TOKEN(TOKEN_QW_LIST_BEGIN);
    }
  }

  if(valid_symbols[TOKEN_ESCAPE_SEQUENCE]) {
    if(lexer->lookahead == '\\') {
      lexer->advance(lexer, false);

      int escape = lexer->lookahead;
      lexer->advance(lexer, false);

      switch(escape) {
        case 'x':
          if(lexer->lookahead == '{')
            skip_braced(lexer);
          else
            skip_hexdigits(lexer, 2);
          break;

        case 'N':
          skip_braced(lexer);
          break;

        case 'o':
          /* TODO: contents should just be octal */
          skip_braced(lexer);
          break;

        case '0':
          skip_octdigits(lexer, 3);
          break;

        default:
          break;
      }

      TOKEN(TOKEN_ESCAPE_SEQUENCE);
    }
  }

  if(valid_symbols[TOKEN_Q_STRING_CONTENT] || valid_symbols[TOKEN_QQ_STRING_CONTENT]) {
    bool is_qq = valid_symbols[TOKEN_QQ_STRING_CONTENT];
    bool valid = false;

    int c;
    while((c = lexer->lookahead)) {
      if(c == '\\')
        break;
      if(state->delim_open && c == state->delim_open)
        state->delim_count++;
      else if(c == state->delim_close) {
        if(state->delim_count)
          state->delim_count--;
        else
          break;
      }
      else if(is_qq && (c == '$' || c == '@'))
        break;

      valid = true;
      lexer->advance(lexer, false);
    }

    if(valid) {
      if(is_qq)
        TOKEN(TOKEN_QQ_STRING_CONTENT);
      else
        TOKEN(TOKEN_Q_STRING_CONTENT);
    }
  }

  if(valid_symbols[TOKEN_QW_LIST_CONTENT]) {
    bool valid = false;

    int c;
    while((c = lexer->lookahead)) {
      if(iswspace(c))
        break;
      if(state->delim_open && c == state->delim_open)
        state->delim_count++;
      else if(c == state->delim_close) {
        if(state->delim_count)
          state->delim_count--;
        else
          break;
      }

      lexer->advance(lexer, false);
      valid = true;
    }

    if(valid)
      TOKEN(TOKEN_QW_LIST_CONTENT);
  }

  if(valid_symbols[TOKEN_QUOTELIKE_END]) {
    if(lexer->lookahead == state->delim_close && !state->delim_count) {
      lexer->advance(lexer, false);

      TOKEN(TOKEN_QUOTELIKE_END);
    }
  }

  return false;
}
