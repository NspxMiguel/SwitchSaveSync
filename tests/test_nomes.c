// Teste de três coisas que não precisam de console nenhum pra rodar:
//
//   1. o leitor de JSON (minijson) — é ele que lê TODA resposta do Google;
//      se ele errar, o login falha ou o app acha a pasta errada no Drive;
//   2. o saneamento de nome de pasta (syncstate_sanitize_name);
//   3. a regra do nome da pasta do save (syncjob_save_folder_name) — que é a
//      suspeita do "mudei d nome e foi nao".
//
// O item 3 é o motivo real deste arquivo existir. A pergunta que ele responde
// é: mudar o apelido da conta no console muda o nome da pasta na nuvem? Se
// mudar, o backup antigo vira órfão e o app trata como primeira sync.

#include "minijson.h"
#include "syncstate.h"
#include "syncjob.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <stdlib.h>

// ----------------------------------------------------------------- o que falta

bool lang_is_pt(void) { return true; }

Result timeGetCurrentTime(TimeType type, u64 *out)
{
    (void)type;
    *out = 0;
    return 1; // falha de propósito: o syncstate cai no caminho sem data
}

// ------------------------------------------------------------------ placar

static int falhas = 0, testes = 0;

static void ok(int cond, const char *msg)
{
    testes++;
    printf(cond ? "  ok   %s\n" : "  FALHA %s\n", msg);
    if (!cond) falhas++;
}

static void eq(const char *got, const char *want, const char *msg)
{
    testes++;
    int bom = strcmp(got, want) == 0;
    printf(bom ? "  ok   %s\n" : "  FALHA %s\n", msg);
    if (!bom) { printf("         esperava: \"%s\"\n         veio:     \"%s\"\n", want, got); falhas++; }
}

// --------------------------------------------------------------------- json

static void teste_json(void)
{
    printf("\n== ler as respostas do Google ==\n");

    // Resposta de verdade do endpoint de device code, com os campos fora de
    // ordem e com espaço entre os dois pontos e o valor — que é como o Google
    // manda mesmo.
    const char *devicecode =
        "{\n"
        "  \"device_code\": \"AH-1Ng2b_xYz\",\n"
        "  \"user_code\": \"GQVQ-JKEC\",\n"
        "  \"expires_in\": 1800,\n"
        "  \"interval\": 5,\n"
        "  \"verification_url\": \"https://www.google.com/device\"\n"
        "}";

    char buf[256];
    ok(json_get_string(devicecode, "device_code", buf, sizeof(buf)), "achou o device_code");
    eq(buf, "AH-1Ng2b_xYz", "e o valor veio inteiro");

    ok(json_get_string(devicecode, "user_code", buf, sizeof(buf)), "achou o codigo que ele digita");
    eq(buf, "GQVQ-JKEC", "com o hifen no meio");

    ok(json_get_string(devicecode, "verification_url", buf, sizeof(buf)), "achou a url");
    eq(buf, "https://www.google.com/device", "com as barras intactas (nao comeu o //)");

    double n = 0;
    ok(json_get_number(devicecode, "expires_in", &n) && n == 1800, "leu o expires_in como numero");
    ok(json_get_number(devicecode, "interval", &n) && n == 5, "leu o intervalo");

    ok(!json_get_string(devicecode, "refresh_token", buf, sizeof(buf)),
        "campo que nao existe da falso, e nao lixo");
    ok(!json_get_number(devicecode, "device_code", &n),
        "pedir numero num campo de texto da falso");

    // O Google escapa as barras em algumas respostas. Se o unescape falhar, a
    // url fica "https:\/\/..." e o link na tela sai quebrado.
    const char *escapado = "{\"verification_url\":\"https:\\/\\/www.google.com\\/device\","
                           "\"nome\":\"Jogo \\\"do\\\" Miguel\",\"quebra\":\"a\\nb\"}";
    ok(json_get_string(escapado, "verification_url", buf, sizeof(buf)), "achou a url escapada");
    eq(buf, "https://www.google.com/device", "desfez o \\/ das barras");
    ok(json_get_string(escapado, "nome", buf, sizeof(buf)), "achou o nome com aspas dentro");
    eq(buf, "Jogo \"do\" Miguel", "desfez o \\\" sem parar no meio");
    ok(json_get_string(escapado, "quebra", buf, sizeof(buf)), "achou o campo com quebra de linha");
    eq(buf, "a\nb", "virou quebra de linha de verdade");

    // Erro de token: o oauth.c le "error" pra saber se e authorization_pending
    // (continua esperando) ou access_denied (desiste).
    const char *erro = "{\n \"error\": \"authorization_pending\",\n"
                       " \"error_description\": \"Precondition Failed\"\n}";
    ok(json_get_string(erro, "error", buf, sizeof(buf)), "achou o erro");
    eq(buf, "authorization_pending", "e leu o certo, nao o error_description");

    // Listagem do Drive: bloco "files" com varios objetos dentro.
    const char *listagem =
        "{\"kind\":\"drive#fileList\",\"incompleteSearch\":false,\"files\":["
        "{\"id\":\"1AAA\",\"name\":\"Animal Crossing (console)\",\"mimeType\":\"application/vnd.google-apps.folder\"},"
        "{\"id\":\"2BBB\",\"name\":\"save.dat\",\"mimeType\":\"application/octet-stream\"},"
        "{\"id\":\"3CCC\",\"name\":\"Zelda [chave], v2\",\"mimeType\":\"application/octet-stream\"}"
        "]}";

    char bloco[1024];
    ok(json_get_block(listagem, "files", bloco, sizeof(bloco)), "achou o bloco files");
    ok(bloco[0] == '[' && bloco[strlen(bloco) - 1] == ']', "veio com os colchetes, do abre ao fecha");

    char elem[512];
    ok(json_first_element(bloco, elem, sizeof(elem)), "pegou o primeiro item");
    ok(json_get_string(elem, "id", buf, sizeof(buf)) && strcmp(buf, "1AAA") == 0,
        "e o id dele e o do primeiro, nao o do ultimo");

    // A iteracao e o que lista pasta inteira. Contar errado aqui = sync
    // deixando arquivo pra tras.
    const char *cursor;
    ok(json_array_begin(bloco, &cursor), "abriu o array pra percorrer");
    int quantos = 0;
    char ids[8][64] = {{0}}, nomes[8][256] = {{0}};
    while (json_array_next(&cursor, elem, sizeof(elem)) && quantos < 8)
    {
        json_get_string(elem, "id", ids[quantos], sizeof(ids[0]));
        json_get_string(elem, "name", nomes[quantos], sizeof(nomes[0]));
        quantos++;
    }
    ok(quantos == 3, "percorreu os 3 itens, nao 1 nem 2");
    eq(ids[0], "1AAA", "o primeiro id");
    eq(ids[2], "3CCC", "o ultimo id");
    eq(nomes[0], "Animal Crossing (console)", "nome com parentese passou inteiro");
    eq(nomes[2], "Zelda [chave], v2", "nome com colchete e virgula nao cortou o item ao meio");

    // Pasta vazia: o drive.c trata "[]" como sucesso com zero itens. Se isso
    // virasse erro, sync de pasta nova falharia.
    ok(json_get_block("{\"files\":[]}", "files", bloco, sizeof(bloco)), "achou o bloco vazio");
    eq(bloco, "[]", "e ele e mesmo so os colchetes");
    ok(!json_first_element(bloco, elem, sizeof(elem)), "array vazio nao devolve elemento");

    // Objeto aninhado: json_get_block tem que respeitar as chaves de dentro.
    const char *aninhado = "{\"a\":{\"b\":{\"c\":1},\"d\":\"}\"},\"e\":2}";
    ok(json_get_block(aninhado, "a", bloco, sizeof(bloco)), "achou o bloco aninhado");
    eq(bloco, "{\"b\":{\"c\":1},\"d\":\"}\"}", "fechou no lugar certo, sem cair na chave dentro da aspas");

    // Buffer curto: tem que recusar, e nao escrever metade nem estourar.
    char curto[8];
    json_get_string(devicecode, "verification_url", curto, sizeof(curto));
    ok(strlen(curto) < sizeof(curto), "com buffer curto, nao estourou o buffer");
}

// ------------------------------------------------------------------- nomes

static void teste_sanitize(void)
{
    printf("\n== limpar nome pra virar pasta ==\n");

    char out[0x201];

    syncstate_sanitize_name("Super Mario 3D World", out, sizeof(out));
    eq(out, "Super Mario 3D World", "nome normal passa igual");

    syncstate_sanitize_name("Pokemon: Let's Go / Eevee", out, sizeof(out));
    eq(out, "Pokemon_ Let's Go _ Eevee", "trocou os dois-pontos e a barra");

    syncstate_sanitize_name("a\\b:c*d?e\"f<g>h|i", out, sizeof(out));
    eq(out, "a_b_c_d_e_f_g_h_i", "trocou todos os proibidos do Windows/FAT");

    syncstate_sanitize_name("com\ttab\ne quebra", out, sizeof(out));
    eq(out, "com_tab_e quebra", "tirou tab e quebra de linha");

    syncstate_sanitize_name("Nome com espaco no fim   ", out, sizeof(out));
    eq(out, "Nome com espaco no fim", "comeu o espaco do fim (FAT nao gosta)");

    syncstate_sanitize_name("Termina com ponto...", out, sizeof(out));
    eq(out, "Termina com ponto", "comeu os pontos do fim");

    syncstate_sanitize_name("   ", out, sizeof(out));
    eq(out, "sem-nome", "so espaco vira sem-nome, e nao pasta de nome vazio");

    syncstate_sanitize_name("", out, sizeof(out));
    eq(out, "sem-nome", "vazio tambem");

    syncstate_sanitize_name("...", out, sizeof(out));
    eq(out, "sem-nome", "so pontos tambem — senao viraria \".\" ou \"..\"");

    // Acento e japones tem que sobreviver: metade dos jogos de quem usa tem.
    syncstate_sanitize_name("Coração & Ação — ゼルダ", out, sizeof(out));
    eq(out, "Coração & Ação — ゼルダ", "acento, traco longo e japones passam inteiros");

    // Buffer curto nao pode estourar nem deixar sem terminador.
    char pequeno[10];
    syncstate_sanitize_name("The Legend of Zelda: Tears of the Kingdom", pequeno, sizeof(pequeno));
    ok(strlen(pequeno) < sizeof(pequeno), "nome longo em buffer curto nao estourou");
    eq(pequeno, "The Legen", "cortou no tamanho do buffer");

    // outsz 0 nao pode escrever nada.
    char sentinela[4] = { 'X', 'X', 'X', 'X' };
    syncstate_sanitize_name("qualquer coisa", sentinela, 0);
    ok(sentinela[0] == 'X', "com outsz 0, nao escreveu byte nenhum");
}

// ------------------------------------------------------- o nome da pasta

static void teste_folder_name(void)
{
    printf("\n== o nome da pasta do save ==\n");

    char out[0x201];

    // Jogo com um save só: sem sufixo nenhum. É o caso da maioria.
    {
        TitleEntry t = {0};
        snprintf(t.name, sizeof(t.name), "Super Mario 3D World");
        snprintf(t.account, sizeof(t.account), "Miguel");
        t.shared_game = false;
        syncjob_save_folder_name(&t, out, sizeof(out));
        eq(out, "Super Mario 3D World", "save unico: sem o apelido no nome");
    }

    // O MESMO jogo, com o apelido trocado. Se o nome da pasta mudar, o backup
    // que já está na nuvem vira órfão.
    {
        TitleEntry t = {0};
        snprintf(t.name, sizeof(t.name), "Super Mario 3D World");
        snprintf(t.account, sizeof(t.account), "Outro Nome");
        t.shared_game = false;
        syncjob_save_folder_name(&t, out, sizeof(out));
        eq(out, "Super Mario 3D World",
            "save unico: trocar o apelido NAO muda a pasta (backup nao vira orfao)");
    }

    // Agora o jogo com dois saves. Aqui o apelido entra no nome — e é aqui que
    // renomear a conta quebra.
    {
        TitleEntry t = {0};
        snprintf(t.name, sizeof(t.name), "Mario Kart 8");
        snprintf(t.account, sizeof(t.account), "Miguel");
        t.shared_game = true;
        syncjob_save_folder_name(&t, out, sizeof(out));
        eq(out, "Mario Kart 8 (Player 1)", "duas contas: o apelido entra no nome");

        snprintf(t.account, sizeof(t.account), "Miguelzinho");
        syncjob_save_folder_name(&t, out, sizeof(out));
        eq(out, "Mario Kart 8 (Miguelzinho)",
            "!! duas contas: trocar o apelido MUDA a pasta — o backup antigo fica orfao");
    }

    // Device save: o sufixo é fixo, "(console)", e não depende de apelido
    // nenhum. Renomear conta não mexe nele. É o caso do Animal Crossing.
    {
        TitleEntry t = {0};
        snprintf(t.name, sizeof(t.name), "Animal Crossing New Horizons");
        snprintf(t.account, sizeof(t.account), "Miguel");
        t.shared_game = true;
        t.device_save = true;
        syncjob_save_folder_name(&t, out, sizeof(out));
        eq(out, "Animal Crossing New Horizons (console)", "save do console: sufixo fixo");

        snprintf(t.account, sizeof(t.account), "Nome Novo");
        syncjob_save_folder_name(&t, out, sizeof(out));
        eq(out, "Animal Crossing New Horizons (console)",
            "save do console: apelido nao entra, entao renomear nao afeta");
    }

    // Conta sem apelido legível: o código decide não chutar, pra não criar
    // pasta órfã na próxima versão quando o apelido aparecer.
    {
        TitleEntry t = {0};
        snprintf(t.name, sizeof(t.name), "Splatoon 3");
        t.shared_game = true;
        t.account[0] = '\0';
        syncjob_save_folder_name(&t, out, sizeof(out));
        eq(out, "Splatoon 3", "sem apelido, fica sem sufixo em vez de \"( )\"");
    }

    // Caractere proibido no apelido tem que ser limpo junto.
    {
        TitleEntry t = {0};
        snprintf(t.name, sizeof(t.name), "Zelda: Tears of the Kingdom");
        snprintf(t.account, sizeof(t.account), "Mi/gu:el");
        t.shared_game = true;
        syncjob_save_folder_name(&t, out, sizeof(out));
        eq(out, "Zelda_ Tears of the Kingdom (Mi_gu_el)",
            "limpou os proibidos do nome do jogo E do apelido");
    }

    // Nome de jogo enorme: quem encurta é o nome, nunca o sufixo — senão os
    // dois saves cairiam na MESMA pasta e um sobrescreveria o outro.
    {
        TitleEntry t = {0};
        for (int i = 0; i < 0x200; i++) t.name[i] = 'A';
        t.name[0x200] = '\0';
        snprintf(t.account, sizeof(t.account), "Miguel");
        t.shared_game = true;
        syncjob_save_folder_name(&t, out, sizeof(out));

        size_t len = strlen(out);
        ok(len >= 8 && strcmp(out + len - 8, "(Player 1)") == 0,
            "nome gigante: o sufixo sobreviveu ao corte (senao dois saves colidiriam)");

        // E o do OUTRO dono tem que sair diferente, mesmo com o nome gigante.
        char outro[0x201];
        snprintf(t.account, sizeof(t.account), "Fulano");
        syncjob_save_folder_name(&t, outro, sizeof(outro));
        ok(strcmp(out, outro) != 0, "e as duas contas ainda dao pastas diferentes");
    }
}

// ------------------------------------- o registro de qual pasta foi usada
//
// É o conserto do "mudei d nome e foi nao". O de cima prova o problema (o
// apelido entra no nome da pasta); este prova a saída: o par
// (application_id + uid) aponta pro nome que foi realmente usado, e o uid não
// muda quando o apelido muda.

static AccountUid conta(u64 a, u64 b)
{
    AccountUid u;
    u.uid[0] = a;
    u.uid[1] = b;
    return u;
}

static void teste_registro_de_pasta(void)
{
    printf("\n== em que pasta da nuvem esse save mora ==\n");

    remove(SYNC_FOLDERS_PATH);

    const u64 MK8   = 0x0100152000022000ull;
    const u64 ZELDA = 0x01007EF00011E000ull;

    AccountUid miguel = conta(0x1111111111111111ull, 0x2222222222222222ull);
    AccountUid Convidado  = conta(0x3333333333333333ull, 0x4444444444444444ull);
    AccountUid console = conta(0, 0);   // device save

    char out[0x201];

    ok(!syncstate_recall_folder(MK8, miguel, out, sizeof(out)),
        "sem arquivo nenhum, nao inventa pasta");

    syncstate_remember_folder(MK8, miguel, "Mario Kart 8 Deluxe (Player 1)");
    ok(syncstate_recall_folder(MK8, miguel, out, sizeof(out)), "gravou e achou de volta");
    ok(strcmp(out, "Mario Kart 8 Deluxe (Player 1)") == 0, "e veio o nome certo");

    // O CORACAO DO CONSERTO: o apelido mudou no console, o uid nao. O registro
    // continua apontando pra pasta antiga, entao o app acha o backup de volta
    // em vez de criar pasta nova e orfanar a velha.
    ok(syncstate_recall_folder(MK8, miguel, out, sizeof(out))
        && strcmp(out, "Mario Kart 8 Deluxe (Player 1)") == 0,
        "!! trocar o apelido nao mexe no registro — e o uid que manda");

    // Outra conta do mesmo jogo tem que ser outra linha.
    ok(!syncstate_recall_folder(MK8, Convidado, out, sizeof(out)),
        "a outra conta do mesmo jogo nao herda a pasta");
    syncstate_remember_folder(MK8, Convidado, "Mario Kart 8 Deluxe (Convidado)");
    ok(syncstate_recall_folder(MK8, Convidado, out, sizeof(out))
        && strcmp(out, "Mario Kart 8 Deluxe (Convidado)") == 0, "e a dela e a dela");
    ok(syncstate_recall_folder(MK8, miguel, out, sizeof(out))
        && strcmp(out, "Mario Kart 8 Deluxe (Player 1)") == 0,
        "gravar a dela nao estragou a dele");

    // Device save: uid zerado e uma chave legitima, separada das contas.
    syncstate_remember_folder(MK8, console, "Mario Kart 8 Deluxe (console)");
    ok(syncstate_recall_folder(MK8, console, out, sizeof(out))
        && strcmp(out, "Mario Kart 8 Deluxe (console)") == 0,
        "o save do console e uma chave propria (uid zerado)");
    ok(syncstate_recall_folder(MK8, miguel, out, sizeof(out))
        && strcmp(out, "Mario Kart 8 Deluxe (Player 1)") == 0,
        "e nao atropelou o da conta");

    // Mesma conta, jogo diferente.
    syncstate_remember_folder(ZELDA, miguel, "The Legend of Zelda_ Breath of the Wild");
    ok(syncstate_recall_folder(ZELDA, miguel, out, sizeof(out))
        && strcmp(out, "The Legend of Zelda_ Breath of the Wild") == 0,
        "outro jogo da mesma conta e outra linha");

    // Regravar a mesma chave SUBSTITUI, nao duplica — senao o arquivo cresceria
    // pra sempre e o recall pegaria a linha velha.
    syncstate_remember_folder(MK8, miguel, "Mario Kart 8 Deluxe (Miguelzinho)");
    ok(syncstate_recall_folder(MK8, miguel, out, sizeof(out))
        && strcmp(out, "Mario Kart 8 Deluxe (Miguelzinho)") == 0,
        "regravar a mesma chave troca o nome");
    {
        int linhas = 0;
        char l[0x280];
        FILE *f = fopen(SYNC_FOLDERS_PATH, "r");
        while (f && fgets(l, sizeof(l), f))
            if (l[0] != '#' && l[0] != '\n')
                linhas++;
        if (f) fclose(f);
        ok(linhas == 4, "e nao duplicou a linha (4 registros, nao 5)");
    }

    // Nome com acento e parentese tem que voltar byte a byte: e o caso normal,
    // nao a excecao ("Pokemon_ Let's Go, Pikachu! (Player 1)").
    syncstate_remember_folder(0xAAull, miguel, "Pokémon_ Let's Go, Pikachu! (Player 1)");
    ok(syncstate_recall_folder(0xAAull, miguel, out, sizeof(out))
        && strcmp(out, "Pokémon_ Let's Go, Pikachu! (Player 1)") == 0,
        "nome com acento, espaco, virgula e parentese volta igual");

    // Esquecer tira so o pedido.
    syncstate_forget_folder(MK8, miguel);
    ok(!syncstate_recall_folder(MK8, miguel, out, sizeof(out)), "esqueceu o pedido");
    ok(syncstate_recall_folder(MK8, Convidado, out, sizeof(out)), "e nao levou os vizinhos junto");
    ok(syncstate_recall_folder(ZELDA, miguel, out, sizeof(out)), "nem o outro jogo");

    // O arquivo e texto, pra dar pra entender com o cartao no PC.
    {
        char l[0x280] = { 0 };
        FILE *f = fopen(SYNC_FOLDERS_PATH, "r");
        if (f) { if (!fgets(l, sizeof(l), f)) l[0] = '\0'; fclose(f); }
        ok(l[0] == '#', "o arquivo comeca explicando o que e");
    }

    // Linha estragada no meio nao pode derrubar o resto — cartao corrompido e
    // edicao na mao acontecem.
    {
        FILE *f = fopen(SYNC_FOLDERS_PATH, "a");
        if (f)
        {
            fprintf(f, "isso nao e hexadecimal nenhum\n");
            fprintf(f, "00000000000000AA\n");            // faltando os uids
            fprintf(f, "00000000000000BB 1 2\n");        // sem o nome
            fprintf(f, "\n");
            fclose(f);
        }
        ok(syncstate_recall_folder(ZELDA, miguel, out, sizeof(out))
            && strcmp(out, "The Legend of Zelda_ Breath of the Wild") == 0,
            "linha estragada no arquivo nao derruba a leitura");
        ok(!syncstate_recall_folder(0xBBull, conta(1, 2), out, sizeof(out)),
            "e a linha sem nome nao vira registro vazio");
    }

    remove(SYNC_FOLDERS_PATH);
    ok(!syncstate_recall_folder(ZELDA, miguel, out, sizeof(out)),
        "apagar o pastas.txt volta ao estado de antes (nao quebra nada)");
}

// --------------------------------------------------------------------- main

int main(void)
{
    // O syncstate escreve log em sdmc:/switch/SwitchSaveSync — sem barra na
    // frente, entao vira pasta relativa aqui.
    mkdir("sdmc:", 0777);
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SwitchSaveSync", 0777);

    teste_json();
    teste_sanitize();
    teste_folder_name();
    teste_registro_de_pasta();

    printf("\n=========================================\n");
    printf("  %d testes, %d falha(s)\n", testes, falhas);
    printf("=========================================\n\n");
    return falhas != 0;
}
