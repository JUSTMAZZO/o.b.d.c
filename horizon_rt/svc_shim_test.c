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
 * O teste de fs cria e apaga ./obdc_sdcard_test e ./obdc_romfs_test no diretorio atual.
 */
#include <stddef.h>
#include <pthread.h>
#include <sched.h>
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

/* ---------------------------------------------- romfs:/ e sdmc:/ pelo stdio (fsdev, romfs, macros de redirecionamento) */
#define TEST_ROMFS "obdc_romfs_test"

static size_t slurp(FILE *f, char *buf, size_t cap)
{
    size_t n = f ? fread(buf, 1, cap - 1, f) : 0;
    buf[n] = '\0';
    if (f)
        fclose(f);
    return n;
}

static void test_devices(void)
{
    char buf[64];
    struct stat st;

    /* assets do romfs criados pelo caminho do host (sem dispositivo: passa direto) */
    shim_sys_mkdir(TEST_ROMFS);
    shim_sys_mkdir(TEST_ROMFS "/data");
    FILE *w = fopen(TEST_ROMFS "/data/a.txt", "w");
    if (w) { fputs("asset romfs", w); fclose(w); }
    w = fopen(TEST_ROMFS "/data/crlf.txt", "wb");
    if (w) { fputs("a\r\nb", w); fclose(w); }

    CHECK(shimRomfsSetRoot("obdc_romfs_nao_existe") == 0 && romfsInit() == LIBNXRESULT(NotFound),
          "romfsInit(raiz inexistente) -> LibnxError_NotFound (0x%x)", LIBNXRESULT(NotFound));
    CHECK(shimRomfsSetRoot(TEST_ROMFS "/") == 0 && romfsInit() == 0, "romfsInit -> romfs:/ = " TEST_ROMFS);
    CHECK(romfsInit() == LIBNXRESULT(AlreadyMapped), "romfsInit de novo -> LibnxError_AlreadyMapped");

    CHECK(slurp(fopen("romfs:/data/a.txt", "r"), buf, sizeof buf) == 11 && !strcmp(buf, "asset romfs"), "fopen(\"romfs:/data/a.txt\") le o asset");
    CHECK(slurp(fopen("romfs:data/./a.txt", "r"), buf, sizeof buf) == 11, "\"romfs:data/./a.txt\" (sem \"/\" inicial) -> mesmo arquivo");
    CHECK(slurp(fopen("romfs:/data/crlf.txt", "r"), buf, sizeof buf) == 4, "modo \"r\" sem traducao de CRLF (4 bytes, como o newlib)");
    CHECK(stat("romfs:/data/a.txt", &st) == 0 && st.st_size == 11 && stat("romfs:/data", &st) == 0 && S_ISDIR(st.st_mode),
          "stat(romfs:/data/a.txt) -> 11 bytes; romfs:/data -> diretorio");
    DIR *dir = opendir("romfs:/data");
    int entries = 0;
    struct dirent *e;
    while (dir && (e = readdir(dir)) != NULL)
        entries += strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
    if (dir)
        closedir(dir);
    CHECK(dir && entries == 2, "opendir(romfs:/data) -> %d entradas", entries);
    CHECK(access("romfs:/data/a.txt", F_OK) == 0 && access("romfs:/nada", F_OK) != 0, "access(romfs:/...) F_OK");

    errno = 0;
    CHECK(fopen("romfs:/novo.txt", "w") == NULL && errno == EROFS, "fopen(romfs:/..., \"w\") -> EROFS");
    errno = 0;
    CHECK(fopen("romfs:/data/a.txt", "r+") == NULL && errno == EROFS, "fopen(romfs:/..., \"r+\") -> EROFS");
    CHECK(mkdir("romfs:/d", 0777) != 0 && errno == EROFS && remove("romfs:/data/a.txt") != 0 && errno == EROFS &&
          rename("romfs:/data/a.txt", "romfs:/b.txt") != 0 && errno == EROFS && access("romfs:/data/a.txt", W_OK) != 0 && errno == EROFS,
          "mkdir/remove/rename/access(W_OK) no romfs -> EROFS");
    errno = 0;
    CHECK(fopen("romfs:/../" TEST_ROMFS "/data/a.txt", "r") == NULL && errno == EACCES &&
          stat("romfs:/data/../../" TEST_ROOT, &st) != 0 && errno == EACCES, "\"..\" acima da raiz do romfs -> EACCES");
    errno = 0;
    CHECK(fopen("romfs:/a\\..\\..\\x", "r") == NULL && errno == EINVAL, "\"\\\" no caminho do romfs -> EINVAL");
    errno = 0;
    CHECK(fopen("nada:/x", "r") == NULL && errno == ENODEV, "dispositivo desconhecido -> ENODEV");

    /* fsdev: sdmc:/ sobre o cartao SD do shim */
    shim_sys_mkdir(TEST_ROOT);
    shimFsSetSdCardRoot(TEST_ROOT);
    CHECK(fsdevMountSdmc() == 0 && fsdevGetDeviceFileSystem("sdmc") != NULL, "fsdevMountSdmc -> sdmc:/ = " TEST_ROOT);
    CHECK(fsdevMountSdmc() == LIBNXRESULT(AlreadyMapped), "fsdevMountSdmc de novo -> AlreadyMapped");
    w = fopen("sdmc:/f.bin", "w");
    size_t wrote = w ? fwrite("0123456789", 1, 10, w) : 0;
    if (w)
        fclose(w);
    FsDirEntryType type;
    FsFile ff;
    s64 fsize = 0;
    FsFileSystem *dev = fsdevGetDeviceFileSystem("sdmc:");
    int fs_ok = wrote == 10 && dev && fsFsGetEntryType(dev, "/f.bin", &type) == 0 && type == FsDirEntryType_File &&
                fsFsOpenFile(dev, "/f.bin", FsOpenMode_Read, &ff) == 0 && fsFileGetSize(&ff, &fsize) == 0 && fsize == 10;
    fsFileClose(&ff);
    CHECK(fs_ok, "fwrite em sdmc:/f.bin aparece no fs (fsdevGetDeviceFileSystem): 10 bytes");
    FILE *hf = fopen(TEST_ROOT "/f.bin", "rb");
    CHECK(slurp(hf, buf, sizeof buf) == 10 && !strcmp(buf, "0123456789"), "sdmc:/f.bin = " TEST_ROOT "/f.bin no host");

    char out[FS_MAX_PATH];
    FsFileSystem *tdev = NULL;
    CHECK(fsdevTranslatePath("sdmc:/x/../f.bin", &tdev, out) == 0 && !strcmp(out, "/f.bin") && tdev == dev &&
          fsdevTranslatePath("f.bin", NULL, out) == 0 && !strcmp(out, "/f.bin") &&
          fsdevTranslatePath("sdmc:", NULL, out) == 0 && !strcmp(out, "/"), "fsdevTranslatePath: sdmc:/x/../f.bin, relativo e raiz");
    errno = 0;
    CHECK(fsdevTranslatePath("romfs:/data/a.txt", NULL, out) == -1 && errno == ENODEV &&
          fsdevGetDeviceFileSystem("romfs") == NULL, "romfs nao e fsdev: fsdevTranslatePath -> ENODEV, sem FsFileSystem");
    CHECK(fsdevCommitDevice("sdmc") == 0 && fsdevCommitDevice("nada") == LIBNXRESULT(NotFound), "fsdevCommitDevice");

    CHECK(mkdir("sdmc:/d", 0777) == 0 && rename("sdmc:/f.bin", "sdmc:/d/g.bin") == 0 && stat("sdmc:/d/g.bin", &st) == 0 &&
          st.st_size == 10 && access("sdmc:/d/g.bin", W_OK) == 0 && remove("sdmc:/d/g.bin") == 0 && rmdir("sdmc:/d") == 0 &&
          stat("sdmc:/d", &st) != 0, "mkdir/rename/stat/access/remove/rmdir em sdmc:/");
    errno = 0;
    CHECK(fopen("sdmc:/../fora.txt", "w") == NULL && errno == EACCES, "\"..\" acima da raiz do sdmc -> EACCES");

    FsFileSystem extra;
    fsOpenSdCardFileSystem(&extra);
    errno = 0;
    CHECK(fsdevMountDevice("a:b", extra) == -1 && errno == EINVAL && fsFsCommit(&extra) == KERNELRESULT(InvalidHandle),
          "fsdevMountDevice(nome com \":\") -> -1 e fecha o fs, como na libnx");
    fsOpenSdCardFileSystem(&extra);
    CHECK(fsdevMountDevice("save", extra) >= 0 && access("save:/", F_OK) == 0, "fsdevMountDevice(\"save\") -> save:/");

    CHECK(fsdevUnmountAll() == 0 && fsdevGetDeviceFileSystem("sdmc") == NULL && fsdevGetDeviceFileSystem("save") == NULL &&
          access("romfs:/data/a.txt", F_OK) == 0, "fsdevUnmountAll desmonta sdmc e save, mantem romfs");
    errno = 0;
    CHECK(fopen("sdmc:/x", "r") == NULL && errno == ENODEV && fsdevUnmountDevice("sdmc") == -1, "sdmc:/ desmontado -> ENODEV");
    CHECK(romfsExit() == 0 && romfsExit() == LIBNXRESULT(NotFound) && access("romfs:/data/a.txt", F_OK) != 0, "romfsExit; de novo -> NotFound");
    shimRomfsSetRoot(NULL);
    shimFsSetSdCardRoot(NULL);

    remove(TEST_ROMFS "/data/a.txt");
    remove(TEST_ROMFS "/data/crlf.txt");
    rmdir(TEST_ROMFS "/data");
    rmdir(TEST_ROMFS);
    rmdir(TEST_ROOT);
}

/* ---------------------------------------------- time:u, ciclo de vida do applet e framebuffer linear */
static s64 days_from_civil(s64 y, u32 m, u32 d)   /* dias desde 1970-01-01 (algoritmo de H. Hinnant) */
{
    y -= m <= 2;
    s64 era = (y >= 0 ? y : y - 399) / 400;
    u32 yoe = (u32)(y - era * 400);
    u32 doy = (153 * (m + (m > 2 ? (u32)-3 : 9)) + 2) / 5 + d - 1;
    u32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (s64)doe - 719468;
}

static void test_time(void)
{
    u64 t = 0, us = 0;
    CHECK(timeGetCurrentTime(TimeType_Default, &t) == LIBNXRESULT(NotInitialized), "timeGetCurrentTime sem timeInitialize -> NotInitialized (0x%x)", LIBNXRESULT(NotInitialized));
    CHECK(timeInitialize() == 0, "timeInitialize");
    u64 t0 = (u64)time(NULL);
    Result rc = timeGetCurrentTime(TimeType_Default, &t);
    u64 t1 = (u64)time(NULL);
    CHECK(rc == 0 && t >= t0 && t <= t1, "timeGetCurrentTime(Default) = time(NULL) (%llu)", (unsigned long long)t);
    u64 tu = 0, tn = 0, tl = 0;
    CHECK(timeGetCurrentTime(TimeType_UserSystemClock, &tu) == 0 && timeGetCurrentTime(TimeType_NetworkSystemClock, &tn) == 0 &&
          timeGetCurrentTime(TimeType_LocalSystemClock, &tl) == 0 && tn - tu <= 1 && tl - tu <= 1, "UserSystemClock / NetworkSystemClock / LocalSystemClock coerentes");
    CHECK(timeGetCurrentTime((TimeType)3, &t) == LIBNXRESULT(BadInput), "TimeType invalido -> BadInput");
    u64 s0 = 0, s1 = 0;
    timeGetCurrentTime(TimeType_Default, &s0);
    shimTimeGetCurrentTimeUs(TimeType_Default, &us);
    timeGetCurrentTime(TimeType_Default, &s1);
    CHECK(us / 1000000ULL >= s0 && us / 1000000ULL <= s1, "shimTimeGetCurrentTimeUs: %llu us -> %llu s (entre %llu e %llu)",
          (unsigned long long)us, (unsigned long long)(us / 1000000ULL), (unsigned long long)s0, (unsigned long long)s1);
    u64 us2 = 0;
    shimTimeGetCurrentTimeUs(TimeType_Default, &us2);
    CHECK(us2 >= us, "microssegundos nao voltam (%llu -> %llu)", (unsigned long long)us, (unsigned long long)us2);

    TimeSteadyClockTimePoint p0, p1;
    CHECK(timeGetStandardSteadyClockTimePoint(&p0) == 0 && timeGetStandardSteadyClockTimePoint(&p1) == 0 && p1.time_point >= p0.time_point &&
          !memcmp(p0.source_id.uuid, p1.source_id.uuid, 16), "timeGetStandardSteadyClockTimePoint monotonico, mesma source_id");

    /* 1700000000 = 2023-11-14 22:13:20 UTC; o calendario local menos o offset tem de voltar ao mesmo instante */
    TimeCalendarTime cal;
    TimeCalendarAdditionalInfo info;
    CHECK(timeToCalendarTimeWithMyRule(1700000000ULL, &cal, &info) == 0, "timeToCalendarTimeWithMyRule(1700000000) -> %04u-%02u-%02u %02u:%02u:%02u %s (UTC%+d s)",
          cal.year, cal.month, cal.day, cal.hour, cal.minute, cal.second, info.timezoneName, info.offset);
    s64 local = days_from_civil(cal.year, cal.month, cal.day) * 86400 + cal.hour * 3600 + cal.minute * 60 + cal.second;
    CHECK(local - info.offset == 1700000000LL, "calendario local - offset = 1700000000 (dias pelo algoritmo civil)");
    s64 wday_ref = (days_from_civil(cal.year, cal.month, cal.day) % 7 + 11) % 7;   /* 1970-01-01 = quinta (4) */
    CHECK((s64)info.wday == wday_ref && info.yday == (u32)(days_from_civil(cal.year, cal.month, cal.day) - days_from_civil(cal.year, 1, 1)),
          "wday = %u, yday = %u", info.wday, info.yday);
    u64 back[2] = {0};
    s32 nback = 0;
    CHECK(timeToPosixTimeWithMyRule(&cal, back, 2, &nback) == 0 && nback == 1 && back[0] == 1700000000ULL, "timeToPosixTimeWithMyRule volta a 1700000000");
    timeExit();
    CHECK(timeGetCurrentTime(TimeType_Default, &t) == LIBNXRESULT(NotInitialized), "timeExit -> NotInitialized de novo");
}

static int s_hook_calls[AppletHookType_Max], s_hook_order[8], s_hook_n;
static void hook_a(AppletHookType type, void *param) { (void)param; s_hook_calls[type]++; if (s_hook_n < 8) s_hook_order[s_hook_n++] = 1; }
static void hook_b(AppletHookType type, void *param) { (void)type; if (s_hook_n < 8) s_hook_order[s_hook_n++] = 2; *(int *)param += 1; }

static void test_applet(void)
{
    shimAppletReset();
    CHECK(appletGetFocusState() == AppletFocusState_InFocus && appletGetOperationMode() == AppletOperationMode_Handheld,
          "appletGetFocusState = InFocus, appletGetOperationMode = Handheld");
    bool apd = true;
    CHECK(appletIsAutoPowerDownRequested(&apd) == 0 && !apd, "appletIsAutoPowerDownRequested -> false");
    shimAppletSetAutoPowerDownRequested(true);
    CHECK(appletIsAutoPowerDownRequested(&apd) == 0 && apd, "pedido de auto power down simulado -> true");
    CHECK(appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend) == 0 && shimAppletGetFocusHandlingMode() == AppletFocusHandlingMode_NoSuspend &&
          appletSetFocusHandlingMode(AppletFocusHandlingMode_Max) == LIBNXRESULT(BadInput), "appletSetFocusHandlingMode; Max -> BadInput");

    AppletHookCookie ca, cb;
    int b_calls = 0;
    appletHook(&ca, hook_a, NULL);
    appletHook(&cb, hook_b, &b_calls);
    CHECK(appletMainLoop() && s_hook_n == 0, "sem mensagens: appletMainLoop true, nenhum hook");
    shimAppletPostFocusState(AppletFocusState_Background);
    shimAppletPostOperationMode(AppletOperationMode_Console);
    shimAppletPostOperationMode(AppletOperationMode_Console);     /* repetido: sem mudanca, sem hook */
    CHECK(appletGetFocusState() == AppletFocusState_InFocus, "mensagem so vale no proximo appletMainLoop");
    CHECK(appletMainLoop() && appletGetFocusState() == AppletFocusState_Background && appletGetOperationMode() == AppletOperationMode_Console &&
          s_hook_calls[AppletHookType_OnFocusState] == 1 && s_hook_calls[AppletHookType_OnOperationMode] == 1 && b_calls == 2,
          "foco Background + modo Console entregues com hooks (1 + 1)");
    CHECK(s_hook_n == 4 && s_hook_order[0] == 2 && s_hook_order[1] == 1, "ordem dos hooks: o ultimo registrado primeiro (como na libnx)");
    shimAppletPostFocusState(AppletFocusState_InFocus);
    CHECK(appletMainLoop() && appletGetFocusState() == AppletFocusState_InFocus && s_hook_calls[AppletHookType_OnFocusState] == 2, "foco volta a InFocus");

    appletUnhook(&cb);
    appletLockExit();
    u32 frame_before = g_shim_frame;
    shimAppletRequestExit();
    CHECK(!appletMainLoop() && s_hook_calls[AppletHookType_OnExitRequest] == 1 && b_calls == 3 && g_shim_frame == frame_before,
          "pedido de saida: appletMainLoop false, OnExitRequest so no hook ainda registrado, quadro nao avanca");
    CHECK(!appletMainLoop() && !appletMainLoop() && s_hook_calls[AppletHookType_OnExitRequest] == 1, "depois do pedido continua false (termino limpo do laco)");
    CHECK(shimAppletIsExitLocked() && appletUnlockExit() == 0 && !shimAppletIsExitLocked(), "appletLockExit / appletUnlockExit");
    appletUnhook(&ca);
    shimAppletReset();
    u32 frames = 0;
    while (appletMainLoop())
        frames++;
    CHECK(frames == SHIM_FRAMES && s_hook_calls[AppletHookType_OnExitRequest] == 1, "shimAppletReset: laco volta a %u quadros, sem hooks", frames);
    shimAppletReset();
}

static u64 fnv_rows(const u8 *p, u32 stride, u32 row_bytes, u32 rows)
{
    u64 h = 0xcbf29ce484222325ULL;
    for (u32 y = 0; y < rows; y++)
        for (u32 k = 0; k < row_bytes; k++)
            h = (h ^ p[(size_t)y * stride + k]) * 0x100000001b3ULL;
    return h;
}

static void test_framebuffer(void)
{
    Framebuffer fb, fb2;
    NWindow *win = nwindowGetDefault();
    CHECK(framebufferCreate(&fb, win, 1280, 720, 3, 2) == LIBNXRESULT(BadInput) && framebufferCreate(&fb, win, 1280, 720, PIXEL_FORMAT_RGBA_8888, 4) == LIBNXRESULT(BadInput) &&
          framebufferCreate(&fb, win, 0, 720, PIXEL_FORMAT_RGBA_8888, 2) == LIBNXRESULT(BadInput), "framebufferCreate: formato 3, 4 buffers ou largura 0 -> BadInput");
    CHECK(shimFramebufferChecksum() == 0 && shimFramebufferDumpPPM(TEST_ROOT "_x.ppm") == LIBNXRESULT(NotInitialized), "sem framebuffer: checksum 0, PPM -> NotInitialized");
    CHECK(framebufferCreate(&fb, win, 1280, 720, PIXEL_FORMAT_RGBA_8888, 2) == 0 && fb.stride == 5120 && fb.width_aligned == 1280 && fb.height_aligned == 768,
          "framebufferCreate(1280x720 RGBA8888, 2 buffers) -> stride %u, %ux%u alinhado", fb.stride, fb.width_aligned, fb.height_aligned);
    CHECK(framebufferCreate(&fb2, win, 1280, 720, PIXEL_FORMAT_RGBA_8888, 2) == LIBNXRESULT(AlreadyInitialized), "segunda framebufferCreate na mesma janela -> AlreadyInitialized");
    CHECK(framebufferMakeLinear(&fb) == 0, "framebufferMakeLinear");
    u32 stride = 0;
    u32 *px = (u32 *)framebufferBegin(&fb, &stride);
    CHECK(px != NULL && stride == 5120 && px[0] == 0, "framebufferBegin -> buffer linear zerado, stride 5120");
    for (u32 y = 0; y < 720; y++)
        for (u32 x = 0; x < 1280; x++)
            px[y * (stride / 4) + x] = RGBA8_MAXALPHA(x & 0xFF, y & 0xFF, (x ^ y) & 0xFF);
    u64 expect = fnv_rows((const u8 *)px, stride, 1280 * 4, 720);
    framebufferEnd(&fb);
    CHECK(shimFramebufferChecksum() == expect && shimFramebufferPresentCount() == 1, "framebufferEnd apresenta: FNV-1a da tela = do quadro (0x%016llx)", (unsigned long long)expect);
    px = (u32 *)framebufferBegin(&fb, &stride);
    CHECK(px[1] == RGBA8_MAXALPHA(1, 0, 1), "buffer linear mantem o quadro anterior (como o shadow buffer da libnx)");
    px[0] = RGBA8_MAXALPHA(0xFF, 0, 0);
    framebufferEnd(&fb);
    CHECK(shimFramebufferChecksum() != expect && shimFramebufferPresentCount() == 2, "segundo quadro muda o checksum (outro buffer da troca dupla)");

    CHECK(shimFramebufferDumpPPM(TEST_ROOT "_fb.ppm") == 0, "shimFramebufferDumpPPM(" TEST_ROOT "_fb.ppm)");
    FILE *f = fopen(TEST_ROOT "_fb.ppm", "rb");
    char hdr[17] = "";                     /* "P6\n1280 720\n255\n" = 16 bytes */
    u8 rgb[6] = {0};
    long size = 0;
    if (f) {
        if (fread(hdr, 1, 16, f) == 16 && fread(rgb, 1, 6, f) == 6) {
            fseek(f, 0, SEEK_END);
            size = ftell(f);
        }
        fclose(f);
    }
    CHECK(!memcmp(hdr, "P6\n1280 720\n255\n", 16) && size == 16 + 1280L * 720 * 3 && rgb[0] == 0xFF && rgb[1] == 0 && rgb[2] == 0 &&
          rgb[3] == 1 && rgb[4] == 0 && rgb[5] == 1, "PPM P6 1280x720: cabecalho, tamanho %ld, pixels (0,0) e (1,0)", size);
    remove(TEST_ROOT "_fb.ppm");
    framebufferClose(&fb);
    CHECK(framebufferBegin(&fb, &stride) == NULL && shimFramebufferChecksum() == 0, "framebufferClose: Begin -> NULL, sem imagem na tela");

    /* formatos de 16 bits e BGRA: conversao para RGB no PPM */
    const u32 fmts[3] = { PIXEL_FORMAT_BGRA_8888, PIXEL_FORMAT_RGB_565, PIXEL_FORMAT_RGBA_4444 };
    int conv_ok = 1;
    for (int k = 0; k < 3; k++) {
        conv_ok &= framebufferCreate(&fb, win, 64, 32, fmts[k], 1) == 0 && fb.stride == (k == 0 ? 256u : 128u) && fb.height_aligned == 128;
        u8 *p = (u8 *)framebufferBegin(&fb, &stride);
        if (k == 0)
            *(u32 *)p = BGRA8_MAXALPHA(0x12, 0x34, 0x56);
        else if (k == 1)
            *(u16 *)p = (u16)RGB565(31, 0, 31);
        else
            *(u16 *)p = (u16)RGBA4_MAXALPHA(0xF, 0x8, 0x0);
        framebufferEnd(&fb);
        conv_ok &= shimFramebufferDumpPPM(TEST_ROOT "_fb.ppm") == 0;
        f = fopen(TEST_ROOT "_fb.ppm", "rb");
        u8 px3[3] = {0};
        conv_ok &= f && fseek(f, 13, SEEK_SET) == 0 && fread(px3, 1, 3, f) == 3;   /* "P6\n64 32\n255\n" = 13 bytes */
        if (f)
            fclose(f);
        remove(TEST_ROOT "_fb.ppm");
        conv_ok &= k == 0 ? px3[0] == 0x12 && px3[1] == 0x34 && px3[2] == 0x56
                 : k == 1 ? px3[0] == 0xFF && px3[1] == 0 && px3[2] == 0xFF
                          : px3[0] == 0xFF && px3[1] == 0x88 && px3[2] == 0;
        framebufferClose(&fb);
    }
    CHECK(conv_ok, "BGRA8888 / RGB565 / RGBA4444 (64x32, 1 buffer): stride e cor no PPM");
}

/* ---------------------------------------------- TLS do Horizon (tpidrro_el0) em pthreads paralelas */
#define TLS_THREADS 8
#define TLS_ROUNDS  2000

typedef struct {
    int id, layout_ok, data_ok, x18_ok;
    u8 *tls;
} TlsArg;

static pthread_barrier_t s_tls_barrier;

static u64 read_x18(void)              /* so le x18 (Shadow Call Stack do Bionic); nao o usa como temporario */
{
#if defined(__aarch64__)
    u64 v;
    __asm__ volatile("mov %0, x18" : "=r"(v));
    return v;
#else
    return 0;
#endif
}

static int tls_layout_ok(u8 *t)
{
    u64 mrs = 0;
    SHIM_MRS_TPIDRRO(mrs);
    ThreadVars *tv = shimGetThreadVars();
    return t != NULL && (uintptr_t)t % SHIM_TLS_SIZE == 0 && t == (u8 *)armGetTls() && mrs == (u64)(uintptr_t)t &&
           (u8 *)tv == t + 0x1E0 && tv->magic == THREADVARS_MAGIC && (u8 *)&tv->tls_tp == t + 0x1F8;
}

static void *tls_worker(void *p)
{
    TlsArg *a = (TlsArg *)p;
    u64 x18_before = read_x18();
    u8 *t = (u8 *)shimGetTls();                /* primeira consulta: aloca o bloco da thread */
    a->x18_ok = read_x18() == x18_before;
    a->tls = t;
    a->layout_ok = tls_layout_ok(t);
    pthread_barrier_wait(&s_tls_barrier);      /* as 8 TLS existem ao mesmo tempo */
    int ok = t != NULL;
    for (int r = 0; ok && r < TLS_ROUNDS; r++) {
        u8 v = (u8)(a->id * 31 + r);
        memset(t, v, SHIM_TLS_IPC_SIZE);       /* buffer IPC da thread */
        if ((r & 63) == 0)
            sched_yield();
        for (int k = 0; k < SHIM_TLS_IPC_SIZE; k++)
            ok &= t[k] == v;
        ok &= (u8 *)shimGetTls() == t;
    }
    a->data_ok = ok;
    pthread_barrier_wait(&s_tls_barrier);
    return NULL;
}

#if defined(__linux__)
static Handle s_tls_handle_seen, s_tls_handle_self;
static void tls_libnx_thread(void *arg)
{
    (void)arg;
    s_tls_handle_seen = shimGetThreadVars()->handle;
    s_tls_handle_self = threadGetCurHandle();
}
#endif

static void test_tls(void)
{
    u8 *main_tls = (u8 *)shimGetTls();
    CHECK(tls_layout_ok(main_tls) && (u8 *)shimGetTls() == main_tls,
          "TLS da thread principal: %p, alinhada em 0x200, estavel; armGetTls = SHIM_MRS_TPIDRRO; ThreadVars em +0x1E0 (magic 0x%x)",
          (void *)main_tls, THREADVARS_MAGIC);
    memset(main_tls, 0xA5, SHIM_TLS_IPC_SIZE);
    u32 live_before = __atomic_load_n(&g_shim_tls_live, __ATOMIC_ACQUIRE);

    pthread_t th[TLS_THREADS];
    TlsArg args[TLS_THREADS];
    pthread_barrier_init(&s_tls_barrier, NULL, TLS_THREADS);
    int created = 1;
    for (int k = 0; k < TLS_THREADS; k++) {
        args[k] = (TlsArg){ .id = k };
        created &= pthread_create(&th[k], NULL, tls_worker, &args[k]) == 0;
    }
    for (int k = 0; k < TLS_THREADS; k++)
        pthread_join(th[k], NULL);
    pthread_barrier_destroy(&s_tls_barrier);

    int layout = 1, data = 1, x18 = 1, distinct = 1;
    for (int k = 0; k < TLS_THREADS; k++) {
        layout &= args[k].layout_ok;
        data &= args[k].data_ok;
        x18 &= args[k].x18_ok;
        distinct &= args[k].tls != main_tls;
        for (int j = 0; j < k; j++)
            distinct &= args[k].tls != args[j].tls;
    }
    CHECK(created && layout, "%d pthreads: TLS propria alinhada em 0x200, ThreadVars no lugar", TLS_THREADS);
    CHECK(distinct, "blocos de TLS distintos entre as %d threads e a principal", TLS_THREADS);
    CHECK(data, "buffer IPC de cada thread intacto em %d rodadas paralelas (sem interferencia)", TLS_ROUNDS);
    int main_intact = 1;
    for (int k = 0; k < SHIM_TLS_IPC_SIZE; k++)
        main_intact &= main_tls[k] == 0xA5;
    CHECK(main_intact && (u8 *)shimGetTls() == main_tls, "TLS da thread principal intacta depois das threads");
    CHECK(__atomic_load_n(&g_shim_tls_live, __ATOMIC_ACQUIRE) == live_before,
          "pthread_key libera a TLS de cada thread no fim (blocos vivos: %u)", __atomic_load_n(&g_shim_tls_live, __ATOMIC_ACQUIRE));
#if defined(__aarch64__)
    CHECK(x18, "x18 (Shadow Call Stack) igual antes e depois da primeira shimGetTls em cada thread");
#else
    CHECK(x18, "x18: so se aplica a AArch64 (host x86_64)");
#endif
#if defined(__linux__)
    Thread lt;
    CHECK(threadCreate(&lt, tls_libnx_thread, NULL, NULL, 0x10000, 0x2C, -2) == 0 && threadStart(&lt) == 0 &&
          threadWaitForExit(&lt) == 0 && threadClose(&lt) == 0 && s_tls_handle_seen != INVALID_HANDLE &&
          s_tls_handle_seen == s_tls_handle_self, "thread da libnx: ThreadVars.handle = threadGetCurHandle() (0x%x)", s_tls_handle_seen);
    CHECK(shimGetThreadVars()->handle == threadGetCurHandle(), "thread principal: ThreadVars.handle = threadGetCurHandle()");
#endif
    memset(main_tls, 0, SHIM_TLS_IPC_SIZE);
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
    /* A ultima leitura do laco pode ter acontecido antes do produtor rodar (num nucleo so, ele faz as
     * 200000 escritas de uma vez e o laco sai ao ver s_live_done). Depois do join, ler de novo e obrigatorio. */
    padUpdate(&pad);
    HidAnalogStickState lf = padGetStickPos(&pad, 0);
    u64 final = pad.buttons_cur;
    shimInputClearLive();
    CHECK(torn == 0 && back == 0 && final == LIVE_WRITES && lf.x == LIVE_WRITES && lf.y == -LIVE_WRITES,
          "buffer ao vivo (seqlock): %llu leituras x %d escritas, %llu estados misturados, %llu regressoes, final = %llu",
          (unsigned long long)reads, LIVE_WRITES, (unsigned long long)torn, (unsigned long long)back, (unsigned long long)final);
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
static volatile u8 *s_tmem_peek;
static void peek_tmem(void) { (void)*s_tmem_peek; }
#endif /* __linux__ */

int main(void)
{
    const char hello[] = "[svc 0x27] svcOutputDebugString\n";
    CHECK(svcOutputDebugString(hello, sizeof hello - 1) == 0, "svcOutputDebugString -> 0");

    /* ---- portavel: entrada (hid/pad) e fs (cartao SD) */
    test_input();
    test_fs();
    test_devices();
    test_time();
    test_applet();
    test_framebuffer();
    test_tls();

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
    CHECK(svcResetSignal(sess) == 0, "svcResetSignal (dummy)");

    /* ---- 0x15 / 0x51 / 0x52 transfer memory */
    TransferMemory tm;
    CHECK(tmemCreate(&tm, 0x3000, Perm_None) == 0 && tm.src_addr != NULL && tm.handle != INVALID_HANDLE && tm.size == 0x3000,
          "tmemCreate(12 KiB, Perm_None) -> handle 0x%x (svcCreateTransferMemory)", tm.handle);
    MemoryInfo tq = query(tm.src_addr);
    CHECK(tq.perm == Perm_None && (tq.attr & MemAttr_IsBorrowed), "origem fica Perm_None + IsBorrowed (svcQueryMemory)");
    s_tmem_peek = (volatile u8 *)tm.src_addr;
    CHECK(child_signal(peek_tmem) == SIGSEGV, "ler a origem emprestada com Perm_None -> SIGSEGV (so o servico enxerga)");
    CHECK(svcSendSyncRequest(tm.handle) == KERNELRESULT(InvalidHandle), "handle de transfer memory nao e sessao IPC");
    CHECK(tmemMap(&tm) == 0 && tm.map_addr != NULL && query(tm.map_addr).type == MemType_TransferMemIsolated,
          "tmemMap (svcMapTransferMemory) -> AliasRegion %p, TransferMemIsolated", tm.map_addr);
    u8 *svc_side = (u8 *)tm.map_addr;
    memset(svc_side, 0xC3, 0x3000);
    svc_side[0x2FFF] = 0x5A;
    CHECK(svcMapTransferMemory(tm.handle, (u8 *)tm.map_addr + 0x10000, 0x3000, Perm_None) == KERNELRESULT(InvalidState),
          "mapear de novo o mesmo objeto -> InvalidState");
    CHECK(tmemUnmap(&tm) == 0 && tm.map_addr == NULL && query(svc_side).type == MemType_Unmapped, "tmemUnmap (svcUnmapTransferMemory) -> destino Unmapped");
    CHECK(svcUnmapTransferMemory(tm.handle, svc_side, 0x3000) == KERNELRESULT(InvalidMemoryRange), "desmapear de novo -> InvalidMemoryRange");
    u8 *tsrc = (u8 *)tm.src_addr;
    CHECK(tmemCloseHandle(&tm) == 0 && query(tsrc).perm == Perm_Rw && !(query(tsrc).attr & MemAttr_IsBorrowed) &&
          tsrc[0] == 0xC3 && tsrc[0x2FFF] == 0x5A, "svcCloseHandle devolve Rw a origem, com o que o servico escreveu pelo alias");
    CHECK(tmemClose(&tm) == 0 && tm.src_addr == NULL, "tmemClose libera a origem do tmemCreate");

    Handle th1 = INVALID_HANDLE, th2 = INVALID_HANDLE;
    u8 *hsrc = h + 0x40000, *hdst = (u8 *)(uintptr_t)alias + 0x200000;
    CHECK(svcCreateTransferMemory(&th1, h + 1, 0x1000, Perm_Rw) == KERNELRESULT(InvalidAddress) &&
          svcCreateTransferMemory(&th1, hsrc, 0x800, Perm_Rw) == KERNELRESULT(InvalidSize) &&
          svcCreateTransferMemory(&th1, hsrc, 0x2000, Perm_Rx) == KERNELRESULT(InvalidMemoryPermissions),
          "svcCreateTransferMemory: desalinhado / tamanho / Perm_Rx -> InvalidAddress / InvalidSize / InvalidMemoryPermissions");
    hsrc[0] = 0x11;
    CHECK(svcCreateTransferMemory(&th1, hsrc, 0x2000, Perm_R) == 0 && query(hsrc).perm == Perm_R,
          "svcCreateTransferMemory na heap (memfd), Perm_R -> origem so leitura");
    CHECK(svcCreateTransferMemory(&th2, hsrc + 0x1000, 0x1000, Perm_None) == KERNELRESULT(InvalidMemoryState), "emprestar de novo a mesma memoria -> InvalidMemoryState");
    CHECK(svcMapTransferMemory(th1, hdst, 0x2000, Perm_Rw) == KERNELRESULT(InvalidState) &&
          svcMapTransferMemory(th1, hdst, 0x1000, Perm_R) == KERNELRESULT(InvalidSize) &&
          svcMapTransferMemory(th1, hsrc + 0x100000, 0x2000, Perm_R) == KERNELRESULT(InvalidMemoryRange),
          "svcMapTransferMemory: perm diferente / tamanho / fora da AliasRegion -> InvalidState / InvalidSize / InvalidMemoryRange");
    CHECK(svcMapTransferMemory(th1, hdst, 0x2000, Perm_R) == 0 && hdst[0] == 0x11 && query(hdst).type == MemType_TransferMem,
          "svcMapTransferMemory da heap -> alias le a origem, TransferMem");
    hdst[0x1FFF] = 0x77;
    CHECK(hsrc[0x1FFF] == 0x77, "escrita pelo alias aparece na origem (mesmo memfd)");
    CHECK(svcCloseHandle(th1) == 0 && query(hsrc).perm == Perm_Rw && query(hdst).type == MemType_Unmapped,
          "svcCloseHandle com o objeto mapeado: desmapeia e devolve Rw");

    void *anon = NULL;
    Handle th3 = INVALID_HANDLE;
    CHECK(posix_memalign(&anon, 0x1000, 0x2000) == 0 && svcCreateTransferMemory(&th3, anon, 0x2000, Perm_Rw) == 0 &&
          svcMapTransferMemory(th3, hdst, 0x2000, Perm_Rw) == KERNELRESULT(InvalidMemoryState) && svcCloseHandle(th3) == 0,
          "origem em malloc (sem memfd): cria, mas svcMapTransferMemory -> InvalidMemoryState");
    free(anon);
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
