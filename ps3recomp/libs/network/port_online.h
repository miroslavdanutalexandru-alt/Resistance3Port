/*
 * port_online - opt-in online groundwork (PORT_ONLINE=1).
 *
 * Config (online.ini, or PORT_ONLINE_CONFIG=path), never compiled in:
 *   rpcn_host=np.rpcs3.net[:31313]   rpcn_user=  rpcn_password=  rpcn_token=
 *   dns=IP                            DNS server for the game's lookups
 *   host.NAME=IP                      static override, wins over dns=
 * PORT_ONLINE_PROBE=1: at startup, connect to rpcn_host over TLS and read the
 * server's protocol version (no login, no account traffic). Logged.
 *
 * Pieces here: config, hostname redirection for sys_net gethostbyname, the
 * RPCN client (Schannel TLS on Windows: ServerInfo, Login, RequestTicket),
 * and the sceNp manager ticket/time/sub-signin exports.
 */
#ifndef PORT_ONLINE_H
#define PORT_ONLINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_ONLINE_MAX_HOSTS 64
#define RPCN_DEFAULT_PORT     31313
#define RPCN_HEADER_SIZE      15
#define RPCN_PROTOCOL_VERSION 34
#define RPCN_CMD_LOGIN          0
#define RPCN_CMD_TERMINATE      1
#define RPCN_CMD_REQUEST_TICKET 28
#define RPCN_PKT_REQUEST     0
#define RPCN_PKT_REPLY       1
#define RPCN_PKT_NOTIFICATION 2
#define RPCN_PKT_SERVERINFO  3

typedef struct {
    char     rpcn_host[128];
    uint16_t rpcn_port;
    char     rpcn_user[17];      /* SCE online id: 3-16 chars */
    char     rpcn_password[128];
    char     rpcn_token[64];
    uint32_t dns_server;         /* network order, 0 = system resolver */
    int      n_hosts;
    struct { char name[128]; uint32_t ip; } hosts[PORT_ONLINE_MAX_HOSTS];  /* ip: network order */
} port_online_config;

/* Parse config text into cfg (cleared first). Returns the number of keys
 * accepted; unknown keys are logged and ignored. */
int  port_online_parse_config(const char* text, port_online_config* cfg);
int  port_online_enabled(void);                 /* PORT_ONLINE set and not "0" */
const port_online_config* port_online_config_get(void);
int  port_online_signed_in(void);               /* enabled + rpcn user and password */

/* Hostname -> IPv4 (network order). Static host.* entries first, then the
 * dns= server. Returns 1 when redirected, 0 to fall back to the system. */
int  port_online_resolve(const char* name, uint32_t* ip_out);

/* DNS wire format (pure, tested). */
int  port_dns_build_query(uint16_t id, const char* name, uint8_t* out, size_t cap);
int  port_dns_parse_a(const uint8_t* msg, size_t len, uint16_t id, uint32_t* ip_out);

/* RPCN wire format (pure, tested). Return bytes written, or -1 if cap is short. */
int  rpcn_write_header(uint8_t* out, uint8_t type, uint16_t cmd, uint32_t total, uint64_t packet_id);
int  rpcn_parse_header(const uint8_t* in, size_t len, uint8_t* type, uint16_t* cmd,
                       uint32_t* total, uint64_t* packet_id);
int  rpcn_build_login(uint8_t* out, size_t cap, uint64_t packet_id,
                      const char* user, const char* password, const char* token);
int  rpcn_build_ticket_request(uint8_t* out, size_t cap, uint64_t packet_id,
                               const char* service_id, const uint8_t* cookie, uint32_t cookie_size);
/* ServerInfo payload -> protocol version (u32 LE). -1 when malformed. */
int64_t rpcn_parse_server_info(const uint8_t* pkt, size_t len);
/* Reply payloads (after the 15-byte header). Return the RPCN ErrorType byte
 * (0 = NoError), or -1 when malformed. */
int  rpcn_parse_login_reply(const uint8_t* p, size_t len, char* online_name, size_t name_cap);
int  rpcn_parse_ticket_reply(const uint8_t* p, size_t len, const uint8_t** ticket, uint32_t* ticket_len);

/* RPCN Login sends PBKDF2-HMAC-SHA3-256(password, RPCS3 salt, 200000) as 64
 * uppercase hex chars; a configured 64-hex-char value is taken as already
 * derived. out gets 64 chars + NUL. */
void port_online_rpcn_password(const char* configured, char out[65]);
void port_online_sha3_256(const void* data, size_t len, uint8_t out[32]);
void port_online_pbkdf2_sha3_256(const char* pass, size_t plen, const uint8_t* salt, size_t slen,
                                 uint32_t iters, uint8_t out[32]);

/* NP ticket (version 0x21010000, as RPCN issues; RPCS3 np::ticket layout).
 * kind: 1 = u32 in v, 2 = u64 in v, 3 = n bytes in data. Returns 0, or -1
 * when the ticket is malformed or the field is missing or the wrong size. */
typedef struct { int kind; uint64_t v; uint8_t data[256]; uint32_t n; } np_ticket_value;
int  np_ticket_get_param(const uint8_t* t, size_t len, int param_id, np_ticket_value* out);

/* Startup: PORT_ONLINE=1 loads the config, sets PS3_NET_ONLINE so the
 * socket layer and cellNetCtl go online, installs the hooks and registers
 * the sceNp ticket exports. No-op without PORT_ONLINE. */
void port_online_register(void);

#ifdef __cplusplus
}
#endif
#endif
