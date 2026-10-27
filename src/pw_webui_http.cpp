// SPDX-License-Identifier: GPL-3.0-or-later
// The WebUI over HTTP: src/pw_webui_http.hpp. The transport follows PS5_RetroArch's src/webui_ps5.cpp (the same
// owner's, GPL-3.0-or-later): libmicrohttpd frames the requests, a request's state lives in its connection context.
#include "pw_webui_http.hpp"
#include <microhttpd.h>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr size_t kBodyLimit = 64 * 1024;
constexpr uint64_t kUploadLimit = UINT64_C(64) << 30;
// A refused request's body up to this size is read and dropped before the answer: answered first, a client still
// sending its piece (8 MiB from the page) saw a broken pipe, not the reason (tests/test_webui_http.py).
constexpr uint64_t kDrainLimit = UINT64_C(16) << 20;

MHD_Daemon *daemon_handle;
unsigned short listen_port;
std::mutex route_lock;
PwWebuiContext served;

struct Upload {
    int fd = -1;
    std::string path, part;
    uint64_t offset = 0, size = 0, expected = 0, received = 0;
};

struct Request {
    std::string body;
    size_t limit = kBodyLimit;   // PUT /api/media/cover: a picture, up to 4 MiB; POST /api/forwarders: 3 MiB
    std::unique_ptr<Upload> upload;
    unsigned error = 0;
    std::string message;
    ~Request()
    {
        if (upload && upload->fd >= 0) close(upload->fd);
    }
};

void headers(MHD_Response *response, const char *type)
{
    MHD_add_response_header(response, "Content-Type", type);
    MHD_add_response_header(response, "Cache-Control", "no-store");
    MHD_add_response_header(response, "X-Content-Type-Options", "nosniff");
    MHD_add_response_header(response, "Referrer-Policy", "no-referrer");
    MHD_add_response_header(response, "Content-Security-Policy",
                            "default-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'; "
                            "font-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; "
                            "form-action 'self'");
}

MHD_Result respond(MHD_Connection *c, unsigned status, const std::string &body, const char *type = "application/json")
{
    MHD_Response *response = MHD_create_response_from_buffer(body.size(), const_cast<char *>(body.data()),
                                                             MHD_RESPMEM_MUST_COPY);
    if (!response) return MHD_NO;
    headers(response, type);
    const MHD_Result result = MHD_queue_response(c, status, response);
    MHD_destroy_response(response);
    return result;
}

std::string quoted(const std::string &text)
{
    std::string out = "\"";
    for (unsigned char ch : text) {
        if (ch == '"' || ch == '\\') out += '\\';
        if (ch < 0x20) continue;
        out += char(ch);
    }
    return out + "\"";
}

MHD_Result refuse(MHD_Connection *c, unsigned status, const std::string &why)
{
    return respond(c, status, "{\"error\":" + quoted(why) + "}");
}

// A file is read in blocks, never handed to MHD as a descriptor: MHD sends a descriptor with sendfile, which on the
// console stops after its first 33 KB (2026-10-26: app.js, i18n.js and the font never finished and the page never
// started; PS5_RetroArch found the same on 2026-10-07). tests/test_webui_http.py refuses a descriptor response.
ssize_t read_file(void *cls, uint64_t position, char *buffer, size_t size)
{
    const ssize_t got = pread(int(intptr_t(cls)), buffer, size, off_t(position));
    return got == 0 ? MHD_CONTENT_READER_END_OF_STREAM : got < 0 ? MHD_CONTENT_READER_END_WITH_ERROR : got;
}
void close_file(void *cls) { close(int(intptr_t(cls))); }

MHD_Result respond_file(MHD_Connection *c, const std::string &path, const char *type, const std::string &download = "")
{
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    struct stat status;
    if (fd < 0 || fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
        if (fd >= 0) close(fd);
        return refuse(c, 404, "Not found.");
    }
    MHD_Response *response = MHD_create_response_from_callback(uint64_t(status.st_size), 256 * 1024, read_file,
                                                               reinterpret_cast<void *>(intptr_t(fd)), close_file);
    if (!response) {
        close(fd);
        return MHD_NO;
    }
    headers(response, type);
    if (!download.empty()) {
        std::string name = download;
        for (char &ch : name)
            if (ch == '"' || ch == '\\' || static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) > 0x7e) ch = '_';
        MHD_add_response_header(response, "Content-Disposition", ("attachment; filename=\"" + name + "\"").c_str());
    }
    const MHD_Result result = MHD_queue_response(c, 200, response);
    MHD_destroy_response(response);
    return result;
}

// The console's address as the client reached it, and the Host (and Origin) the request names: they must match.
bool local_origin(MHD_Connection *c)
{
    const char *host = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Host");
    const auto *info = MHD_get_connection_info(c, MHD_CONNECTION_INFO_CONNECTION_FD);
    if (!host || !info) return false;
    sockaddr_in address{};
    socklen_t size = sizeof address;
    char ip[INET_ADDRSTRLEN];
    if (getsockname(info->connect_fd, reinterpret_cast<sockaddr *>(&address), &size) != 0 || address.sin_family != AF_INET ||
        !inet_ntop(AF_INET, &address.sin_addr, ip, sizeof ip))
        return false;
    const std::string expected = std::string(ip) + ":" + std::to_string(listen_port);
    if (host != expected) return false;
    const char *origin = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Origin");
    return !origin || std::string(origin) == "http://" + expected;
}

MHD_Result collect_arg(void *cls, MHD_ValueKind, const char *key, const char *value)
{
    if (key) (*static_cast<std::map<std::string, std::string> *>(cls))[key] = value ? value : "";
    return MHD_YES;
}

const char *arg(MHD_Connection *c, const char *key)
{
    const char *value = MHD_lookup_connection_value(c, MHD_GET_ARGUMENT_KIND, key);
    return value ? value : "";
}

bool number(const char *text, uint64_t &out)
{
    if (!*text) return false;
    char *end = nullptr;
    errno = 0;
    out = std::strtoull(text, &end, 10);
    return errno == 0 && end && !*end;
}

// PUT /api/upload: a piece of a file at offset; the file is whole at size. The piece is written to <file>.part, which
// must already hold exactly offset bytes (a resumed upload asks GET /api/upload/status first).
void prepare_upload(MHD_Connection *c, Request &r)
{
    auto upload = std::make_unique<Upload>();
    const char *length = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Content-Length");
    std::string why;
    const std::string prefix = *arg(c, "prefix") ? arg(c, "prefix") : "mihawk";
    if (!length || !number(length, upload->expected) || !number(arg(c, "offset"), upload->offset) ||
        !number(arg(c, "size"), upload->size) || upload->size > kUploadLimit ||
        upload->offset + upload->expected > upload->size) {
        r.error = 400;
        r.message = "An upload names its offset and size and sends a Content-Length.";
        return;
    }
    {
        std::lock_guard<std::mutex> guard(route_lock);
        if (!pw_webui_upload_path(served, prefix, arg(c, "path"), &upload->path, &why)) {
            r.error = 400;
            r.message = why;
            return;
        }
    }
    upload->part = upload->path + ".part";
    upload->fd = open(upload->part.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW | (upload->offset ? 0 : O_TRUNC), 0666);
    struct stat status;   // open to every process, as the FTP server's copies are (src/pw_webui.cpp make_dir)
    if (upload->fd < 0 || fchmod(upload->fd, 0666) != 0 || fstat(upload->fd, &status) != 0) {
        r.error = 507;
        r.message = "The file cannot be written.";
        return;
    }
    if (uint64_t(status.st_size) != upload->offset) {
        r.error = 409;
        r.message = "The file on the console has " + std::to_string((long long)status.st_size) + " bytes; resume there.";
        return;
    }
    if (lseek(upload->fd, off_t(upload->offset), SEEK_SET) != off_t(upload->offset)) {   // the piece goes after the rest
        r.error = 507;
        r.message = "The file cannot be written.";
        return;
    }
    r.upload = std::move(upload);
}

// The upload's end: the part is renamed once whole; an answer either way.
MHD_Result finish_upload(MHD_Connection *c, Request &r)
{
    Upload &u = *r.upload;
    if (u.received != u.expected) return refuse(c, 400, "The piece ended early.");
    if (fsync(u.fd) != 0 || close(u.fd) != 0) {
        u.fd = -1;
        return refuse(c, 507, "The file could not be written.");
    }
    u.fd = -1;
    const uint64_t have = u.offset + u.received;
    if (have == u.size && rename(u.part.c_str(), u.path.c_str()) != 0) return refuse(c, 507, "The file could not be finished.");
    return respond(c, 200, "{\"have\":" + std::to_string(have) + ",\"done\":" + (have == u.size ? "true" : "false") + "}");
}

// ---- a folder as a ZIP (GET /api/zip): stored, each file's CRC and sizes in a data descriptor after it, so the ZIP is
// built as it goes out and holds no file in memory; the central directory last. No ZIP64: pw_webui_zip_plan keeps it
// under 65000 files and 4 GiB.
uint32_t crc_table[256];

void crc_init()
{
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

uint32_t crc_update(uint32_t crc, const unsigned char *data, size_t size)
{
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = crc_table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

struct ZipStream {
    std::vector<PwWebuiZipFile> files;
    size_t index = 0;
    int fd = -1;
    enum { header, data, descriptor, central, done } phase = header;
    std::string pending;                 // bytes made, not yet handed out
    uint64_t offset = 0;                 // bytes of the ZIP so far
    struct Entry { uint32_t crc, size, offset, dos; };
    std::vector<Entry> entries;
    uint32_t crc = 0, size = 0;
    ~ZipStream()
    {
        if (fd >= 0) close(fd);
    }
};

void put16(std::string &out, uint32_t v) { out += char(v & 0xff); out += char((v >> 8) & 0xff); }
void put32(std::string &out, uint32_t v) { put16(out, v & 0xffff); put16(out, v >> 16); }

uint32_t dos_time(long long mtime)
{
    const time_t when = time_t(mtime);
    struct tm parts{};
    gmtime_r(&when, &parts);
    if (parts.tm_year < 80) return (1u << 21) | (1u << 16);   // 1980-01-01
    return uint32_t((parts.tm_year - 80) << 25 | (parts.tm_mon + 1) << 21 | parts.tm_mday << 16 | parts.tm_hour << 11 |
                    parts.tm_min << 5 | parts.tm_sec / 2);
}

void zip_header(std::string &out, const PwWebuiZipFile &file, uint32_t dos)
{
    put32(out, 0x04034b50);
    put16(out, 20);
    put16(out, 0x0808);   // sizes in the descriptor; UTF-8 names
    put16(out, 0);
    put32(out, dos);
    put32(out, 0);
    put32(out, 0);
    put32(out, 0);
    put16(out, uint32_t(file.name.size()));
    put16(out, 0);
    out += file.name;
}

ssize_t zip_read(void *cls, uint64_t, char *buffer, size_t max)
{
    ZipStream &z = *static_cast<ZipStream *>(cls);
    size_t given = 0;
    while (given < max) {
        if (!z.pending.empty()) {
            const size_t n = std::min(max - given, z.pending.size());
            std::memcpy(buffer + given, z.pending.data(), n);
            z.pending.erase(0, n);
            given += n;
            z.offset += n;
            continue;
        }
        if (z.phase == ZipStream::done) break;
        if (z.phase == ZipStream::header) {
            if (z.index == z.files.size()) {
                z.phase = ZipStream::central;
                continue;
            }
            const PwWebuiZipFile &file = z.files[z.index];
            z.entries.push_back({0, 0, uint32_t(z.offset), dos_time(file.mtime)});
            zip_header(z.pending, file, z.entries.back().dos);
            z.fd = file.path.empty() ? -1 : open(file.path.c_str(), O_RDONLY | O_NOFOLLOW);
            z.crc = 0;
            z.size = 0;
            z.phase = ZipStream::data;
            continue;
        }
        if (z.phase == ZipStream::data) {
            const PwWebuiZipFile &file = z.files[z.index];
            ssize_t got = 0;
            if (file.path.empty()) {   // a file planned with its text
                got = ssize_t(std::min(max - given, file.text.size() - z.size));
                std::memcpy(buffer + given, file.text.data() + z.size, size_t(got));
            } else if (z.fd >= 0) {
                got = read(z.fd, buffer + given, max - given);
            }
            if (got > 0) {   // handed out at once: counted here, not through pending
                z.crc = crc_update(z.crc, reinterpret_cast<unsigned char *>(buffer + given), size_t(got));
                z.size += uint32_t(got);
                given += size_t(got);
                z.offset += uint64_t(got);
                continue;
            }
            if (z.fd >= 0) close(z.fd);   // a file that vanished or failed ends where it is: its descriptor says so
            z.fd = -1;
            z.phase = ZipStream::descriptor;
            continue;
        }
        if (z.phase == ZipStream::descriptor) {
            z.entries.back().crc = z.crc;
            z.entries.back().size = z.size;
            put32(z.pending, 0x08074b50);
            put32(z.pending, z.crc);
            put32(z.pending, z.size);
            put32(z.pending, z.size);
            ++z.index;
            z.phase = ZipStream::header;
            continue;
        }
        // the central directory and its end
        const uint64_t start = z.offset;
        std::string dir;
        for (size_t i = 0; i < z.entries.size(); ++i) {
            const auto &e = z.entries[i];
            const std::string &name = z.files[i].name;
            put32(dir, 0x02014b50);
            put16(dir, 20);
            put16(dir, 20);
            put16(dir, 0x0808);
            put16(dir, 0);
            put32(dir, e.dos);
            put32(dir, e.crc);
            put32(dir, e.size);
            put32(dir, e.size);
            put16(dir, uint32_t(name.size()));
            put16(dir, 0);
            put16(dir, 0);
            put16(dir, 0);
            put16(dir, 0);
            put32(dir, 0);
            put32(dir, e.offset);
            dir += name;
        }
        const uint32_t central_size = uint32_t(dir.size());   // the entries alone, before the end record
        put32(dir, 0x06054b50);
        put16(dir, 0);
        put16(dir, 0);
        put16(dir, uint32_t(z.entries.size()));
        put16(dir, uint32_t(z.entries.size()));
        put32(dir, central_size);
        put32(dir, uint32_t(start));
        put16(dir, 0);
        z.pending = dir;
        z.phase = ZipStream::done;
    }
    return given ? ssize_t(given) : MHD_CONTENT_READER_END_OF_STREAM;
}

void zip_free(void *cls) { delete static_cast<ZipStream *>(cls); }

// report: the bug report (pw_webui_report_plan), else the folder the query names
MHD_Result respond_zip(MHD_Connection *c, bool report)
{
    std::string why;
    auto *z = new (std::nothrow) ZipStream;
    if (!z) return MHD_NO;
    const std::string prefix = *arg(c, "prefix") ? arg(c, "prefix") : "mihawk",
                      path = report ? "mihawkos-report" : arg(c, "path");
    bool planned = true;
    {
        std::lock_guard<std::mutex> guard(route_lock);
        if (report) pw_webui_report_plan(served, &z->files);
        else planned = pw_webui_zip_plan(served, prefix, path, &z->files, &why);
    }
    if (!planned) {
        delete z;
        return refuse(c, 404, why);
    }
    MHD_Response *response = MHD_create_response_from_callback(MHD_SIZE_UNKNOWN, 256 * 1024, zip_read, z, zip_free);
    if (!response) {
        delete z;
        return MHD_NO;
    }
    headers(response, "application/zip");
    std::string name = path.substr(path.rfind('/') + 1);
    for (char &ch : name)
        if (ch == '"' || ch == '\\' || static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) > 0x7e) ch = '_';
    MHD_add_response_header(response, "Content-Disposition", ("attachment; filename=\"" + name + ".zip\"").c_str());
    const MHD_Result result = MHD_queue_response(c, 200, response);
    MHD_destroy_response(response);
    return result;
}

MHD_Result handle(void *, MHD_Connection *c, const char *url, const char *method, const char *, const char *data,
                  size_t *size, void **context)
{
    if (!*context) {
        auto *r = new (std::nothrow) Request;
        if (!r) return MHD_NO;
        *context = r;
        if (!local_origin(c)) return refuse(c, 403, "Open this page with the console's address and port 6767.");
        const char *length = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Content-Length");
        const uint64_t declared = length ? std::strtoull(length, nullptr, 10) : 0;
        const bool write = std::strcmp(method, "GET") != 0 && std::strcmp(method, "HEAD") != 0;
        const char *token = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "X-Mihawk-Token");
        if (write && (!token || served.token != token)) {
            r->error = 403;
            r->message = "Reload the page: its session is from an earlier start.";
        } else if (std::strcmp(method, "PUT") == 0 && std::strcmp(url, "/api/upload") == 0) {
            prepare_upload(c, *r);
        } else if (std::strcmp(method, "PUT") == 0 && std::strcmp(url, "/api/media/cover") == 0 && declared <= (4u << 20)) {
            r->limit = 4u << 20;
        } else if (std::strcmp(method, "POST") == 0 && std::strcmp(url, "/api/forwarders") == 0 && declared <= (3u << 20)) {
            r->limit = 3u << 20;   // a tile and its icon, in base64
        } else if (declared > kBodyLimit) {
            r->error = 413;
            r->message = "This request is too large.";
        }
        if (r->error && declared > kDrainLimit) return refuse(c, r->error, r->message);
        return MHD_YES;   // a refusal is answered once its body is read (kDrainLimit)
    }
    Request &r = *static_cast<Request *>(*context);
    if (*size) {
        if (r.error) {
            // dropped: the answer is the refusal
        } else if (r.upload) {
            Upload &u = *r.upload;
            if (*size > u.expected - u.received) {
                r.error = 413;
                r.message = "The piece is longer than it said.";
            } else if (!r.error) {
                for (size_t done = 0; done < *size;) {
                    const ssize_t wrote = write(u.fd, data + done, *size - done);
                    if (wrote <= 0) {
                        r.error = 507;
                        r.message = "The console's storage refused the file.";
                        break;
                    }
                    done += size_t(wrote);
                }
                u.received += *size;
            }
        } else if (r.body.size() + *size > r.limit) {
            r.error = 413;
            r.message = "This request is too large.";
        } else {
            r.body.append(data, *size);
        }
        *size = 0;
        return MHD_YES;
    }
    if (r.error) return refuse(c, r.error, r.message);
    if (r.upload) return finish_upload(c, r);
    if (std::strcmp(method, "GET") == 0 && std::strcmp(url, "/api/zip") == 0) return respond_zip(c, false);
    if (std::strcmp(method, "GET") == 0 && std::strcmp(url, "/api/report") == 0) return respond_zip(c, true);
    PwWebuiRequest request;
    request.method = method;
    request.path = url;
    request.body = r.body;
    const char *token = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "X-Mihawk-Token");
    request.token = token ? token : "";
    MHD_get_connection_values(c, MHD_GET_ARGUMENT_KIND, collect_arg, &request.args);
    PwWebuiResponse response;
    {
        std::lock_guard<std::mutex> guard(route_lock);
        response = pw_webui_handle(served, request);
    }
    if (!response.file.empty()) return respond_file(c, response.file, response.type.c_str(), response.download);
    return respond(c, response.status, response.body, response.type.c_str());
}

void completed(void *, MHD_Connection *, void **context, MHD_RequestTerminationCode)
{
    delete static_cast<Request *>(*context);
    *context = nullptr;
}

}  // namespace

std::string pw_webui_http_nonce()
{
    unsigned char bytes[16];
    arc4random_buf(bytes, sizeof bytes);
    std::string out;
    for (unsigned char byte : bytes) {
        char hex[3];
        std::snprintf(hex, sizeof hex, "%02x", byte);
        out += hex;
    }
    return out;
}

bool pw_webui_http_start(const PwWebuiContext &context, unsigned short port, std::string *why)
{
    if (daemon_handle) return true;
    {
        std::lock_guard<std::mutex> guard(route_lock);
        served = context;
    }
    listen_port = port;
    crc_init();
    daemon_handle = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ITC, port, nullptr, nullptr, handle, nullptr,
                                     MHD_OPTION_THREAD_POOL_SIZE, 4u, MHD_OPTION_CONNECTION_LIMIT, 32u,
                                     MHD_OPTION_CONNECTION_TIMEOUT, 30u, MHD_OPTION_CONNECTION_MEMORY_LIMIT,
                                     size_t(256 * 1024), MHD_OPTION_THREAD_STACK_SIZE, size_t(256 * 1024),
                                     MHD_OPTION_NOTIFY_COMPLETED, completed, nullptr, MHD_OPTION_END);
    if (!daemon_handle && why) *why = std::string("port ") + std::to_string(port) + " cannot be opened (" + std::strerror(errno) + ")";
    return daemon_handle != nullptr;
}

void pw_webui_http_stop()
{
    if (!daemon_handle) return;
    MHD_stop_daemon(daemon_handle);
    daemon_handle = nullptr;
}

bool pw_webui_http_running() { return daemon_handle != nullptr; }
