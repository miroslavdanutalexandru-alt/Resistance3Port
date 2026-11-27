/*
 * test_http_redirect - cellHttpSendRequest connects to the address the
 * PORT_ONLINE resolver hook (online.ini host.* / dns=) gives for the URI
 * host, and keeps the original name in the Host header.
 *
 * The URI names a host that does not resolve; the hook maps it to 127.0.0.1,
 * where a loopback listener accepts the request. Winsock and POSIX.
 *
 *   clang-cl /nologo /I include libs/network/tests/test_http_redirect.c ws2_32.lib
 *   cc -std=gnu17 -I include -o test_http_redirect libs/network/tests/test_http_redirect.c
 */
#include "../cellHttp.c"
#ifndef _WIN32
#include <arpa/inet.h>
#endif

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

int (*g_net_resolve_hook)(const char*, uint32_t*);

static int fails = 0;
#define CHECK(c, ...) do { printf((c) ? "[PASS] " : "[FAIL] "); printf(__VA_ARGS__); printf("\n"); \
                           if (!(c)) fails++; } while (0)

static const char* FAKE_HOST = "igloo.r3lan.invalid";

static int test_hook(const char* name, uint32_t* ip)
{
    if (strcmp(name, FAKE_HOST) != 0) return 0;
    const uint8_t lo[4] = { 127, 0, 0, 1 };   /* network order, like s_addr */
    memcpy(ip, lo, 4);
    return 1;
}

static void put_str(u32 ea, const char* s) { strcpy((char*)vm_base + ea, s); }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    vm_base = calloc(1, 65536);
    ppu_vm_size = 65536;
    if (cellHttpInit(NULL, 0) != CELL_OK) { puts("FAIL: cellHttpInit"); return 1; }

    http_socket_t lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    socklen_t al = sizeof a;
    if (bind(lfd, (struct sockaddr*)&a, sizeof a) != 0 || listen(lfd, 4) != 0 ||
        getsockname(lfd, (struct sockaddr*)&a, &al) != 0) {
        puts("SKIP: no loopback listener");
        return 0;
    }
    u16 port = ntohs(a.sin_port);

    put_str(0x100, "http");
    put_str(0x110, FAKE_HOST);
    put_str(0x140, "/r3/login");
    put_str(0x180, "GET");
    const u32 URI = 0x200, CLIENT = 0x300, TRANS = 0x304, SENT = 0x308;
    vm_write32(URI + 0, 0x100);
    vm_write32(URI + 4, 0x110);
    vm_write32(URI + 8, 0);
    vm_write32(URI + 12, 0);
    vm_write32(URI + 16, 0x140);
    vm_write32(URI + 20, port);
    vm_write32(URI + 24, 0);

    g_net_resolve_hook = test_hook;
    CHECK(cellHttpCreateClient((CellHttpClientId*)(uintptr_t)CLIENT) == CELL_OK, "create client");
    CellHttpClientId client = *(CellHttpClientId*)(vm_base + CLIENT);
    CHECK(cellHttpCreateTransaction((CellHttpTransId*)(uintptr_t)TRANS, client,
                                    (const char*)(uintptr_t)0x180, (const CellHttpUri*)(uintptr_t)URI) == CELL_OK,
          "create transaction");
    CellHttpTransId trans = vm_read32(TRANS);
    s32 rc = cellHttpSendRequest(trans, NULL, 0, (u32*)(uintptr_t)SENT);
    CHECK(rc == CELL_OK, "send request to the redirected host (rc=0x%08x)", (unsigned)rc);

    char buf[2048]; int n = 0;
    if (rc == CELL_OK) {
        http_socket_t cfd = accept(lfd, NULL, NULL);
        while (cfd != (http_socket_t)-1 && n < (int)sizeof buf - 1) {
            int r = (int)recv(cfd, buf + n, (int)sizeof buf - 1 - n, 0);
            if (r <= 0) break;
            n += r; buf[n] = 0;
            if (strstr(buf, "\r\n\r\n")) break;
        }
        http_closesocket(cfd);
    }
    buf[n] = 0;
    CHECK(strncmp(buf, "GET /r3/login HTTP/1.1\r\n", 24) == 0, "request reached the listener");
    CHECK(strstr(buf, "Host: igloo.r3lan.invalid") != NULL, "Host header keeps the original name");

    cellHttpDestroyTransaction(trans);
    cellHttpDestroyClient(client);
    g_net_resolve_hook = NULL;
    cellHttpEnd();
    http_closesocket(lfd);
    free(vm_base);
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
