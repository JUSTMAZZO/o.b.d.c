/*
 * cmif_test.c - Testes unitarios do parser e despachante IPC/CMIF do Horizon HLE.
 *
 * Valida:
 *   - Estruturas de cabecalho CmifInHeader e CmifOutHeader (SFCI / SFCO)
 *   - cmifParseInHeader e cmifFormatOutHeader
 *   - svcConnectToNamedPort ("sm:")
 *   - svcSendSyncRequest com handshaking de servicos:
 *       * sm: Initialize (cmd 0) -> SFCO, Result 0
 *       * sm: GetService (cmd 1, "time") -> SFCO, Result 0, handle valido
 *       * time: GetCurrentTime (cmd 0) -> SFCO, Result 0, payload de timestamp
 *       * set: GetLanguageCode (cmd 0) -> SFCO, Result 0, payload de idioma
 *       * applet: ping -> SFCO, Result 0
 *   - Pacotes diretos (SFCI em offset 0) e encapsulados em HIPC (SFCI em offset 0x10)
 *   - Tratamento de handles invalidos e retrocompatibilidade com chamadas dummy (sem SFCI)
 *
 * Build:
 *   Host:   clang -O2 -Wall -Wextra -I . horizon_rt/cmif_test.c -o horizon_rt/cmif_test.exe -lpthread
 *   Android: aarch64-linux-android35-clang -O2 -Wall -Wextra -I . -c horizon_rt/cmif_test.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "switch_hle_shim.h"

static int s_ok, s_fail;

#define CHECK(cond, ...) do { \
        if (cond) { s_ok++; printf("[ok]      "); } else { s_fail++; printf("[FALHOU]  "); } \
        printf(__VA_ARGS__); printf("\n"); \
    } while (0)

/* ------------------------------------------------------------------ Teste das estruturas */
static void test_cmif_structures(void)
{
    CHECK(CMIF_IN_HEADER_MAGIC == 0x49434653U, "CMIF_IN_HEADER_MAGIC ('SFCI') = 0x49434653");
    CHECK(CMIF_OUT_HEADER_MAGIC == 0x4F434653U, "CMIF_OUT_HEADER_MAGIC ('SFCO') = 0x4F434653");
    CHECK(SFCI_MAGIC == 0x49434653U && SFCO_MAGIC == 0x4F434653U, "SFCI_MAGIC e SFCO_MAGIC alias coerentes");

    CmifInHeader in;
    memset(&in, 0, sizeof(in));
    in.magic = CMIF_IN_HEADER_MAGIC;
    in.version = 1;
    in.command_id = 42;
    in.token = 0x1234;
    in.type = 4;
    in.raw_size = 8;
    in.objects = 2;

    CHECK(in.Magic == CMIF_IN_HEADER_MAGIC && in.Version == 1 && in.CommandId == 42 &&
          in.cmd_id == 42 && in.Token == 0x1234 && in.Type == 4 && in.RawSize == 8 && in.Objects == 2,
          "CmifInHeader: campos camelCase, PascalCase e aliases (cmd_id/CommandId)");

    CmifOutHeader out;
    memset(&out, 0, sizeof(out));
    out.magic = CMIF_OUT_HEADER_MAGIC;
    out.version = 0;
    out.result = 0;
    out.token = 0x5678;

    CHECK(out.Magic == CMIF_OUT_HEADER_MAGIC && out.Result == 0 && out.Token == 0x5678,
          "CmifOutHeader: campos Magic, Version, Result, Token");
}

/* ------------------------------------------------------------------ Teste parser / formatador */
static void test_cmif_parser(void)
{
    u32 buf[16];
    memset(buf, 0, sizeof(buf));

    /* Sem SFCI */
    CmifInHeader in;
    size_t off = 0;
    CHECK(cmifParseInHeader(buf, sizeof(buf), &in, &off) == -1, "cmifParseInHeader sem SFCI -> -1");

    /* SFCI em offset 0 */
    buf[0] = CMIF_IN_HEADER_MAGIC;
    buf[1] = 0;
    buf[2] = 100; /* cmd_id */
    buf[3] = 0xABCD;
    CHECK(cmifParseInHeader(buf, sizeof(buf), &in, &off) == 0 && off == 0 &&
          in.magic == CMIF_IN_HEADER_MAGIC && in.command_id == 100 && in.token == 0xABCD,
          "cmifParseInHeader SFCI em offset 0");

    /* SFCI em offset 0x10 (word 4) */
    memset(buf, 0, sizeof(buf));
    buf[0] = 4; /* HIPC Type 4 */
    buf[1] = 8; /* HIPC raw_size 8 */
    buf[4] = CMIF_IN_HEADER_MAGIC;
    buf[5] = 1;
    buf[6] = 200;
    buf[7] = 0xEF01;
    CHECK(cmifParseInHeader(buf, sizeof(buf), &in, &off) == 0 && off == 16 &&
          in.magic == CMIF_IN_HEADER_MAGIC && in.command_id == 200 && in.type == 4 && in.raw_size == 8,
          "cmifParseInHeader SFCI em offset 0x10 com metadados HIPC");

    /* Formatacao */
    CmifOutHeader out;
    memset(&out, 0, sizeof(out));
    out.magic = CMIF_OUT_HEADER_MAGIC;
    out.result = 0;
    out.token = 0x9999;
    CHECK(cmifFormatOutHeader(buf, sizeof(buf), 16, &out) == 0 &&
          buf[4] == CMIF_OUT_HEADER_MAGIC && buf[6] == 0 && buf[7] == 0x9999,
          "cmifFormatOutHeader grava SFCO, result e token no offset correto");
}

/* ------------------------------------------------------------------ Teste de servicos com svcSendSyncRequest */
static void test_cmif_services(void)
{
    void *tls = shimGetTls();
    CHECK(tls != NULL, "shimGetTls() fornece buffer TLS da thread");
    u32 *ipc_buf = (u32 *)tls;

    /* 1. Conectar a sm: */
    Handle sm_sess = INVALID_HANDLE;
    CHECK(svcConnectToNamedPort(&sm_sess, "sm:") == 0 && sm_sess != INVALID_HANDLE,
          "svcConnectToNamedPort(\"sm:\") -> sessao 0x%x", sm_sess);

    /* 2. sm: Initialize (cmd 0) */
    memset(ipc_buf, 0, SHIM_TLS_IPC_SIZE);
    ipc_buf[0] = CMIF_IN_HEADER_MAGIC; /* 'SFCI' */
    ipc_buf[1] = 0;                    /* version */
    ipc_buf[2] = 0;                    /* cmd_id = 0 (Initialize) */
    ipc_buf[3] = 0;                    /* token */
    ipc_buf[4] = 0;                    /* pid_placeholder */
    ipc_buf[5] = 0;

    Result rc = svcSendSyncRequest(sm_sess);
    CHECK(rc == 0, "svcSendSyncRequest(sm_sess, Initialize) -> 0");
    CHECK(ipc_buf[0] == CMIF_OUT_HEADER_MAGIC, "Resposta contem SFCO magic (0x4F434653)");
    CHECK(ipc_buf[2] == 0, "Resposta contem Result 0 (Sucesso)");

    /* 3. sm: GetService("time") (cmd 1) */
    memset(ipc_buf, 0, SHIM_TLS_IPC_SIZE);
    ipc_buf[0] = CMIF_IN_HEADER_MAGIC;
    ipc_buf[1] = 0;
    ipc_buf[2] = 1;                    /* cmd_id = 1 (GetService) */
    ipc_buf[3] = 0;
    /* Nome do servico: 8 bytes */
    char *srv_name = (char *)&ipc_buf[4];
    strncpy(srv_name, "time", 8);

    rc = svcSendSyncRequest(sm_sess);
    CHECK(rc == 0, "svcSendSyncRequest(sm_sess, GetService(\"time\")) -> 0");
    CHECK(ipc_buf[0] == CMIF_OUT_HEADER_MAGIC, "sm: GetService resposta SFCO");
    CHECK(ipc_buf[2] == 0, "sm: GetService Result 0");

    Handle time_sess = ipc_buf[4];
    CHECK(time_sess != INVALID_HANDLE && time_sess >= SHIM_HANDLE_BASE,
          "sm: GetService alocou sessao valida para 'time' (handle 0x%x)", time_sess);

    /* 4. Despacho no servico time (cmd 0 = GetCurrentTime) */
    memset(ipc_buf, 0, SHIM_TLS_IPC_SIZE);
    ipc_buf[0] = CMIF_IN_HEADER_MAGIC;
    ipc_buf[1] = 0;
    ipc_buf[2] = 0;                    /* cmd 0: GetCurrentTime */
    ipc_buf[3] = 0;

    time_t t_before = time(NULL);
    rc = svcSendSyncRequest(time_sess);
    time_t t_after = time(NULL);

    CHECK(rc == 0, "svcSendSyncRequest(time_sess, GetCurrentTime) -> 0");
    CHECK(ipc_buf[0] == CMIF_OUT_HEADER_MAGIC && ipc_buf[2] == 0, "time resposta SFCO com Result 0");

    u64 returned_time = 0;
    memcpy(&returned_time, &ipc_buf[4], sizeof(u64));
    CHECK(returned_time >= (u64)t_before && returned_time <= (u64)t_after,
          "time payload coerente: %llu (host time: %llu)", (unsigned long long)returned_time, (unsigned long long)t_before);

    /* 5. sm: GetService("set") (cmd 1) */
    memset(ipc_buf, 0, SHIM_TLS_IPC_SIZE);
    ipc_buf[0] = CMIF_IN_HEADER_MAGIC;
    ipc_buf[1] = 0;
    ipc_buf[2] = 1;
    ipc_buf[3] = 0;
    srv_name = (char *)&ipc_buf[4];
    strncpy(srv_name, "set", 8);

    rc = svcSendSyncRequest(sm_sess);
    CHECK(rc == 0 && ipc_buf[0] == CMIF_OUT_HEADER_MAGIC && ipc_buf[2] == 0, "sm: GetService(\"set\") -> SFCO ok");
    Handle set_sess = ipc_buf[4];
    CHECK(set_sess != INVALID_HANDLE, "sessao para 'set' valida (handle 0x%x)", set_sess);

    /* 6. Despacho no servico set (cmd 0 = GetLanguageCode) */
    memset(ipc_buf, 0, SHIM_TLS_IPC_SIZE);
    ipc_buf[0] = CMIF_IN_HEADER_MAGIC;
    ipc_buf[1] = 0;
    ipc_buf[2] = 0;
    ipc_buf[3] = 0;

    rc = svcSendSyncRequest(set_sess);
    CHECK(rc == 0 && ipc_buf[0] == CMIF_OUT_HEADER_MAGIC && ipc_buf[2] == 0, "set resposta SFCO com Result 0");
    u64 lang_code = 0;
    memcpy(&lang_code, &ipc_buf[4], sizeof(u64));
    CHECK(lang_code == 0x000053552d6e65ULL, "set payload coerente: codigo de idioma \"en-US\"");

    /* 7. sm: GetService("appletOE") e ping */
    memset(ipc_buf, 0, SHIM_TLS_IPC_SIZE);
    ipc_buf[0] = CMIF_IN_HEADER_MAGIC;
    ipc_buf[1] = 0;
    ipc_buf[2] = 1;
    ipc_buf[3] = 0;
    srv_name = (char *)&ipc_buf[4];
    strncpy(srv_name, "appletOE", 8);

    rc = svcSendSyncRequest(sm_sess);
    CHECK(rc == 0 && ipc_buf[0] == CMIF_OUT_HEADER_MAGIC && ipc_buf[2] == 0, "sm: GetService(\"appletOE\") -> SFCO ok");
    Handle applet_sess = ipc_buf[4];
    CHECK(applet_sess != INVALID_HANDLE, "sessao para 'appletOE' valida (handle 0x%x)", applet_sess);

    memset(ipc_buf, 0, SHIM_TLS_IPC_SIZE);
    ipc_buf[0] = CMIF_IN_HEADER_MAGIC;
    ipc_buf[1] = 0;
    ipc_buf[2] = 0; /* Ping / open */
    ipc_buf[3] = 0;
    rc = svcSendSyncRequest(applet_sess);
    CHECK(rc == 0 && ipc_buf[0] == CMIF_OUT_HEADER_MAGIC && ipc_buf[2] == 0, "appletOE ping resposta SFCO Result 0");

    /* 8. Pacote simulando libnx completa (HIPC header em [0..3] + SFCI em [4..7]) */
    memset(ipc_buf, 0, SHIM_TLS_IPC_SIZE);
    ipc_buf[0] = 4; /* HIPC Type 4 (Request) */
    ipc_buf[1] = 6; /* 6 raw words */
    ipc_buf[2] = 0;
    ipc_buf[3] = 0;
    ipc_buf[4] = CMIF_IN_HEADER_MAGIC; /* SFCI em offset 16 */
    ipc_buf[5] = 0;
    ipc_buf[6] = 0; /* cmd 0 */
    ipc_buf[7] = 0x4321;

    rc = svcSendSyncRequest(time_sess);
    CHECK(rc == 0, "svcSendSyncRequest com pacote libnx HIPC+CMIF -> 0");
    CHECK(ipc_buf[4] == CMIF_OUT_HEADER_MAGIC, "Resposta HIPC: SFCO gravado no offset 16");
    CHECK(ipc_buf[6] == 0, "Resposta HIPC: Result 0");
    CHECK(ipc_buf[7] == 0x4321, "Resposta HIPC: token de contexto preservado (0x4321)");

    /* 9. Chamada dummy sem SFCI preserva retrocompatibilidade */
    memset(ipc_buf, 0, SHIM_TLS_IPC_SIZE);
    CHECK(svcSendSyncRequest(sm_sess) == 0, "Buffer sem SFCI -> retorna 0 (retrocompativel com dummy)");

    /* 10. Handle invalido e fechamento */
    CHECK(svcSendSyncRequest(0x9999) == KERNELRESULT(InvalidHandle), "Handle invalido -> InvalidHandle");
    svcCloseHandle(time_sess);
    svcCloseHandle(set_sess);
    svcCloseHandle(applet_sess);
    svcCloseHandle(sm_sess);
    CHECK(svcSendSyncRequest(sm_sess) == KERNELRESULT(InvalidHandle), "Handle fechado -> InvalidHandle");
}

int main(void)
{
    printf("=== Testes de IPC / CMIF do Horizon (switch_hle_shim.h) ===\n");
    test_cmif_structures();
    test_cmif_parser();
    test_cmif_services();

    printf("\nResultado: %d ok, %d falhas\n", s_ok, s_fail);
    return s_fail ? 1 : 0;
}
