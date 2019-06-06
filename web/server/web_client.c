// SPDX-License-Identifier: GPL-3.0-or-later

#include "web_client.h"

// this is an async I/O implementation of the web server request parser
// it is used by all netdata web servers

int respect_web_browser_do_not_track_policy = 0;
char *web_x_frame_options = NULL;

#ifdef NETDATA_WITH_ZLIB
int web_enable_gzip = 1, web_gzip_level = 3, web_gzip_strategy = Z_DEFAULT_STRATEGY;
#endif /* NETDATA_WITH_ZLIB */

inline int web_client_permission_denied(struct web_client *w) {
    w->response.data->contenttype = CT_TEXT_PLAIN;
    buffer_flush(w->response.data);
    buffer_strcat(w->response.data, "You are not allowed to access this resource.");
    w->response.code = 403;
    return 403;
}

static inline int web_client_crock_socket(struct web_client *w) {
#ifdef TCP_CORK
    if(likely(web_client_is_corkable(w) && !w->tcp_cork && w->ofd != -1)) {
        w->tcp_cork = 1;
        if(unlikely(setsockopt(w->ofd, IPPROTO_TCP, TCP_CORK, (char *) &w->tcp_cork, sizeof(int)) != 0)) {
            error("%llu: failed to enable TCP_CORK on socket.", w->id);

            w->tcp_cork = 0;
            return -1;
        }
    }
#else
    (void)w;
#endif /* TCP_CORK */

    return 0;
}

static inline int web_client_uncrock_socket(struct web_client *w) {
#ifdef TCP_CORK
    if(likely(w->tcp_cork && w->ofd != -1)) {
        w->tcp_cork = 0;
        if(unlikely(setsockopt(w->ofd, IPPROTO_TCP, TCP_CORK, (char *) &w->tcp_cork, sizeof(int)) != 0)) {
            error("%llu: failed to disable TCP_CORK on socket.", w->id);
            w->tcp_cork = 1;
            return -1;
        }
    }
#else
    (void)w;
#endif /* TCP_CORK */

    return 0;
}

static inline char *strip_control_characters(char *url) {
    char *s = url;
    if(!s) return "";

    if(iscntrl(*s)) *s = ' ';
    while(*++s) {
        if(iscntrl(*s)) *s = ' ';
    }

    return url;
}

void web_client_request_done(struct web_client *w) {
    web_client_uncrock_socket(w);

    debug(D_WEB_CLIENT, "%llu: Resetting client.", w->id);

    if(likely(w->last_url[0])) {
        struct timeval tv;
        now_realtime_timeval(&tv);

        size_t size = (w->mode == WEB_CLIENT_MODE_FILECOPY)?w->response.rlen:w->response.data->len;
        size_t sent = size;
#ifdef NETDATA_WITH_ZLIB
        if(likely(w->response.zoutput)) sent = (size_t)w->response.zstream.total_out;
#endif

        // --------------------------------------------------------------------
        // global statistics

        finished_web_request_statistics(dt_usec(&tv, &w->tv_in),
                                        w->stats_received_bytes,
                                        w->stats_sent_bytes,
                                        size,
                                        sent);

        w->stats_received_bytes = 0;
        w->stats_sent_bytes = 0;


        // --------------------------------------------------------------------

        const char *mode;
        switch(w->mode) {
            case WEB_CLIENT_MODE_FILECOPY:
                mode = "FILECOPY";
                break;

            case WEB_CLIENT_MODE_OPTIONS:
                mode = "OPTIONS";
                break;

            case WEB_CLIENT_MODE_STREAM:
                mode = "STREAM";
                break;

            case WEB_CLIENT_MODE_NORMAL:
                mode = "DATA";
                break;

            default:
                mode = "UNKNOWN";
                break;
        }

        // access log
        log_access("%llu: %d '[%s]:%s' '%s' (sent/all = %zu/%zu bytes %0.0f%%, prep/sent/total = %0.2f/%0.2f/%0.2f ms) %d '%s'",
                   w->id
                   , gettid()
                   , w->client_ip
                   , w->client_port
                   , mode
                   , sent
                   , size
                   , -((size > 0) ? ((size - sent) / (double) size * 100.0) : 0.0)
                   , dt_usec(&w->tv_ready, &w->tv_in) / 1000.0
                   , dt_usec(&tv, &w->tv_ready) / 1000.0
                   , dt_usec(&tv, &w->tv_in) / 1000.0
                   , w->response.code
                   , strip_control_characters(w->last_url)
        );
    }

    if(unlikely(w->mode == WEB_CLIENT_MODE_FILECOPY)) {
        if(w->ifd != w->ofd) {
            debug(D_WEB_CLIENT, "%llu: Closing filecopy input file descriptor %d.", w->id, w->ifd);

            if(web_server_mode != WEB_SERVER_MODE_STATIC_THREADED) {
                if (w->ifd != -1) {
                    close(w->ifd);
                }
            }

            w->ifd = w->ofd;
        }
    }

    w->last_url[0] = '\0';
    w->cookie1[0] = '\0';
    w->cookie2[0] = '\0';
    w->origin[0] = '*';
    w->origin[1] = '\0';

    freez(w->user_agent); w->user_agent = NULL;
    if (w->auth_bearer_token) {
        freez(w->auth_bearer_token);
        w->auth_bearer_token = NULL;
    }

    w->mode = WEB_CLIENT_MODE_NORMAL;

    w->tcp_cork = 0;
    web_client_disable_donottrack(w);
    web_client_disable_tracking_required(w);
    web_client_disable_keepalive(w);
    w->decoded_url[0] = '\0';

    buffer_reset(w->response.header_output);
    buffer_reset(w->response.header);
    buffer_reset(w->response.data);
    w->response.rlen = 0;
    w->response.sent = 0;
    w->response.code = 0;

    w->header_parse_tries = 0;
    w->header_parse_last_size = 0;

    web_client_enable_wait_receive(w);
    web_client_disable_wait_send(w);

    w->response.zoutput = 0;

    // if we had enabled compression, release it
#ifdef NETDATA_WITH_ZLIB
    if(w->response.zinitialized) {
        debug(D_DEFLATE, "%llu: Freeing compression resources.", w->id);
        deflateEnd(&w->response.zstream);
        w->response.zsent = 0;
        w->response.zhave = 0;
        w->response.zstream.avail_in = 0;
        w->response.zstream.avail_out = 0;
        w->response.zstream.total_in = 0;
        w->response.zstream.total_out = 0;
        w->response.zinitialized = 0;
    }
#endif // NETDATA_WITH_ZLIB
}

uid_t web_files_uid(void) {
    static char *web_owner = NULL;
    static uid_t owner_uid = 0;

    if(unlikely(!web_owner)) {
        // getpwuid() is not thread safe,
        // but we have called this function once
        // while single threaded
        struct passwd *pw = getpwuid(geteuid());
        web_owner = config_get(CONFIG_SECTION_WEB, "web files owner", (pw)?(pw->pw_name?pw->pw_name:""):"");
        if(!web_owner || !*web_owner)
            owner_uid = geteuid();
        else {
            // getpwnam() is not thread safe,
            // but we have called this function once
            // while single threaded
            pw = getpwnam(web_owner);
            if(!pw) {
                error("User '%s' is not present. Ignoring option.", web_owner);
                owner_uid = geteuid();
            }
            else {
                debug(D_WEB_CLIENT, "Web files owner set to %s.", web_owner);
                owner_uid = pw->pw_uid;
            }
        }
    }

    return(owner_uid);
}

gid_t web_files_gid(void) {
    static char *web_group = NULL;
    static gid_t owner_gid = 0;

    if(unlikely(!web_group)) {
        // getgrgid() is not thread safe,
        // but we have called this function once
        // while single threaded
        struct group *gr = getgrgid(getegid());
        web_group = config_get(CONFIG_SECTION_WEB, "web files group", (gr)?(gr->gr_name?gr->gr_name:""):"");
        if(!web_group || !*web_group)
            owner_gid = getegid();
        else {
            // getgrnam() is not thread safe,
            // but we have called this function once
            // while single threaded
            gr = getgrnam(web_group);
            if(!gr) {
                error("Group '%s' is not present. Ignoring option.", web_group);
                owner_gid = getegid();
            }
            else {
                debug(D_WEB_CLIENT, "Web files group set to %s.", web_group);
                owner_gid = gr->gr_gid;
            }
        }
    }

    return(owner_gid);
}

static struct {
    const char *extension;
    uint32_t hash;
    uint8_t contenttype;
} mime_types[] = {
        {  "html" , 0    , CT_TEXT_HTML}
        , {"js"   , 0    , CT_APPLICATION_X_JAVASCRIPT}
        , {"css"  , 0    , CT_TEXT_CSS}
        , {"xml"  , 0    , CT_TEXT_XML}
        , {"xsl"  , 0    , CT_TEXT_XSL}
        , {"txt"  , 0    , CT_TEXT_PLAIN}
        , {"svg"  , 0    , CT_IMAGE_SVG_XML}
        , {"ttf"  , 0    , CT_APPLICATION_X_FONT_TRUETYPE}
        , {"otf"  , 0    , CT_APPLICATION_X_FONT_OPENTYPE}
        , {"woff2", 0    , CT_APPLICATION_FONT_WOFF2}
        , {"woff" , 0    , CT_APPLICATION_FONT_WOFF}
        , {"eot"  , 0    , CT_APPLICATION_VND_MS_FONTOBJ}
        , {"png"  , 0    , CT_IMAGE_PNG}
        , {"jpg"  , 0    , CT_IMAGE_JPG}
        , {"jpeg" , 0    , CT_IMAGE_JPG}
        , {"gif"  , 0    , CT_IMAGE_GIF}
        , {"bmp"  , 0    , CT_IMAGE_BMP}
        , {"ico"  , 0    , CT_IMAGE_XICON}
        , {"icns" , 0    , CT_IMAGE_ICNS}
        , {       NULL, 0, 0}
};

static inline uint8_t contenttype_for_filename(const char *filename) {
    // info("checking filename '%s'", filename);

    static int initialized = 0;
    int i;

    if(unlikely(!initialized)) {
        for (i = 0; mime_types[i].extension; i++)
            mime_types[i].hash = simple_hash(mime_types[i].extension);

        initialized = 1;
    }

    const char *s = filename, *last_dot = NULL;

    // find the last dot
    while(*s) {
        if(unlikely(*s == '.')) last_dot = s;
        s++;
    }

    if(unlikely(!last_dot || !*last_dot || !last_dot[1])) {
        // info("no extension for filename '%s'", filename);
        return CT_APPLICATION_OCTET_STREAM;
    }
    last_dot++;

    // info("extension for filename '%s' is '%s'", filename, last_dot);

    uint32_t hash = simple_hash(last_dot);
    for(i = 0; mime_types[i].extension ; i++) {
        if(unlikely(hash == mime_types[i].hash && !strcmp(last_dot, mime_types[i].extension))) {
            // info("matched extension for filename '%s': '%s'", filename, last_dot);
            return mime_types[i].contenttype;
        }
    }

    // info("not matched extension for filename '%s': '%s'", filename, last_dot);
    return CT_APPLICATION_OCTET_STREAM;
}

static inline int access_to_file_is_not_permitted(struct web_client *w, const char *filename) {
    w->response.data->contenttype = CT_TEXT_HTML;
    buffer_strcat(w->response.data, "Access to file is not permitted: ");
    buffer_strcat_htmlescape(w->response.data, filename);
    return 403;
}

int mysendfile(struct web_client *w, char *filename) {
    debug(D_WEB_CLIENT, "%llu: Looking for file '%s/%s'", w->id, netdata_configured_web_dir, filename);

    if(!web_client_can_access_dashboard(w))
        return web_client_permission_denied(w);

    // skip leading slashes
    while (*filename == '/') filename++;

    // if the filename contains "strange" characters, refuse to serve it
    char *s;
    for(s = filename; *s ;s++) {
        if(!isalnum(*s) && *s != '/' && *s != '.' && *s != '-' && *s != '_') {
            debug(D_WEB_CLIENT_ACCESS, "%llu: File '%s' is not acceptable.", w->id, filename);
            w->response.data->contenttype = CT_TEXT_HTML;
            buffer_sprintf(w->response.data, "Filename contains invalid characters: ");
            buffer_strcat_htmlescape(w->response.data, filename);
            return 400;
        }
    }

    // if the filename contains a .. refuse to serve it
    if(strstr(filename, "..") != 0) {
        debug(D_WEB_CLIENT_ACCESS, "%llu: File '%s' is not acceptable.", w->id, filename);
        w->response.data->contenttype = CT_TEXT_HTML;
        buffer_strcat(w->response.data, "Relative filenames are not supported: ");
        buffer_strcat_htmlescape(w->response.data, filename);
        return 400;
    }

    // find the physical file on disk
    char webfilename[FILENAME_MAX + 1];
    snprintfz(webfilename, FILENAME_MAX, "%s/%s", netdata_configured_web_dir, filename);

    struct stat statbuf;
    int done = 0;
    while(!done) {
        // check if the file exists
        if (lstat(webfilename, &statbuf) != 0) {
            debug(D_WEB_CLIENT_ACCESS, "%llu: File '%s' is not found.", w->id, webfilename);
            w->response.data->contenttype = CT_TEXT_HTML;
            buffer_strcat(w->response.data, "File does not exist, or is not accessible: ");
            buffer_strcat_htmlescape(w->response.data, webfilename);
            return 404;
        }

        if ((statbuf.st_mode & S_IFMT) == S_IFDIR) {
            snprintfz(webfilename, FILENAME_MAX, "%s/%s/index.html", netdata_configured_web_dir, filename);
            continue;
        }

        if ((statbuf.st_mode & S_IFMT) != S_IFREG) {
            error("%llu: File '%s' is not a regular file. Access Denied.", w->id, webfilename);
            return access_to_file_is_not_permitted(w, webfilename);
        }

        // check if the file is owned by expected user
        if (statbuf.st_uid != web_files_uid()) {
            error("%llu: File '%s' is owned by user %u (expected user %u). Access Denied.", w->id, webfilename, statbuf.st_uid, web_files_uid());
            return access_to_file_is_not_permitted(w, webfilename);
        }

        // check if the file is owned by expected group
        if (statbuf.st_gid != web_files_gid()) {
            error("%llu: File '%s' is owned by group %u (expected group %u). Access Denied.", w->id, webfilename, statbuf.st_gid, web_files_gid());
            return access_to_file_is_not_permitted(w, webfilename);
        }

        done = 1;
    }

    // open the file
    w->ifd = open(webfilename, O_NONBLOCK, O_RDONLY);
    if(w->ifd == -1) {
        w->ifd = w->ofd;

        if(errno == EBUSY || errno == EAGAIN) {
            error("%llu: File '%s' is busy, sending 307 Moved Temporarily to force retry.", w->id, webfilename);
            w->response.data->contenttype = CT_TEXT_HTML;
            buffer_sprintf(w->response.header, "Location: /%s\r\n", filename);
            buffer_strcat(w->response.data, "File is currently busy, please try again later: ");
            buffer_strcat_htmlescape(w->response.data, webfilename);
            return 307;
        }
        else {
            error("%llu: Cannot open file '%s'.", w->id, webfilename);
            w->response.data->contenttype = CT_TEXT_HTML;
            buffer_strcat(w->response.data, "Cannot open file: ");
            buffer_strcat_htmlescape(w->response.data, webfilename);
            return 404;
        }
    }

    sock_setnonblock(w->ifd);

    w->response.data->contenttype = contenttype_for_filename(webfilename);
    debug(D_WEB_CLIENT_ACCESS, "%llu: Sending file '%s' (%ld bytes, ifd %d, ofd %d).", w->id, webfilename, statbuf.st_size, w->ifd, w->ofd);

    w->mode = WEB_CLIENT_MODE_FILECOPY;
    web_client_enable_wait_receive(w);
    web_client_disable_wait_send(w);
    buffer_flush(w->response.data);
    buffer_need_bytes(w->response.data, (size_t)statbuf.st_size);
    w->response.rlen = (size_t)statbuf.st_size;
#ifdef __APPLE__
    w->response.data->date = statbuf.st_mtimespec.tv_sec;
#else
    w->response.data->date = statbuf.st_mtim.tv_sec;
#endif /* __APPLE__ */
    buffer_cacheable(w->response.data);

    return 200;
}


#ifdef NETDATA_WITH_ZLIB
void web_client_enable_deflate(struct web_client *w, int gzip) {
    if(unlikely(w->response.zinitialized)) {
        debug(D_DEFLATE, "%llu: Compression has already be initialized for this client.", w->id);
        return;
    }

    if(unlikely(w->response.sent)) {
        error("%llu: Cannot enable compression in the middle of a conversation.", w->id);
        return;
    }

    w->response.zstream.zalloc = Z_NULL;
    w->response.zstream.zfree = Z_NULL;
    w->response.zstream.opaque = Z_NULL;

    w->response.zstream.next_in = (Bytef *)w->response.data->buffer;
    w->response.zstream.avail_in = 0;
    w->response.zstream.total_in = 0;

    w->response.zstream.next_out = w->response.zbuffer;
    w->response.zstream.avail_out = 0;
    w->response.zstream.total_out = 0;

    w->response.zstream.zalloc = Z_NULL;
    w->response.zstream.zfree = Z_NULL;
    w->response.zstream.opaque = Z_NULL;

//  if(deflateInit(&w->response.zstream, Z_DEFAULT_COMPRESSION) != Z_OK) {
//      error("%llu: Failed to initialize zlib. Proceeding without compression.", w->id);
//      return;
//  }

    // Select GZIP compression: windowbits = 15 + 16 = 31
    if(deflateInit2(&w->response.zstream, web_gzip_level, Z_DEFLATED, 15 + ((gzip)?16:0), 8, web_gzip_strategy) != Z_OK) {
        error("%llu: Failed to initialize zlib. Proceeding without compression.", w->id);
        return;
    }

    w->response.zsent = 0;
    w->response.zoutput = 1;
    w->response.zinitialized = 1;

    debug(D_DEFLATE, "%llu: Initialized compression.", w->id);
}
#endif // NETDATA_WITH_ZLIB

void buffer_data_options2string(BUFFER *wb, uint32_t options) {
    int count = 0;

    if(options & RRDR_OPTION_NONZERO) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "nonzero");
    }

    if(options & RRDR_OPTION_REVERSED) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "flip");
    }

    if(options & RRDR_OPTION_JSON_WRAP) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "jsonwrap");
    }

    if(options & RRDR_OPTION_MIN2MAX) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "min2max");
    }

    if(options & RRDR_OPTION_MILLISECONDS) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "ms");
    }

    if(options & RRDR_OPTION_ABSOLUTE) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "absolute");
    }

    if(options & RRDR_OPTION_SECONDS) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "seconds");
    }

    if(options & RRDR_OPTION_NULL2ZERO) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "null2zero");
    }

    if(options & RRDR_OPTION_OBJECTSROWS) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "objectrows");
    }

    if(options & RRDR_OPTION_GOOGLE_JSON) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "google_json");
    }

    if(options & RRDR_OPTION_PERCENTAGE) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "percentage");
    }

    if(options & RRDR_OPTION_NOT_ALIGNED) {
        if(count++) buffer_strcat(wb, " ");
        buffer_strcat(wb, "unaligned");
    }
}

static inline int check_host_and_call(RRDHOST *host, struct web_client *w, char *url, int (*func)(RRDHOST *, struct web_client *, char *)) {
    //if(unlikely(host->rrd_memory_mode == RRD_MEMORY_MODE_NONE)) {
    //    buffer_flush(w->response.data);
    //    buffer_strcat(w->response.data, "This host does not maintain a database");
    //    return 400;
    //}

    return func(host, w, url);
}

static inline int check_host_and_dashboard_acl_and_call(RRDHOST *host, struct web_client *w, char *url, int (*func)(RRDHOST *, struct web_client *, char *)) {
    if(!web_client_can_access_dashboard(w))
        return web_client_permission_denied(w);

    return check_host_and_call(host, w, url, func);
}

static inline int check_host_and_mgmt_acl_and_call(RRDHOST *host, struct web_client *w, char *url, int (*func)(RRDHOST *, struct web_client *, char *)) {
    if(!web_client_can_access_mgmt(w))
        return web_client_permission_denied(w);

    return check_host_and_call(host, w, url, func);
}

int web_client_api_request(RRDHOST *host, struct web_client *w, char *url)
{
    // get the api version
    char *tok = mystrsep(&url, "/");
    (void)tok;
    char *body = w->version.body;
    size_t length = w->version.length;
    if(body) {
    //if(tok && *tok) {
        //debug(D_WEB_CLIENT, "%llu: Searching for API version '%s'.", w->id, tok);
        debug(D_WEB_CLIENT, "%llu: Searching for API version'.", w->id);
        //if(strcmp(tok, "v1") == 0)
        if(strncmp(body, "v1",length) == 0) {
            return web_client_api_request_v1(host, w, url);
        } else {
            char response[NETDATA_WEB_REQUEST_URL_SIZE];
            strncpy(response,w->version.body,length);
            response[length] = 0x00;
            buffer_flush(w->response.data);

            w->response.data->contenttype = CT_TEXT_HTML;
            buffer_strcat(w->response.data, "Unsupported API version: ");
            buffer_strcat_htmlescape(w->response.data, response);
            //buffer_strcat_htmlescape(w->response.data, tok);
            return 404;
        }
    }
    else {
        buffer_flush(w->response.data);
        buffer_sprintf(w->response.data, "Which API version?");
        return 400;
    }
}

const char *web_content_type_to_string(uint8_t contenttype) {
    switch(contenttype) {
        case CT_TEXT_HTML:
            return "text/html; charset=utf-8";

        case CT_APPLICATION_XML:
            return "application/xml; charset=utf-8";

        case CT_APPLICATION_JSON:
            return "application/json; charset=utf-8";

        case CT_APPLICATION_X_JAVASCRIPT:
            return "application/x-javascript; charset=utf-8";

        case CT_TEXT_CSS:
            return "text/css; charset=utf-8";

        case CT_TEXT_XML:
            return "text/xml; charset=utf-8";

        case CT_TEXT_XSL:
            return "text/xsl; charset=utf-8";

        case CT_APPLICATION_OCTET_STREAM:
            return "application/octet-stream";

        case CT_IMAGE_SVG_XML:
            return "image/svg+xml";

        case CT_APPLICATION_X_FONT_TRUETYPE:
            return "application/x-font-truetype";

        case CT_APPLICATION_X_FONT_OPENTYPE:
            return "application/x-font-opentype";

        case CT_APPLICATION_FONT_WOFF:
            return "application/font-woff";

        case CT_APPLICATION_FONT_WOFF2:
            return "application/font-woff2";

        case CT_APPLICATION_VND_MS_FONTOBJ:
            return "application/vnd.ms-fontobject";

        case CT_IMAGE_PNG:
            return "image/png";

        case CT_IMAGE_JPG:
            return "image/jpeg";

        case CT_IMAGE_GIF:
            return "image/gif";

        case CT_IMAGE_XICON:
            return "image/x-icon";

        case CT_IMAGE_BMP:
            return "image/bmp";

        case CT_IMAGE_ICNS:
            return "image/icns";

        case CT_PROMETHEUS:
            return "text/plain; version=0.0.4";

        default:
        case CT_TEXT_PLAIN:
            return "text/plain; charset=utf-8";
    }
}


const char *web_response_code_to_string(int code) {
    switch(code) {
        case 200:
            return "OK";

        case 301:
            return "Moved Permanently";

        case 307:
            return "Temporary Redirect";

        case 400:
            return "Bad Request";

        case 403:
            return "Forbidden";

        case 404:
            return "Not Found";

        case 412:
            return "Preconditions Failed";

        default:
            if(code >= 100 && code < 200)
                return "Informational";

            if(code >= 200 && code < 300)
                return "Successful";

            if(code >= 300 && code < 400)
                return "Redirection";

            if(code >= 400 && code < 500)
                return "Bad Request";

            if(code >= 500 && code < 600)
                return "Server Error";

            return "Undefined Error";
    }
}

static inline char *http_header_parse(struct web_client *w, char *s, int parse_useragent) {
    static uint32_t hash_origin = 0, hash_connection = 0, hash_donottrack = 0, hash_useragent = 0, hash_authorization = 0, hash_host = 0;
#ifdef NETDATA_WITH_ZLIB
    static uint32_t hash_accept_encoding = 0;
#endif

    if(unlikely(!hash_origin)) {
        hash_origin = simple_uhash("Origin");
        hash_connection = simple_uhash("Connection");
#ifdef NETDATA_WITH_ZLIB
        hash_accept_encoding = simple_uhash("Accept-Encoding");
#endif
        hash_donottrack = simple_uhash("DNT");
        hash_useragent = simple_uhash("User-Agent");
        hash_authorization = simple_uhash("X-Auth-Token");
        hash_host = simple_uhash("Host");
    }

    char *e = s;

    // find the :
    while(*e && *e != ':') e++;
    if(!*e) return e;

    // get the name
    *e = '\0';

    // find the value
    char *v = e + 1, *ve;

    // skip leading spaces from value
    while(*v == ' ') v++;
    ve = v;

    // find the \r
    while(*ve && *ve != '\r') ve++;
    if(!*ve || ve[1] != '\n') {
        *e = ':';
        return ve;
    }

    // terminate the value
    *ve = '\0';

    // fprintf(stderr, "HEADER: '%s' = '%s'\n", s, v);
    uint32_t hash = simple_uhash(s);

    if(hash == hash_origin && !strcasecmp(s, "Origin"))
        strncpyz(w->origin, v, NETDATA_WEB_REQUEST_ORIGIN_HEADER_SIZE);

    else if(hash == hash_connection && !strcasecmp(s, "Connection")) {
        if(strcasestr(v, "keep-alive"))
            web_client_enable_keepalive(w);
    }
    else if(respect_web_browser_do_not_track_policy && hash == hash_donottrack && !strcasecmp(s, "DNT")) {
        if(*v == '0') web_client_disable_donottrack(w);
        else if(*v == '1') web_client_enable_donottrack(w);
    }
    else if(parse_useragent && hash == hash_useragent && !strcasecmp(s, "User-Agent")) {
        w->user_agent = strdupz(v);
    } else if(hash == hash_authorization&& !strcasecmp(s, "X-Auth-Token")) {
        w->auth_bearer_token = strdupz(v);
    }
    else if(hash == hash_host && !strcasecmp(s, "Host")) {
        strncpyz(w->host, v, (ve - v));
    }
#ifdef NETDATA_WITH_ZLIB
    else if(hash == hash_accept_encoding && !strcasecmp(s, "Accept-Encoding")) {
        if(web_enable_gzip) {
            if(strcasestr(v, "gzip"))
                web_client_enable_deflate(w, 1);
            //
            // does not seem to work
            // else if(strcasestr(v, "deflate"))
            //  web_client_enable_deflate(w, 0);
        }
    }
#endif /* NETDATA_WITH_ZLIB */

    *e = ':';
    *ve = '\r';
    return ve;
}

static inline HTTP_VALIDATION web_client_is_complete(char *begin,char *end,size_t length){
    if ( begin == end){
        return HTTP_VALIDATION_INCOMPLETE;
    }

    if ( length > 3  ){
        begin = end - 4;
    }

    uint32_t counter = 0;
    do{
        if (*begin == '\r'){
            begin++;
            if ( begin == end )
            {
                break;
            }

            if (*begin == '\n')
            {
                counter++;
            }
        } else if (*begin == '\n') {
            begin++;
            counter++;
        }

        if ( counter == 2){
            break;
        }
    }
    while(begin != end);

    return (counter == 2)?HTTP_VALIDATION_OK:HTTP_VALIDATION_INCOMPLETE;
}

static inline char *web_client_parse_method(struct web_client *w,char *s) {
    if(!strncmp(s, "GET ", 4)) {
        s = &s[4];
        w->mode = WEB_CLIENT_MODE_NORMAL;
    }
    else if(!strncmp(s, "OPTIONS ", 8)) {
        s = &s[8];
        w->mode = WEB_CLIENT_MODE_OPTIONS;
    }
    else if(!strncmp(s, "STREAM ", 7)) {
#ifdef ENABLE_HTTPS
        if ((w->ssl.flags) && (netdata_use_ssl_on_stream & NETDATA_SSL_FORCE)) {
            w->header_parse_tries = 0;
            w->header_parse_last_size = 0;
            web_client_disable_wait_receive(w);
            char hostname[256];
            char *copyme = strstr(s,"hostname=");
            if (copyme) {
                copyme += 9;
                char *end = strchr(copyme,'&');
                if(end) {
                    size_t length = end - copyme;
                    memcpy(hostname,copyme,length);
                    hostname[length] = 0X00;
                } else {
                    memcpy(hostname,"not available",13);
                    hostname[13] = 0x00;
                }
            }
            else{
                memcpy(hostname,"not available",13);
                hostname[13] = 0x00;
            }
            error("The server is configured to always use encrypt connection, please enable the SSL on slave with hostname '%s'.",hostname);
            return NULL;
        }
#endif
        s = &s[7];
        w->mode = WEB_CLIENT_MODE_STREAM;
    }
    else {
        s = NULL;
    }

    return s;
}

static inline char *web_client_find_protocol(struct web_client *w,char *s) {
    s = url_find_protocol(s);

    w->protocol.body = s+1;
    char *end = strchr(s+6,'\n');
    if (end) {
        w->protocol.length = end - w->protocol.body ;
    }

    return s;
}

static inline void web_client_parse_headers(struct web_client *w,char *s) {
    while(*s) {
        // find a line feed
        while(*s && *s++ != '\r');

        // did we reach the end?
        if(unlikely(!*s)) break;

        if (*s == '\n') {
            s++;
        }

        s = http_header_parse(w, s,
                              (w->mode == WEB_CLIENT_MODE_STREAM) // parse user agent
        );

    }
}

int web_client_parse_request(struct web_client *w,char *divisor) {
    if(!divisor) {
        w->total_params = 0;
        return 0;
    }

    uint32_t i = url_parse_query_string(w->param_name,w->param_values,w->query_string.body+1,divisor);
    w->total_params = i;

    return i;
}

static inline void web_client_set_directory(struct web_client *w,char *begin,char *enddir,char *endcmd) {
    if (enddir) {
        w->directory.body = begin;
        w->directory.length = enddir - begin;

        if (!strncmp(w->directory.body,"api",w->directory.length)) {
            begin = enddir + 1;
            enddir = strchr(begin,'/');
            if(enddir) {
                w->version.body = begin;
                w->version.length = enddir - begin;

                enddir++;
                w->command.body = enddir;
                w->command.length = (size_t) (endcmd - enddir);
            }
        }
    }
    else{
        w->directory.body = begin;
        w->directory.length = w->path.length - 1;
        w->version.body = NULL;
        w->version.length = 0;
        w->command.body = NULL;
        w->command.length = 0;
    }
}

static inline void web_client_set_without_query_string(struct web_client *w) {
    w->query_string.body = NULL;
    w->query_string.length = 0;

    char *test = w->path.body+1;
    if (!strncmp(test,"api/v1/",7) ) {
        test += 7;
        if (!strncmp(test,"info",4)) {
            w->command.length = 4;
        }
        else if (!strncmp(test,"charts",6)) {
            w->command.length = 6;
        }
        else {
            test = NULL;
            w->command.length = 0;
        }
    }else{
        w->command.length = w->path.length;
    }
    w->command.body = test;
    w->total_params = 0;
}

static inline void web_client_split_path_query(struct web_client *w) {
    w->path.body = w->decoded_url;
    w->decoded_length = strlen(w->decoded_url);
    char *moveme = strchr(w->path.body,'?');
    char *enddir;
    if (moveme) {
        w->path.length = moveme - w->path.body;
        w->query_string.body = moveme;
        w->query_string.length = w->decoded_length - w->path.length;

        enddir = strchr(w->path.body+1,'/');
        char *begin = w->path.body+1;
        web_client_set_directory(w,begin,enddir,moveme);
        if (w->query_string.body) {
            enddir = strchr(moveme,'=');
            if (!web_client_parse_request(w,enddir) ) {
                moveme++;
                size_t length = strlen(moveme);
                w->param_name[0].body = moveme;
                w->param_name[0].length = length;
                w->param_values[0].body = moveme;
                w->param_values[0].length = length;

                w->total_params = 1;
            }
        }
    } else {
        w->path.length = w->decoded_length;

        enddir = strchr(w->path.body+1,'/');
        w->directory.body = w->path.body + 1;
        if(enddir) {
            w->directory.length = (size_t)(enddir - w->directory.body);
            enddir++;

            w->version.body = enddir;
            enddir = strchr(++enddir,'/');
            if(enddir) {
                w->version.length = (size_t)(enddir - w->version.body);

                enddir++;
                w->command.body = enddir;
                w->command.length = (size_t)(moveme - enddir);
            } else{
                w->version.length = strlen(w->version.body);
            }

        }else {
            w->directory.length = w->decoded_length - 1;
        }
        web_client_set_without_query_string(w);
    }
}

static inline HTTP_VALIDATION http_request_validate(struct web_client *w) {
    char *s = (char *)buffer_tostring(w->response.data), *encoded_url = NULL;
    size_t status;

    w->header_parse_tries++;
    w->header_parse_last_size = buffer_strlen(w->response.data);
    status = w->header_parse_last_size;

    // make sure we have complete request
    // complete requests contain: \r\n\r\n
    status = url_is_request_complete(s,&s[status],status);
    if (w->header_parse_tries > 10) {
        if (status == HTTP_VALIDATION_INCOMPLETE) {
            info("Disabling slow client after %zu attempts to read the request (%zu bytes received)", w->header_parse_tries, buffer_strlen(w->response.data));
            w->header_parse_tries = 0;
            w->header_parse_last_size = 0;
            web_client_disable_wait_receive(w);
            return HTTP_VALIDATION_NOT_SUPPORTED;
        }
    } else{
        if (status == HTTP_VALIDATION_INCOMPLETE) {
            web_client_enable_wait_receive(w);
            return HTTP_VALIDATION_INCOMPLETE;
        }
    }
    //Parse the method used to communicate
    s = web_client_parse_method(w,s);
    if (!s) {
        w->header_parse_tries = 0;
        w->header_parse_last_size = 0;
        web_client_disable_wait_receive(w);
        return HTTP_VALIDATION_NOT_SUPPORTED;
    }

    encoded_url = s;

    s = web_client_find_protocol(w,s);
    // incomplete requests
    if(unlikely(!*s)) {
        web_client_enable_wait_receive(w);
        return HTTP_VALIDATION_INCOMPLETE;
    }

    // we have the end of encoded_url - remember it
    char *ue = s;

    *ue = '\0';
    url_decode_r(w->decoded_url, encoded_url, NETDATA_WEB_REQUEST_URL_SIZE + 1);

    web_client_split_path_query(w);
    *ue = ' ';
    web_client_parse_headers(w,s);

    // copy the URL - we are going to overwrite parts of it
    // TODO -- ideally we we should avoid copying buffers around
    strncpyz(w->last_url, w->decoded_url, NETDATA_WEB_REQUEST_URL_SIZE);

#ifdef ENABLE_HTTPS
    if ((!web_client_check_unix(w)) && (netdata_srv_ctx) ) {
        if ((w->ssl.conn) && ((w->ssl.flags & NETDATA_SSL_NO_HANDSHAKE) && (netdata_use_ssl_on_http & NETDATA_SSL_FORCE) && (w->mode != WEB_CLIENT_MODE_STREAM)) ) {
            w->header_parse_tries = 0;
            w->header_parse_last_size = 0;
            web_client_disable_wait_receive(w);
            return HTTP_VALIDATION_REDIRECT;
        }
    }
#endif

    w->header_parse_tries = 0;
    w->header_parse_last_size = 0;
    web_client_disable_wait_receive(w);
    return HTTP_VALIDATION_OK;
}

static inline ssize_t web_client_send_data(struct web_client *w,const void *buf,size_t len, int flags)
{
    ssize_t bytes;
#ifdef ENABLE_HTTPS
    if ((!web_client_check_unix(w)) && (netdata_srv_ctx)) {
        if ((w->ssl.conn) && (!w->ssl.flags)) {
            bytes = SSL_write(w->ssl.conn,buf, len) ;
        } else {
            bytes = send(w->ofd,buf, len , flags);
        }
    } else {
        bytes = send(w->ofd,buf, len , flags);
    }
#else
    bytes = send(w->ofd, buf, len, flags);
#endif

    return bytes;
}

static inline void web_client_send_http_header(struct web_client *w) {
    if(unlikely(w->response.code != 200))
        buffer_no_cacheable(w->response.data);

    // set a proper expiration date, if not already set
    if(unlikely(!w->response.data->expires)) {
        if(w->response.data->options & WB_CONTENT_NO_CACHEABLE)
            w->response.data->expires = w->tv_ready.tv_sec + localhost->rrd_update_every;
        else
            w->response.data->expires = w->tv_ready.tv_sec + 86400;
    }

    // prepare the HTTP response header
    debug(D_WEB_CLIENT, "%llu: Generating HTTP header with response %d.", w->id, w->response.code);

    const char *content_type_string = web_content_type_to_string(w->response.data->contenttype);
    const char *code_msg = web_response_code_to_string(w->response.code);

    // prepare the last modified and expiration dates
    char date[32], edate[32];
    {
        struct tm tmbuf, *tm;

        tm = gmtime_r(&w->response.data->date, &tmbuf);
        strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S %Z", tm);

        tm = gmtime_r(&w->response.data->expires, &tmbuf);
        strftime(edate, sizeof(edate), "%a, %d %b %Y %H:%M:%S %Z", tm);
    }

    char headerbegin[8328];
    if (w->response.code == 301) {
        memcpy(headerbegin,"\r\nLocation: https://",20);
        size_t headerlength = strlen(w->host);
        memcpy(&headerbegin[20],w->host,headerlength);
        headerlength += 20;
        size_t tmp = strlen(w->last_url);
        memcpy(&headerbegin[headerlength],w->last_url,tmp);
        headerlength += tmp;
        memcpy(&headerbegin[headerlength],"\r\n",2);
        headerlength += 2;
        headerbegin[headerlength] = 0x00;
    }else {
        memcpy(headerbegin,"\r\n",2);
        headerbegin[2]=0x00;
    }

    buffer_sprintf(w->response.header_output,
            "HTTP/1.1 %d %s\r\n"
                    "Connection: %s\r\n"
                    "Server: NetData Embedded HTTP Server %s\r\n"
                    "Access-Control-Allow-Origin: %s\r\n"
                    "Access-Control-Allow-Credentials: true\r\n"
                    "Content-Type: %s\r\n"
                    "Date: %s%s"
                   , w->response.code, code_msg
                   , web_client_has_keepalive(w)?"keep-alive":"close"
                   , VERSION
                   , w->origin
                   , content_type_string
                   , date
                   , headerbegin
    );

    if(unlikely(web_x_frame_options))
        buffer_sprintf(w->response.header_output, "X-Frame-Options: %s\r\n", web_x_frame_options);

    if(w->cookie1[0] || w->cookie2[0]) {
        if(w->cookie1[0]) {
            buffer_sprintf(w->response.header_output,
                    "Set-Cookie: %s\r\n",
                    w->cookie1);
        }

        if(w->cookie2[0]) {
            buffer_sprintf(w->response.header_output,
                    "Set-Cookie: %s\r\n",
                    w->cookie2);
        }

        if(respect_web_browser_do_not_track_policy)
            buffer_sprintf(w->response.header_output,
                    "Tk: T;cookies\r\n");
    }
    else {
        if(respect_web_browser_do_not_track_policy) {
            if(web_client_has_tracking_required(w))
                buffer_sprintf(w->response.header_output,
                        "Tk: T;cookies\r\n");
            else
                buffer_sprintf(w->response.header_output,
                        "Tk: N\r\n");
        }
    }

    if(w->mode == WEB_CLIENT_MODE_OPTIONS) {
        buffer_strcat(w->response.header_output,
                "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
                        "Access-Control-Allow-Headers: accept, x-requested-with, origin, content-type, cookie, pragma, cache-control\r\n"
                        "Access-Control-Max-Age: 1209600\r\n" // 86400 * 14
        );
    }
    else {
        buffer_sprintf(w->response.header_output,
                "Cache-Control: %s\r\n"
                        "Expires: %s\r\n",
                (w->response.data->options & WB_CONTENT_NO_CACHEABLE)?"no-cache":"public",
                edate);
    }

    // copy a possibly available custom header
    if(unlikely(buffer_strlen(w->response.header)))
        buffer_strcat(w->response.header_output, buffer_tostring(w->response.header));

    // headers related to the transfer method
    if(likely(w->response.zoutput)) {
        buffer_strcat(w->response.header_output,
                "Content-Encoding: gzip\r\n"
                        "Transfer-Encoding: chunked\r\n"
        );
    }
    else {
        if(likely((w->response.data->len || w->response.rlen))) {
            // we know the content length, put it
            buffer_sprintf(w->response.header_output, "Content-Length: %zu\r\n", w->response.data->len? w->response.data->len: w->response.rlen);
        }
        else {
            // we don't know the content length, disable keep-alive
            web_client_disable_keepalive(w);
        }
    }

    // end of HTTP header
    buffer_strcat(w->response.header_output, "\r\n");

    // sent the HTTP header
    debug(D_WEB_DATA, "%llu: Sending response HTTP header of size %zu: '%s'"
          , w->id
          , buffer_strlen(w->response.header_output)
          , buffer_tostring(w->response.header_output)
    );

    web_client_crock_socket(w);

    size_t count = 0;
    ssize_t bytes;
#ifdef ENABLE_HTTPS
    if ((!web_client_check_unix(w)) && (netdata_srv_ctx)) {
           if ((w->ssl.conn) && (!w->ssl.flags)) {
                while((bytes = SSL_write(w->ssl.conn, buffer_tostring(w->response.header_output), buffer_strlen(w->response.header_output))) < 0) {
                    count++;
                    if(count > 100 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                        error("Cannot send HTTP headers to web client.");
                        break;
                    }
                }
            } else {
                while((bytes = send(w->ofd, buffer_tostring(w->response.header_output), buffer_strlen(w->response.header_output), 0)) == -1) {
                    count++;

                    if(count > 100 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                        error("Cannot send HTTP headers to web client.");
                        break;
                    }
                }
            }
    } else {
            while((bytes = send(w->ofd, buffer_tostring(w->response.header_output), buffer_strlen(w->response.header_output), 0)) == -1) {
                count++;

                if(count > 100 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                    error("Cannot send HTTP headers to web client.");
                    break;
                }
            }
    }
#else
    while((bytes = send(w->ofd, buffer_tostring(w->response.header_output), buffer_strlen(w->response.header_output), 0)) == -1) {
        count++;

        if(count > 100 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            error("Cannot send HTTP headers to web client.");
            break;
        }
    }
#endif

    if(bytes != (ssize_t) buffer_strlen(w->response.header_output)) {
        if(bytes > 0)
            w->stats_sent_bytes += bytes;

        error("HTTP headers failed to be sent (I sent %zu bytes but the system sent %zd bytes). Closing web client."
              , buffer_strlen(w->response.header_output)
              , bytes);

        WEB_CLIENT_IS_DEAD(w);
        return;
    }
    else
        w->stats_sent_bytes += bytes;
}

static inline int web_client_process_url(RRDHOST *host, struct web_client *w, char *url);

static inline int web_client_switch_host(RRDHOST *host, struct web_client *w, char *url) {
    static uint32_t hash_localhost = 0;

    if(unlikely(!hash_localhost)) {
        hash_localhost = simple_hash("localhost");
    }

    if(host != localhost) {
        buffer_flush(w->response.data);
        buffer_strcat(w->response.data, "Nesting of hosts is not allowed.");
        return 400;
    }

    char *tok = strchr(url,'/');
    if (tok) {
        w->switch_host = 1;
        debug(D_WEB_CLIENT, "%llu: Searching for host with name '%s'.", w->id, url);

        // copy the URL, we need it to serve files
        w->last_url[0] = '/';
        if(*(tok+1) != ' ') {
            strncpyz(&w->last_url[1], tok+1, NETDATA_WEB_REQUEST_URL_SIZE - 1);
            char *enddir;
            if (w->total_params) {
                enddir = strchr(tok+1,'/');
                if (enddir) {
                    char *moveme = strchr(enddir,'?');
                    if (moveme) {
                        web_client_set_directory(w,tok+1,enddir,moveme);
                    }
                }
                else {
                    w->directory.body = tok;
                    w->directory.length = strlen(tok);
                }
            } else {
                enddir = strchr(tok+1,'/');
                if(enddir) {
                    w->directory.body = tok + 1;
                    w->directory.length = enddir - tok - 1;
                } else {
                    if (!strlen(tok + 1)) {
                        w->directory.body = tok;
                        w->directory.length = 1;
                    }
                }
            }
        }
        else {
            w->last_url[1] = '\0';
        }
        *tok = 0x00;
        uint32_t hash = simple_hash(url);

        host = rrdhost_find_by_hostname(url, hash);
        if(!host) host = rrdhost_find_by_guid(url, hash);
        *tok = '/';

        if(host) return web_client_process_url(host, w, tok);
    }

    buffer_flush(w->response.data);
    w->response.data->contenttype = CT_TEXT_HTML;
    buffer_strcat(w->response.data, "This netdata does not maintain a database for host: ");
    buffer_strcat_htmlescape(w->response.data, tok?tok:"");
    return 404;
}

static inline int web_client_process_url(RRDHOST *host, struct web_client *w, char *url) {
    static uint32_t
            hash_api = 0,
            hash_netdata_conf = 0,
            hash_host = 0;

#ifdef NETDATA_INTERNAL_CHECKS
    static uint32_t hash_exit = 0, hash_debug = 0, hash_mirror = 0;
#endif

    if(unlikely(!hash_api)) {
        hash_api = simple_hash("api");
        hash_netdata_conf = simple_hash("netdata.conf");
        hash_host = simple_hash("host");
#ifdef NETDATA_INTERNAL_CHECKS
        hash_exit = simple_hash("exit");
        hash_debug = simple_hash("debug");
        hash_mirror = simple_hash("mirror");
#endif
    }

    if (w->path.length > 1) {
        char *cmp = w->directory.body;
        size_t len = w->directory.length;
        uint32_t hash = simple_nhash(cmp,len);
        debug(D_WEB_CLIENT, "%llu: Processing command '%s'.", w->id, w->command.body);

        if(unlikely(hash == hash_api && strncmp(cmp, "api",len) == 0)) {                           // current API
            debug(D_WEB_CLIENT_ACCESS, "%llu: API request ...", w->id);
            return check_host_and_call(host, w, url, web_client_api_request);
        }
        else if(unlikely(hash == hash_host && strncmp(cmp, "host",len) == 0)) {                    // host switching
            debug(D_WEB_CLIENT_ACCESS, "%llu: host switch request ...", w->id);
            return web_client_switch_host(host, w, cmp+5);
        }
        else if(unlikely(hash == hash_netdata_conf && strncmp(cmp, "netdata.conf",len) == 0)) {                           // current API
            if(unlikely(!web_client_can_access_netdataconf(w)))
                return web_client_permission_denied(w);

            debug(D_WEB_CLIENT_ACCESS, "%llu: generating netdata.conf ...", w->id);
            w->response.data->contenttype = CT_TEXT_PLAIN;
            buffer_flush(w->response.data);
            config_generate(w->response.data, 0);
            return 200;
        }
#ifdef NETDATA_INTERNAL_CHECKS
        else if(unlikely(hash == hash_exit && strncmp(cmp, "exit",len) == 0)) {
            if(unlikely(!web_client_can_access_netdataconf(w)))
                return web_client_permission_denied(w);

            w->response.data->contenttype = CT_TEXT_PLAIN;
            buffer_flush(w->response.data);

            if(!netdata_exit)
                buffer_strcat(w->response.data, "ok, will do...");
            else
                buffer_strcat(w->response.data, "I am doing it already");

            error("web request to exit received.");
            netdata_cleanup_and_exit(0);
            return 200;
        }
        else if(unlikely(hash == hash_debug && strncmp(cmp, "debug",len) == 0)) {
            if(unlikely(!web_client_can_access_netdataconf(w)))
                return web_client_permission_denied(w);

            buffer_flush(w->response.data);

            // get the name of the data to show
            char *tok = mystrsep(&url, "/?");
            if(likely(tok && *tok)) {
                tok = mystrsep(&url, "&");
                if(tok && *tok) {
                    debug(D_WEB_CLIENT, "%llu: Searching for RRD data with name '%s'.", w->id, tok);

                    // do we have such a data set?
                    RRDSET *st = rrdset_find_byname(host, tok);
                    if(!st) st = rrdset_find(host, tok);
                    if(!st) {
                        w->response.data->contenttype = CT_TEXT_HTML;
                        buffer_strcat(w->response.data, "Chart is not found: ");
                        buffer_strcat_htmlescape(w->response.data, tok);
                        debug(D_WEB_CLIENT_ACCESS, "%llu: %s is not found.", w->id, tok);
                        return 404;
                    }

                    debug_flags |= D_RRD_STATS;

                    if(rrdset_flag_check(st, RRDSET_FLAG_DEBUG))
                        rrdset_flag_clear(st, RRDSET_FLAG_DEBUG);
                    else
                        rrdset_flag_set(st, RRDSET_FLAG_DEBUG);

                    w->response.data->contenttype = CT_TEXT_HTML;
                    buffer_sprintf(w->response.data, "Chart has now debug %s: ", rrdset_flag_check(st, RRDSET_FLAG_DEBUG)?"enabled":"disabled");
                    buffer_strcat_htmlescape(w->response.data, tok);
                    debug(D_WEB_CLIENT_ACCESS, "%llu: debug for %s is %s.", w->id, tok, rrdset_flag_check(st, RRDSET_FLAG_DEBUG)?"enabled":"disabled");
                    return 200;
                }
            }

            buffer_flush(w->response.data);
            buffer_strcat(w->response.data, "debug which chart?\r\n");
            return 400;
        }
        else if(unlikely(hash == hash_mirror && strncmp(cmp, "mirror",len) == 0)) {
            if(unlikely(!web_client_can_access_netdataconf(w)))
                return web_client_permission_denied(w);

            debug(D_WEB_CLIENT_ACCESS, "%llu: Mirroring...", w->id);

            // replace the zero bytes with spaces
            buffer_char_replace(w->response.data, '\0', ' ');

            // just leave the buffer as is
            // it will be copied back to the client

            return 200;
        }
#endif  /* NETDATA_INTERNAL_CHECKS */
    }

    w->switch_host = 0;
    char *tok = mystrsep(&url, "/?");
    (void)tok;

    char filename[FILENAME_MAX+1];
    url = filename;
    strncpyz(filename, w->last_url, FILENAME_MAX);
    tok = mystrsep(&url, "?");
    buffer_flush(w->response.data);

    return mysendfile(w, (w->path.length > 1)?tok:"/");
}

void web_client_process_request(struct web_client *w) {

    // start timing us
    now_realtime_timeval(&w->tv_in);

    switch(http_request_validate(w)) {
        case HTTP_VALIDATION_OK:
            switch(w->mode) {
                case WEB_CLIENT_MODE_STREAM:
                    if(unlikely(!web_client_can_access_stream(w))) {
                        web_client_permission_denied(w);
                        return;
                    }

                    w->response.code = rrdpush_receiver_thread_spawn(localhost, w, w->decoded_url);
                    return;

                case WEB_CLIENT_MODE_OPTIONS:
                    if(unlikely(
                            !web_client_can_access_dashboard(w) &&
                            !web_client_can_access_registry(w) &&
                            !web_client_can_access_badges(w) &&
                            !web_client_can_access_mgmt(w) &&
                            !web_client_can_access_netdataconf(w)
                    )) {
                        web_client_permission_denied(w);
                        break;
                    }

                    w->response.data->contenttype = CT_TEXT_PLAIN;
                    buffer_flush(w->response.data);
                    buffer_strcat(w->response.data, "OK");
                    w->response.code = 200;
                    break;

                case WEB_CLIENT_MODE_FILECOPY:
                case WEB_CLIENT_MODE_NORMAL:
                    if(unlikely(
                            !web_client_can_access_dashboard(w) &&
                            !web_client_can_access_registry(w) &&
                            !web_client_can_access_badges(w) &&
                            !web_client_can_access_mgmt(w) &&
                            !web_client_can_access_netdataconf(w)
                    )) {
                        web_client_permission_denied(w);
                        break;
                    }

                    w->response.code = web_client_process_url(localhost, w, w->decoded_url);
                    break;
            }
            break;

        case HTTP_VALIDATION_INCOMPLETE:
            if(w->response.data->len > NETDATA_WEB_REQUEST_MAX_SIZE) {
                strcpy(w->last_url, "too big request");

                debug(D_WEB_CLIENT_ACCESS, "%llu: Received request is too big (%zu bytes).", w->id, w->response.data->len);

                buffer_flush(w->response.data);
                buffer_sprintf(w->response.data, "Received request is too big  (%zu bytes).\r\n", w->response.data->len);
                w->response.code = 400;
            }
            else {
                // wait for more data
                return;
            }
            break;
#ifdef ENABLE_HTTPS
        case HTTP_VALIDATION_REDIRECT:
        {
            buffer_flush(w->response.data);
            w->response.data->contenttype = CT_TEXT_HTML;
            buffer_strcat(w->response.data, "<!DOCTYPE html><!-- SPDX-License-Identifier: GPL-3.0-or-later --><html><body onload=\"window.location.href ='https://'+ window.location.hostname + ':' + window.location.port +  window.location.pathname\">Redirecting to safety connection, case your browser does not support redirection, please click <a onclick=\"window.location.href ='https://'+ window.location.hostname + ':' + window.location.port +  window.location.pathname\">here</a>.</body></html>");
            w->response.code = 301;
            break;
        }
#endif
        case HTTP_VALIDATION_NOT_SUPPORTED:
            debug(D_WEB_CLIENT_ACCESS, "%llu: Cannot understand '%s'.", w->id, w->response.data->buffer);

            buffer_flush(w->response.data);
            buffer_strcat(w->response.data, "I don't understand you...\r\n");
            w->response.code = 400;
            break;
    }

    // keep track of the time we done processing
    now_realtime_timeval(&w->tv_ready);

    w->response.sent = 0;

    // set a proper last modified date
    if(unlikely(!w->response.data->date))
        w->response.data->date = w->tv_ready.tv_sec;

    web_client_send_http_header(w);

    // enable sending immediately if we have data
    if(w->response.data->len) web_client_enable_wait_send(w);
    else web_client_disable_wait_send(w);

    switch(w->mode) {
        case WEB_CLIENT_MODE_STREAM:
            debug(D_WEB_CLIENT, "%llu: STREAM done.", w->id);
            break;

        case WEB_CLIENT_MODE_OPTIONS:
            debug(D_WEB_CLIENT, "%llu: Done preparing the OPTIONS response. Sending data (%zu bytes) to client.", w->id, w->response.data->len);
            break;

        case WEB_CLIENT_MODE_NORMAL:
            debug(D_WEB_CLIENT, "%llu: Done preparing the response. Sending data (%zu bytes) to client.", w->id, w->response.data->len);
            break;

        case WEB_CLIENT_MODE_FILECOPY:
            if(w->response.rlen) {
                debug(D_WEB_CLIENT, "%llu: Done preparing the response. Will be sending data file of %zu bytes to client.", w->id, w->response.rlen);
                web_client_enable_wait_receive(w);

                /*
                // utilize the kernel sendfile() for copying the file to the socket.
                // this block of code can be commented, without anything missing.
                // when it is commented, the program will copy the data using async I/O.
                {
                    long len = sendfile(w->ofd, w->ifd, NULL, w->response.data->rbytes);
                    if(len != w->response.data->rbytes)
                        error("%llu: sendfile() should copy %ld bytes, but copied %ld. Falling back to manual copy.", w->id, w->response.data->rbytes, len);
                    else
                        web_client_request_done(w);
                }
                */
            }
            else
                debug(D_WEB_CLIENT, "%llu: Done preparing the response. Will be sending an unknown amount of bytes to client.", w->id);
            break;

        default:
            fatal("%llu: Unknown client mode %u.", w->id, w->mode);
            break;
    }
}

ssize_t web_client_send_chunk_header(struct web_client *w, size_t len)
{
    debug(D_DEFLATE, "%llu: OPEN CHUNK of %zu bytes (hex: %zx).", w->id, len, len);
    char buf[24];
    ssize_t bytes;
    bytes = (ssize_t)sprintf(buf, "%zX\r\n", len);
    buf[bytes] = 0x00;

    bytes = web_client_send_data(w,buf,strlen(buf),0);
    if(bytes > 0) {
        debug(D_DEFLATE, "%llu: Sent chunk header %zd bytes.", w->id, bytes);
        w->stats_sent_bytes += bytes;
    }

    else if(bytes == 0) {
        debug(D_WEB_CLIENT, "%llu: Did not send chunk header to the client.", w->id);
        WEB_CLIENT_IS_DEAD(w);
    }
    else {
        debug(D_WEB_CLIENT, "%llu: Failed to send chunk header to client.", w->id);
        WEB_CLIENT_IS_DEAD(w);
    }

    return bytes;
}

ssize_t web_client_send_chunk_close(struct web_client *w)
{
    //debug(D_DEFLATE, "%llu: CLOSE CHUNK.", w->id);

    ssize_t bytes;
    bytes = web_client_send_data(w,"\r\n",2,0);
    if(bytes > 0) {
        debug(D_DEFLATE, "%llu: Sent chunk suffix %zd bytes.", w->id, bytes);
        w->stats_sent_bytes += bytes;
    }

    else if(bytes == 0) {
        debug(D_WEB_CLIENT, "%llu: Did not send chunk suffix to the client.", w->id);
        WEB_CLIENT_IS_DEAD(w);
    }
    else {
        debug(D_WEB_CLIENT, "%llu: Failed to send chunk suffix to client.", w->id);
        WEB_CLIENT_IS_DEAD(w);
    }

    return bytes;
}

ssize_t web_client_send_chunk_finalize(struct web_client *w)
{
    //debug(D_DEFLATE, "%llu: FINALIZE CHUNK.", w->id);

    ssize_t bytes;
    bytes = web_client_send_data(w,"\r\n0\r\n\r\n",7,0);
    if(bytes > 0) {
        debug(D_DEFLATE, "%llu: Sent chunk suffix %zd bytes.", w->id, bytes);
        w->stats_sent_bytes += bytes;
    }

    else if(bytes == 0) {
        debug(D_WEB_CLIENT, "%llu: Did not send chunk finalize suffix to the client.", w->id);
        WEB_CLIENT_IS_DEAD(w);
    }
    else {
        debug(D_WEB_CLIENT, "%llu: Failed to send chunk finalize suffix to client.", w->id);
        WEB_CLIENT_IS_DEAD(w);
    }

    return bytes;
}

#ifdef NETDATA_WITH_ZLIB
ssize_t web_client_send_deflate(struct web_client *w)
{
    ssize_t len = 0, t = 0;

    // when using compression,
    // w->response.sent is the amount of bytes passed through compression

    debug(D_DEFLATE, "%llu: web_client_send_deflate(): w->response.data->len = %zu, w->response.sent = %zu, w->response.zhave = %zu, w->response.zsent = %zu, w->response.zstream.avail_in = %u, w->response.zstream.avail_out = %u, w->response.zstream.total_in = %lu, w->response.zstream.total_out = %lu.",
        w->id, w->response.data->len, w->response.sent, w->response.zhave, w->response.zsent, w->response.zstream.avail_in, w->response.zstream.avail_out, w->response.zstream.total_in, w->response.zstream.total_out);

    if(w->response.data->len - w->response.sent == 0 && w->response.zstream.avail_in == 0 && w->response.zhave == w->response.zsent && w->response.zstream.avail_out != 0) {
        // there is nothing to send

        debug(D_WEB_CLIENT, "%llu: Out of output data.", w->id);

        // finalize the chunk
        if(w->response.sent != 0) {
            t = web_client_send_chunk_finalize(w);
            if(t < 0) return t;
        }

        if(w->mode == WEB_CLIENT_MODE_FILECOPY && web_client_has_wait_receive(w) && w->response.rlen && w->response.rlen > w->response.data->len) {
            // we have to wait, more data will come
            debug(D_WEB_CLIENT, "%llu: Waiting for more data to become available.", w->id);
            web_client_disable_wait_send(w);
            return t;
        }

        if(unlikely(!web_client_has_keepalive(w))) {
            debug(D_WEB_CLIENT, "%llu: Closing (keep-alive is not enabled). %zu bytes sent.", w->id, w->response.sent);
            WEB_CLIENT_IS_DEAD(w);
            return t;
        }

        // reset the client
        web_client_request_done(w);
        debug(D_WEB_CLIENT, "%llu: Done sending all data on socket.", w->id);
        return t;
    }

    if(w->response.zhave == w->response.zsent) {
        // compress more input data

        // close the previous open chunk
        if(w->response.sent != 0) {
            t = web_client_send_chunk_close(w);
            if(t < 0) return t;
        }

        debug(D_DEFLATE, "%llu: Compressing %zu new bytes starting from %zu (and %u left behind).", w->id, (w->response.data->len - w->response.sent), w->response.sent, w->response.zstream.avail_in);

        // give the compressor all the data not passed through the compressor yet
        if(w->response.data->len > w->response.sent) {
            w->response.zstream.next_in = (Bytef *)&w->response.data->buffer[w->response.sent - w->response.zstream.avail_in];
            w->response.zstream.avail_in += (uInt) (w->response.data->len - w->response.sent);
        }

        // reset the compressor output buffer
        w->response.zstream.next_out = w->response.zbuffer;
        w->response.zstream.avail_out = NETDATA_WEB_RESPONSE_ZLIB_CHUNK_SIZE;

        // ask for FINISH if we have all the input
        int flush = Z_SYNC_FLUSH;
        if(w->mode == WEB_CLIENT_MODE_NORMAL
            || (w->mode == WEB_CLIENT_MODE_FILECOPY && !web_client_has_wait_receive(w) && w->response.data->len == w->response.rlen)) {
            flush = Z_FINISH;
            debug(D_DEFLATE, "%llu: Requesting Z_FINISH, if possible.", w->id);
        }
        else {
            debug(D_DEFLATE, "%llu: Requesting Z_SYNC_FLUSH.", w->id);
        }

        // compress
        if(deflate(&w->response.zstream, flush) == Z_STREAM_ERROR) {
            error("%llu: Compression failed. Closing down client.", w->id);
            web_client_request_done(w);
            return(-1);
        }

        w->response.zhave = NETDATA_WEB_RESPONSE_ZLIB_CHUNK_SIZE - w->response.zstream.avail_out;
        w->response.zsent = 0;

        // keep track of the bytes passed through the compressor
        w->response.sent = w->response.data->len;

        debug(D_DEFLATE, "%llu: Compression produced %zu bytes.", w->id, w->response.zhave);

        // open a new chunk
        ssize_t t2 = web_client_send_chunk_header(w, w->response.zhave);
        if(t2 < 0) return t2;
        t += t2;
    }
    
    debug(D_WEB_CLIENT, "%llu: Sending %zu bytes of data (+%zd of chunk header).", w->id, w->response.zhave - w->response.zsent, t);

    len = web_client_send_data(w,&w->response.zbuffer[w->response.zsent], (size_t) (w->response.zhave - w->response.zsent), MSG_DONTWAIT);
    if(len > 0) {
        w->stats_sent_bytes += len;
        w->response.zsent += len;
        len += t;
        debug(D_WEB_CLIENT, "%llu: Sent %zd bytes.", w->id, len);
    }
    else if(len == 0) {
        debug(D_WEB_CLIENT, "%llu: Did not send any bytes to the client (zhave = %zu, zsent = %zu, need to send = %zu).",
            w->id, w->response.zhave, w->response.zsent, w->response.zhave - w->response.zsent);

        WEB_CLIENT_IS_DEAD(w);
    }
    else {
        debug(D_WEB_CLIENT, "%llu: Failed to send data to client.", w->id);
        WEB_CLIENT_IS_DEAD(w);
    }

    return(len);
}
#endif // NETDATA_WITH_ZLIB

ssize_t web_client_send(struct web_client *w) {
#ifdef NETDATA_WITH_ZLIB
    if(likely(w->response.zoutput)) return web_client_send_deflate(w);
#endif // NETDATA_WITH_ZLIB

    ssize_t bytes;

    if(unlikely(w->response.data->len - w->response.sent == 0)) {
        // there is nothing to send

        debug(D_WEB_CLIENT, "%llu: Out of output data.", w->id);

        // there can be two cases for this
        // A. we have done everything
        // B. we temporarily have nothing to send, waiting for the buffer to be filled by ifd

        if(w->mode == WEB_CLIENT_MODE_FILECOPY && web_client_has_wait_receive(w) && w->response.rlen && w->response.rlen > w->response.data->len) {
            // we have to wait, more data will come
            debug(D_WEB_CLIENT, "%llu: Waiting for more data to become available.", w->id);
            web_client_disable_wait_send(w);
            return 0;
        }

        if(unlikely(!web_client_has_keepalive(w))) {
            debug(D_WEB_CLIENT, "%llu: Closing (keep-alive is not enabled). %zu bytes sent.", w->id, w->response.sent);
            WEB_CLIENT_IS_DEAD(w);
            return 0;
        }

        web_client_request_done(w);
        debug(D_WEB_CLIENT, "%llu: Done sending all data on socket. Waiting for next request on the same socket.", w->id);
        return 0;
    }

    bytes = web_client_send_data(w,&w->response.data->buffer[w->response.sent], w->response.data->len - w->response.sent, MSG_DONTWAIT);
    if(likely(bytes > 0)) {
        w->stats_sent_bytes += bytes;
        w->response.sent += bytes;
        debug(D_WEB_CLIENT, "%llu: Sent %zd bytes.", w->id, bytes);
    }
    else if(likely(bytes == 0)) {
        debug(D_WEB_CLIENT, "%llu: Did not send any bytes to the client.", w->id);
        WEB_CLIENT_IS_DEAD(w);
    }
    else {
        debug(D_WEB_CLIENT, "%llu: Failed to send data to client.", w->id);
        WEB_CLIENT_IS_DEAD(w);
    }

    return(bytes);
}

ssize_t web_client_read_file(struct web_client *w)
{
    if(unlikely(w->response.rlen > w->response.data->size))
        buffer_need_bytes(w->response.data, w->response.rlen - w->response.data->size);

    if(unlikely(w->response.rlen <= w->response.data->len))
        return 0;

    ssize_t left = w->response.rlen - w->response.data->len;
    ssize_t bytes = read(w->ifd, &w->response.data->buffer[w->response.data->len], (size_t)left);
    if(likely(bytes > 0)) {
        size_t old = w->response.data->len;
        (void)old;

        w->response.data->len += bytes;
        w->response.data->buffer[w->response.data->len] = '\0';

        debug(D_WEB_CLIENT, "%llu: Read %zd bytes.", w->id, bytes);
        debug(D_WEB_DATA, "%llu: Read data: '%s'.", w->id, &w->response.data->buffer[old]);

        web_client_enable_wait_send(w);

        if(w->response.rlen && w->response.data->len >= w->response.rlen)
            web_client_disable_wait_receive(w);
    }
    else if(likely(bytes == 0)) {
        debug(D_WEB_CLIENT, "%llu: Out of input file data.", w->id);

        // if we cannot read, it means we have an error on input.
        // if however, we are copying a file from ifd to ofd, we should not return an error.
        // in this case, the error should be generated when the file has been sent to the client.

        // we are copying data from ifd to ofd
        // let it finish copying...
        web_client_disable_wait_receive(w);

        debug(D_WEB_CLIENT, "%llu: Read the whole file.", w->id);

        if(web_server_mode != WEB_SERVER_MODE_STATIC_THREADED) {
            if (w->ifd != w->ofd) close(w->ifd);
        }

        w->ifd = w->ofd;
    }
    else {
        debug(D_WEB_CLIENT, "%llu: read data failed.", w->id);
        WEB_CLIENT_IS_DEAD(w);
    }

    return(bytes);
}

ssize_t web_client_receive(struct web_client *w)
{
    if(unlikely(w->mode == WEB_CLIENT_MODE_FILECOPY))
        return web_client_read_file(w);

    ssize_t bytes;
    ssize_t left = w->response.data->size - w->response.data->len;

    // do we have any space for more data?
    buffer_need_bytes(w->response.data, NETDATA_WEB_REQUEST_RECEIVE_SIZE);

#ifdef ENABLE_HTTPS
    if ((!web_client_check_unix(w)) && (netdata_srv_ctx)) {
        if ((w->ssl.conn) && (!w->ssl.flags)) {
            bytes = SSL_read(w->ssl.conn, &w->response.data->buffer[w->response.data->len], (size_t) (left - 1));
        }else {
            bytes = recv(w->ifd, &w->response.data->buffer[w->response.data->len], (size_t) (left - 1), MSG_DONTWAIT);
        }
    }
    else{
        bytes = recv(w->ifd, &w->response.data->buffer[w->response.data->len], (size_t) (left - 1), MSG_DONTWAIT);
    }
#else
    bytes = recv(w->ifd, &w->response.data->buffer[w->response.data->len], (size_t) (left - 1), MSG_DONTWAIT);
#endif

    if(likely(bytes > 0)) {
        w->stats_received_bytes += bytes;

        size_t old = w->response.data->len;
        (void)old;

        w->response.data->len += bytes;
        w->response.data->buffer[w->response.data->len] = '\0';

        debug(D_WEB_CLIENT, "%llu: Received %zd bytes.", w->id, bytes);
        debug(D_WEB_DATA, "%llu: Received data: '%s'.", w->id, &w->response.data->buffer[old]);
    }
    else {
        debug(D_WEB_CLIENT, "%llu: receive data failed.", w->id);
        WEB_CLIENT_IS_DEAD(w);
    }

    return(bytes);
}
