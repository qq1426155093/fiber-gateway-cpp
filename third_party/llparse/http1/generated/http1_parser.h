#ifndef FIBER_HTTP1_PARSER_H
#define FIBER_HTTP1_PARSER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Http1Parser Http1Parser;

Http1Parser *http1_parser_create(void);
void http1_parser_reset(Http1Parser *parser);
void http1_parser_destroy(Http1Parser *parser);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // FIBER_HTTP1_PARSER_H
