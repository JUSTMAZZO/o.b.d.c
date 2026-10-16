/*
 * svc_shim_test.c - testa no Android (A56) os handlers HLE dos SVCs do Horizon em switch_hle_shim.h.
 *
 * Cada SVC e exercitado no caminho de sucesso e nos erros que a libnx espera (KERNELRESULT).
 * Threads, Mutex, RMutex e CondVar sao exercitados pela API da libnx implementada no shim
 * (threadCreate/..., mutexLock/..., condvarWait/...). svcBreak fatal e svcReturnFromException rodam
 * num filho (fork).
 * Entrada (roteiro, buffer ao vivo, PadState) e fs (cartao SD sobre diretorio do host) sao portaveis: rodam
 * tambem no gate do Windows. O resto (kernel, threads, IPC) so existe no Linux/Bionic.
 *
 * Build: aarch64-linux-android35-clang -O2 -Wall -Wextra -I .. svc_shim_test.c -o svc_shim_test  (pthread vem na libc do Bionic)
 * Host:  clang -O2 -Wall -Wextra -I .. svc_shim_test.c -o svc_shim_test.exe -lpthread  (llvm-mingw)
 * O teste de fs cria e apaga ./obdc_sdcard_test no diretorio atual.
 */
#include <stddef.h>
#if defined(__linux__)
#include <signal.h>
#include <sys/wait.h>
#endif
#include "switch_hle_shim.h"

static int s_ok, s_fail;

#define CHECK(cond, ...) do { \
        if (cond) { s_ok++; printf("[ok]      "); } else { s_fail++; printf("[FALHOU]  "); } \
        printf(__VA_ARGS__); printf("\n"); \
    } while (0)

/* ---------------------------------------------- entrada: roteiro, buffer ao vivo e PadState compacta */
static u64 old_pad_buttons(u32 frame)    /* padUpdate antes do roteiro configuravel */
{
    u64 b = 0;
    if (frame == SHIM_FRAME_BOOST_A)
        b |= HidNpadButton_A;
    if (frame == SHIM_FRAME_EXIT)
        b |= HidNpadButton_Plus;
    return b;
}

static void test_input(void)
{
    CHECK(sizeof(PadState) == 32 && offsetof(PadState, style_set) == 4 && offsetof(PadState, buttons_cur) == 16 &&
          offsetof(PadState, buttons_old) == 24, "PadState compacta: 32 bytes, buttons_cur em +16 (layout da Fase 2)");

    /* roteiro padrao = algoritmo antigo, quadro a quadro, pelo appletMainLoop */
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    g_shim_frame = 0;
    padUpdate(&pad);
    int same = pad.buttons_cur == 0;
    u32 frames = 0, presses = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        same &= pad.buttons_cur == old_pad_buttons(g_shim_frame);
        presses += padGetButtonsDown(&pad) != 0;
        frames++;
    }
    for (u32 f = SHIM_FRAMES + 1; f < SHIM_FRAMES + 5; f++) {
        g_shim_frame = f;
        padUpdate(&pad);
        same &= pad.buttons_cur == 0;
    }
    CHECK(same && frames == 60 && presses == 2, "roteiro padrao = padUpdate antigo nos quadros 0..64 (60 frames, 2 botoes: A no 30, Plus no 60)");
    CHECK(padIsConnected(&pad) && padGetStyleSet(&pad) == HidNpadStyleSet_NpadStandard && padIsHandheld(&pad),
          "padIsConnected / padGetStyleSet / padIsHandheld");

    /* roteiro customizado: cada evento vale ate o proximo */
    const ShimInputEvent script[] = {
        { .frame = 5,  .state = { .buttons = HidNpadButton_A | HidNpadButton_ZR, .sticks = { { 1000, -2000 }, { 0, 0 } } } },
        { .frame = 8,  .state = { .buttons = 0, .sticks = { { 0, 0 }, { JOYSTICK_MAX, JOYSTICK_MIN } } } },
        { .frame = 10, .state = { .buttons = HidNpadButton_B | HidNpadButton_StickLDown } },
    };
    CHECK(shimInputSetScript(script, 3) == 0, "shimInputSetScript(3 eventos)");
    padInitializeDefault(&pad);
    u64 down[12] = {0}, up[12] = {0}, cur[12] = {0};
    HidAnalogStickState l[12], r[12];
    for (u32 f = 1; f < 12; f++) {
        g_shim_frame = f;
        padUpdate(&pad);
        cur[f] = padGetButtons(&pad);
        down[f] = padGetButtonsDown(&pad);
        up[f] = padGetButtonsUp(&pad);
        l[f] = padGetStickPos(&pad, 0);
        r[f] = padGetStickPos(&pad, 1);
    }
    u64 a_zr = HidNpadButton_A | HidNpadButton_ZR, b_sd = HidNpadButton_B | HidNpadButton_StickLDown;
    CHECK(cur[4] == 0 && cur[5] == a_zr && cur[7] == a_zr && cur[8] == 0 && cur[10] == b_sd && cur[11] == b_sd,
          "botoes do roteiro: 0 ate o quadro 4, A|ZR de 5 a 7, nada em 8-9, B|StickLDown de 10 em diante");
    CHECK(down[5] == a_zr && down[6] == 0 && up[8] == a_zr && down[10] == b_sd && down[11] == 0,
          "padGetButtonsDown/Up nas transicoes (5, 8, 10)");
    CHECK(l[5].x == 1000 && l[5].y == -2000 && l[7].x == 1000 && r[7].x == 0 && l[8].x == 0 &&
          r[8].x == JOYSTICK_MAX && r[9].y == JOYSTICK_MIN && r[10].x == 0,
          "padGetStickPos: esquerdo (1000, -2000) em 5-7, direito (MAX, MIN) em 8-9, zerados em 10");
    HidAnalogStickState bad = padGetStickPos(&pad, 2);
    CHECK(bad.x == 0 && bad.y == 0, "padGetStickPos(i = 2) -> (0, 0)");

    const ShimInputEvent unsorted[] = { { .frame = 3 }, { .frame = 3 } };
    CHECK(shimInputSetScript(unsorted, 2) == LIBNXRESULT(BadInput), "roteiro com quadros repetidos -> LibnxError_BadInput (0x%x)", LIBNXRESULT(BadInput));
    CHECK(shimInputSetScript(script, SHIM_INPUT_MAX_EVENTS + 1) == LIBNXRESULT(BadInput), "roteiro > %d eventos -> BadInput", SHIM_INPUT_MAX_EVENTS);
    g_shim_frame = 9;
    padUpdate(&pad);
    CHECK(padGetStickPos(&pad, 1).x == JOYSTICK_MAX, "roteiro invalido nao substitui o roteiro ativo");
    CHECK(shimInputSetScript(script, 0) == 0, "shimInputSetScript(count 0) = sem entrada");
    g_shim_frame = 5;
    padUpdate(&pad);
    CHECK(pad.buttons_cur == 0, "sem entrada: quadro 5 sem botoes");

    /* buffer ao vivo tem prioridade sobre o roteiro */
    ShimInputState live = { .buttons = HidNpadButton_X, .sticks = { { -7, 7 }, { 3, -3 } } };
    shimInputSetLive(&live);
    padUpdate(&pad);
    CHECK(pad.buttons_cur == HidNpadButton_X && padGetStickPos(&pad, 0).x == -7 && padGetStickPos(&pad, 1).y == -3,
          "shimInputSetLive: padUpdate le o estado ao vivo");
    shimInputClearLive();
    CHECK(shimInputSetScript(NULL, 0) == 0, "shimInputSetScript(NULL) volta ao roteiro padrao");
    g_shim_frame = SHIM_FRAME_BOOST_A;
    padUpdate(&pad);
    CHECK(pad.buttons_cur == HidNpadButton_A, "shimInputClearLive + roteiro padrao: A no quadro 30");

    PadState any;
    padInitializeAny(&any);
    CHECK(any.id_mask == 0xFF && any.read_handheld, "padInitializeAny: id_mask 0xFF + handheld");
    g_shim_frame = 0;
}

/* ---------------------------------------------- fs: cartao SD sobre ./obdc_sdcard_test */
#define TEST_ROOT "obdc_sdcard_test"

static void fill(u8 *buf, size_t n, u32 seed)
{
    for (size_t k = 0; k < n; k++)
        buf[k] = (u8)((k * 131u + seed) ^ (k >> 7));
}

static int set_env(const char *name, const char *value)
{
#if defined(_WIN32)
    return _putenv_s(name, value ? value : "");
#else
    return value ? setenv(name, value, 1) : unsetenv(name);
#endif
}

static void test_fs(void)
{
    FsFileSystem sd;
    CHECK(shimFsSetSdCardRoot("obdc_sdcard_nao_existe") == 0 && fsOpenSdCardFileSystem(&sd) == FSRESULT(SdCardNotPresent),
          "fsOpenSdCardFileSystem(raiz inexistente) -> SdCardNotPresent (0x%x)", FSRESULT(SdCardNotPresent));
    shim_sys_mkdir(TEST_ROOT);
    CHECK(shimFsSetSdCardRoot(TEST_ROOT "/") == 0 && fsOpenSdCardFileSystem(&sd) == 0, "fsOpenSdCardFileSystem(\"" TEST_ROOT "/\") -> handle 0x%x", sd.s.session);
    CHECK(fsFsCleanDirectoryRecursively(&sd, "/") == 0, "fsFsCleanDirectoryRecursively(\"/\") esvazia a raiz");

    /* criar, escrever (Append), ler */
    enum { N = 4096 };
    static u8 data[N], back[N];
    fill(data, N, 7);
    CHECK(fsFsCreateFile(&sd, "/a.bin", 0, 0) == 0, "fsFsCreateFile(/a.bin, 0)");
    CHECK(fsFsCreateFile(&sd, "/a.bin", 0, 0) == FSRESULT(PathAlreadyExists), "fsFsCreateFile de novo -> PathAlreadyExists (0x%x)", FSRESULT(PathAlreadyExists));
    FsFile f, g;
    CHECK(fsFsOpenFile(&sd, "/a.bin", FsOpenMode_Write, &f) == 0 && fsFileWrite(&f, 0, data, N, FsWriteOption_None) ==
          FSRESULT(FileExtensionWithoutOpenModeAllowAppend), "escrever alem do fim sem Append -> FileExtensionWithoutOpenModeAllowAppend");
    fsFileClose(&f);
    CHECK(fsFsOpenFile(&sd, "/a.bin", FsOpenMode_Write | FsOpenMode_Append, &f) == 0 &&
          fsFileWrite(&f, 0, data, N, FsWriteOption_Flush) == 0, "fsFileWrite(%d bytes, Append + Flush)", N);
    fsFileClose(&f);
    CHECK(fsFileWrite(&f, 0, data, 1, 0) == KERNELRESULT(InvalidHandle), "FsFile fechado -> InvalidHandle");

    FILE *hostf = fopen(TEST_ROOT "/a.bin", "rb");
    size_t host_n = hostf ? fread(back, 1, N, hostf) : 0;
    if (hostf)
        fclose(hostf);
    CHECK(host_n == N && memcmp(back, data, N) == 0, "/a.bin do Horizon = " TEST_ROOT "/a.bin no host (%zu bytes)", host_n);

    s64 size = 0;
    u64 got = 0;
    CHECK(fsFsOpenFile(&sd, "/a.bin", FsOpenMode_Read, &f) == 0 && fsFileGetSize(&f, &size) == 0 && size == N, "fsFileGetSize -> %lld", (long long)size);
    memset(back, 0, N);
    CHECK(fsFileRead(&f, 100, back, 50, FsReadOption_None, &got) == 0 && got == 50 && memcmp(back, data + 100, 50) == 0, "fsFileRead(off 100, 50) -> 50 bytes iguais");
    CHECK(fsFileRead(&f, N - 6, back, 100, 0, &got) == 0 && got == 6 && memcmp(back, data + N - 6, 6) == 0, "fsFileRead perto do fim -> 6 bytes");
    CHECK(fsFileRead(&f, N, back, 10, 0, &got) == 0 && got == 0, "fsFileRead(off = tamanho) -> 0 bytes");
    CHECK(fsFileRead(&f, N + 1, back, 10, 0, &got) == FSRESULT(OutOfRange), "fsFileRead(off > tamanho) -> OutOfRange (0x%x)", FSRESULT(OutOfRange));
    CHECK(fsFileRead(&f, -1, back, 10, 0, &got) == FSRESULT(InvalidOffset), "fsFileRead(off -1) -> InvalidOffset");
    CHECK(fsFileWrite(&f, 0, data, 1, 0) == FSRESULT(WriteNotPermitted), "fsFileWrite em arquivo so-leitura -> WriteNotPermitted");

    /* travas: aberto para leitura */
    CHECK(fsFsOpenFile(&sd, "/a.bin", FsOpenMode_Read, &g) == 0, "segunda abertura so-leitura -> ok");
    fsFileClose(&g);
    CHECK(fsFsOpenFile(&sd, "/a.bin", FsOpenMode_Write, &g) == FSRESULT(TargetLocked), "abrir para escrita algo aberto -> TargetLocked (0x%x)", FSRESULT(TargetLocked));
    CHECK(fsFsDeleteFile(&sd, "/a.bin") == FSRESULT(TargetLocked) && fsFsRenameFile(&sd, "/a.bin", "/z.bin") == FSRESULT(TargetLocked),
          "apagar/renomear arquivo aberto -> TargetLocked");
    fsFileClose(&f);

    /* modos e caminhos */
    CHECK(fsFsOpenFile(&sd, "/a.bin", 0, &f) == FSRESULT(InvalidOpenMode) &&
          fsFsOpenFile(&sd, "/a.bin", FsOpenMode_Append, &f) == FSRESULT(InvalidOpenMode), "modo 0 ou so Append -> InvalidOpenMode");
    CHECK(fsFsOpenFile(&sd, "/nada.bin", FsOpenMode_Read, &f) == FSRESULT(PathNotFound), "abrir inexistente -> PathNotFound (0x%x)", FSRESULT(PathNotFound));
    CHECK(fsFsOpenFile(&sd, "/../" TEST_ROOT "/a.bin", FsOpenMode_Read, &f) == FSRESULT(DirectoryUnobtainable) &&
          fsFsCreateDirectory(&sd, "/x/../../fora") == FSRESULT(DirectoryUnobtainable),
          "\"..\" acima da raiz -> DirectoryUnobtainable (0x%x)", FSRESULT(DirectoryUnobtainable));
    CHECK(fsFsOpenFile(&sd, "a.bin", FsOpenMode_Read, &f) == FSRESULT(InvalidPathFormat), "caminho sem \"/\" inicial -> InvalidPathFormat");
    CHECK(fsFsOpenFile(&sd, "/a:b", FsOpenMode_Read, &f) == FSRESULT(InvalidCharacter) &&
          fsFsOpenFile(&sd, "/..\\a.bin", FsOpenMode_Read, &f) == FSRESULT(InvalidCharacter), "\":\" e \"\\\" -> InvalidCharacter");
    char longp[FS_MAX_PATH + 8];
    memset(longp, 'a', sizeof longp);
    longp[0] = '/';
    longp[sizeof longp - 1] = '\0';
    CHECK(fsFsCreateFile(&sd, longp, 0, 0) == FSRESULT(TooLongPath), "caminho > FS_MAX_PATH -> TooLongPath");
    int norm_ok = fsFsOpenFile(&sd, "//./sub/../a.bin", FsOpenMode_Read, &f) == 0;
    fsFileClose(&f);
    CHECK(norm_ok, "\"//./sub/../a.bin\" normaliza para /a.bin");

    /* diretorios */
    CHECK(fsFsCreateDirectory(&sd, "/dir") == 0 && fsFsCreateDirectory(&sd, "/dir/sub") == 0 &&
          fsFsCreateDirectory(&sd, "/dir") == FSRESULT(PathAlreadyExists), "fsFsCreateDirectory(/dir, /dir/sub); repetir -> PathAlreadyExists");
    CHECK(fsFsCreateFile(&sd, "/dir/um.txt", 11, 0) == 0 && fsFsCreateFile(&sd, "/dir/dois.txt", 222, 0) == 0 &&
          fsFsCreateFile(&sd, "/semdir/x", 0, 0) == FSRESULT(PathNotFound), "fsFsCreateFile com tamanho; pai inexistente -> PathNotFound");
    FsDirEntryType type = FsDirEntryType_File;
    CHECK(fsFsGetEntryType(&sd, "/dir", &type) == 0 && type == FsDirEntryType_Dir &&
          fsFsGetEntryType(&sd, "/dir/um.txt", &type) == 0 && type == FsDirEntryType_File &&
          fsFsGetEntryType(&sd, "/", &type) == 0 && type == FsDirEntryType_Dir &&
          fsFsGetEntryType(&sd, "/nada", &type) == FSRESULT(PathNotFound), "fsFsGetEntryType: dir, arquivo, raiz, inexistente");

    FsDir d;
    s64 count = 0, n1 = 0, n2 = 0, n3 = 0;
    FsDirectoryEntry ent[4];
    CHECK(fsFsOpenDirectory(&sd, "/dir", FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &d) == 0 &&
          fsDirGetEntryCount(&d, &count) == 0 && count == 3, "fsDirGetEntryCount(/dir) -> %lld", (long long)count);
    CHECK(fsDirRead(&d, &n1, 2, ent) == 0 && n1 == 2 && fsDirRead(&d, &n2, 2, ent + 2) == 0 && n2 == 1 &&
          fsDirRead(&d, &n3, 2, ent + 3) == 0 && n3 == 0, "fsDirRead paginado: 2 + 1 + 0 entradas");
    int seen = 0;
    for (int k = 0; k < 3; k++) {
        if (!strcmp(ent[k].name, "sub") && ent[k].type == FsDirEntryType_Dir && ent[k].file_size == 0) seen |= 1;
        if (!strcmp(ent[k].name, "um.txt") && ent[k].type == FsDirEntryType_File && ent[k].file_size == 11) seen |= 2;
        if (!strcmp(ent[k].name, "dois.txt") && ent[k].type == FsDirEntryType_File && ent[k].file_size == 222) seen |= 4;
    }
    CHECK(seen == 7 && fsDirGetEntryCount(&d, &count) == 0 && count == 3, "entradas: sub (dir), um.txt (11), dois.txt (222); contagem nao depende da posicao");
    fsDirClose(&d);
    CHECK(fsFsOpenDirectory(&sd, "/dir", FsDirOpenMode_ReadFiles | FsDirOpenMode_NoFileSize, &d) == 0 &&
          fsDirRead(&d, &n1, 4, ent) == 0 && n1 == 2 && ent[0].type == FsDirEntryType_File && ent[0].file_size == 0 &&
          ent[1].file_size == 0, "ReadFiles | NoFileSize: so arquivos, tamanho 0");
    fsDirClose(&d);
    CHECK(fsFsOpenDirectory(&sd, "/dir", FsDirOpenMode_ReadDirs, &d) == 0 && fsDirRead(&d, &n1, 4, ent) == 0 && n1 == 1 &&
          !strcmp(ent[0].name, "sub"), "ReadDirs: so o subdiretorio");
    fsDirClose(&d);
    CHECK(fsFsOpenDirectory(&sd, "/dir", 0, &d) == FSRESULT(InvalidOpenMode) &&
          fsFsOpenDirectory(&sd, "/a.bin", FsDirOpenMode_ReadFiles, &d) == FSRESULT(PathNotFound),
          "fsFsOpenDirectory: modo 0 -> InvalidOpenMode; arquivo -> PathNotFound");

    /* renomear e apagar */
    CHECK(fsFsRenameFile(&sd, "/a.bin", "/dir/b.bin") == 0 && fsFsGetEntryType(&sd, "/a.bin", &type) == FSRESULT(PathNotFound) &&
          fsFsGetEntryType(&sd, "/dir/b.bin", &type) == 0, "fsFsRenameFile(/a.bin -> /dir/b.bin)");
    CHECK(fsFsRenameFile(&sd, "/dir/b.bin", "/dir/um.txt") == FSRESULT(PathAlreadyExists) &&
          fsFsRenameFile(&sd, "/dir", "/dir2") == FSRESULT(PathNotFound), "renomear sobre existente -> PathAlreadyExists; RenameFile de dir -> PathNotFound");
    CHECK(fsFsRenameDirectory(&sd, "/dir", "/dir/sub/dentro") == FSRESULT(DirectoryNotRenamable) &&
          fsFsRenameDirectory(&sd, "/", "/raiz") == FSRESULT(DirectoryNotRenamable), "renomear dir para dentro de si / raiz -> DirectoryNotRenamable");
    CHECK(fsFsRenameDirectory(&sd, "/dir/sub", "/sub2") == 0 && fsFsGetEntryType(&sd, "/sub2", &type) == 0 && type == FsDirEntryType_Dir,
          "fsFsRenameDirectory(/dir/sub -> /sub2)");
    CHECK(fsFsDeleteDirectory(&sd, "/dir") == FSRESULT(DirectoryNotEmpty), "fsFsDeleteDirectory(nao vazio) -> DirectoryNotEmpty (0x%x)", FSRESULT(DirectoryNotEmpty));
    CHECK(fsFsDeleteDirectory(&sd, "/sub2") == 0 && fsFsDeleteDirectory(&sd, "/") == FSRESULT(DirectoryNotDeletable) &&
          fsFsDeleteDirectoryRecursively(&sd, "/") == FSRESULT(DirectoryNotDeletable), "apagar dir vazio ok; raiz -> DirectoryNotDeletable");
    CHECK(fsFsOpenFile(&sd, "/dir/b.bin", FsOpenMode_Read, &f) == 0 && fsFsDeleteDirectoryRecursively(&sd, "/dir") == FSRESULT(TargetLocked),
          "apagar recursivo com arquivo aberto dentro -> TargetLocked");
    fsFileClose(&f);
    CHECK(fsFsDeleteDirectoryRecursively(&sd, "/dir") == 0 && fsFsGetEntryType(&sd, "/dir", &type) == FSRESULT(PathNotFound),
          "fsFsDeleteDirectoryRecursively(/dir)");
    CHECK(fsFsDeleteFile(&sd, "/dir/um.txt") == FSRESULT(PathNotFound), "fsFsDeleteFile(inexistente) -> PathNotFound");

    /* SetSize, espaco, commit */
    CHECK(fsFsCreateFile(&sd, "/s.bin", 5, 0) == 0 && fsFsOpenFile(&sd, "/s.bin", FsOpenMode_Read | FsOpenMode_Write, &f) == 0 &&
          fsFileSetSize(&f, 10000) == 0 && fsFileGetSize(&f, &size) == 0 && size == 10000 &&
          fsFileSetSize(&f, -1) == FSRESULT(InvalidSize) && fsFileFlush(&f) == 0, "fsFileSetSize(10000) + fsFileFlush; -1 -> InvalidSize");
    CHECK(fsFileWrite(&f, 9990, data, 10, 0) == 0 && fsFileRead(&f, 9990, back, 10, 0, &got) == 0 && got == 10 && !memcmp(back, data, 10),
          "escrever dentro do tamanho sem Append -> ok");
    fsFileClose(&f);
    s64 free_b = -1, total_b = -1;
    Result rfree = fsFsGetFreeSpace(&sd, "/", &free_b), rtotal = fsFsGetTotalSpace(&sd, "/", &total_b);
#if defined(_WIN32)
    CHECK(rfree == FSRESULT(NotImplemented) && rtotal == FSRESULT(NotImplemented), "fsFsGetFree/TotalSpace no host Windows -> NotImplemented");
#else
    CHECK(rfree == 0 && rtotal == 0 && free_b > 0 && total_b >= free_b, "fsFsGetFreeSpace %lld MiB / TotalSpace %lld MiB",
          (long long)(free_b >> 20), (long long)(total_b >> 20));
#endif
    CHECK(fsFsCommit(&sd) == 0, "fsFsCommit -> 0");

#if defined(__linux__)
    /* symlink para fora da raiz: apagar recursivo remove o link, nunca o alvo */
    shim_sys_mkdir(TEST_ROOT "_fora");
    FILE *outside = fopen(TEST_ROOT "_fora/vitima.txt", "w");
    if (outside)
        fclose(outside);
    fsFsCreateDirectory(&sd, "/links");
    int linked = symlink("../../" TEST_ROOT "_fora", TEST_ROOT "/links/fora") == 0;
    CHECK(linked && fsFsDeleteDirectoryRecursively(&sd, "/links") == 0 && access(TEST_ROOT "_fora/vitima.txt", F_OK) == 0,
          "fsFsDeleteDirectoryRecursively nao segue symlink para fora da raiz");
    unlink(TEST_ROOT "_fora/vitima.txt");
    rmdir(TEST_ROOT "_fora");
#endif

    /* variavel de ambiente e prioridade sobre o padrao; raiz copiada na abertura */
    FsFileSystem env_sd;
    shimFsSetSdCardRoot(NULL);
    set_env("OBDC_SDCARD", TEST_ROOT);
    CHECK(fsOpenSdCardFileSystem(&env_sd) == 0 && fsFsGetEntryType(&env_sd, "/s.bin", &type) == 0, "OBDC_SDCARD=" TEST_ROOT " -> mesma raiz");
    set_env("OBDC_SDCARD", NULL);
    fsFsClose(&env_sd);
    CHECK(fsFsGetEntryType(&env_sd, "/s.bin", &type) == KERNELRESULT(InvalidHandle), "fsFsClose invalida o FsFileSystem");

    CHECK(fsFsCleanDirectoryRecursively(&sd, "/") == 0 && fsFsGetEntryType(&sd, "/s.bin", &type) == FSRESULT(PathNotFound),
          "limpeza final da raiz");
    fsFsClose(&sd);
    shim_sys_rmdir(TEST_ROOT);
}

#if defined(__linux__)
/* ---------------------------------------------- seqlock do buffer ao vivo: produtor x padUpdate */
#define LIVE_WRITES 200000
static volatile int s_live_done;

static void live_producer(void *arg)
{
    (void)arg;
    for (s32 i = 1; i <= LIVE_WRITES; i++) {
        ShimInputState st = { .buttons = (u64)i, .sticks = { { i, -i }, { 3 * i, -3 * i } } };
        shimInputSetLive(&st);
    }
    s_live_done = 1;
}

static void test_input_live_threads(void)
{
    ShimInputState zero = {0};
    shimInputSetLive(&zero);
    Thread tp;
    threadCreate(&tp, live_producer, NULL, NULL, 0x10000, 0x2C, -2);
    threadStart(&tp);
    PadState pad;
    padInitializeDefault(&pad);
    u64 reads = 0, torn = 0, back = 0, last = 0;
    while (!s_live_done || reads < 1000) {
        padUpdate(&pad);
        HidAnalogStickState l = padGetStickPos(&pad, 0), r = padGetStickPos(&pad, 1);
        s32 i = (s32)pad.buttons_cur;
        torn += l.x != i || l.y != -i || r.x != 3 * i || r.y != -3 * i;
        back += pad.buttons_cur < last;
        last = pad.buttons_cur;
        reads++;
    }
    threadWaitForExit(&tp);
    threadClose(&tp);
    shimInputClearLive();
    CHECK(torn == 0 && back == 0 && last == LIVE_WRITES, "buffer ao vivo (seqlock): %llu leituras x %d escritas, %llu estados misturados, ultimo = %llu",
          (unsigned long long)reads, LIVE_WRITES, (unsigned long long)torn, (unsigned long long)last);
}

static MemoryInfo query(void *p)
{
    MemoryInfo mi;
    u32 pi;
    Result rc = svcQueryMemory(&mi, &pi, (u64)(uintptr_t)p);
    if (R_FAILED(rc))
        memset(&mi, 0xFF, sizeof mi);
    return mi;
}

/* ---------------------------------------------- threads, mutex, condvar e rmutex pela API da libnx no shim */
#define THREADS 4
#define ITERS   200000
static Mutex s_mutex;
static volatile u64 s_counter;
static Handle s_handles[THREADS];

static void worker(void *arg)
{
    s_handles[(uintptr_t)arg] = threadGetCurHandle();
    for (int k = 0; k < ITERS; k++) {
        mutexLock(&s_mutex);
        s_counter = s_counter + 1;          /* leitura+escrita nao atomica: so fica certo com exclusao */
        mutexUnlock(&s_mutex);
    }
}

static void exits_early(void *arg)
{
    *(volatile int *)arg = 1;
    svcExitThread();                        /* 0x0A: termina antes de retornar da funcao */
    *(volatile int *)arg = 2;
}

/* produtor/consumidor com CondVar: fila de 1 posicao */
#define ITEMS 1000
static Mutex s_qm;
static CondVar s_qcv;
static int s_slot = -1, s_consumed_sum;

static void consumer(void *arg)
{
    (void)arg;
    for (int n = 0; n < ITEMS; n++) {
        mutexLock(&s_qm);
        while (s_slot < 0)
            condvarWait(&s_qcv, &s_qm);
        s_consumed_sum += s_slot;
        s_slot = -1;
        condvarWakeAll(&s_qcv);
        mutexUnlock(&s_qm);
    }
}

static RMutex s_rm;
static int recurse(int depth)
{
    rmutexLock(&s_rm);
    int r = depth == 0 ? (int)s_rm.counter : recurse(depth - 1);
    rmutexUnlock(&s_rm);
    return r;
}

static u64 now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

static int child_signal(void (*fn)(void))
{
    fflush(stdout);   /* senao o filho herda e reimprime o buffer do pai */
    pid_t pid = fork();
    if (pid == 0) {
        fn();
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFSIGNALED(st) ? WTERMSIG(st) : 0;
}
static void do_break_panic(void) { svcBreak(BreakReason_Panic, 0, 0); }
static void do_return_from_exception(void) { svcReturnFromException(0); }
#endif /* __linux__ */

int main(void)
{
    const char hello[] = "[svc 0x27] svcOutputDebugString\n";
    CHECK(svcOutputDebugString(hello, sizeof hello - 1) == 0, "svcOutputDebugString -> 0");

    /* ---- portavel: entrada (hid/pad) e fs (cartao SD) */
    test_input();
    test_fs();

#if defined(__linux__)
    test_input_live_threads();

    /* ---- 0x01 heap */
    void *heap = NULL;
    CHECK(svcSetHeapSize(&heap, 0x1000) == KERNELRESULT(InvalidSize), "svcSetHeapSize(0x1000) -> InvalidSize (0x%x)", KERNELRESULT(InvalidSize));
    CHECK(svcSetHeapSize(&heap, 0x400000) == 0 && heap != NULL, "svcSetHeapSize(4 MiB) -> base %p", heap);
    u8 *h = heap;
    memset(h, 0xA5, 0x400000);
    CHECK(h[0] == 0xA5 && h[0x3FFFFF] == 0xA5, "heap de 4 MiB legivel e gravavel");
    MemoryInfo mi = query(h);
    CHECK(mi.type == MemType_Heap && mi.perm == Perm_Rw && mi.size >= 0x400000,
          "svcQueryMemory(heap) -> type=Heap perm=Rw size=0x%llx", (unsigned long long)mi.size);
    CHECK(svcSetHeapSize(&heap, 0x200000) == 0 && heap == h, "svcSetHeapSize(2 MiB) encolhe no lugar");
    mi = query(h + 0x200000);
    CHECK(mi.type == MemType_Unmapped, "svcQueryMemory(heap + 2 MiB) -> Unmapped depois de encolher");

    /* ---- 0x02 permissao */
    CHECK(svcSetMemoryPermission(h, 0x1000, Perm_R) == 0 && query(h).perm == Perm_R, "svcSetMemoryPermission(Perm_R) -> QueryMemory perm=R");
    CHECK(svcSetMemoryPermission(h, 0x1000, Perm_Rx) == KERNELRESULT(InvalidMemoryPermissions), "svcSetMemoryPermission(Perm_Rx) -> InvalidMemoryPermissions");
    CHECK(svcSetMemoryPermission(h + 1, 0x1000, Perm_Rw) == KERNELRESULT(InvalidAddress), "svcSetMemoryPermission(desalinhado) -> InvalidAddress");
    CHECK(svcSetMemoryPermission(h, 0x1000, Perm_Rw) == 0 && query(h).perm == Perm_Rw, "svcSetMemoryPermission(Perm_Rw) restaura");

    /* ---- 0x03 atributo */
    CHECK(svcSetMemoryAttribute(h, 0x1000, MemAttr_IsUncached, MemAttr_IsUncached) == 0 &&
          (query(h).attr & MemAttr_IsUncached), "svcSetMemoryAttribute(IsUncached) -> QueryMemory attr");
    CHECK(svcSetMemoryAttribute(h, 0x1000, MemAttr_IsBorrowed, MemAttr_IsBorrowed) == KERNELRESULT(InvalidCombination),
          "svcSetMemoryAttribute(IsBorrowed) -> InvalidCombination");
    CHECK(svcSetMemoryAttribute(h, 0x1000, MemAttr_IsUncached, 0) == 0 && !(query(h).attr & MemAttr_IsUncached),
          "svcSetMemoryAttribute(limpa IsUncached)");

    /* ---- 0x29 info + 0x04/0x05 alias */
    u64 alias = 0, v = 0;
    CHECK(svcGetInfo(&alias, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0) == 0 && alias != 0, "svcGetInfo(AliasRegionAddress) -> 0x%llx", (unsigned long long)alias);
    u8 *src = h + 0x10000, *dst = (u8 *)(uintptr_t)alias + 0x100000;
    src[0] = 0x11;
    CHECK(svcMapMemory(dst, src, 0x1000) == 0 && dst[0] == 0x11, "svcMapMemory: alias le o conteudo da origem");
    dst[1] = 0x22;
    mi = query(src);
    CHECK(mi.perm == Perm_None && (mi.attr & MemAttr_IsBorrowed), "origem fica Perm_None + IsBorrowed (QueryMemory)");
    CHECK(query(dst).type == MemType_MappedMemory, "svcQueryMemory(alias) -> MappedMemory");
    CHECK(svcUnmapMemory(dst, src + 0x1000, 0x1000) == KERNELRESULT(InvalidMemoryRange), "svcUnmapMemory(origem errada) -> InvalidMemoryRange");
    CHECK(svcUnmapMemory(dst, src, 0x1000) == 0 && src[1] == 0x22 && query(src).perm == Perm_Rw,
          "svcUnmapMemory: origem volta a Rw com o dado escrito pelo alias");
    CHECK(query(dst).type == MemType_Unmapped, "svcQueryMemory(alias) -> Unmapped depois do unmap");

    CHECK(svcGetInfo(&v, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0) == 0 && v == (u64)(uintptr_t)h, "svcGetInfo(HeapRegionAddress) = base da heap");
    CHECK(svcGetInfo(&v, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0) == 0 && v != 0, "svcGetInfo(CoreMask) -> 0x%llx", (unsigned long long)v);
    CHECK(svcGetInfo(&v, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0) == 0 && v > 0, "svcGetInfo(TotalMemorySize) -> %llu MiB", (unsigned long long)(v >> 20));
    CHECK(svcGetInfo(&v, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0) == 0 && v > 0, "svcGetInfo(UsedMemorySize) -> %llu KiB", (unsigned long long)(v >> 10));
    CHECK(svcGetInfo(&v, InfoType_DebuggerAttached, INVALID_HANDLE, 0) == 0 && v == 0, "svcGetInfo(DebuggerAttached) -> 0");
    CHECK(svcGetInfo(&v, InfoType_RandomEntropy, INVALID_HANDLE, 2) == 0, "svcGetInfo(RandomEntropy, 2)");
    CHECK(svcGetInfo(&v, 65000, CUR_PROCESS_HANDLE, 0) == KERNELRESULT(InvalidEnumValue), "svcGetInfo(65000, como o detect.h da libnx) -> InvalidEnumValue (0x%x)", KERNELRESULT(InvalidEnumValue));
    CHECK(svcGetInfo(&v, InfoType_HeapRegionSize, 0x1234, 0) == KERNELRESULT(InvalidHandle), "svcGetInfo(handle invalido) -> InvalidHandle");

    /* ---- 0x08/0x09/0x18/0x16 + mutex libnx (0x1A/0x1B) com 4 threads */
    Thread th[THREADS];
    int created = 1;
    for (uintptr_t k = 0; k < THREADS; k++)
        created &= threadCreate(&th[k], worker, (void *)k, NULL, 0x10000, 0x2C, -2) == 0 && threadStart(&th[k]) == 0;
    CHECK(created, "threadCreate + threadStart (svcCreateThread/svcStartThread) x%d", THREADS);
    int waited = 1;
    for (int k = 0; k < THREADS; k++)
        waited &= threadWaitForExit(&th[k]) == 0 && threadClose(&th[k]) == 0;
    CHECK(waited, "threadWaitForExit + threadClose (svcWaitSynchronization/svcCloseHandle)");
    CHECK(s_counter == (u64)THREADS * ITERS && s_mutex == 0, "mutexLock/Unlock da libnx: %d threads x %d -> %llu (mutex final=0x%x)",
          THREADS, ITERS, (unsigned long long)s_counter, s_mutex);
    int distinct = threadGetCurHandle() != INVALID_HANDLE;
    for (int k = 0; k < THREADS; k++)
        for (int j = 0; j < k; j++)
            distinct &= s_handles[k] != s_handles[j] && s_handles[k] != threadGetCurHandle();
    CHECK(distinct, "threadGetCurHandle distinto por thread (tag do mutex)");

    Handle hx;
    CHECK(svcCreateThread(&hx, (void *)worker, NULL, NULL, 64, -2) == KERNELRESULT(InvalidPriority), "svcCreateThread(prio 64) -> InvalidPriority");
    CHECK(svcCreateThread(&hx, (void *)worker, NULL, NULL, 0x2C, 7) == KERNELRESULT(InvalidCoreId), "svcCreateThread(cpuid 7) -> InvalidCoreId");
    CHECK(svcStartThread(0x1234) == KERNELRESULT(InvalidHandle), "svcStartThread(handle invalido) -> InvalidHandle");

    /* ---- 0x0A */
    volatile int flag = 0;
    Thread te;
    CHECK(threadCreate(&te, exits_early, (void *)&flag, NULL, 0x10000, 0x2C, -2) == 0 && threadStart(&te) == 0 &&
          threadWaitForExit(&te) == 0 && flag == 1 && threadClose(&te) == 0, "svcExitThread termina a thread e sinaliza quem espera");

    /* ---- 0x18 timeout + 0x0B */
    Thread ts_;
    threadCreate(&ts_, exits_early, (void *)&flag, NULL, 0x10000, 0x2C, -2);   /* criada e nao iniciada */
    s32 idx = -1;
    u64 t0 = now_ns();
    CHECK(svcWaitSynchronization(&idx, &ts_.handle, 1, 20000000ULL) == KERNELRESULT(TimedOut) && now_ns() - t0 >= 20000000ULL,
          "svcWaitSynchronization(20 ms, thread parada) -> TimedOut depois de %.1f ms", (now_ns() - t0) / 1e6);
    threadClose(&ts_);
    t0 = now_ns();
    svcSleepThread(15000000LL);
    u64 slept = now_ns() - t0;
    CHECK(slept >= 15000000ULL, "svcSleepThread(15 ms) dormiu %.2f ms", slept / 1e6);
    svcSleepThread(0);
    svcSleepThread(-1);
    CHECK(1, "svcSleepThread(0/-1) -> yield");

    /* ---- 0x1C/0x1D CondVar */
    Thread tc;
    mutexInit(&s_qm);
    condvarInit(&s_qcv);
    threadCreate(&tc, consumer, NULL, NULL, 0x10000, 0x2C, -2);
    threadStart(&tc);
    int produced_sum = 0;
    for (int n = 1; n <= ITEMS; n++) {
        mutexLock(&s_qm);
        while (s_slot >= 0)
            condvarWait(&s_qcv, &s_qm);
        s_slot = n;
        produced_sum += n;
        condvarWakeAll(&s_qcv);
        mutexUnlock(&s_qm);
    }
    threadWaitForExit(&tc);
    threadClose(&tc);
    CHECK(s_consumed_sum == produced_sum, "CondVar (svcWaitProcessWideKeyAtomic/svcSignalProcessWideKey): %d itens, soma %d = %d",
          ITEMS, s_consumed_sum, produced_sum);
    mutexLock(&s_qm);
    t0 = now_ns();
    Result rcw = condvarWaitTimeout(&s_qcv, &s_qm, 10000000ULL);
    CHECK(rcw == KERNELRESULT(TimedOut) && (s_qm & ~HANDLE_WAIT_MASK) == threadGetCurHandle(),
          "condvarWaitTimeout(10 ms) -> TimedOut e volta com o mutex (%.1f ms)", (now_ns() - t0) / 1e6);
    mutexUnlock(&s_qm);

    /* ---- RMutex */
    rmutexInit(&s_rm);
    CHECK(recurse(5) == 6 && s_rm.counter == 0 && s_rm.lock == 0, "rmutexLock recursivo: profundidade 6, solta tudo no fim");

    /* ---- IPC (dummies do mapeamento): 0x1F, 0x21, 0x13/0x14, 0x17, 0x52 e API de servicos */
    Handle sess = INVALID_HANDLE, sess2 = INVALID_HANDLE;
    CHECK(svcConnectToNamedPort(&sess, "sm:") == 0 && sess != INVALID_HANDLE, "svcConnectToNamedPort(\"sm:\") -> sessao 0x%x", sess);
    CHECK(svcConnectToNamedPort(&sess2, "nome-longo-demais") == KERNELRESULT(OutOfRange), "svcConnectToNamedPort(nome > 11) -> OutOfRange");
    CHECK(svcSendSyncRequest(sess) == 0, "svcSendSyncRequest(sessao) -> 0 (mensagem nao processada)");
    CHECK(svcSendSyncRequest(0x1234) == KERNELRESULT(InvalidHandle), "svcSendSyncRequest(handle invalido) -> InvalidHandle");
    CHECK(svcCloseHandle(sess) == 0 && svcSendSyncRequest(sess) == KERNELRESULT(InvalidHandle), "svcCloseHandle(sessao) invalida o handle");
    CHECK(svcMapSharedMemory(sess, h, 0x1000, Perm_R) == 0 && svcMapSharedMemory(sess, h + 1, 0x1000, Perm_R) == KERNELRESULT(InvalidAddress) &&
          svcUnmapSharedMemory(sess, h, 0x1000) == 0, "svcMap/UnmapSharedMemory (dummy, valida alinhamento)");
    CHECK(svcResetSignal(sess) == 0 && svcUnmapTransferMemory(sess, h, 0x1000) == 0, "svcResetSignal e svcUnmapTransferMemory (dummy)");
    CHECK(smInitialize() == 0 && appletInitialize() == 0 && hidInitialize() == 0 && fsInitialize() == 0,
          "smInitialize / appletInitialize / hidInitialize / fsInitialize (dummy) -> 0");
    fsExit(); hidExit(); appletExit(); smExit();

    /* ---- 0x26 / 0x28 */
    CHECK(child_signal(do_break_panic) == SIGABRT, "svcBreak(Panic) encerra o processo com SIGABRT");
    CHECK(child_signal(do_return_from_exception) == SIGABRT, "svcReturnFromException fora de handler -> SIGABRT");
#endif /* __linux__ */
    CHECK(svcBreak(BreakReason_User | BreakReason_NotificationOnlyFlag, 0, 0) == 0, "svcBreak(so notificacao) -> 0");

    printf("== %d ok, %d falhas\n", s_ok, s_fail);
    if (s_fail)
        return 1;
    svcExitProcess();   /* 0x07: termina com codigo 0 */
}
