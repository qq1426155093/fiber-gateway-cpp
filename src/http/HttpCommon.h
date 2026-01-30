//
// Created by dear on 2026/1/30.
//

#ifndef FIBER_HTTPCOMMON_H
#define FIBER_HTTPCOMMON_H

namespace fiber::http {

struct HttpUri {
    std::string_view path{}; // ngx_http_request::uri
    std::string_view query{}; // ngx_http_request::args
    std::string_view exten{}; // ngx_http_request::exten
    std::string_view unparsed_uri{}; // ngx_http_request::unparsed_uri
};

enum class HttpVersion {
    HTTP_0_9 = 9,
    HTTP_1_0 = 1000,
    HTTP_1_1 = 1001,
    HTTP_2_0 = 2000,
    HTTP_3_0 = 3000,
};

enum class HttpMethod {
    Unknown = 0x00000001,
    Get = 0x00000002,
    Head = 0x00000004,
    Post = 0x00000008,
    Put = 0x00000010,
    Delete = 0x00000020,
    MKCOL = 0x00000040,
    Copy = 0x00000080,
    Move = 0x00000100,
    Options = 0x00000200,
    PropFind = 0x00000400,
    PropPatch = 0x00000800,
    Lock = 0x00001000,
    Unlock = 0x00002000,
    Patch = 0x00004000,
    Trace = 0x00008000,
    Connect = 0x00010000,
};
}


#endif // FIBER_HTTPCOMMON_H
