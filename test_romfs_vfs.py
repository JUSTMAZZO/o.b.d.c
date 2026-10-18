#!/usr/bin/env python3
"""
test_romfs_vfs.py - Suíte de testes unitários do subsistema RomFS VFS em switch_hle_shim.h (Fase 4).
"""
import errno
import os
import shutil
import subprocess
import tempfile
import unittest


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
#if defined(_WIN32)
#include <direct.h>
#define test_mkdir(p) _mkdir(p)
#else
#include <unistd.h>
#define test_mkdir(p) mkdir(p, 0777)
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

    def _run_subtest(self, subtest_name):
        res = subprocess.run([self.runner_bin, subtest_name], capture_output=True, text=True)
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
