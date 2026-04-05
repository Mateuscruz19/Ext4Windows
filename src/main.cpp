#include <windows.h>
#include <winfsp/winfsp.h>

#include "ext4_filesystem.hpp"
#include "blockdev_file.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// Global event used to signal the main thread to unmount and exit.
// The Ctrl+C handler sets this event so WaitForSingleObject returns.
static HANDLE g_stop_event = nullptr;

static BOOL WINAPI console_ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        if (g_stop_event)
            SetEvent(g_stop_event);
        return TRUE;
    }
    return FALSE;
}

static void print_banner()
{
    printf("\n");
    printf("                                                            ,,                    ,,\n");
    printf("  `7MM\"\"\"YMM               mm         `7MMF'     A     `7MF'db                  `7MM\n");
    printf("    MM    `7               MM           `MA     ,MA     ,V                        MM\n");
    printf("    MM   d    `7M'   `MF'mmMMmm      ,AM VM:   ,VVM:   ,V `7MM  `7MMpMMMb.   ,M\"\"bMM  ,pW\"Wq.`7M'    ,A    `MF',pP\"Ybd\n");
    printf("    MMmmMM      `VA ,V'    MM       AVMM  MM.  M' MM.  M'   MM    MM    MM ,AP    MM 6W'   `Wb VA   ,VAA   ,V  8I   `\"\n");
    printf("    MM   Y  ,     XMX      MM     ,W' MM  `MM A'  `MM A'    MM    MM    MM 8MI    MM 8M     M8  VA ,V  VA ,V   `YMMMa.\n");
    printf("    MM     ,M   ,V' VA.    MM   ,W'   MM   :MM;    :MM;     MM    MM    MM `Mb    MM YA.   ,A9   VVV    VVV    L.   I8\n");
    printf("  .JMMmmmmMMM .AM.   .MA.  `MbmoAmmmmmMMmm  VF      VF    .JMML..JMML  JMML.`Wbmd\"MML.`Ybmd9'     W      W     M9mmmP'\n");
    printf("                                      MM\n");
    printf("                                      MM\n");
    printf("\n");
    printf("  Mount ext4 Linux drives on Windows\n");
    printf("  github.com/Mateuscruz19/Ext4Windows\n");
    printf("\n");
}

static void print_welcome()
{
    print_banner();
    printf("  HOW TO USE:\n");
    printf("  1. Choose an ext4 image file (.img)\n");
    printf("  2. Pick a drive letter (or let us pick one)\n");
    printf("  3. Access your Linux files in File Explorer!\n");
    printf("\n");
    printf("  TIP: You can drag and drop a file into this window.\n");
    printf("\n");
    printf("  --------------------------------------------------------\n");
    printf("\n");
}

static void print_usage()
{
    print_banner();
    printf("  USAGE:\n");
    printf("    ext4windows <image-file> [drive-letter]\n");
    printf("\n");
    printf("  EXAMPLES:\n");
    printf("    ext4windows C:\\linux.img Z:     Mount on Z:\n");
    printf("    ext4windows C:\\linux.img        Auto-pick drive letter\n");
    printf("\n");
    printf("  Run without arguments for interactive mode.\n");
    printf("\n");
}

// Find the first available drive letter, starting from Z: going down.
// Returns L'\0' if none found.
static wchar_t find_free_drive_letter()
{
    DWORD used = GetLogicalDrives();
    for (wchar_t letter = L'Z'; letter >= L'D'; letter--) {
        int bit = letter - L'A';
        if (!(used & (1 << bit)))
            return letter;
    }
    return L'\0';
}

// Check if a file exists at the given path.
static bool file_exists(const wchar_t* path)
{
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES &&
           !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// Read a line from stdin into a wide string buffer. Returns false on EOF.
static bool read_line(wchar_t* buf, int max_chars)
{
    if (!fgetws(buf, max_chars, stdin))
        return false;

    // Remove trailing newline
    size_t len = wcslen(buf);
    if (len > 0 && buf[len - 1] == L'\n')
        buf[len - 1] = L'\0';

    return true;
}

// Remove surrounding quotes from a path (drag-and-drop adds them).
static void strip_quotes(wchar_t* path)
{
    size_t len = wcslen(path);
    if (len >= 2 && path[0] == L'"' && path[len - 1] == L'"') {
        memmove(path, path + 1, (len - 2) * sizeof(wchar_t));
        path[len - 2] = L'\0';
    }
}

// --- Internationalization (i18n) ---
// 0 = English, 1 = Portuguese
static int g_lang = 0;

static const char* tr(const char* en, const char* pt)
{
    return g_lang == 1 ? pt : en;
}

static const wchar_t* tr(const wchar_t* en, const wchar_t* pt)
{
    return g_lang == 1 ? pt : en;
}

static int ask_language()
{
    printf("  Select language / Selecione o idioma:\n");
    printf("\n");
    printf("    [1] English\n");
    printf("    [2] Portugues\n");
    printf("\n");
    printf("  > ");
    fflush(stdout);

    wchar_t input[64] = {};
    if (!read_line(input, 64))
        return 0;

    if (input[0] == L'2')
        return 1;
    return 0;
}

static void print_help_text()
{
    printf("\n");
    printf("  --------------------------------------------------------\n");
    printf("\n");
    printf("  %s\n", tr(
        "WHAT IS THIS?",
        "O QUE E ISSO?"));
    printf("\n");
    printf("  %s\n", tr(
        "  Ext4Windows lets you read Linux disks and partitions",
        "  Ext4Windows permite ler discos e particoes Linux"));
    printf("  %s\n", tr(
        "  directly on Windows — no Linux installation needed.",
        "  direto no Windows — sem precisar instalar Linux."));
    printf("\n");
    printf("  %s\n", tr(
        "  ext4 is the default filesystem of Linux (like NTFS is",
        "  ext4 e o sistema de arquivos padrao do Linux (assim como"));
    printf("  %s\n", tr(
        "  for Windows). Windows can't read ext4 natively, but",
        "  NTFS e para Windows). Windows nao le ext4 nativamente,"));
    printf("  %s\n", tr(
        "  this tool makes it possible.",
        "  mas essa ferramenta torna isso possivel."));
    printf("\n");
    printf("  %s\n", tr(
        "  HOW IT WORKS:",
        "  COMO FUNCIONA:"));
    printf("  %s\n", tr(
        "  1. You provide an ext4 image file (.img)",
        "  1. Voce fornece um arquivo de imagem ext4 (.img)"));
    printf("  %s\n", tr(
        "  2. We mount it as a Windows drive (e.g. Z:)",
        "  2. Nos montamos como um drive Windows (ex: Z:)"));
    printf("  %s\n", tr(
        "  3. Browse your Linux files in File Explorer!",
        "  3. Navegue seus arquivos Linux no Explorador!"));
    printf("\n");
    printf("  %s\n", tr(
        "  Currently read-only. Your files are safe.",
        "  Atualmente somente leitura. Seus arquivos estao seguros."));
    printf("\n");
    printf("  --------------------------------------------------------\n");
    printf("\n");
}

// Ask the user for the image path interactively.
static bool ask_image_path(wchar_t* out, int max_chars)
{
    printf("  %s\n", tr(
        "IMAGE FILE",
        "ARQUIVO DE IMAGEM"));
    printf("  %s\n", tr(
        "Enter the path to your ext4 image (.img):",
        "Digite o caminho do seu arquivo ext4 (.img):"));
    printf("  %s\n", tr(
        "(TIP: you can drag and drop the file here)",
        "(DICA: voce pode arrastar e soltar o arquivo aqui)"));
    printf("\n");
    printf("  > ");
    fflush(stdout);

    if (!read_line(out, max_chars))
        return false;

    strip_quotes(out);
    return true;
}

// Ask the user for the drive letter, or auto-pick one.
static bool ask_drive_letter(wchar_t* out, int max_chars)
{
    wchar_t free_letter = find_free_drive_letter();

    printf("\n");
    printf("  %s\n", tr(
        "DRIVE LETTER",
        "LETRA DO DRIVE"));

    if (free_letter) {
        printf("  %s (%c:):\n", tr(
            "Choose a letter or press Enter for auto",
            "Escolha uma letra ou pressione Enter para auto"),
            (char)free_letter);
    } else {
        printf("  %s:\n", tr(
            "Choose a drive letter (A-Z)",
            "Escolha uma letra de drive (A-Z)"));
    }
    printf("\n");
    printf("  > ");
    fflush(stdout);

    wchar_t input[64] = {};
    if (!read_line(input, 64))
        return false;

    if (input[0] == L'\0' && free_letter) {
        swprintf(out, max_chars, L"%c:", free_letter);
    } else if (input[0] != L'\0') {
        wchar_t letter = towupper(input[0]);
        if (letter < L'A' || letter > L'Z') {
            printf("\n  %s '%c'.\n", tr(
                "Error: invalid drive letter:",
                "Erro: letra de drive invalida:"),
                (char)input[0]);
            return false;
        }
        swprintf(out, max_chars, L"%c:", letter);
    } else {
        printf("\n  %s\n", tr(
            "Error: no free drive letters available.",
            "Erro: nenhuma letra de drive disponivel."));
        return false;
    }

    return true;
}

// Wait for user to press Enter (keeps the window open after errors).
static void pause_before_exit()
{
    printf("\n%s", tr("Press Enter to exit...", "Pressione Enter para sair..."));
    fflush(stdout);
    getwchar();
}

// Mount and run until Ctrl+C or the stop event is signaled.
static int run(const wchar_t* image_path, const wchar_t* mount_point,
               bool interactive)
{
    // Convert image path to UTF-8 for lwext4
    char image_path_utf8[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, image_path, -1,
                        image_path_utf8, sizeof(image_path_utf8),
                        nullptr, nullptr);

    // Validate the image file exists
    if (!file_exists(image_path)) {
        printf("\n  %s %s\n", tr(
            "Error: file not found:",
            "Erro: arquivo nao encontrado:"),
            image_path_utf8);
        if (interactive) pause_before_exit();
        return 1;
    }

    printf("\n");
    printf("  --------------------------------------------------------\n");
    printf("\n");
    printf("  %s %s\n", tr("Image:", "Imagem:"), image_path_utf8);
    wprintf(L"  %s %s\\\n", tr(L"Mount:", L"Drive:"), mount_point);
    printf("\n");

    // Create block device backed by the image file
    printf("  %s", tr("Mounting...", "Montando..."));
    fflush(stdout);

    struct ext4_blockdev* bdev = create_file_blockdev(image_path_utf8);
    if (!bdev) {
        printf("\n\n  %s\n", tr(
            "Error: could not open the image file.",
            "Erro: nao foi possivel abrir o arquivo de imagem."));
        printf("  %s\n", tr(
            "Is it a valid ext4 image?",
            "E uma imagem ext4 valida?"));
        if (interactive) pause_before_exit();
        return 1;
    }

    // Mount the ext4 filesystem via WinFsp
    Ext4FileSystem fs;
    NTSTATUS status = fs.Mount(bdev, mount_point, true /* read-only */);
    if (!NT_SUCCESS(status)) {
        printf("\n\n");
        if (status == 0xC0000035) {
            wprintf(L"  %s %s\n", tr(
                L"Error: drive already in use:",
                L"Erro: drive ja esta em uso:"),
                mount_point);
            printf("  %s\n", tr(
                "Pick a different letter.",
                "Escolha outra letra."));
        } else if (status == 0xC0000022) {
            wprintf(L"  %s %s\n", tr(
                L"Error: access denied for drive:",
                L"Erro: acesso negado ao drive:"),
                mount_point);
            printf("  %s\n", tr(
                "Try a different letter (like Z: or Y:).",
                "Tente outra letra (como Z: ou Y:)."));
        } else {
            printf("  %s (0x%08lX)\n", tr(
                "Error: failed to mount",
                "Erro: falha ao montar"),
                status);
            printf("  %s\n", tr(
                "The file might not be a valid ext4 image.",
                "O arquivo pode nao ser uma imagem ext4 valida."));
        }
        destroy_file_blockdev(bdev);
        if (interactive) pause_before_exit();
        return 1;
    }

    printf(" OK!\n");
    printf("\n");
    wprintf(L"  %s %s\\ (%s)\n",
        tr(L"Mounted at", L"Montado em"),
        mount_point,
        tr(L"read-only", L"somente leitura"));
    printf("\n");

    if (interactive) {
        printf("  %s\n", tr(
            "Your Linux files are now in File Explorer!",
            "Seus arquivos Linux estao no Explorador de Arquivos!"));
        printf("  %s\n", tr(
            "Press Enter here to unmount and exit.",
            "Pressione Enter aqui para desmontar e sair."));
        printf("\n");

        g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

        HANDLE stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
        HANDLE handles[] = { stdin_handle, g_stop_event };
        WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    } else {
        printf("  %s\n", tr(
            "Press Ctrl+C to unmount.",
            "Pressione Ctrl+C para desmontar."));
        printf("\n");

        g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
        WaitForSingleObject(g_stop_event, INFINITE);
    }

    // Cleanup
    printf("  %s", tr("Unmounting...", "Desmontando..."));
    fflush(stdout);
    fs.Unmount();
    destroy_file_blockdev(bdev);
    printf(" OK!\n");

    if (interactive) {
        printf("\n");
        pause_before_exit();
    }

    return 0;
}

int wmain(int argc, wchar_t* argv[])
{
    // CLI mode: ext4windows <image> [drive]
    if (argc >= 2) {
        // Check for --help / -h / /?
        if (wcscmp(argv[1], L"--help") == 0 ||
            wcscmp(argv[1], L"-h") == 0 ||
            wcscmp(argv[1], L"/?") == 0) {
            print_usage();
            return 0;
        }

        const wchar_t* image_path = argv[1];
        wchar_t mount_buf[8] = {};

        if (argc >= 3) {
            wcsncpy(mount_buf, argv[2], 7);
        } else {
            wchar_t letter = find_free_drive_letter();
            if (!letter) {
                printf("  %s\n", tr(
                    "Error: no free drive letters available.",
                    "Erro: nenhuma letra de drive disponivel."));
                return 1;
            }
            swprintf(mount_buf, 8, L"%c:", letter);
        }

        print_banner();
        return run(image_path, mount_buf, false);
    }

    // Interactive mode: no arguments
    print_welcome();

    g_lang = ask_language();
    printf("\n");

    print_help_text();

    wchar_t image_path[512] = {};
    if (!ask_image_path(image_path, 512)) {
        pause_before_exit();
        return 1;
    }

    wchar_t mount_point[8] = {};
    if (!ask_drive_letter(mount_point, 8)) {
        pause_before_exit();
        return 1;
    }

    return run(image_path, mount_point, true);
}
