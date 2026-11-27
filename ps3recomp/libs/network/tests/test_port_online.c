/*
 * test_port_online - PORT_ONLINE groundwork: online.ini parsing, hostname
 * redirection (static entries and a loopback mock DNS server), the RPCN wire
 * format, and the sceNp manager ticket exports. No traffic leaves the machine.
 *
 *   clang-cl /I include /Fe:test_port_online.exe libs/network/tests/test_port_online.c ws2_32.lib
 *   cc -std=gnu17 -I include -o test_port_online libs/network/tests/test_port_online.c -lpthread
 */
#include "../port_online.c"
#include <assert.h>

uint8_t* vm_base;
int g_resv_store_active = 0;
uint32_t g_ww_lo = 0, g_ww_hi = 0;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
int  spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t addr) { (void)addr; }
void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width) { (void)addr; (void)val; (void)width; }

/* The hooks and sceNp pieces port_online.c reaches. */
int (*g_net_resolve_hook)(const char*, uint32_t*);
int (*g_np_signed_in_hook)(void);
const char* (*g_np_online_id_hook)(void);
static int s_np_init = 1;
int  np_is_initialized(void) { return s_np_init; }
void np_manager_get_callback(uint32_t* cb, uint32_t* arg) { *cb = 0x10000; *arg = 0x20000; }
static int s_cb_calls; static uint64_t s_cb_args[8]; static uint32_t s_cb_opd;
s32 cellSysutilQueueGuestCallbackArgs(u32 opd, const u64 args[8])
{
    s_cb_calls++; s_cb_opd = opd; memcpy(s_cb_args, args, sizeof(s_cb_args));
    return 0;
}

static struct { uint32_t nid; void (*fn)(ppu_context*); } s_reg[32];
static int s_nreg;
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{
    (void)name;
    s_reg[s_nreg].nid = nid; s_reg[s_nreg].fn = fn; s_nreg++;
}
static int32_t call(const char* name, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    uint32_t nid = ps3_compute_nid(name);
    ppu_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[3] = a3; ctx.gpr[4] = a4; ctx.gpr[5] = a5; ctx.gpr[6] = a6; ctx.gpr[7] = a7;
    for (int i = 0; i < s_nreg; i++)
        if (s_reg[i].nid == nid) { s_reg[i].fn(&ctx); return (int32_t)ctx.gpr[3]; }
    fprintf(stderr, "no handler for %s\n", name);
    assert(0);
    return 0;
}

static void set_env(const char* kv)
{
#ifdef _WIN32
    _putenv(kv);
#else
    putenv((char*)kv);
#endif
}

/* Mock DNS: answers one query for "game.example" with a CNAME then an A
 * record (compressed names), and NXDOMAIN for anything else. */
static po_socket_t s_mock;
static void mock_dns_once(void)
{
    uint8_t q[512], r[512];
    struct sockaddr_in from; socklen_t fl = sizeof(from);
    int n = (int)recvfrom(s_mock, (char*)q, sizeof(q), 0, (struct sockaddr*)&from, &fl);
    if (n < 12) return;
    int known = n > 12 + 14 && !memcmp(q + 12, "\4game\7example\0", 14);
    memcpy(r, q, (size_t)n);
    r[2] = 0x81; r[3] = known ? 0x80 : 0x83;
    int o = n;
    if (known) {
        r[7] = 2;
        /* answer 1: CNAME -> pointer to qname */
        const uint8_t cname[] = { 0xC0, 12, 0, 5, 0, 1, 0, 0, 0, 60, 0, 2, 0xC0, 12 };
        memcpy(r + o, cname, sizeof(cname)); o += (int)sizeof(cname);
        const uint8_t a[] = { 0xC0, 12, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 203, 0, 113, 9 };
        memcpy(r + o, a, sizeof(a)); o += (int)sizeof(a);
    }
    sendto(s_mock, (const char*)r, o, 0, (struct sockaddr*)&from, fl);
}
#ifdef _WIN32
static DWORD WINAPI mock_thread(LPVOID u) { (void)u; mock_dns_once(); mock_dns_once(); return 0; }
static void start_mock(void) { CloseHandle(CreateThread(NULL, 0, mock_thread, NULL, 0, NULL)); }
#else
#include <pthread.h>
static void* mock_thread(void* u) { (void)u; mock_dns_once(); mock_dns_once(); return NULL; }
static void start_mock(void) { pthread_t t; pthread_create(&t, NULL, mock_thread, NULL); pthread_detach(t); }
#endif

static uint32_t ip4(int a, int b, int c, int d)
{
    const uint8_t x[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
    uint32_t v; memcpy(&v, x, 4); return v;
}

static char s_fake_service[64];
static uint32_t s_fake_cookie_size;
static uint8_t s_fake_cookie0;
static int s_fake_fail;
static const uint8_t* s_fake_ticket = (const uint8_t*)"TICKET";
static uint32_t s_fake_ticket_len = 6;
static int fake_session(const char* svc, const uint8_t* cookie, uint32_t cookie_size,
                        uint8_t* out, uint32_t cap, uint32_t* out_len)
{
    strcpy(s_fake_service, svc);
    s_fake_cookie_size = cookie_size;
    s_fake_cookie0 = cookie_size ? cookie[0] : 0;
    if (s_fake_fail) return -1;
    assert(cap >= s_fake_ticket_len);
    memcpy(out, s_fake_ticket, s_fake_ticket_len);
    *out_len = s_fake_ticket_len;
    return 0;
}

/* A ticket laid out like RPCN's: version, size, body blob 0x3000 with the
 * twelve fields, signature blob 0x3002. */
static size_t tk_node(uint8_t* p, uint16_t id, const void* d, uint16_t n)
{
    p[0] = (uint8_t)(id >> 8); p[1] = (uint8_t)id; p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n;
    if (n && d) memcpy(p + 4, d, n);
    return 4 + (size_t)n;
}
static size_t build_ticket(uint8_t* t)
{
    uint8_t body[512]; size_t b = 0;
    uint8_t serial[20]; memset(serial, 0x5A, 20);
    const uint8_t issuer[4] = { 0, 0, 0x01, 0x00 };
    const uint8_t issued[8] = { 0, 0, 0x01, 0x8F, 0, 0, 0, 1 };
    const uint8_t expire[8] = { 0, 0, 0x01, 0x8F, 0, 0, 0, 2 };
    const uint8_t account[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
    uint8_t online[32] = "Tester_1";
    const uint8_t status[4] = { 0, 0, 0, 7 };
    uint8_t service[24] = "UP9000-BCUS98103_00";
    b += tk_node(body + b, 8, serial, 20);
    b += tk_node(body + b, 1, issuer, 4);
    b += tk_node(body + b, 7, issued, 8);
    b += tk_node(body + b, 7, expire, 8);
    b += tk_node(body + b, 2, account, 8);
    b += tk_node(body + b, 4, online, 32);
    b += tk_node(body + b, 8, "us\0\0", 4);
    b += tk_node(body + b, 4, "un\0\0", 4);
    b += tk_node(body + b, 8, service, 24);
    b += tk_node(body + b, 1, status, 4);
    b += tk_node(body + b, 0, NULL, 0);
    b += tk_node(body + b, 0, NULL, 0);
    uint8_t sig[16]; size_t s = tk_node(sig, 8, "SIG!", 4);
    size_t o = 8;
    o += tk_node(t + o, 0x3000, body, (uint16_t)b);
    o += tk_node(t + o, 0x3002, sig, (uint16_t)s);
    const uint32_t ver = 0x21010000u, sz = (uint32_t)(o - 8);
    for (int i = 0; i < 4; i++) { t[i] = (uint8_t)(ver >> (24 - 8 * i)); t[4 + i] = (uint8_t)(sz >> (24 - 8 * i)); }
    return o;
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, 1u << 20);

    /* --- config ---------------------------------------------------------- */
    port_online_config c;
    const char* ini =
        "# comment\r\n"
        "[rpcn]\n"
        "rpcn_host = np.example.net:40000\r\n"
        "rpcn_user=Tester_1\n"
        "rpcn_password = p@ss=word\n"
        "rpcn_token=ABCD\n"
        "dns=127.0.0.1\n"
        "host.Resistance2.Example.com = 10.1.2.3\n"
        "host.bad = 1.2.3\n"
        "bogus=1\n";
    assert(port_online_parse_config(ini, &c) == 6);
    assert(!strcmp(c.rpcn_host, "np.example.net") && c.rpcn_port == 40000);
    assert(!strcmp(c.rpcn_user, "Tester_1") && !strcmp(c.rpcn_password, "p@ss=word"));
    assert(!strcmp(c.rpcn_token, "ABCD"));
    assert(c.dns_server == ip4(127, 0, 0, 1));
    assert(c.n_hosts == 1 && c.hosts[0].ip == ip4(10, 1, 2, 3));
    assert(port_online_parse_config("rpcn_host=np.rpcs3.net\n", &c) == 1 && c.rpcn_port == RPCN_DEFAULT_PORT);
    assert(port_online_parse_config("rpcn_host=x:99999\n", &c) == 0);

    /* --- RPCN password derivation (PBKDF2-HMAC-SHA3-256, Python hashlib) -- */
    {
        uint8_t d[32], big[200];
        char hx[65];
        port_online_sha3_256("", 0, d);
        for (int i = 0; i < 32; i++) sprintf(hx + 2 * i, "%02x", d[i]);
        assert(!strcmp(hx, "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a"));
        memset(big, 'a', sizeof(big));
        port_online_sha3_256(big, sizeof(big), d);   /* spans the 136-byte rate */
        for (int i = 0; i < 32; i++) sprintf(hx + 2 * i, "%02x", d[i]);
        assert(!strcmp(hx, "cce34485baf2bf2aca99b94833892a4f52896d3d153f7b840cc4f9fe695f1387"));
        memset(big, 'k', sizeof(big));               /* key longer than the block */
        port_online_pbkdf2_sha3_256((const char*)big, sizeof(big), (const uint8_t*)"salt", 4, 3, d);
        for (int i = 0; i < 32; i++) sprintf(hx + 2 * i, "%02x", d[i]);
        assert(!strcmp(hx, "a0c51840c29c7de3e2a0aacdf167b269c03f133d0c65769befd852edb3ddddb3"));
        port_online_rpcn_password("p@ss=word", hx);
        assert(!strcmp(hx, "4087EC21929B552EBD6404870CE2283F812FEB9EC6720A77FA91B0F64C5CA49B"));
        port_online_rpcn_password("4087ec21929b552ebd6404870ce2283f812feb9ec6720a77fa91b0f64c5ca49b", hx);
        assert(!strcmp(hx, "4087EC21929B552EBD6404870CE2283F812FEB9EC6720A77FA91B0F64C5CA49B"));
    }

    /* --- DNS wire format -------------------------------------------------- */
    uint8_t q[300];
    int ql = port_dns_build_query(0x1234, "a.bc", q, sizeof(q));
    const uint8_t want[] = { 0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 'a', 2, 'b', 'c', 0, 0, 1, 0, 1 };
    assert(ql == (int)sizeof(want) && !memcmp(q, want, sizeof(want)));
    assert(port_dns_build_query(1, "a..b", q, sizeof(q)) < 0);
    assert(port_dns_build_query(1, "", q, sizeof(q)) < 0);
    uint32_t ip = 0;
    assert(!port_dns_parse_a(q, (size_t)ql, 0x1234, &ip));      /* a query, not a response */

    /* --- off unless PORT_ONLINE ------------------------------------------- */
    port_online_register();
    assert(s_nreg == 0 && !g_net_resolve_hook && !port_online_signed_in());

    /* --- PORT_ONLINE with a config and a loopback DNS --------------------- */
    po_net_startup();
    s_mock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(s_mock, (struct sockaddr*)&sa, sizeof(sa)) == 0);
    socklen_t sl = sizeof(sa);
    getsockname(s_mock, (struct sockaddr*)&sa, &sl);
    s_dns_port = ntohs(sa.sin_port);

    FILE* f = fopen("test_port_online.ini", "wb");
    assert(f);
    fputs("rpcn_user=Tester_1\nrpcn_password=secret\ndns=127.0.0.1\nhost.svo.example=10.9.8.7\n", f);
    fclose(f);
    set_env("PORT_ONLINE_CONFIG=test_port_online.ini");
    set_env("PORT_ONLINE=1");
    port_online_register();
    assert(getenv("PS3_NET_ONLINE"));
    assert(g_net_resolve_hook == port_online_resolve && g_np_signed_in_hook() == 1);
    assert(!strcmp(g_np_online_id_hook(), "Tester_1"));

    assert(port_online_resolve("SVO.example", &ip) && ip == ip4(10, 9, 8, 7));   /* case-insensitive */
    start_mock();
    assert(port_online_resolve("game.example", &ip) && ip == ip4(203, 0, 113, 9));
    assert(!port_online_resolve("missing.example", &ip));                       /* NXDOMAIN -> system */
    po_close(s_mock);
    remove("test_port_online.ini");

    /* --- RPCN wire format ------------------------------------------------- */
    uint8_t pkt[256];
    int n = rpcn_build_login(pkt, sizeof(pkt), 0x0102030405060708ull, "Tester_1", "pw", "");
    const uint8_t login[] = { 0, 0, 0, 28, 0, 0, 0, 8, 7, 6, 5, 4, 3, 2, 1,
                              'T','e','s','t','e','r','_','1',0, 'p','w',0, 0 };
    assert(n == (int)sizeof(login) && !memcmp(pkt, login, sizeof(login)));
    uint8_t type; uint16_t cmd; uint32_t total; uint64_t pid;
    assert(rpcn_parse_header(pkt, (size_t)n, &type, &cmd, &total, &pid) == RPCN_HEADER_SIZE);
    assert(type == RPCN_PKT_REQUEST && cmd == RPCN_CMD_LOGIN && total == 28 && pid == 0x0102030405060708ull);
    assert(rpcn_build_login(pkt, 20, 1, "Tester_1", "pw", "") < 0);

    const uint8_t cookie[3] = { 0xAA, 0xBB, 0xCC };
    n = rpcn_build_ticket_request(pkt, sizeof(pkt), 9, "UP9000-BCUS98103_00", cookie, 3);
    assert(n == RPCN_HEADER_SIZE + 20 + 4 + 3);
    assert(pkt[1] == RPCN_CMD_REQUEST_TICKET && pkt[2] == 0 && pkt[3] == (uint8_t)n);
    assert(!memcmp(pkt + 15, "UP9000-BCUS98103_00", 20));
    assert(pkt[35] == 3 && pkt[36] == 0 && pkt[37] == 0 && pkt[38] == 0 && pkt[39] == 0xAA && pkt[41] == 0xCC);

    uint8_t si[19];
    rpcn_write_header(si, RPCN_PKT_SERVERINFO, 0, 19, 0);
    si[15] = 34; si[16] = si[17] = si[18] = 0;
    assert(rpcn_parse_server_info(si, sizeof(si)) == RPCN_PROTOCOL_VERSION);
    si[0] = RPCN_PKT_REPLY;
    assert(rpcn_parse_server_info(si, sizeof(si)) == -1);

    /* Login reply: error, online_name, avatar_url, user_id, friend lists. */
    char name[32];
    const uint8_t lr[] = { 0, 'T','e','s','t','e','r','_','1',0, 'h','t','t','p',0, 1,0,0,0,0,0,0,0, 0,0,0,0 };
    assert(rpcn_parse_login_reply(lr, sizeof(lr), name, sizeof(name)) == 0 && !strcmp(name, "Tester_1"));
    const uint8_t lbad[] = { 7 };   /* LoginInvalidPassword etc. */
    assert(rpcn_parse_login_reply(lbad, 1, name, sizeof(name)) == 7 && !name[0]);
    assert(rpcn_parse_login_reply(lr, 4, name, sizeof(name)) == -1);   /* unterminated name */
    /* Ticket reply: error, u32 LE size, bytes. */
    const uint8_t tr[] = { 0, 3, 0, 0, 0, 'a', 'b', 'c' };
    const uint8_t* tk; uint32_t tl;
    assert(rpcn_parse_ticket_reply(tr, sizeof(tr), &tk, &tl) == 0 && tl == 3 && tk == tr + 5);
    assert(rpcn_parse_ticket_reply(tr, sizeof(tr) - 1, &tk, &tl) == -1);
    assert(rpcn_parse_ticket_reply(lbad, 1, &tk, &tl) == 7);

    /* --- sceNp manager exports -------------------------------------------- */
    enum { NPID = 0x1000, SVC = 0x1100, COOKIE = 0x1200, SIZE = 0x1300, BUF = 0x1400, TICK = 0x1500 };
    s_ticket_sync = 1;
    strcpy((char*)vm_base + SVC, "UP9000-BCUS98103_00");
    /* No rpcn_host configured: the request still completes, with an error event. */
    /* R2 signature: (npId, serviceId, cookie, size, ...); R3: (npId, version, serviceId, ...). */
    assert(call("sceNpManagerRequestTicket", NPID, SVC, COOKIE, 3, 0) == 0);
    assert(s_cb_calls == 1 && s_cb_opd == 0x10000);
    assert(s_cb_args[0] == SCE_NP_MANAGER_EVENT_GOT_TICKET && (uint32_t)s_cb_args[1] == SCE_NP_ERROR_OFFLINE);
    assert(s_cb_args[2] == 0x20000);
    assert((uint32_t)call("sceNpManagerRequestTicket2", NPID, 0x1600, SVC, COOKIE, 2000) == SCE_NP_AUTH_EINVALID_ARGUMENT);

    vm_write32(SIZE, 64);
    assert(call("sceNpManagerGetTicket", 0, SIZE, 0, 0, 0) == 0 && vm_read32(SIZE) == 0);
    assert((uint32_t)call("sceNpManagerGetTicket", BUF, 0, 0, 0, 0) == SCE_NP_ERROR_INVALID_ARGUMENT);
    assert((uint32_t)call("sceNpManagerGetTicketParam", 3, BUF, 0, 0, 0) == SCE_NP_ERROR_INVALID_STATE);
    assert((uint32_t)call("sceNpManagerGetTicketParam", 12, BUF, 0, 0, 0) == SCE_NP_ERROR_INVALID_ARGUMENT);

    /* A server that issues a ticket: GOT_TICKET carries its size, GetTicket copies it. */
    strcpy(s_cfg.rpcn_host, "rpcn.invalid");
    s_session_fn = fake_session;
    vm_write8(COOKIE, 0xAA); vm_write8(COOKIE + 1, 0xBB);
    assert(call("sceNpManagerRequestTicket2", NPID, 0x1600, SVC, COOKIE, 2) == 0);
    assert(!strcmp(s_fake_service, "UP9000-BCUS98103_00") && s_fake_cookie_size == 2 && s_fake_cookie0 == 0xAA);
    assert(s_cb_calls == 2 && (int32_t)s_cb_args[1] == 6);
    vm_write32(SIZE, 0);
    assert(call("sceNpManagerGetTicket", 0, SIZE, 0, 0, 0) == 0 && vm_read32(SIZE) == 6);
    vm_write32(SIZE, 4);
    assert(call("sceNpManagerGetTicket", BUF, SIZE, 0, 0, 0) == 0 && vm_read32(SIZE) == 4);
    assert(!memcmp(vm_base + BUF, "TICK", 4));
    s_fake_fail = 1;
    assert(call("sceNpManagerRequestTicket", NPID, SVC, 0, 0, 0) == 0);
    assert(s_cb_calls == 3 && (uint32_t)s_cb_args[1] == SCE_NP_ERROR_OFFLINE);
    vm_write32(SIZE, 0);
    assert(call("sceNpManagerGetTicket", 0, SIZE, 0, 0, 0) == 0 && vm_read32(SIZE) == 0);

    /* Ticket fields. "TICKET" is not a ticket: fields are INVALID_STATE. */
    static uint8_t tkb[1024];
    size_t tkl = build_ticket(tkb);
    np_ticket_value tv;
    assert(np_ticket_get_param(tkb, tkl, 1, &tv) == 0 && tv.kind == 1 && tv.v == 0x100);
    assert(np_ticket_get_param(tkb, tkl, 4, &tv) == 0 && tv.kind == 2 && tv.v == 0x1122334455667788ull);
    assert(np_ticket_get_param(tkb, tkl, 8, &tv) == 0 && tv.kind == 3 && tv.n == 24 && !strcmp((char*)tv.data, "UP9000-BCUS98103_00"));
    assert(np_ticket_get_param(tkb, tkl, 11, &tv) == 0 && tv.kind == 2 && tv.v == 0);
    assert(np_ticket_get_param(tkb, tkl - 1, 1, &tv) < 0);           /* size field mismatch */
    tkb[12 + 1] = 9;                                                   /* serial node id 8 -> 9 */
    assert(np_ticket_get_param(tkb, tkl, 1, &tv) < 0);
    tkb[12 + 1] = 8;

    s_fake_fail = 0;
    s_fake_ticket = tkb; s_fake_ticket_len = (uint32_t)tkl;
    assert(call("sceNpManagerRequestTicket", NPID, SVC, 0, 0, 0) == 0 && (uint32_t)s_cb_args[1] == tkl);
    assert(call("sceNpManagerGetTicketParam", 5, BUF, 0, 0, 0) == 0 && !strcmp((char*)vm_base + BUF, "Tester_1"));
    assert(call("sceNpManagerGetTicketParam", 3, BUF, 0, 0, 0) == 0 && vm_read64(BUF) == 0x0000018F00000002ull);
    assert(call("sceNpManagerGetTicketParam", 9, BUF, 0, 0, 0) == 0 && vm_read32(BUF) == 7);

    assert(call("sceNpManagerGetNetworkTime", TICK, 0, 0, 0, 0) == 0);
    uint64_t tick = vm_read64(TICK);
    /* 2020-01-01 .. 2100-01-01 in CellRtcTick microseconds */
    assert(tick > 63713433600000000ull && tick < 66238214400000000ull);
    assert(call("sceNpManagerSubSignin", 1, 0, 0, 0, 0) == 0);

    s_np_init = 0;
    assert((uint32_t)call("sceNpManagerGetNetworkTime", TICK, 0, 0, 0, 0) == SCE_NP_ERROR_NOT_INITIALIZED);

    printf("test_port_online: all passed\n");
    return 0;
}
