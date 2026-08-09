// SwitchSaveSync — spike de validação (ver ANALISE.md, passo 1 do roadmap).
//
// Isso NAO e o produto final (que vai ser um sysmodule + overlay Ultrahand,
// ver ANALISE.md). Isso e um NRO avulso, pra provar num app simples e facil
// de depurar que a cadeia inteira funciona: login OAuth Device Flow do
// Google, leitura do save real do jogo, e upload/download no Drive.
//
// A UI e um menu navegavel (cima/baixo + A). Nada de decorar botao.

extern "C" {
#include "config.h"   // DRIVE_APP_FOLDER_NAME (mostrado na tela)
#include "http.h"
#include "oauth.h"
#include "drive.h"
#include "titles.h"
#include "savemount.h"
}

#include <switch.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <sys/stat.h>

#define TEST_DIR         "sdmc:/switch/SwitchSaveSync"
#define TEST_LOCAL_PATH  TEST_DIR "/test_upload.txt"
#define TEST_DOWN_PATH   TEST_DIR "/test_downloaded.txt"
#define TEST_REMOTE_NAME "switchsavesync_test.txt"
#define STAGING_DIR      TEST_DIR "/staging"
#define MAX_TITLES       64

// O console da libnx e 80x45 e entende um subconjunto de ANSI: reset(0),
// negrito(1), video reverso(7), cores 30-37 (frente) e 40-47 (fundo).
// Nada de UTF-8/box drawing — a fonte embutida e ASCII, caractere fora
// disso vira lixo na tela.
#define C_OFF   "\x1b[0m"
#define C_HEAD  "\x1b[36m"   // ciano
#define C_OK    "\x1b[32m"   // verde
#define C_WARN  "\x1b[33m"   // amarelo
#define C_ERR   "\x1b[31m"   // vermelho
#define C_DIM   "\x1b[30;1m" // cinza
#define C_SEL   "\x1b[7m"    // video reverso (linha selecionada)

#define SCREEN_W 80
#define LIST_ROWS 16   // quantos jogos cabem na janela rolante

static PadState g_pad;

// ===================== primitivas de tela =====================

static void ui_rule(char c = '-') {
    for (int i = 0; i < SCREEN_W - 1; i++) putchar(c);
    putchar('\n');
}

static void ui_header() {
    printf("\x1b[2J\x1b[H"); // limpa tela, cursor pro topo
    printf(C_HEAD " SwitchSaveSync" C_OFF C_DIM "  ~  spike de validacao" C_OFF "\n");
    ui_rule('=');
    printf(" Conta Google : %s\n",
           oauth_is_logged_in() ? C_OK "conectada" C_OFF
                                : C_WARN "desconectada" C_OFF);
    printf(" Pasta no Drive: " C_DIM DRIVE_APP_FOLDER_NAME "/" C_OFF "\n");
    ui_rule('=');
    printf("\n");
}

// Tela de uma acao em andamento: cabecalho + titulo da operacao.
static void ui_screen(const char *title) {
    ui_header();
    printf(C_HEAD " %s" C_OFF "\n", title);
    ui_rule();
    printf("\n");
}

static void ui_ok(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ui_ok(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf(C_OK "  [ok] " C_OFF); vprintf(fmt, ap); printf("\n");
    va_end(ap); consoleUpdate(NULL);
}

static void ui_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ui_err(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf(C_ERR "  [x]  " C_OFF); vprintf(fmt, ap); printf("\n");
    va_end(ap); consoleUpdate(NULL);
}

static void ui_step(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ui_step(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf(C_DIM "  ..   " C_OFF); vprintf(fmt, ap); printf("\n");
    va_end(ap); consoleUpdate(NULL);
}

// Espera qualquer botao e devolve a mascara do que foi apertado.
// Devolve 0 se o applet foi encerrado (usuario fechou o app).
static u64 ui_wait_input() {
    while (appletMainLoop()) {
        padUpdate(&g_pad);
        u64 k = padGetButtonsDown(&g_pad);
        if (k) return k;
        consoleUpdate(NULL);
    }
    return 0;
}

static void ui_pause() {
    printf("\n" C_DIM "  Pressione qualquer botao para voltar." C_OFF "\n");
    consoleUpdate(NULL);
    ui_wait_input();
}

// D-pad ou qualquer um dos dois analogicos (aliases da propria libnx, ver
// HidNpadButton_AnyUp em switch/services/hid.h).
static const u64 kUpMask   = HidNpadButton_AnyUp;
static const u64 kDownMask = HidNpadButton_AnyDown;

// ===================== menu generico =====================

struct MenuItem {
    const char *label;  // NULL = separador (nao selecionavel)
    const char *hint;
    bool danger;        // marca em vermelho as opcoes que escrevem em save
};

// Desenha o menu e devolve o indice escolhido, ou -1 se cancelou (+ / B).
static int ui_menu(const MenuItem *items, int count, int start_sel) {
    int sel = start_sel;
    // garante que o inicial e selecionavel
    while (sel < count && !items[sel].label) sel++;

    while (appletMainLoop()) {
        ui_header();
        for (int i = 0; i < count; i++) {
            if (!items[i].label) { printf("\n"); continue; }
            bool cur = (i == sel);
            printf("%s %-34s" C_OFF, cur ? C_SEL " >" : "  ", items[i].label);
            if (items[i].hint) {
                printf(" %s%s" C_OFF, items[i].danger ? C_WARN : C_DIM, items[i].hint);
            }
            printf("\n");
        }
        printf("\n");
        ui_rule();
        printf(C_DIM "  Cima/Baixo mover   A confirmar   + sair" C_OFF "\n");
        consoleUpdate(NULL);

        u64 k = ui_wait_input();
        if (k == 0) return -1;

        if (k & kUpMask) {
            do { sel = (sel == 0) ? count - 1 : sel - 1; } while (!items[sel].label);
        } else if (k & kDownMask) {
            do { sel = (sel + 1) % count; } while (!items[sel].label);
        } else if (k & HidNpadButton_A) {
            return sel;
        } else if (k & HidNpadButton_Plus) {
            return -1;
        }
    }
    return -1;
}

// ===================== lista de jogos (com rolagem) =====================

// Troca caracteres problematicos pra path local (o nome do jogo pode ter
// ":", "/" etc. que nao dao pra usar em diretorio) por "_". O nome de
// verdade (com esses caracteres) continua sendo o que vai pro Drive — essa
// versao saneada e so pra staging local.
static void sanitize_for_local_path(const char *in, char *out, size_t outsz) {
    size_t oi = 0;
    for (const char *p = in; *p && oi + 1 < outsz; p++) {
        char c = *p;
        out[oi++] = (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                     c == '"' || c == '<' || c == '>' || c == '|') ? '_' : c;
    }
    out[oi] = '\0';
}

// Lista os jogos com save data numa janela rolante de LIST_ROWS linhas — com
// dezenas de jogos, imprimir a lista inteira estourava a tela e o cursor
// sumia pra fora do visivel.
static int pick_title(TitleEntry *titles, size_t count, const char *action_label, bool danger) {
    if (count == 0) {
        ui_screen("Nenhum jogo encontrado");
        printf("  Nao achei nenhum save de conta de usuario nesse console.\n");
        ui_pause();
        return -1;
    }

    size_t sel = 0, top = 0;
    while (appletMainLoop()) {
        // mantem a selecao dentro da janela visivel
        if (sel < top) top = sel;
        if (sel >= top + LIST_ROWS) top = sel - LIST_ROWS + 1;

        ui_header();
        printf("%s %s" C_OFF "   %s%zu/%zu" C_OFF "\n",
               danger ? C_WARN : C_HEAD, action_label, C_DIM, sel + 1, count);
        ui_rule();

        for (size_t row = 0; row < LIST_ROWS; row++) {
            size_t i = top + row;
            if (i >= count) { printf("\n"); continue; }
            bool cur = (i == sel);
            // corta o nome pra nao quebrar linha e baguncar a lista
            printf("%s %-72.72s" C_OFF "\n", cur ? C_SEL " >" : "  ", titles[i].name);
        }

        ui_rule();
        if (top > 0)                printf(C_DIM "  ^ mais acima" C_OFF "\n");
        else if (top + LIST_ROWS < count) printf(C_DIM "  v mais abaixo" C_OFF "\n");
        else                        printf("\n");
        printf(C_DIM "  Cima/Baixo mover   A confirmar   B voltar" C_OFF "\n");
        consoleUpdate(NULL);

        u64 k = ui_wait_input();
        if (k == 0) return -1;

        if (k & kUpMask)          sel = (sel == 0) ? count - 1 : sel - 1;
        else if (k & kDownMask)   sel = (sel + 1) % count;
        else if (k & HidNpadButton_A) return (int)sel;
        else if (k & HidNpadButton_B) return -1;
    }
    return -1;
}

// ===================== progresso das arvores =====================

// Chamado pelo drive.c a cada arquivo/pasta. Mantem a tela viva durante
// operacoes longas em vez de congelar numa linha so.
extern "C" void on_drive_progress(const char *action, const char *name, bool ok) {
    const char *verb = (strcmp(action, "dir") == 0) ? "pasta"
                     : (strcmp(action, "up")  == 0) ? "enviado"
                                                    : "baixado";
    if (ok) ui_ok("%s: %.48s", verb, name);
    else    ui_err("falhou: %.48s", name);
}

// ===================== acoes =====================

static void on_oauth_status(const char *msg) {
    ui_screen("Entrar na conta Google");
    printf("%s\n", msg);
    printf("\n" C_DIM "  B cancela." C_OFF "\n");
    consoleUpdate(NULL);
}

static bool check_cancel_button() {
    padUpdate(&g_pad);
    return (padGetButtonsDown(&g_pad) & HidNpadButton_B) != 0;
}

static void do_login() {
    ui_screen("Entrar na conta Google");
    ui_step("pedindo codigo ao Google...");

    bool ok = oauth_start_device_flow(on_oauth_status, check_cancel_button);

    ui_screen("Entrar na conta Google");
    if (ok) {
        ui_ok("conectado. O token ficou salvo no SD, nao precisa repetir.");
    } else {
        ui_err("login nao concluido.");
        printf("\n" C_DIM "  Se apareceu \"app nao verificado\" no navegador,\n"
               "  clique em Avancado e depois em Acessar SwitchSaveSync." C_OFF "\n");
    }
    ui_pause();
}

static bool ensure_logged_in_with_token(const char *screen, char *access_token, size_t outsz) {
    if (!oauth_is_logged_in()) {
        ui_screen(screen);
        ui_err("voce ainda nao esta conectado.");
        printf("\n  Escolha \"Entrar na conta Google\" no menu primeiro.\n");
        ui_pause();
        return false;
    }
    if (!oauth_get_fresh_access_token(access_token, outsz)) {
        ui_screen(screen);
        ui_err("nao consegui renovar o acesso. Entre na conta de novo.");
        ui_pause();
        return false;
    }
    return true;
}

static void write_test_file() {
    mkdir(TEST_DIR, 0777);
    FILE *f = fopen(TEST_LOCAL_PATH, "w");
    if (!f) return;
    fprintf(f, "SwitchSaveSync spike - arquivo de teste.\n");
    fprintf(f, "Se voce esta vendo isso depois de um download, o ciclo upload->Drive->download funcionou.\n");
    fclose(f);
}

// Teste de conexao: sobe e baixa um arquivinho, so pra validar rede + TLS +
// API sem encostar em save nenhum.
static void do_connection_test() {
    char access_token[512];
    if (!ensure_logged_in_with_token("Teste de conexao", access_token, sizeof(access_token))) return;

    ui_screen("Teste de conexao");

    ui_step("procurando a pasta no Drive...");
    char folder_id[128];
    if (!drive_ensure_app_folder(access_token, folder_id, sizeof(folder_id))) {
        ui_err("nao consegui criar/achar a pasta no Drive.");
        ui_pause();
        return;
    }
    ui_ok("pasta pronta.");

    write_test_file();
    ui_step("enviando arquivo de teste...");
    if (!drive_upload(access_token, folder_id, TEST_REMOTE_NAME, TEST_LOCAL_PATH, "text/plain")) {
        ui_err("upload falhou.");
        ui_pause();
        return;
    }
    ui_ok("upload OK.");

    ui_step("baixando de volta...");
    if (!drive_download(access_token, folder_id, TEST_REMOTE_NAME, TEST_DOWN_PATH)) {
        ui_err("download falhou.");
        ui_pause();
        return;
    }
    ui_ok("download OK. Conteudo recebido:");
    FILE *f = fopen(TEST_DOWN_PATH, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) printf(C_DIM "       %s" C_OFF, line);
        fclose(f);
    }
    printf("\n");
    ui_ok("rede, TLS e Drive API funcionando.");
    ui_pause();
}

// Backup: monta o save do jogo escolhido em SOMENTE LEITURA, copia pra uma
// pasta de staging local, sobe a arvore inteira pro Drive dentro de
// "<pasta do app>/<Nome do Jogo>/".
static void do_backup_real() {
    char access_token[512];
    if (!ensure_logged_in_with_token("Backup", access_token, sizeof(access_token))) return;

    static TitleEntry titles[MAX_TITLES];
    ui_screen("Backup");
    ui_step("procurando jogos com save data...");
    size_t count = titles_list_with_savedata(titles, MAX_TITLES);

    int idx = pick_title(titles, count, "BACKUP - escolha o jogo", false);
    if (idx < 0) return;
    TitleEntry *t = &titles[idx];

    ui_screen("Backup");
    printf("  Jogo: " C_HEAD "%s" C_OFF "\n\n", t->name);
    consoleUpdate(NULL);

    char local_name[0x201];
    sanitize_for_local_path(t->name, local_name, sizeof(local_name));
    char staging_path[600];
    snprintf(staging_path, sizeof(staging_path), STAGING_DIR "/%s", local_name);
    mkdir(STAGING_DIR, 0777);
    mkdir(staging_path, 0777);

    // read_only=true: backup nao consegue corromper o save nem por bug.
    ui_step("montando o save (somente leitura)...");
    if (!savemount_mount(t->application_id, t->uid, true)) {
        ui_err("nao consegui montar o save desse jogo.");
        ui_pause();
        return;
    }

    ui_step("copiando o save pro cartao...");
    bool copy_ok = savemount_copy_tree("save:/", staging_path);
    savemount_unmount(false); // so lemos, nao precisa commit

    if (!copy_ok) {
        ui_err("falha ao copiar o save pra area de staging.");
        ui_pause();
        return;
    }
    ui_ok("save copiado.");

    ui_step("preparando as pastas no Drive...");
    char app_folder_id[128], game_folder_id[128];
    if (!drive_ensure_app_folder(access_token, app_folder_id, sizeof(app_folder_id)) ||
        !drive_ensure_subfolder(access_token, app_folder_id, t->name, game_folder_id, sizeof(game_folder_id))) {
        ui_err("nao consegui criar/achar as pastas no Drive.");
        ui_pause();
        return;
    }

    ui_step("enviando pro Drive...");
    bool ok = drive_upload_tree(access_token, game_folder_id, staging_path);

    printf("\n");
    if (ok) {
        ui_ok("backup concluido.");
        printf(C_DIM "       Confira em " DRIVE_APP_FOLDER_NAME "/%s/ no seu Drive." C_OFF "\n", t->name);
    } else {
        ui_err("backup terminou com erro em pelo menos um arquivo (marcados acima).");
    }
    ui_pause();
}

// Restore: baixa a arvore de "<pasta do app>/<Nome do Jogo>/" do Drive pra
// staging local, copia pro save montado com escrita, e comita.
static void do_restore_real() {
    char access_token[512];
    if (!ensure_logged_in_with_token("Restaurar", access_token, sizeof(access_token))) return;

    static TitleEntry titles[MAX_TITLES];
    ui_screen("Restaurar");
    ui_step("procurando jogos com save data...");
    size_t count = titles_list_with_savedata(titles, MAX_TITLES);

    int idx = pick_title(titles, count, "RESTAURAR - escolha o jogo", true);
    if (idx < 0) return;
    TitleEntry *t = &titles[idx];

    ui_screen("Restaurar");
    printf("  Jogo: " C_HEAD "%s" C_OFF "\n\n", t->name);
    printf(C_WARN "  ATENCAO: isso SOBRESCREVE o save que esta no console com o\n"
           "  que estiver no Drive. O save atual do jogo se perde." C_OFF "\n\n");
    printf("  " C_SEL " A " C_OFF " confirmar      " C_SEL " B " C_OFF " cancelar\n");
    consoleUpdate(NULL);

    bool confirmed = false;
    while (appletMainLoop()) {
        u64 k = ui_wait_input();
        if (k == 0) return;
        if (k & HidNpadButton_A) { confirmed = true; break; }
        if (k & HidNpadButton_B) { confirmed = false; break; }
    }
    if (!confirmed) return;

    char local_name[0x201];
    sanitize_for_local_path(t->name, local_name, sizeof(local_name));
    char staging_path[600];
    snprintf(staging_path, sizeof(staging_path), STAGING_DIR "/%s", local_name);
    mkdir(STAGING_DIR, 0777);

    ui_screen("Restaurar");
    printf("  Jogo: " C_HEAD "%s" C_OFF "\n\n", t->name);
    ui_step("procurando a pasta do jogo no Drive...");
    char app_folder_id[128], game_folder_id[128];
    if (!drive_ensure_app_folder(access_token, app_folder_id, sizeof(app_folder_id)) ||
        !drive_ensure_subfolder(access_token, app_folder_id, t->name, game_folder_id, sizeof(game_folder_id))) {
        ui_err("nao achei a pasta desse jogo no Drive (ja fez backup dele?).");
        ui_pause();
        return;
    }

    ui_step("baixando do Drive...");
    if (!drive_download_tree(access_token, game_folder_id, staging_path)) {
        ui_err("falha ao baixar o save do Drive. Nada foi escrito no console.");
        ui_pause();
        return;
    }

    ui_step("montando o save (escrita)...");
    if (!savemount_mount(t->application_id, t->uid, false)) {
        ui_err("nao consegui montar o save. Nada foi escrito.");
        ui_pause();
        return;
    }

    ui_step("gravando no save do console...");
    bool copy_ok = savemount_copy_tree(staging_path, "save:/");
    savemount_unmount(copy_ok); // so comita se a copia foi ok

    printf("\n");
    if (copy_ok) ui_ok("restore concluido.");
    else         ui_err("falha ao gravar — o save NAO foi commitado, continua como estava.");
    ui_pause();
}

static void do_logout() {
    oauth_logout();
    ui_screen("Sair da conta");
    ui_ok("desconectado. O token salvo no cartao foi apagado.");
    ui_pause();
}

// ===================== main =====================

enum {
    MI_BACKUP = 0,
    MI_RESTORE,
    MI_SEP1,
    MI_LOGIN,
    MI_LOGOUT,
    MI_SEP2,
    MI_TEST,
    MI_QUIT,
    MI_COUNT
};

static const MenuItem g_menu[MI_COUNT] = {
    { "Fazer backup de um jogo",  "le o save e envia pro Drive",        false },
    { "Restaurar um jogo",        "sobrescreve o save do console!",     true  },
    { NULL, NULL, false },
    { "Entrar na conta Google",   "codigo no celular ou PC",            false },
    { "Sair da conta",            "apaga o token salvo no cartao",      false },
    { NULL, NULL, false },
    { "Testar conexao",           "sobe e baixa um arquivinho de teste", false },
    { "Fechar o app",             NULL,                                 false },
};

int main(int argc, char *argv[]) {
    consoleInit(NULL);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    if (R_FAILED(socketInitializeDefault())) {
        printf(C_ERR " Falha ao inicializar a rede (socketInitializeDefault).\n" C_OFF);
        consoleUpdate(NULL);
        ui_wait_input();
        goto cleanup_console;
    }

    if (R_FAILED(romfsInit())) {
        printf(C_ERR " Falha ao montar o romfs (o cacert.pem embutido nao vai carregar).\n" C_OFF);
        consoleUpdate(NULL);
        ui_wait_input();
        goto cleanup_socket;
    }

    if (!http_init()) {
        printf(C_ERR " Falha ao iniciar a libcurl.\n" C_OFF);
        consoleUpdate(NULL);
        ui_wait_input();
        goto cleanup_romfs;
    }

    drive_set_progress_cb(on_drive_progress);
    oauth_load_saved_login();

    for (int sel = 0; appletMainLoop(); ) {
        sel = ui_menu(g_menu, MI_COUNT, sel < 0 ? 0 : sel);
        if (sel < 0 || sel == MI_QUIT) break;

        switch (sel) {
            case MI_BACKUP:  do_backup_real();     break;
            case MI_RESTORE: do_restore_real();    break;
            case MI_LOGIN:   do_login();           break;
            case MI_LOGOUT:  do_logout();          break;
            case MI_TEST:    do_connection_test(); break;
            default: break;
        }
    }

    http_shutdown();
cleanup_romfs:
    romfsExit();
cleanup_socket:
    socketExit();
cleanup_console:
    consoleExit(NULL);
    return 0;
}
