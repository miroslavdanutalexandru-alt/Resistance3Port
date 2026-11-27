/*
 * port_online - opt-in online groundwork. See port_online.h for what it
 * does and what is left.
 *
 * RPCN wire format follows RPCS3's rpcn_client.cpp (GPLv2, vendored under
 * third_party/rpcs3/rpcs3/Emu/NP): 15-byte little-endian header
 *   u8 type, u16 command, u32 total size (header included), u64 packet id
 * Login payload "user\0password\0token\0"; RequestTicket payload
 * "service_id\0" + u32 LE cookie size + cookie.
 */
#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    typedef SOCKET po_socket_t;
    #define PO_BAD_SOCKET INVALID_SOCKET
    #define po_close closesocket
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <errno.h>
    typedef int po_socket_t;
    #define PO_BAD_SOCKET (-1)
    #define po_close close
#endif

#include "port_online.h"
#include "sceNp.h"
#include "../../runtime/ppu/ppu_context.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "../../include/ps3emu/nid.h"
#include "../system/cellSysutil.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>

extern void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));

/* Hooks owned by the files they bend (kept as pointers so those files and
 * their own tests do not link against this one). */
extern int (*g_net_resolve_hook)(const char* name, uint32_t* ip_out);   /* sysNet.c */
extern int (*g_np_signed_in_hook)(void);                                 /* sceNp.c */
extern const char* (*g_np_online_id_hook)(void);                         /* np_psnr.c */
void np_manager_get_callback(uint32_t* cb, uint32_t* arg);               /* sceNp.c */
int  np_is_initialized(void);                                            /* sceNp.c */

#define SCE_NP_MANAGER_EVENT_GOT_TICKET 255
#define SCE_NP_AUTH_EINVALID_ARGUMENT   0x8002a015
#define SCE_NP_COOKIE_MAX_SIZE          1024
#define SCE_NP_TICKET_PARAM_MAX_ID      11

/* ---------------------------------------------------------------------------
 * Config
 * -----------------------------------------------------------------------*/

static port_online_config s_cfg;
static int s_cfg_loaded;

static char* po_trim(char* s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char* e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static int po_parse_ipv4(const char* s, uint32_t* out)
{
    unsigned a, b, c, d; char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    const uint8_t bytes[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
    memcpy(out, bytes, 4);
    return 1;
}

static void po_copy(char* dst, size_t cap, const char* src)
{
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

int port_online_parse_config(const char* text, port_online_config* cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->rpcn_port = RPCN_DEFAULT_PORT;
    if (!text) return 0;
    int accepted = 0;
    const char* p = text;
    while (*p) {
        const char* nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char line[512];
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, p, n); line[n] = '\0';
        p = nl ? nl + 1 : p + strlen(p);

        char* l = po_trim(line);
        if (!*l || *l == '#' || *l == ';' || *l == '[') continue;
        char* eq = strchr(l, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = po_trim(l);
        char* val = po_trim(eq + 1);

        if (!strcmp(key, "rpcn_host")) {
            char* colon = strrchr(val, ':');
            if (colon) {
                int port = atoi(colon + 1);
                if (port <= 0 || port > 65535) { printf("[online] config: bad rpcn_host port\n"); continue; }
                cfg->rpcn_port = (uint16_t)port;
                *colon = '\0';
            }
            po_copy(cfg->rpcn_host, sizeof(cfg->rpcn_host), val);
        } else if (!strcmp(key, "rpcn_user")) {
            po_copy(cfg->rpcn_user, sizeof(cfg->rpcn_user), val);
        } else if (!strcmp(key, "rpcn_password")) {
            po_copy(cfg->rpcn_password, sizeof(cfg->rpcn_password), val);
        } else if (!strcmp(key, "rpcn_token")) {
            po_copy(cfg->rpcn_token, sizeof(cfg->rpcn_token), val);
        } else if (!strcmp(key, "dns")) {
            if (!po_parse_ipv4(val, &cfg->dns_server)) { printf("[online] config: bad dns '%s'\n", val); continue; }
        } else if (!strncmp(key, "host.", 5) && key[5]) {
            uint32_t ip;
            if (!po_parse_ipv4(val, &ip)) { printf("[online] config: bad IP for %s\n", key); continue; }
            if (cfg->n_hosts >= PORT_ONLINE_MAX_HOSTS) { printf("[online] config: too many host.* entries\n"); continue; }
            po_copy(cfg->hosts[cfg->n_hosts].name, sizeof(cfg->hosts[0].name), key + 5);
            cfg->hosts[cfg->n_hosts].ip = ip;
            cfg->n_hosts++;
        } else {
            printf("[online] config: unknown key '%s' ignored\n", key);
            continue;
        }
        accepted++;
    }
    return accepted;
}

int port_online_enabled(void)
{
    const char* e = getenv("PORT_ONLINE");
    return e && *e && strcmp(e, "0") != 0;
}

static char* po_read_file(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 1 << 20) { fclose(f); return NULL; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

static void po_load_config(void)
{
    if (s_cfg_loaded) return;
    s_cfg_loaded = 1;
    char path[1024];
    const char* env = getenv("PORT_ONLINE_CONFIG");
    char* text = NULL;
    if (env && *env) {
        po_copy(path, sizeof(path), env);
        text = po_read_file(path);
    } else {
        po_copy(path, sizeof(path), "online.ini");
        text = po_read_file(path);
#ifdef _WIN32
        if (!text) {  /* next to the exe when started from elsewhere */
            DWORD n = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
            char* slash = n ? strrchr(path, '\\') : NULL;
            if (slash && (size_t)(slash - path) + 12 < sizeof(path)) {
                strcpy(slash + 1, "online.ini");
                text = po_read_file(path);
            }
        }
#endif
    }
    if (!text) {
        port_online_parse_config(NULL, &s_cfg);
        printf("[online] no config (%s): offline identity, system DNS\n", path);
        return;
    }
    int n = port_online_parse_config(text, &s_cfg);
    free(text);
    /* Never log the password or token. */
    printf("[online] config %s: %d key(s), rpcn %s:%u user '%s' password %s token %s, dns %s, %d host override(s)\n",
           path, n, s_cfg.rpcn_host[0] ? s_cfg.rpcn_host : "(none)", (unsigned)s_cfg.rpcn_port,
           s_cfg.rpcn_user, s_cfg.rpcn_password[0] ? "set" : "unset",
           s_cfg.rpcn_token[0] ? "set" : "unset", s_cfg.dns_server ? "custom" : "system", s_cfg.n_hosts);
}

const port_online_config* port_online_config_get(void)
{
    po_load_config();
    return &s_cfg;
}

int port_online_signed_in(void)
{
    if (!port_online_enabled()) return 0;
    po_load_config();
    return s_cfg.rpcn_user[0] && s_cfg.rpcn_password[0];
}

static const char* po_online_id(void)
{
    return port_online_signed_in() ? s_cfg.rpcn_user : NULL;
}

/* ---------------------------------------------------------------------------
 * DNS (RFC 1035 A query over UDP)
 * -----------------------------------------------------------------------*/

int port_dns_build_query(uint16_t id, const char* name, uint8_t* out, size_t cap)
{
    size_t nlen = strlen(name);
    if (!nlen || nlen > 253 || cap < 12 + nlen + 2 + 4) return -1;
    memset(out, 0, 12);
    out[0] = (uint8_t)(id >> 8); out[1] = (uint8_t)id;
    out[2] = 0x01;                 /* RD */
    out[5] = 1;                    /* QDCOUNT */
    size_t o = 12;
    const char* s = name;
    while (*s) {
        const char* dot = strchr(s, '.');
        size_t lab = dot ? (size_t)(dot - s) : strlen(s);
        if (lab == 0 || lab > 63) return -1;
        out[o++] = (uint8_t)lab;
        memcpy(out + o, s, lab); o += lab;
        s += lab;
        if (*s == '.') s++;
    }
    out[o++] = 0;
    out[o++] = 0; out[o++] = 1;    /* QTYPE A */
    out[o++] = 0; out[o++] = 1;    /* QCLASS IN */
    return (int)o;
}

/* Skip a (possibly compressed) name; returns the offset after it or 0. */
static size_t po_dns_skip_name(const uint8_t* m, size_t len, size_t o)
{
    for (int guard = 0; guard < 128 && o < len; guard++) {
        uint8_t c = m[o];
        if (c == 0) return o + 1;
        if ((c & 0xC0) == 0xC0) return o + 2 <= len ? o + 2 : 0;
        if (c & 0xC0) return 0;
        o += 1 + (size_t)c;
    }
    return 0;
}

int port_dns_parse_a(const uint8_t* m, size_t len, uint16_t id, uint32_t* ip_out)
{
    if (len < 12) return 0;
    if (((m[0] << 8) | m[1]) != id) return 0;
    if (!(m[2] & 0x80) || (m[3] & 0x0F)) return 0;  /* not a response, or RCODE != 0 */
    unsigned qd = (unsigned)(m[4] << 8 | m[5]), an = (unsigned)(m[6] << 8 | m[7]);
    size_t o = 12;
    for (unsigned i = 0; i < qd; i++) {
        o = po_dns_skip_name(m, len, o);
        if (!o || o + 4 > len) return 0;
        o += 4;
    }
    for (unsigned i = 0; i < an; i++) {
        o = po_dns_skip_name(m, len, o);
        if (!o || o + 10 > len) return 0;
        unsigned type = (unsigned)(m[o] << 8 | m[o + 1]);
        unsigned cls  = (unsigned)(m[o + 2] << 8 | m[o + 3]);
        unsigned rdl  = (unsigned)(m[o + 8] << 8 | m[o + 9]);
        o += 10;
        if (o + rdl > len) return 0;
        if (type == 1 && cls == 1 && rdl == 4) { memcpy(ip_out, m + o, 4); return 1; }
        o += rdl;  /* CNAME etc.: the A record follows in the same answer */
    }
    return 0;
}

static void po_net_startup(void)
{
#ifdef _WIN32
    static int done;
    if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = 1; }
#endif
}

static int po_dns_query(uint32_t server, uint16_t port, const char* name, uint32_t* ip_out)
{
    uint8_t q[300], r[1500];
    uint16_t id = (uint16_t)(rand() ^ (unsigned)time(NULL));
    int qlen = port_dns_build_query(id, name, q, sizeof(q));
    if (qlen < 0) return 0;
    po_net_startup();
    po_socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == PO_BAD_SOCKET) return 0;
#ifdef _WIN32
    DWORD tmo = 1500;
#else
    struct timeval tmo = { 1, 500000 };
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = server;
    int ok = 0;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        if (sendto(s, (const char*)q, qlen, 0, (struct sockaddr*)&sa, sizeof(sa)) != qlen) break;
        for (;;) {
            int n = (int)recv(s, (char*)r, sizeof(r), 0);
            if (n <= 0) break;
            if (port_dns_parse_a(r, (size_t)n, id, ip_out)) { ok = 1; break; }
        }
    }
    po_close(s);
    return ok;
}

static uint16_t s_dns_port = 53;   /* tests point this at a loopback mock */

int port_online_resolve(const char* name, uint32_t* ip_out)
{
    if (!name || !*name) return 0;
    po_load_config();
    for (int i = 0; i < s_cfg.n_hosts; i++) {
#ifdef _WIN32
        if (!_stricmp(s_cfg.hosts[i].name, name)) {
#else
        if (!strcasecmp(s_cfg.hosts[i].name, name)) {
#endif
            *ip_out = s_cfg.hosts[i].ip;
            printf("[online] %s -> host override\n", name);
            return 1;
        }
    }
    if (s_cfg.dns_server) {
        if (po_dns_query(s_cfg.dns_server, s_dns_port, name, ip_out)) {
            printf("[online] %s -> via configured dns\n", name);
            return 1;
        }
        printf("[online] %s: configured dns gave no A record, using system resolver\n", name);
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * RPCN password: RPCN stores PBKDF2-HMAC-SHA3-256(password, salt, 200000)
 * as 64 uppercase hex chars and Login carries that digest, never the plain
 * text (RPCS3 rpcn_settings_dialog.cpp derive_password). SHA3 is not in every
 * Windows CNG, so a small Keccak lives here.
 * -----------------------------------------------------------------------*/

#define PO_SHA3_RATE 136   /* SHA3-256: 1600 - 2*256 bits */

static void po_keccakf(uint64_t s[25])
{
    static const uint64_t rc[24] = {
        0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL,
        0x000000000000808bULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
        0x000000000000008aULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
        0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
        0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
        0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL };
    static const int rot[25] = { 0, 1, 62, 28, 27, 36, 44, 6, 55, 20, 3, 10, 43, 25, 39,
                                 41, 45, 15, 21, 8, 18, 2, 61, 56, 14 };
    for (int r = 0; r < 24; r++) {
        uint64_t c[5], b[25];
        for (int x = 0; x < 5; x++) c[x] = s[x] ^ s[x + 5] ^ s[x + 10] ^ s[x + 15] ^ s[x + 20];
        for (int x = 0; x < 5; x++) {
            uint64_t d = c[(x + 4) % 5] ^ (c[(x + 1) % 5] << 1 | c[(x + 1) % 5] >> 63);
            for (int y = 0; y < 25; y += 5) s[y + x] ^= d;
        }
        for (int x = 0; x < 5; x++)
            for (int y = 0; y < 5; y++) {
                int i = x + 5 * y, n = rot[i];
                b[y + 5 * ((2 * x + 3 * y) % 5)] = n ? (s[i] << n | s[i] >> (64 - n)) : s[i];
            }
        for (int y = 0; y < 25; y += 5)
            for (int x = 0; x < 5; x++) s[y + x] = b[y + x] ^ (~b[y + (x + 1) % 5] & b[y + (x + 2) % 5]);
        s[0] ^= rc[r];
    }
}

typedef struct { uint64_t s[25]; size_t n; } po_sha3;

static void po_sha3_init(po_sha3* h) { memset(h, 0, sizeof(*h)); }

static void po_sha3_update(po_sha3* h, const uint8_t* p, size_t len)
{
    while (len--) {
        h->s[h->n >> 3] ^= (uint64_t)*p++ << (8 * (h->n & 7));
        if (++h->n == PO_SHA3_RATE) { po_keccakf(h->s); h->n = 0; }
    }
}

static void po_sha3_final(po_sha3* h, uint8_t out[32])
{
    h->s[h->n >> 3] ^= (uint64_t)0x06 << (8 * (h->n & 7));
    h->s[(PO_SHA3_RATE - 1) >> 3] ^= (uint64_t)0x80 << (8 * ((PO_SHA3_RATE - 1) & 7));
    po_keccakf(h->s);
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(h->s[i >> 3] >> (8 * (i & 7)));
}

void port_online_sha3_256(const void* data, size_t len, uint8_t out[32])
{
    po_sha3 h;
    po_sha3_init(&h);
    po_sha3_update(&h, (const uint8_t*)data, len);
    po_sha3_final(&h, out);
}

/* HMAC keyed state: inner/outer hashes after absorbing the padded key. */
typedef struct { po_sha3 in, out; } po_hmac;

static void po_hmac_init(po_hmac* m, const uint8_t* key, size_t klen)
{
    uint8_t k[PO_SHA3_RATE] = { 0 }, pad[PO_SHA3_RATE];
    if (klen > PO_SHA3_RATE) port_online_sha3_256(key, klen, k);
    else memcpy(k, key, klen);
    for (int i = 0; i < PO_SHA3_RATE; i++) pad[i] = k[i] ^ 0x36;
    po_sha3_init(&m->in); po_sha3_update(&m->in, pad, sizeof(pad));
    for (int i = 0; i < PO_SHA3_RATE; i++) pad[i] = k[i] ^ 0x5c;
    po_sha3_init(&m->out); po_sha3_update(&m->out, pad, sizeof(pad));
}

static void po_hmac_run(const po_hmac* m, const uint8_t* a, size_t alen,
                        const uint8_t* b, size_t blen, uint8_t out[32])
{
    po_sha3 h = m->in;
    uint8_t inner[32];
    po_sha3_update(&h, a, alen);
    if (blen) po_sha3_update(&h, b, blen);
    po_sha3_final(&h, inner);
    h = m->out;
    po_sha3_update(&h, inner, sizeof(inner));
    po_sha3_final(&h, out);
}

/* PBKDF2-HMAC-SHA3-256, one 32-byte block. */
void port_online_pbkdf2_sha3_256(const char* pass, size_t plen, const uint8_t* salt, size_t slen,
                                 uint32_t iters, uint8_t out[32])
{
    po_hmac m;
    uint8_t u[32];
    static const uint8_t block1[4] = { 0, 0, 0, 1 };
    po_hmac_init(&m, (const uint8_t*)pass, plen);
    po_hmac_run(&m, salt, slen, block1, 4, u);
    memcpy(out, u, 32);
    for (uint32_t i = 1; i < iters; i++) {
        po_hmac_run(&m, u, 32, NULL, 0, u);
        for (int j = 0; j < 32; j++) out[j] ^= u[j];
    }
}

/* online.ini may hold the plain password or the already derived 64-hex-char
 * value (e.g. copied from RPCS3's rpcn.yml); the latter goes out unchanged. */
void port_online_rpcn_password(const char* configured, char out[65])
{
    static const char salt[] = "No matter where you go, everybody's connected.";
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(configured);
    int is_hex = n == 64;
    for (size_t i = 0; is_hex && i < n; i++) is_hex = isxdigit((unsigned char)configured[i]) != 0;
    if (is_hex) {
        for (size_t i = 0; i < 64; i++) out[i] = (char)toupper((unsigned char)configured[i]);
        out[64] = '\0';
        return;
    }
    uint8_t d[32];
    port_online_pbkdf2_sha3_256(configured, n, (const uint8_t*)salt, sizeof(salt) - 1, 200000, d);
    for (int i = 0; i < 32; i++) { out[2 * i] = hex[d[i] >> 4]; out[2 * i + 1] = hex[d[i] & 15]; }
    out[64] = '\0';
}

/* ---------------------------------------------------------------------------
 * RPCN wire format
 * -----------------------------------------------------------------------*/

static void po_le16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void po_le32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void po_le64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t po_rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int rpcn_write_header(uint8_t* out, uint8_t type, uint16_t cmd, uint32_t total, uint64_t packet_id)
{
    out[0] = type;
    po_le16(out + 1, cmd);
    po_le32(out + 3, total);
    po_le64(out + 7, packet_id);
    return RPCN_HEADER_SIZE;
}

int rpcn_parse_header(const uint8_t* in, size_t len, uint8_t* type, uint16_t* cmd,
                      uint32_t* total, uint64_t* packet_id)
{
    if (len < RPCN_HEADER_SIZE) return -1;
    *type = in[0];
    *cmd = (uint16_t)(in[1] | in[2] << 8);
    *total = po_rd32(in + 3);
    *packet_id = (uint64_t)po_rd32(in + 7) | (uint64_t)po_rd32(in + 11) << 32;
    return *total < RPCN_HEADER_SIZE ? -1 : RPCN_HEADER_SIZE;
}

static int po_put_str(uint8_t* out, size_t cap, size_t* o, const char* s)
{
    size_t n = strlen(s ? s : "") + 1;
    if (*o + n > cap) return -1;
    memcpy(out + *o, s ? s : "", n);
    *o += n;
    return 0;
}

int rpcn_build_login(uint8_t* out, size_t cap, uint64_t packet_id,
                     const char* user, const char* password, const char* token)
{
    size_t o = RPCN_HEADER_SIZE;
    if (cap < o) return -1;
    if (po_put_str(out, cap, &o, user) || po_put_str(out, cap, &o, password) ||
        po_put_str(out, cap, &o, token)) return -1;
    rpcn_write_header(out, RPCN_PKT_REQUEST, RPCN_CMD_LOGIN, (uint32_t)o, packet_id);
    return (int)o;
}

int rpcn_build_ticket_request(uint8_t* out, size_t cap, uint64_t packet_id,
                              const char* service_id, const uint8_t* cookie, uint32_t cookie_size)
{
    size_t o = RPCN_HEADER_SIZE;
    if (cap < o) return -1;
    if (po_put_str(out, cap, &o, service_id)) return -1;
    if (o + 4 + cookie_size > cap) return -1;
    po_le32(out + o, cookie_size); o += 4;
    if (cookie_size) memcpy(out + o, cookie, cookie_size);
    o += cookie_size;
    rpcn_write_header(out, RPCN_PKT_REQUEST, RPCN_CMD_REQUEST_TICKET, (uint32_t)o, packet_id);
    return (int)o;
}

int64_t rpcn_parse_server_info(const uint8_t* pkt, size_t len)
{
    uint8_t type; uint16_t cmd; uint32_t total; uint64_t id;
    if (rpcn_parse_header(pkt, len, &type, &cmd, &total, &id) < 0) return -1;
    if (type != RPCN_PKT_SERVERINFO || total != RPCN_HEADER_SIZE + 4 || len < total) return -1;
    return (int64_t)po_rd32(pkt + RPCN_HEADER_SIZE);
}

/* ---------------------------------------------------------------------------
 * RPCN replies
 * -----------------------------------------------------------------------*/

int rpcn_parse_login_reply(const uint8_t* p, size_t len, char* online_name, size_t name_cap)
{
    if (len < 1) return -1;
    int err = p[0];
    if (online_name && name_cap) online_name[0] = '\0';
    if (err) return err;   /* the server sends the rest regardless; nothing in it matters on failure */
    const uint8_t* nul = (const uint8_t*)memchr(p + 1, 0, len - 1);
    if (!nul) return -1;
    if (online_name && name_cap) po_copy(online_name, name_cap, (const char*)p + 1);
    return 0;
}

int rpcn_parse_ticket_reply(const uint8_t* p, size_t len, const uint8_t** ticket, uint32_t* ticket_len)
{
    if (len < 1) return -1;
    if (p[0]) return p[0];
    if (len < 5) return -1;
    uint32_t n = po_rd32(p + 1);
    if (n > len - 5) return -1;
    *ticket = p + 5;
    *ticket_len = n;
    return 0;
}

/* ---------------------------------------------------------------------------
 * NP ticket fields (port of RPCS3 np::ticket::parse/get_value)
 * -----------------------------------------------------------------------*/

static uint32_t po_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t po_be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* Validate one node at o (end = enclosing limit); returns its total size or 0. */
static size_t po_ticket_node(const uint8_t* t, size_t end, size_t o)
{
    if (o + 4 > end) return 0;
    uint16_t id = po_be16(t + o), n = po_be16(t + o + 2);
    if (o + 4 + n > end) return 0;
    switch (id) {
    case 0: if (n != 0) return 0; break;
    case 1: if (n != 4) return 0; break;
    case 2: case 7: if (n != 8) return 0; break;
    case 4: case 8: break;
    default:
        if ((id & 0x3000) != 0x3000) return 0;
        for (size_t s = o + 4; s < o + 4 + n; ) {
            size_t k = po_ticket_node(t, o + 4 + n, s);
            if (!k) return 0;
            s += k;
        }
    }
    return 4 + (size_t)n;
}

int np_ticket_get_param(const uint8_t* t, size_t len, int param_id, np_ticket_value* out)
{
    static const uint16_t want_id[10] = { 8, 1, 7, 7, 2, 4, 8, 4, 8, 1 };
    memset(out, 0, sizeof(*out));
    if (param_id < 0 || param_id > SCE_NP_TICKET_PARAM_MAX_ID) return -1;
    if (len < 8 || po_be32(t) != 0x21010000u || (size_t)po_be32(t + 4) + 8 != len) return -1;
    /* Exactly two blobs: 0x3000 (body) then 0x3002 (signature). */
    size_t body = po_ticket_node(t, len, 8);
    if (!body || po_be16(t + 8) != 0x3000) return -1;
    size_t sig = po_ticket_node(t, len, 8 + body);
    if (!sig || po_be16(t + 8 + body) != 0x3002 || 8 + body + sig != len) return -1;

    size_t sub[12], o = 12, end = 8 + body;
    int count = 0;
    while (o < end && count < 12) {
        sub[count++] = o;
        o += po_ticket_node(t, end, o);
    }
    if (count < 12) return -1;
    for (int i = 0; i < 10; i++)
        if (po_be16(t + sub[i]) != want_id[i]) return -1;

    if (param_id >= 10) { out->kind = 2; return 0; }   /* STATUS_DURATION, SUBJECT_DOB: 0 */
    const uint8_t* d = t + sub[param_id] + 4;
    uint16_t n = po_be16(t + sub[param_id] + 2);
    static const uint16_t want_len[10] = { 20, 0, 0, 0, 0, 0x20, 4, 4, 24, 0 };
    switch (param_id) {
    case 1: case 9: out->kind = 1; out->v = po_be32(d); return 0;
    case 2: case 3: case 4: out->kind = 2; out->v = (uint64_t)po_be32(d) << 32 | po_be32(d + 4); return 0;
    default:
        if (n != want_len[param_id]) return -1;
        out->kind = 3; out->n = n;
        memcpy(out->data, d, n);
        return 0;
    }
}

/* ---------------------------------------------------------------------------
 * RPCN client: TLS 1.2 over TCP. RPCS3 does not verify the server
 * certificate (SSL_VERIFY_NONE); Schannel's equivalent is manual validation
 * that we never perform. Windows only -- elsewhere the request fails cleanly.
 * -----------------------------------------------------------------------*/

#define PO_TIMEOUT_MS     10000
#define PO_MAX_PACKET     (1u << 20)

static po_socket_t po_tcp_connect(const char* host, uint16_t port, int timeout_ms)
{
    struct addrinfo hints, *res = NULL;
    char ps[8];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof(ps), "%u", (unsigned)port);
    po_net_startup();
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) {
        printf("[online] rpcn: cannot resolve %s\n", host);
        return PO_BAD_SOCKET;
    }
    po_socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int ok = 0;
    if (s != PO_BAD_SOCKET) {
#ifdef _WIN32
        u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
#else
        fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
#endif
        connect(s, res->ai_addr, (int)res->ai_addrlen);
        fd_set w, e; FD_ZERO(&w); FD_ZERO(&e); FD_SET(s, &w); FD_SET(s, &e);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        if (select((int)s + 1, NULL, &w, &e, &tv) > 0 && FD_ISSET(s, &w) && !FD_ISSET(s, &e)) {
            int err = 0; socklen_t el = sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
            ok = err == 0;
        }
#ifdef _WIN32
        nb = 0; ioctlsocket(s, FIONBIO, &nb);
        DWORD tmo = (DWORD)timeout_ms;
#else
        fcntl(s, F_SETFL, fcntl(s, F_GETFL) & ~O_NONBLOCK);
        struct timeval tmo = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
#endif
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo));
        if (!ok) { po_close(s); s = PO_BAD_SOCKET; }
    }
    freeaddrinfo(res);
    if (!ok) printf("[online] rpcn: TCP %s:%u not reachable\n", host, (unsigned)port);
    return s;
}

static int po_send_all(po_socket_t s, const uint8_t* d, size_t n)
{
    while (n) {
        int k = (int)send(s, (const char*)d, (int)n, 0);
        if (k <= 0) return -1;
        d += k; n -= (size_t)k;
    }
    return 0;
}

#ifdef _WIN32
#define SECURITY_WIN32
#include <security.h>
#include <schannel.h>
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "ws2_32.lib")

#define PO_TLS_BUF (64 * 1024)

typedef struct {
    po_socket_t s;
    CredHandle  cred;
    CtxtHandle  ctx;
    int         have_cred, have_ctx;
    SecPkgContext_StreamSizes sizes;
    uint8_t*    in;    size_t in_len;          /* ciphertext not yet consumed */
    uint8_t*    plain; size_t plain_len, plain_off;
} po_tls;

static int po_tls_fill(po_tls* t)
{
    if (t->in_len >= PO_TLS_BUF) return -1;
    int k = (int)recv(t->s, (char*)t->in + t->in_len, (int)(PO_TLS_BUF - t->in_len), 0);
    if (k <= 0) return -1;
    t->in_len += (size_t)k;
    return 0;
}

static void po_tls_close(po_tls* t)
{
    if (t->have_ctx) DeleteSecurityContext(&t->ctx);
    if (t->have_cred) FreeCredentialsHandle(&t->cred);
    if (t->s != PO_BAD_SOCKET) po_close(t->s);
    free(t->in); free(t->plain);
    memset(t, 0, sizeof(*t));
    t->s = PO_BAD_SOCKET;
}

static int po_tls_connect(po_tls* t, const char* host, uint16_t port)
{
    memset(t, 0, sizeof(*t));
    t->s = po_tcp_connect(host, port, PO_TIMEOUT_MS);
    if (t->s == PO_BAD_SOCKET) return -1;
    t->in = (uint8_t*)malloc(PO_TLS_BUF);
    t->plain = (uint8_t*)malloc(PO_TLS_BUF);
    if (!t->in || !t->plain) return -1;

    SCHANNEL_CRED sc;
    memset(&sc, 0, sizeof(sc));
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
    sc.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
    SECURITY_STATUS ss = AcquireCredentialsHandleA(NULL, (SEC_CHAR*)UNISP_NAME_A, SECPKG_CRED_OUTBOUND,
                                                   NULL, &sc, NULL, NULL, &t->cred, NULL);
    if (ss != SEC_E_OK) { printf("[online] rpcn: AcquireCredentialsHandle 0x%08lX\n", (unsigned long)ss); return -1; }
    t->have_cred = 1;

    const DWORD req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                      ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION;
    DWORD attrs = 0;
    int need_recv = 0;
    for (int round = 0; round < 64; round++) {
        SecBuffer ob = { 0, SECBUFFER_TOKEN, NULL };
        SecBufferDesc od = { SECBUFFER_VERSION, 1, &ob };
        SecBuffer ib[2] = { { (unsigned long)t->in_len, SECBUFFER_TOKEN, t->in }, { 0, SECBUFFER_EMPTY, NULL } };
        SecBufferDesc id = { SECBUFFER_VERSION, 2, ib };
        if (round == 0) {
            ss = InitializeSecurityContextA(&t->cred, NULL, (SEC_CHAR*)host, req, 0, 0, NULL, 0,
                                            &t->ctx, &od, &attrs, NULL);
            if (ss == SEC_I_CONTINUE_NEEDED) t->have_ctx = 1;
        } else {
            if (need_recv && po_tls_fill(t) < 0) { printf("[online] rpcn: connection closed during TLS handshake\n"); return -1; }
            ib[0].cbBuffer = (unsigned long)t->in_len;
            ss = InitializeSecurityContextA(&t->cred, &t->ctx, (SEC_CHAR*)host, req, 0, 0, &id, 0,
                                            NULL, &od, &attrs, NULL);
        }
        if (ss == SEC_E_INCOMPLETE_MESSAGE) { need_recv = 1; continue; }
        if (ob.cbBuffer && ob.pvBuffer) {
            int r = po_send_all(t->s, (const uint8_t*)ob.pvBuffer, ob.cbBuffer);
            FreeContextBuffer(ob.pvBuffer);
            if (r < 0) return -1;
        }
        if (ss != SEC_E_OK && ss != SEC_I_CONTINUE_NEEDED) {
            printf("[online] rpcn: TLS handshake failed 0x%08lX\n", (unsigned long)ss);
            return -1;
        }
        if (round > 0) {
            if (ib[1].BufferType == SECBUFFER_EXTRA && ib[1].cbBuffer) {
                memmove(t->in, t->in + t->in_len - ib[1].cbBuffer, ib[1].cbBuffer);
                t->in_len = ib[1].cbBuffer;
            } else {
                t->in_len = 0;
            }
        }
        if (ss == SEC_E_OK) {
            if (QueryContextAttributesA(&t->ctx, SECPKG_ATTR_STREAM_SIZES, &t->sizes) != SEC_E_OK) return -1;
            return 0;
        }
        need_recv = t->in_len == 0;
    }
    return -1;
}

static int po_tls_send(po_tls* t, const uint8_t* d, size_t n)
{
    const size_t hdr = t->sizes.cbHeader, trl = t->sizes.cbTrailer, max = t->sizes.cbMaximumMessage;
    uint8_t* buf = (uint8_t*)malloc(hdr + max + trl);
    if (!buf) return -1;
    int rc = 0;
    while (n && rc == 0) {
        size_t c = n < max ? n : max;
        memcpy(buf + hdr, d, c);
        SecBuffer b[4] = { { (unsigned long)hdr, SECBUFFER_STREAM_HEADER, buf },
                           { (unsigned long)c, SECBUFFER_DATA, buf + hdr },
                           { (unsigned long)trl, SECBUFFER_STREAM_TRAILER, buf + hdr + c },
                           { 0, SECBUFFER_EMPTY, NULL } };
        SecBufferDesc bd = { SECBUFFER_VERSION, 4, b };
        if (EncryptMessage(&t->ctx, 0, &bd, 0) != SEC_E_OK) rc = -1;
        else rc = po_send_all(t->s, buf, b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer);
        d += c; n -= c;
    }
    free(buf);
    return rc;
}

static int po_tls_recv(po_tls* t, uint8_t* out, size_t n)
{
    while (n) {
        if (t->plain_off < t->plain_len) {
            size_t c = t->plain_len - t->plain_off;
            if (c > n) c = n;
            memcpy(out, t->plain + t->plain_off, c);
            t->plain_off += c; out += c; n -= c;
            continue;
        }
        if (t->in_len == 0 && po_tls_fill(t) < 0) return -1;
        SecBuffer b[4] = { { (unsigned long)t->in_len, SECBUFFER_DATA, t->in },
                           { 0, SECBUFFER_EMPTY, NULL }, { 0, SECBUFFER_EMPTY, NULL }, { 0, SECBUFFER_EMPTY, NULL } };
        SecBufferDesc bd = { SECBUFFER_VERSION, 4, b };
        SECURITY_STATUS ss = DecryptMessage(&t->ctx, &bd, 0, NULL);
        if (ss == SEC_E_INCOMPLETE_MESSAGE) { if (po_tls_fill(t) < 0) return -1; continue; }
        if (ss != SEC_E_OK) {
            printf("[online] rpcn: TLS receive ended 0x%08lX\n", (unsigned long)ss);
            return -1;
        }
        t->plain_len = t->plain_off = 0;
        SecBuffer* extra = NULL;
        for (int i = 1; i < 4; i++) {
            if (b[i].BufferType == SECBUFFER_DATA && b[i].cbBuffer <= PO_TLS_BUF) {
                memcpy(t->plain, b[i].pvBuffer, b[i].cbBuffer);
                t->plain_len = b[i].cbBuffer;
            } else if (b[i].BufferType == SECBUFFER_EXTRA) {
                extra = &b[i];
            }
        }
        if (extra) {
            memmove(t->in, t->in + t->in_len - extra->cbBuffer, extra->cbBuffer);
            t->in_len = extra->cbBuffer;
        } else {
            t->in_len = 0;
        }
    }
    return 0;
}

/* Read one packet; the payload (after the header) is malloc'd into *payload. */
static int po_rpcn_read(po_tls* t, uint8_t* type, uint16_t* cmd, uint64_t* id, uint8_t** payload, uint32_t* plen)
{
    uint8_t h[RPCN_HEADER_SIZE];
    uint32_t total;
    if (po_tls_recv(t, h, sizeof(h)) < 0) return -1;
    if (rpcn_parse_header(h, sizeof(h), type, cmd, &total, id) < 0 || total > PO_MAX_PACKET) return -1;
    *plen = total - RPCN_HEADER_SIZE;
    *payload = (uint8_t*)malloc(*plen ? *plen : 1);
    if (!*payload) return -1;
    if (*plen && po_tls_recv(t, *payload, *plen) < 0) { free(*payload); *payload = NULL; return -1; }
    return 0;
}

/* Wait for the reply to (cmd, id), skipping notifications. */
static int po_rpcn_reply(po_tls* t, uint16_t want_cmd, uint64_t want_id, uint8_t** payload, uint32_t* plen)
{
    for (int i = 0; i < 256; i++) {
        uint8_t type; uint16_t cmd; uint64_t id;
        if (po_rpcn_read(t, &type, &cmd, &id, payload, plen) < 0) return -1;
        if (type == RPCN_PKT_REPLY && cmd == want_cmd && id == want_id) return 0;
        free(*payload); *payload = NULL;
    }
    return -1;
}

/* Connect, check the protocol version; with service_id also log in and fetch
 * a ticket into out/out_len (cap bytes). Returns 0 on success. */
static int po_rpcn_session(const char* service_id, const uint8_t* cookie, uint32_t cookie_size,
                           uint8_t* out, uint32_t cap, uint32_t* out_len)
{
    po_tls t;
    int rc = -1;
    uint8_t* p = NULL; uint32_t pl = 0;
    uint8_t type; uint16_t cmd; uint64_t id;
    if (po_tls_connect(&t, s_cfg.rpcn_host, s_cfg.rpcn_port) < 0) goto done;
    if (po_rpcn_read(&t, &type, &cmd, &id, &p, &pl) < 0 || type != RPCN_PKT_SERVERINFO || pl != 4) {
        printf("[online] rpcn: no ServerInfo from %s\n", s_cfg.rpcn_host);
        goto done;
    }
    uint32_t ver = po_rd32(p);
    free(p); p = NULL;
    if (ver != RPCN_PROTOCOL_VERSION) {
        printf("[online] rpcn: server protocol %u, client speaks %u\n", ver, RPCN_PROTOCOL_VERSION);
        goto done;
    }
    printf("[online] rpcn: %s:%u TLS ok, protocol %u\n", s_cfg.rpcn_host, (unsigned)s_cfg.rpcn_port, ver);
    if (!service_id) { rc = 0; goto done; }

    uint8_t buf[RPCN_HEADER_SIZE + 4 + 64 + SCE_NP_COOKIE_MAX_SIZE + 256];
    static char s_derived[65];
    if (!s_derived[0]) port_online_rpcn_password(s_cfg.rpcn_password, s_derived);
    int n = rpcn_build_login(buf, sizeof(buf), 1, s_cfg.rpcn_user, s_derived, s_cfg.rpcn_token);
    if (n < 0 || po_tls_send(&t, buf, (size_t)n) < 0 || po_rpcn_reply(&t, RPCN_CMD_LOGIN, 1, &p, &pl) < 0) {
        printf("[online] rpcn: login exchange failed\n");
        goto done;
    }
    char name[64];
    int err = rpcn_parse_login_reply(p, pl, name, sizeof(name));
    free(p); p = NULL;
    if (err) {
        printf("[online] rpcn: login refused (error %d: check rpcn_user/password/token)\n", err);
        goto done;
    }
    printf("[online] rpcn: logged in as %s\n", name);

    n = rpcn_build_ticket_request(buf, sizeof(buf), 2, service_id, cookie, cookie_size);
    if (n < 0 || po_tls_send(&t, buf, (size_t)n) < 0 || po_rpcn_reply(&t, RPCN_CMD_REQUEST_TICKET, 2, &p, &pl) < 0) {
        printf("[online] rpcn: ticket exchange failed\n");
        goto done;
    }
    const uint8_t* tk; uint32_t tl;
    err = rpcn_parse_ticket_reply(p, pl, &tk, &tl);
    if (err || tl > cap) {
        printf("[online] rpcn: ticket refused (error %d, %u bytes)\n", err, err ? 0u : tl);
        goto done;
    }
    memcpy(out, tk, tl);
    *out_len = tl;
    printf("[online] rpcn: ticket for '%s', %u bytes\n", service_id, tl);
    rc = 0;
    /* Polite close, as RPCS3 does on shutdown; the reply is not awaited. */
    n = 0;
    rpcn_write_header(buf, RPCN_PKT_REQUEST, RPCN_CMD_TERMINATE, RPCN_HEADER_SIZE, 3);
    po_tls_send(&t, buf, RPCN_HEADER_SIZE);
done:
    free(p);
    po_tls_close(&t);
    return rc;
}
#else
static int po_rpcn_session(const char* service_id, const uint8_t* cookie, uint32_t cookie_size,
                           uint8_t* out, uint32_t cap, uint32_t* out_len)
{
    (void)service_id; (void)cookie; (void)cookie_size; (void)out; (void)cap; (void)out_len;
    printf("[online] rpcn: TLS transport is Windows-only (Schannel)\n");
    return -1;
}
#endif

/* PORT_ONLINE_PROBE: TLS connect + ServerInfo only. No login, no payload. */
static void po_probe(void)
{
    if (!s_cfg.rpcn_host[0]) { printf("[online] probe: no rpcn_host configured\n"); return; }
    int ok = po_rpcn_session(NULL, NULL, 0, NULL, 0, NULL) == 0;
    printf("[online] probe: %s:%u %s\n", s_cfg.rpcn_host, (unsigned)s_cfg.rpcn_port,
           ok ? "OK" : "FAILED");
}

/* ---------------------------------------------------------------------------
 * sceNp manager exports (registered only under PORT_ONLINE)
 * -----------------------------------------------------------------------*/

#define R32(n)  ((uint32_t)ctx->gpr[n])
#define S32(n)  ((int32_t)(uint32_t)ctx->gpr[n])
#define RET(v)  (ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)(uint32_t)(v))

#define PO_TICKET_CAP 4096
static uint8_t  s_ticket[PO_TICKET_CAP];
static uint32_t s_ticket_len;
static volatile long s_ticket_busy;

typedef struct {
    char     service[64];
    uint8_t  cookie[SCE_NP_COOKIE_MAX_SIZE];
    uint32_t cookie_size;
} po_ticket_job;

/* Test seam: replaces the network session (tests never touch the network). */
static int (*s_session_fn)(const char*, const uint8_t*, uint32_t, uint8_t*, uint32_t, uint32_t*) = po_rpcn_session;

static void po_ticket_worker(po_ticket_job* job)
{
    static uint8_t tmp[PO_TICKET_CAP];
    uint32_t len = 0;
    int32_t result;
    if (!s_cfg.rpcn_host[0]) {
        printf("[online] RequestTicket('%s'): no rpcn_host in online.ini\n", job->service);
        result = (int32_t)SCE_NP_ERROR_OFFLINE;
    } else if (s_session_fn(job->service, job->cookie, job->cookie_size, tmp, sizeof(tmp), &len) == 0) {
        memcpy(s_ticket, tmp, len);
        s_ticket_len = len;
        result = (int32_t)len;
    } else {
        result = (int32_t)SCE_NP_ERROR_OFFLINE;
    }
    free(job);
    /* not busy before the event is queued: a GOT_TICKET callback that re-requests must start a new worker */
#ifdef _WIN32
    InterlockedExchange(&s_ticket_busy, 0);
#else
    __atomic_store_n(&s_ticket_busy, 0, __ATOMIC_SEQ_CST);
#endif
    uint32_t cb = 0, arg = 0;
    np_manager_get_callback(&cb, &arg);
    if (cb) {
        const uint64_t args[8] = { SCE_NP_MANAGER_EVENT_GOT_TICKET, (uint32_t)result, arg, 0, 0, 0, 0, 0 };
        cellSysutilQueueGuestCallbackArgs(cb, args);
    }
}

#ifdef _WIN32
static DWORD WINAPI po_ticket_thread(LPVOID p) { po_ticket_worker((po_ticket_job*)p); return 0; }
#else
#include <pthread.h>
static void* po_ticket_thread(void* p) { po_ticket_worker((po_ticket_job*)p); return NULL; }
#endif
static int s_ticket_sync;   /* tests: run the worker inline */

/* Like the console, the request returns at once and the result arrives as a
 * GOT_TICKET manager event (result = ticket size, or a negative error). */
static int32_t po_request_ticket(uint32_t service_ea, uint32_t cookie_ea, uint32_t cookie_size)
{
    if (!np_is_initialized()) return (int32_t)SCE_NP_ERROR_NOT_INITIALIZED;
    if (!service_ea || cookie_size > SCE_NP_COOKIE_MAX_SIZE) return (int32_t)SCE_NP_AUTH_EINVALID_ARGUMENT;
    if (!port_online_signed_in()) return (int32_t)SCE_NP_ERROR_OFFLINE;
#ifdef _WIN32
    if (InterlockedCompareExchange(&s_ticket_busy, 1, 0) != 0) return CELL_OK;   /* in flight: one event */
#else
    if (__sync_val_compare_and_swap(&s_ticket_busy, 0, 1) != 0) return CELL_OK;
#endif
    po_ticket_job* job = (po_ticket_job*)calloc(1, sizeof(*job));
    if (!job) { s_ticket_busy = 0; return (int32_t)SCE_NP_ERROR_OFFLINE; }
    po_copy(job->service, sizeof(job->service), (const char*)vm_ptr8(service_ea));
    if (cookie_ea && cookie_size) {
        memcpy(job->cookie, vm_ptr8(cookie_ea), cookie_size);
        job->cookie_size = cookie_size;
    }
    s_ticket_len = 0;
    printf("[online] RequestTicket(service '%s', cookie %u bytes)\n", job->service, job->cookie_size);
    if (s_ticket_sync) { po_ticket_worker(job); return CELL_OK; }
#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 0, po_ticket_thread, job, 0, NULL);
    if (th) CloseHandle(th); else po_ticket_worker(job);
#else
    pthread_t th;
    if (pthread_create(&th, NULL, po_ticket_thread, job) == 0) pthread_detach(th); else po_ticket_worker(job);
#endif
    return CELL_OK;
}

/* sceNpManagerRequestTicket(npId, serviceId, cookie, cookieSize, entitlementId, consumedCount) */
static void h_request_ticket(ppu_context* ctx)  { RET(po_request_ticket(R32(4), R32(5), R32(6))); }
/* sceNpManagerRequestTicket2(npId, version, serviceId, cookie, cookieSize, entitlementId, consumedCount) */
static void h_request_ticket2(ppu_context* ctx) { RET(po_request_ticket(R32(5), R32(6), R32(7))); }

/* sceNpManagerGetTicket(buffer, bufferSize*) */
static void h_get_ticket(ppu_context* ctx)
{
    uint32_t buf = R32(3), size_ea = R32(4);
    if (!np_is_initialized()) { RET(SCE_NP_ERROR_NOT_INITIALIZED); return; }
    if (!size_ea) { RET(SCE_NP_ERROR_INVALID_ARGUMENT); return; }
    uint32_t n = s_ticket_len;
    if (buf) {
        uint32_t cap = vm_read32(size_ea);
        if (n > cap) n = cap;
        if (n) memcpy(vm_ptr8(buf), s_ticket, n);
    }
    vm_write32(size_ea, n);
    RET(CELL_OK);
}

/* sceNpManagerGetTicketParam(paramId, SceNpTicketParam*) -- a 256-byte union. */
static void h_get_ticket_param(ppu_context* ctx)
{
    if (!np_is_initialized()) { RET(SCE_NP_ERROR_NOT_INITIALIZED); return; }
    if (!R32(4) || S32(3) < 0 || S32(3) > SCE_NP_TICKET_PARAM_MAX_ID) { RET(SCE_NP_ERROR_INVALID_ARGUMENT); return; }
    np_ticket_value v;
    if (!s_ticket_len || np_ticket_get_param(s_ticket, s_ticket_len, S32(3), &v) < 0) {
        RET(SCE_NP_ERROR_INVALID_STATE);
        return;
    }
    if (v.kind == 1) vm_write32(R32(4), (uint32_t)v.v);
    else if (v.kind == 2) vm_write64(R32(4), v.v);
    else memcpy(vm_ptr8(R32(4)), v.data, v.n);
    RET(CELL_OK);
}

/* sceNpManagerGetNetworkTime(CellRtcTick*): microseconds since 0001-01-01. */
static void h_get_network_time(ppu_context* ctx)
{
    if (!np_is_initialized()) { RET(SCE_NP_ERROR_NOT_INITIALIZED); return; }
    if (!R32(3)) { RET(SCE_NP_ERROR_INVALID_ARGUMENT); return; }
    if (!port_online_signed_in()) { RET(SCE_NP_ERROR_OFFLINE); return; }
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    const uint64_t epoch_0001 = 62135596800ull;
    uint64_t tick = ((uint64_t)ts.tv_sec + epoch_0001) * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
    vm_write64(R32(3), tick);
    RET(CELL_OK);
}

/* Sub sign-in is the split-screen guest's PSN account (R3 online co-op).
 * Accepted without a second account, as RPCS3 does. */
static void h_ok(ppu_context* ctx) { RET(np_is_initialized() ? CELL_OK : SCE_NP_ERROR_NOT_INITIALIZED); }

void port_online_register(void)
{
    if (!port_online_enabled()) return;
    po_load_config();
#ifdef _WIN32
    if (!getenv("PS3_NET_ONLINE")) _putenv("PS3_NET_ONLINE=1");
#else
    if (!getenv("PS3_NET_ONLINE")) setenv("PS3_NET_ONLINE", "1", 0);
#endif
    g_net_resolve_hook = port_online_resolve;
    g_np_signed_in_hook = port_online_signed_in;
    g_np_online_id_hook = po_online_id;

    static const struct { const char* name; void (*fn)(ppu_context*); } tab[] = {
        { "sceNpManagerRequestTicket",    h_request_ticket },
        { "sceNpManagerRequestTicket2",   h_request_ticket2 },
        { "sceNpManagerGetTicket",        h_get_ticket },
        { "sceNpManagerGetTicketParam",   h_get_ticket_param },
        { "sceNpManagerGetNetworkTime",   h_get_network_time },
        { "sceNpManagerSubSignin",        h_ok },
        { "sceNpManagerSubSigninAbortGui", h_ok },
        { "sceNpManagerSubSignout",       h_ok },
    };
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        ps3_hle_register_ctx(ps3_compute_nid(tab[i].name), tab[i].name, tab[i].fn);
    printf("[online] PORT_ONLINE: %s, %u sceNp exports\n",
           port_online_signed_in() ? "signed in (RPCN credentials present)" : "online, not signed in (no rpcn_user/password)",
           (unsigned)(sizeof(tab) / sizeof(tab[0])));
    if (getenv("PORT_ONLINE_PROBE")) po_probe();
}
