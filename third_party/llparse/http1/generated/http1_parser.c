#include "http1_parser.h"

#include <stdlib.h>

struct Http1Parser {
    int reserved;
};

Http1Parser *http1_parser_create(void) {
    return (Http1Parser *)calloc(1, sizeof(Http1Parser));
}

void http1_parser_reset(Http1Parser *parser) {
    if (parser == NULL) {
        return;
    }
    parser->reserved = 0;
}

void http1_parser_destroy(Http1Parser *parser) {
    free(parser);
}
