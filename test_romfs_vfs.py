#!/usr/bin/env python3
"""
test_romfs_vfs.py - Suíte de testes unitários do subsistema RomFS VFS em switch_hle_shim.h (Fase 4).
"""
import errno
import os
import shutil
import struct
import subprocess
import tempfile
import unittest

from test_nro_aset import pack_synthetic_romfs, make_aset, make_nacp, make_synthetic_nro_header


def find_host_clang():
    """Localiza o compilador clang do host."""
    paths = [
        r"C:\Users\user\o.b.d.c\tools\llvm-mingw-20260922-ucrt-x86_64\bin\clang.exe",
        "clang",
    ]
    for p in paths:
        if os.path.exists(p) or shutil.which(p):
            return p
    return None


def find_ndk_clang():
    """Localiza o compilador clang do Android NDK r28."""
    ndk_clang = r"C:\Users\user\AppData\Local\Android\Sdk\ndk\28.2.13676358\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android35-clang.cmd"
    if os.path.exists(ndk_clang):
        return ndk_clang
    return None


C_TEST_RUNNER_SRC = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define test_mkdir(p) _mkdir(p)
#if !defined(O_BINARY)
#define O_BINARY _O_BINARY
#endif
#else
#include <unistd.h>
#define test_mkdir(p) mkdir(p, 0777)
#if !defined(O_BINARY)
#define O_BINARY 0
#endif
#endif

#include "switch_hle_shim.h"

int run_lifecycle(void) {
    const char *tmp_dir = "obdc_test_tmp_romfs_life";
    test_mkdir(tmp_dir);
    shimRomfsSetRoot(tmp_dir);

    /* 1. Estado inicial */
    if (s_romfs_mounted != 0) return 1;

    /* 2. romfsInit() -> 0, s_romfs_mounted = 1 */
    Result rc = romfsInit();
    if (rc != 0) return 2;
    if (s_romfs_mounted != 1) return 3;
    if (strcmp(s_romfs_base_path, tmp_dir) != 0) return 4;

    /* 3. romfsInit() duplicado -> LibnxError_AlreadyMapped (0x759) */
    if (romfsInit() != LIBNXRESULT(AlreadyMapped)) return 5;

    /* 4. romfsExit() -> 0, s_romfs_mounted = 0 */
    rc = romfsExit();
    if (rc != 0) return 6;
    if (s_romfs_mounted != 0) return 7;
    if (s_romfs_base_path[0] != '\0') return 8;

    /* 5. romfsExit() duplicado -> LibnxError_NotFound (0x1359) */
    if (romfsExit() != LIBNXRESULT(NotFound)) return 9;

    shimRomfsSetRoot(NULL);
    rmdir(tmp_dir);
    return 0;
}

int run_path_resolution(void) {
    char out[512];
    shimRomfsSetRoot("vfs_root");

    /* romfs:/data/test.txt */
    const char *res = hle_resolve_vfs_path("romfs:/data/test.txt", out, sizeof out);
    if (!res) return 1;
#if defined(_WIN32)
    if (strcmp(out, "vfs_root\\data\\test.txt") != 0) return 2;
#else
    if (strcmp(out, "vfs_root/data/test.txt") != 0) return 2;
#endif

    /* romfs:data/test.txt (sem barra) */
    res = hle_resolve_vfs_path("romfs:data/test.txt", out, sizeof out);
    if (!res) return 3;
#if defined(_WIN32)
    if (strcmp(out, "vfs_root\\data\\test.txt") != 0) return 4;
#else
    if (strcmp(out, "vfs_root/data/test.txt") != 0) return 4;
#endif

    /* Subdiretórios profundos */
    res = hle_resolve_vfs_path("romfs:/a/b/c/d.bin", out, sizeof out);
    if (!res) return 5;
#if defined(_WIN32)
    if (strcmp(out, "vfs_root\\a\\b\\c\\d.bin") != 0) return 6;
#else
    if (strcmp(out, "vfs_root/a/b/c/d.bin") != 0) return 6;
#endif

    shimRomfsSetRoot(NULL);
    return 0;
}

int run_fallback(void) {
    char out[512];
    shimRomfsSetRoot(NULL);
    s_romfs_base_path[0] = '\0';
    s_romfs_mounted = 0;

    /* Caso 1: sem OBDC_ROMFS -> ./romfs */
#if defined(_WIN32)
    _putenv("OBDC_ROMFS=");
#else
    unsetenv("OBDC_ROMFS");
#endif

    const char *res = hle_resolve_vfs_path("romfs:/sample.txt", out, sizeof out);
    if (!res) return 1;
#if defined(_WIN32)
    if (strcmp(out, ".\\romfs\\sample.txt") != 0) return 2;
#else
    if (strcmp(out, "./romfs/sample.txt") != 0) return 2;
#endif

    /* Caso 2: com OBDC_ROMFS setado */
#if defined(_WIN32)
    _putenv("OBDC_ROMFS=my_custom_romfs");
#else
    setenv("OBDC_ROMFS", "my_custom_romfs", 1);
#endif

    res = hle_resolve_vfs_path("romfs:/sample.txt", out, sizeof out);
    if (!res) return 3;
#if defined(_WIN32)
    if (strcmp(out, "my_custom_romfs\\sample.txt") != 0) return 4;
#else
    if (strcmp(out, "my_custom_romfs/sample.txt") != 0) return 4;
#endif

#if defined(_WIN32)
    _putenv("OBDC_ROMFS=");
#else
    unsetenv("OBDC_ROMFS");
#endif
    return 0;
}

int run_file_read(void) {
    const char *tmp_dir = "obdc_test_tmp_romfs_read";
    test_mkdir(tmp_dir);

    char file_path[256];
    snprintf(file_path, sizeof file_path, "%s/dummy.txt", tmp_dir);
    FILE *f = fopen(file_path, "wb");
    if (!f) return 1;
    const char *expected = "RomFS Virtual Filesystem Verification 2026";
    fwrite(expected, 1, strlen(expected), f);
    fclose(f);

    shimRomfsSetRoot(tmp_dir);

    /* Antes de romfsInit: deve falhar com ENODEV */
    errno = 0;
    FILE *early_f = hle_fopen("romfs:/dummy.txt", "r");
    if (early_f != NULL || errno != ENODEV) return 20;

    if (romfsInit() != 0) return 2;

    /* Leitura via hle_fopen */
    FILE *rf = hle_fopen("romfs:/dummy.txt", "r");
    if (!rf) return 3;
    char buf[128] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, rf);
    fclose(rf);
    if (n != strlen(expected) || strcmp(buf, expected) != 0) return 4;

    /* Acesso somente leitura: tentativa de escrita deve falhar com EROFS */
    errno = 0;
    FILE *wf = hle_fopen("romfs:/dummy.txt", "w");
    if (wf != NULL || errno != EROFS) return 5;

    errno = 0;
    FILE *af = hle_fopen("romfs:/dummy.txt", "a");
    if (af != NULL || errno != EROFS) return 6;

    romfsExit();

    /* Depois de romfsExit: deve falhar com ENODEV */
    errno = 0;
    FILE *late_f = hle_fopen("romfs:/dummy.txt", "r");
    if (late_f != NULL || errno != ENODEV) return 21;

    shimRomfsSetRoot(NULL);
    remove(file_path);
    rmdir(tmp_dir);
    return 0;
}

int run_traversal(void) {
    char out[512];
    shimRomfsSetRoot("vfs_root");

    /* 1. romfs:/../../etc/passwd */
    errno = 0;
    const char *res = hle_resolve_vfs_path("romfs:/../../etc/passwd", out, sizeof out);
    if (res != NULL || errno != EACCES) return 1;

    /* 2. romfs:../../secret */
    errno = 0;
    res = hle_resolve_vfs_path("romfs:../../secret", out, sizeof out);
    if (res != NULL || errno != EACCES) return 2;

    /* 3. romfs:/sub/../../escaped */
    errno = 0;
    res = hle_resolve_vfs_path("romfs:/sub/../../escaped", out, sizeof out);
    if (res != NULL || errno != EACCES) return 3;

    /* 4. hle_fopen com traversal (montado para testar EACCES e nao ENODEV) */
    const char *tmp_dir = "obdc_test_tmp_romfs_trav";
    test_mkdir(tmp_dir);
    shimRomfsSetRoot(tmp_dir);
    if (romfsInit() != 0) {
        rmdir(tmp_dir);
        return 40;
    }

    errno = 0;
    FILE *f = hle_fopen("romfs:/../../etc/passwd", "r");
    if (f != NULL || errno != EACCES) {
        romfsExit();
        rmdir(tmp_dir);
        return 4;
    }

    romfsExit();
    rmdir(tmp_dir);
    shimRomfsSetRoot(NULL);
    return 0;
}

int run_alias(void) {
    const char *tmp_dir = "obdc_test_tmp_romfs_alias";
    test_mkdir(tmp_dir);
    shimRomfsSetRoot(tmp_dir);

    /* Monta alias 'custom' */
    if (romfsMount("custom") != 0) return 1;

    /* Desmonta alias 'custom' */
    if (romfsUnmount("custom") != 0) return 2;

    shimRomfsSetRoot(NULL);
    rmdir(tmp_dir);
    return 0;
}

int run_header_struct(void) {
    if (sizeof(RomfsHeader) != 80) return 1;
    RomfsHeader h = {0};
    h.header_size = 80;
    h.dir_hash_table_offset = 0x50;
    h.data_offset = 0x1000;
    if (h.header_size != 80 || h.data_offset != 0x1000) return 2;
    return 0;
}

static int read_whole_file(const char *path, void **out_buf, size_t *out_sz) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    void *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    *out_buf = buf;
    *out_sz = (size_t)sz;
    return 0;
}

int run_mount_memory(const char *bin_path) {
    void *buf = NULL;
    size_t sz = 0;
    if (read_whole_file(bin_path, &buf, &sz) != 0) return 1;

    Result rc = romfsMountFromMemory(buf, sz, "rom_mem");
    if (rc != 0) { free(buf); return 2; }

    /* Duplicate mount should fail with AlreadyMapped */
    if (romfsMountFromMemory(buf, sz, "rom_mem") != LIBNXRESULT(AlreadyMapped)) {
        romfsUnmount("rom_mem");
        free(buf);
        return 3;
    }

    /* Read root file */
    FILE *f = hle_fopen("rom_mem:/hello.txt", "r");
    if (!f) { romfsUnmount("rom_mem"); free(buf); return 4; }
    char content[64] = {0};
    fread(content, 1, sizeof(content) - 1, f);
    fclose(f);
    if (strcmp(content, "Hello from RomFS!") != 0) {
        romfsUnmount("rom_mem");
        free(buf);
        return 5;
    }

    /* Read nested file */
    f = hle_fopen("rom_mem:/sub/nested.txt", "r");
    if (!f) { romfsUnmount("rom_mem"); free(buf); return 6; }
    memset(content, 0, sizeof content);
    fread(content, 1, sizeof(content) - 1, f);
    fclose(f);
    if (strcmp(content, "Nested content in RomFS") != 0) {
        romfsUnmount("rom_mem");
        free(buf);
        return 7;
    }

    /* Read-only verification: writing must fail with EROFS */
    errno = 0;
    FILE *wf = hle_fopen("rom_mem:/hello.txt", "w");
    if (wf != NULL || errno != EROFS) {
        if (wf) fclose(wf);
        romfsUnmount("rom_mem");
        free(buf);
        return 8;
    }

    /* Unmount */
    rc = romfsUnmount("rom_mem");
    if (rc != 0) { free(buf); return 9; }

    /* After unmount, fopen should fail with ENODEV */
    errno = 0;
    FILE *af = hle_fopen("rom_mem:/hello.txt", "r");
    if (af != NULL || errno != ENODEV) {
        if (af) fclose(af);
        free(buf);
        return 10;
    }

    free(buf);
    return 0;
}

int run_mount_file(const char *file_path) {
    FILE *f = fopen(file_path, "rb");
    if (!f) return 1;

    Result rc = romfsMountFromFile(f, 0, "rom_file");
    fclose(f);
    if (rc != 0) return 2;

    FILE *rf = hle_fopen("rom_file:/hello.txt", "r");
    if (!rf) { romfsUnmount("rom_file"); return 3; }
    char content[64] = {0};
    fread(content, 1, sizeof(content) - 1, rf);
    fclose(rf);
    if (strcmp(content, "Hello from RomFS!") != 0) {
        romfsUnmount("rom_file");
        return 4;
    }

    rc = romfsUnmount("rom_file");
    if (rc != 0) return 5;

    return 0;
}

int run_mount_fd(const char *file_path) {
#if defined(_WIN32)
    int fd = _open(file_path, _O_RDONLY | _O_BINARY);
#else
    int fd = open(file_path, O_RDONLY);
#endif
    if (fd < 0) return 1;

    Result rc = romfsMountFromFd(fd, 0, "rom_fd");
#if defined(_WIN32)
    _close(fd);
#else
    close(fd);
#endif
    if (rc != 0) return 2;

    FILE *rf = hle_fopen("rom_fd:/hello.txt", "r");
    if (!rf) { romfsUnmount("rom_fd"); return 3; }
    char content[64] = {0};
    fread(content, 1, sizeof(content) - 1, rf);
    fclose(rf);
    if (strcmp(content, "Hello from RomFS!") != 0) {
        romfsUnmount("rom_fd");
        return 4;
    }

    rc = romfsUnmount("rom_fd");
    if (rc != 0) return 5;

    return 0;
}

int run_mount_self_image(const char *image_path) {
#if defined(_WIN32)
    char env_buf[512];
    snprintf(env_buf, sizeof env_buf, "OBDC_ROMFS=%s", image_path);
    _putenv(env_buf);
#else
    setenv("OBDC_ROMFS", image_path, 1);
#endif

    s_romfs_mounted = 0;
    s_romfs_base_path[0] = '\0';
    shimRomfsSetRoot(NULL);

    Result rc = romfsInit();
    if (rc != 0) return 1;
    if (s_romfs_mounted != 1) return 2;

    char recorded_temp[512];
    snprintf(recorded_temp, sizeof recorded_temp, "%s", s_romfs_base_path);

    /* Verify recorded_temp directory actually exists */
    struct stat st;
    if (stat(recorded_temp, &st) != 0 || !(st.st_mode & S_IFDIR)) return 3;

    /* Read a file from romfs */
    FILE *rf = hle_fopen("romfs:/hello.txt", "r");
    if (!rf) { romfsExit(); return 4; }
    char content[64] = {0};
    fread(content, 1, sizeof(content) - 1, rf);
    fclose(rf);
    if (strcmp(content, "Hello from RomFS!") != 0) {
        romfsExit();
        return 5;
    }

    rc = romfsExit();
    if (rc != 0) return 6;
    if (s_romfs_mounted != 0) return 7;

    /* Verify that recorded_temp directory was completely deleted */
    if (stat(recorded_temp, &st) == 0) return 8;

#if defined(_WIN32)
    _putenv("OBDC_ROMFS=");
#else
    unsetenv("OBDC_ROMFS");
#endif
    return 0;
}

int run_mount_current_process(void) {
    const char *tmp_dir = "obdc_test_tmp_romfs_proc";
    test_mkdir(tmp_dir);
    shimRomfsSetRoot(tmp_dir);

    Result rc = romfsMountFromCurrentProcess("proc_test");
    if (rc != 0) {
        shimRomfsSetRoot(NULL);
        rmdir(tmp_dir);
        return 1;
    }

    rc = romfsUnmount("proc_test");
    if (rc != 0) {
        shimRomfsSetRoot(NULL);
        rmdir(tmp_dir);
        return 2;
    }

    shimRomfsSetRoot(NULL);
    rmdir(tmp_dir);
    return 0;
}

int run_invalid_inputs(void) {
    char dummy[128] = {0};
    if (romfsMountFromMemory(NULL, sizeof(dummy), "test") != LIBNXRESULT(BadInput)) return 1;
    if (romfsMountFromMemory(dummy, sizeof(RomfsHeader) - 1, "test") != LIBNXRESULT(BadInput)) return 2;
    if (romfsMountFromMemory(dummy, sizeof(dummy), NULL) != LIBNXRESULT(BadInput)) return 3;
    if (romfsMountFromMemory(dummy, sizeof(dummy), "") != LIBNXRESULT(BadInput)) return 4;

    if (romfsMountFromFile(NULL, 0, "test") != LIBNXRESULT(BadInput)) return 5;
    FILE *dummy_f = fopen("switch_hle_shim.h", "rb");
    if (dummy_f) {
        if (romfsMountFromFile(dummy_f, 0, NULL) != LIBNXRESULT(BadInput)) { fclose(dummy_f); return 6; }
        if (romfsMountFromFile(dummy_f, 0, "") != LIBNXRESULT(BadInput)) { fclose(dummy_f); return 7; }
        fclose(dummy_f);
    }

    if (romfsMountFromFd(-1, 0, "test") != LIBNXRESULT(BadInput)) return 8;
    if (romfsMountFromFd(0, 0, NULL) != LIBNXRESULT(BadInput)) return 9;
    if (romfsMountFromFd(0, 0, "") != LIBNXRESULT(BadInput)) return 10;

    return 0;
}

int run_corrupted_image(const char *bad_path) {
    void *buf = NULL;
    size_t sz = 0;
    if (read_whole_file(bad_path, &buf, &sz) != 0) return 1;
    Result rc = romfsMountFromMemory(buf, sz, "bad");
    free(buf);
    if (rc == 0) return 2; /* Must fail */
    return 0;
}

int run_mount_file_fail(const char *file_path) {
    FILE *f = fopen(file_path, "rb");
    if (!f) return 1;
    Result rc = romfsMountFromFile(f, 0, "fail_mount");
    fclose(f);
    if (rc == 0) {
        romfsUnmount("fail_mount");
        return 2;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return 99;
    const char *cmd = argv[1];
    if (strcmp(cmd, "lifecycle") == 0) return run_lifecycle();
    if (strcmp(cmd, "path_resolution") == 0) return run_path_resolution();
    if (strcmp(cmd, "fallback") == 0) return run_fallback();
    if (strcmp(cmd, "file_read") == 0) return run_file_read();
    if (strcmp(cmd, "traversal") == 0) return run_traversal();
    if (strcmp(cmd, "alias") == 0) return run_alias();
    if (strcmp(cmd, "header_struct") == 0) return run_header_struct();
    if (strcmp(cmd, "mount_memory") == 0 && argc >= 3) return run_mount_memory(argv[2]);
    if (strcmp(cmd, "mount_file") == 0 && argc >= 3) return run_mount_file(argv[2]);
    if (strcmp(cmd, "mount_fd") == 0 && argc >= 3) return run_mount_fd(argv[2]);
    if (strcmp(cmd, "mount_self_image") == 0 && argc >= 3) return run_mount_self_image(argv[2]);
    if (strcmp(cmd, "mount_current_process") == 0) return run_mount_current_process();
    if (strcmp(cmd, "invalid_inputs") == 0) return run_invalid_inputs();
    if (strcmp(cmd, "corrupted_image") == 0 && argc >= 3) return run_corrupted_image(argv[2]);
    if (strcmp(cmd, "mount_file_fail") == 0 && argc >= 3) return run_mount_file_fail(argv[2]);
    return 100;
}
"""


class TestRomfsVfs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.clang = find_host_clang()
        if not cls.clang:
            raise unittest.SkipTest("Compilador Clang host não encontrado.")

        cls.runner_c = os.path.abspath("test_romfs_runner.c")
        cls.runner_bin = os.path.abspath("test_romfs_runner.exe")

        with open(cls.runner_c, "w", encoding="utf-8") as f:
            f.write(C_TEST_RUNNER_SRC)

        compile_cmd = [
            cls.clang,
            "-O3",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            ".",
            cls.runner_c,
            "-o",
            cls.runner_bin,
            "-lpthread",
        ]
        res = subprocess.run(compile_cmd, capture_output=True, text=True)
        if res.returncode != 0:
            raise RuntimeError(f"Falha ao compilar test_romfs_runner.c:\n{res.stderr}")

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "runner_c") and os.path.exists(cls.runner_c):
            try:
                os.remove(cls.runner_c)
            except OSError:
                pass
        if hasattr(cls, "runner_bin") and os.path.exists(cls.runner_bin):
            try:
                os.remove(cls.runner_bin)
            except OSError:
                pass

    def _run_subtest(self, subtest_name, *args):
        cmd = [self.runner_bin, subtest_name] + list(args)
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(
            res.returncode,
            0,
            f"Subteste {subtest_name} falhou com código {res.returncode}.\nSTDOUT: {res.stdout}\nSTDERR: {res.stderr}",
        )

    def test_romfs_header_definition(self):
        """Verifica a definição da struct RomfsHeader (tamanho 80 bytes e campos)."""
        with open("switch_hle_shim.h", "r", encoding="utf-8", errors="ignore") as f:
            content = f.read()

        required_fields = [
            "uint64_t header_size;",
            "uint64_t dir_hash_table_offset;",
            "uint64_t dir_hash_table_size;",
            "uint64_t dir_meta_table_offset;",
            "uint64_t dir_meta_table_size;",
            "uint64_t file_hash_table_offset;",
            "uint64_t file_hash_table_size;",
            "uint64_t file_meta_table_offset;",
            "uint64_t file_meta_table_size;",
            "uint64_t data_offset;",
        ]
        self.assertIn("typedef struct {", content)
        self.assertIn("} RomfsHeader;", content)
        for field in required_fields:
            self.assertIn(field, content)

        self._run_subtest("header_struct")

    def test_romfs_global_state_defined(self):
        """Verifica a presença das variáveis globais de estado s_romfs_mounted e s_romfs_base_path."""
        with open("switch_hle_shim.h", "r", encoding="utf-8", errors="ignore") as f:
            content = f.read()
        self.assertIn("s_romfs_mounted", content)
        self.assertIn("s_romfs_base_path", content)

    def test_romfs_lifecycle(self):
        """Testa o ciclo de vida: romfsInit, romfsExit, s_romfs_mounted e retornos de erro."""
        self._run_subtest("lifecycle")

    def test_vfs_path_resolution(self):
        """Testa a resolução de caminhos virtuais 'romfs:/' e 'romfs:' e normalização de barras."""
        self._run_subtest("path_resolution")

    def test_vfs_fallback_resolution(self):
        """Testa a ordem de fallback: OBDC_ROMFS e padrão ./romfs."""
        self._run_subtest("fallback")

    def test_file_read_in_temp_romfs(self):
        """Testa abertura e leitura de arquivo dummy via hle_fopen e proteção somente-leitura EROFS."""
        self._run_subtest("file_read")

    def test_directory_traversal_protection(self):
        """Testa proteção contra directory traversal ('..') retornando NULL e errno = EACCES."""
        self._run_subtest("traversal")

    def test_romfs_alias_mount_and_unmount(self):
        """Testa o suporte a aliases com romfsMount e romfsUnmount."""
        self._run_subtest("alias")

    def test_romfs_mount_from_memory(self):
        """Testa romfsMountFromMemory com imagem RomFS sintética contendo arquivos e subpastas."""
        files = {
            "hello.txt": b"Hello from RomFS!",
            "sub/nested.txt": b"Nested content in RomFS",
        }
        romfs_bin = pack_synthetic_romfs(files)
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(romfs_bin)
            tmp_path = f.name
        try:
            self._run_subtest("mount_memory", tmp_path)
        finally:
            if os.path.exists(tmp_path):
                os.remove(tmp_path)

    def test_romfs_mount_from_file(self):
        """Testa romfsMountFromFile com imagem RomFS bruta em disco."""
        files = {
            "hello.txt": b"Hello from RomFS!",
            "sub/nested.txt": b"Nested content in RomFS",
        }
        romfs_bin = pack_synthetic_romfs(files)
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(romfs_bin)
            tmp_path = f.name
        try:
            self._run_subtest("mount_file", tmp_path)
        finally:
            if os.path.exists(tmp_path):
                os.remove(tmp_path)

    def test_romfs_mount_from_fd(self):
        """Testa romfsMountFromFd usando descritor de arquivo aberto."""
        files = {
            "hello.txt": b"Hello from RomFS!",
            "sub/nested.txt": b"Nested content in RomFS",
        }
        romfs_bin = pack_synthetic_romfs(files)
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(romfs_bin)
            tmp_path = f.name
        try:
            self._run_subtest("mount_fd", tmp_path)
        finally:
            if os.path.exists(tmp_path):
                os.remove(tmp_path)

    def test_romfs_mount_from_nro_aset(self):
        """Testa romfsMountFromFile com um executável NRO contendo seção ASET e RomFS embutida."""
        files = {
            "hello.txt": b"Hello from RomFS!",
            "sub/nested.txt": b"Nested content in RomFS",
        }
        romfs_bin = pack_synthetic_romfs(files)
        aset_blob = make_aset(icon_bytes=b"ICON" * 16, nacp_bytes=make_nacp(), romfs_bytes=romfs_bin)
        nro_blob = bytearray(make_synthetic_nro_header(size=0x10000))
        nro_blob.extend(aset_blob)

        with tempfile.NamedTemporaryFile(suffix=".nro", delete=False) as f:
            f.write(nro_blob)
            tmp_path = f.name
        try:
            self._run_subtest("mount_file", tmp_path)
        finally:
            if os.path.exists(tmp_path):
                os.remove(tmp_path)

    def test_romfs_mount_self_raw_romfs_image(self):
        """Testa romfsInit/romfsExit com OBDC_ROMFS apontando diretamente para um arquivo de imagem RomFS."""
        files = {
            "hello.txt": b"Hello from RomFS!",
            "sub/nested.txt": b"Nested content in RomFS",
        }
        romfs_bin = pack_synthetic_romfs(files)
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(romfs_bin)
            tmp_path = f.name
        try:
            self._run_subtest("mount_self_image", tmp_path)
        finally:
            if os.path.exists(tmp_path):
                os.remove(tmp_path)

    def test_romfs_mount_current_process(self):
        """Testa romfsMountFromCurrentProcess delegando para romfsMountSelf."""
        self._run_subtest("mount_current_process")

    def test_romfs_invalid_inputs(self):
        """Testa validação de ponteiros nulos, tamanhos e fds inválidos em romfsMount*."""
        self._run_subtest("invalid_inputs")

    def test_romfs_corrupted_images(self):
        """Testa rejeição graciosa de imagens RomFS truncadas ou com cabeçalho inválido."""
        # Cabeçalho RomFS truncado (< 80 bytes)
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(b"short_data_not_a_romfs")
            tmp_path = f.name
        try:
            self._run_subtest("corrupted_image", tmp_path)
        finally:
            if os.path.exists(tmp_path):
                os.remove(tmp_path)

        # Cabeçalho com offsets apontando para fora do buffer
        bad_hdr = bytearray(80)
        struct.pack_into("<10Q", bad_hdr, 0, 80, 0x100000, 0x100, 0x200000, 0x100, 0x300000, 0x100, 0x400000, 0x100, 0x500000)
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(bytes(bad_hdr))
            tmp_path = f.name
        try:
            self._run_subtest("corrupted_image", tmp_path)
        finally:
            if os.path.exists(tmp_path):
                os.remove(tmp_path)

    def test_romfs_traversal_in_image_metadata_rejected(self):
        """Testa rejeição de imagem RomFS maliciosa contendo directory traversal no nome de arquivo."""
        files = {
            "../escape.txt": b"malicious",
        }
        romfs_bin = pack_synthetic_romfs(files)
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(romfs_bin)
            tmp_path = f.name
        try:
            self._run_subtest("corrupted_image", tmp_path)
        finally:
            if os.path.exists(tmp_path):
                os.remove(tmp_path)

    def test_romfs_cyclic_directory_rejected_without_stack_overflow(self):
        """Testa que ciclo de diretórios na partição RomFS é rejeitado sem causar stack overflow."""
        files = {
            "sub/nested.txt": b"test content",
        }
        romfs_bin = bytearray(pack_synthetic_romfs(files))
        dir_meta_off = struct.unpack_from("<Q", romfs_bin, 0x18)[0]
        # Aponta o child_dir da raiz para 0 (ela mesma, ciclo direto)
        struct.pack_into("<I", romfs_bin, dir_meta_off + 8, 0)
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(bytes(romfs_bin))
            tmp_path = f.name
        try:
            self._run_subtest("corrupted_image", tmp_path)
        finally:
            if os.path.exists(tmp_path):
                os.remove(tmp_path)

    def test_romfs_integer_overflow_file_size_rejected(self):
        """Testa que tamanho de arquivo uint64 com overflow em RomFS é rejeitado graciosamente."""
        files = {
            "overflow.txt": b"small",
        }
        romfs_bin = bytearray(pack_synthetic_romfs(files))
        file_meta_off = struct.unpack_from("<Q", romfs_bin, 0x38)[0]
        # Define tamanho do arquivo como UINT64_MAX
        struct.pack_into("<Q", romfs_bin, file_meta_off + 16, 0xFFFFFFFFFFFFFFFF)
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(bytes(romfs_bin))
            tmp_path = f.name
        try:
            self._run_subtest("corrupted_image", tmp_path)
        finally:
            if os.path.exists(tmp_path):
                os.remove(tmp_path)

    def test_romfs_invalid_characters_colon_rejected(self):
        """Testa que nomes de arquivos com dois pontos (colon) são estritamente rejeitados."""
        files = {
            "colon:stream.txt": b"invalid stream",
        }
        romfs_bin = pack_synthetic_romfs(files)
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(romfs_bin)
            tmp_path = f.name
        try:
            self._run_subtest("corrupted_image", tmp_path)
        finally:
            if os.path.exists(tmp_path):
                os.remove(tmp_path)

    def test_romfs_nro_without_romfs_rejected(self):
        """Testa que executável NRO sem partição RomFS (romfs_size == 0 ou sem ASET) falha graciosamente."""
        # NRO com cabeçalho ASET mas romfs_size == 0
        aset_blob = make_aset(icon_bytes=b"ICON" * 16, nacp_bytes=make_nacp(), romfs_bytes=b"")
        nro_blob = bytearray(make_synthetic_nro_header(size=0x10000))
        nro_blob.extend(aset_blob)
        with tempfile.NamedTemporaryFile(suffix=".nro", delete=False) as f:
            f.write(nro_blob)
            tmp_path = f.name
        try:
            self._run_subtest("mount_file_fail", tmp_path)
        finally:
            if os.path.exists(tmp_path):
                os.remove(tmp_path)

        # NRO puro sem seção ASET
        nro_pure = bytearray(make_synthetic_nro_header(size=0x10000))
        with tempfile.NamedTemporaryFile(suffix=".nro", delete=False) as f:
            f.write(nro_pure)
            tmp_path = f.name
        try:
            self._run_subtest("mount_file_fail", tmp_path)
        finally:
            if os.path.exists(tmp_path):
                os.remove(tmp_path)

    def test_strict_compilation_host_and_ndk(self):
        """Compila switch_hle_shim.h com -O3 -Wall -Wextra -Werror no host e no NDK Clang."""
        res_host = subprocess.run(
            [self.clang, "-O3", "-Wall", "-Wextra", "-Werror", "-I", ".", "-c", "-x", "c", "-o", "nul", "switch_hle_shim.h"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(res_host.returncode, 0, f"Falha na compilação do host clang:\n{res_host.stderr}")

        ndk_clang = find_ndk_clang()
        if ndk_clang:
            res_ndk = subprocess.run(
                [ndk_clang, "-O3", "-Wall", "-Wextra", "-Werror", "-I", ".", "-c", "-x", "c", "-o", "nul", "switch_hle_shim.h"],
                capture_output=True,
                text=True,
            )
            self.assertEqual(res_ndk.returncode, 0, f"Falha na compilação do NDK clang:\n{res_ndk.stderr}")


if __name__ == "__main__":
    unittest.main()
