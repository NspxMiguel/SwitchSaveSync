#include "parental.hpp"

#include <switch.h>

#include <cstdio>
#include <cstring>

extern "C" {
#include "lang.h"
#include "syncstate.h"
}

#define PIN_PATH SYNC_APP_DIR "/pin.txt"

// Guarda um hash, não os dígitos.
//
// Sem ilusão sobre o que isso é: 4 dígitos são 10 mil combinações, então quem
// tiver o arquivo e vontade descobre a senha em segundos. O que resolve é o
// caso real desta casa — a criança abrir o cartão no PC (ou o FTP do DBI) e
// LER a senha do pai escrita em texto puro. Pra isso basta, e não arrasta uma
// biblioteca de criptografia pra dentro de um app de save.
//
// FNV-1a de 64 bits, com um tempero fixo só pra o resultado não bater com o de
// qualquer outra tabela FNV pronta que exista por aí.
static u64 hash_pin(const std::string& pin)
{
    u64 h = 0xcbf29ce484222325ULL;
    for (unsigned char c : ("SwitchSaveSync:" + pin))
    {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

static bool read_hash(u64* out)
{
    FILE* f = fopen(PIN_PATH, "r");
    if (!f)
        return false;

    unsigned long long v = 0;
    bool ok              = (fscanf(f, "%llx", &v) == 1);
    fclose(f);

    if (ok)
        *out = (u64)v;
    return ok;
}

bool Parental::isSet()
{
    u64 ignored;
    return read_hash(&ignored);
}

bool Parental::save(const std::string& pin)
{
    syncstate_ensure_dirs();

    FILE* f = fopen(PIN_PATH, "w");
    if (!f)
        return false;

    fprintf(f, "%016llx\n", (unsigned long long)hash_pin(pin));
    // fclose devolve erro quando o cartão encheu ou saiu no meio da escrita.
    // Sem conferir, o app diria "senha gravada" e na próxima vez abriria
    // conversa privada removida do historico
    return fclose(f) == 0;
}

void Parental::clear()
{
    remove(PIN_PATH);
}

bool Parental::matches(const std::string& pin)
{
    u64 saved;
    if (!read_hash(&saved))
        return true; // sem senha gravada, tudo passa

    return hash_pin(pin) == saved;
}

// Teclado numérico do console, chamado direto na libnx.
//
// Não uso o brls::Swkbd::openForNumber aqui por dois motivos concretos:
// ele passa o que foi digitado por std::stol, o que (a) come o zero da frente
// — "0123" viraria 123, e senha de criança começa com zero o tempo todo — e
// (b) joga exceção se o texto não for número. Aqui a senha fica sendo a
// string exata que ele digitou.
bool Parental::prompt(const std::string& header, const std::string& sub, std::string& out)
{
    out.clear();

    SwkbdConfig cfg;
    if (R_FAILED(swkbdCreate(&cfg, 0)))
        return false;

    swkbdConfigMakePresetDefault(&cfg);
    swkbdConfigSetType(&cfg, SwkbdType_NumPad);
    swkbdConfigSetHeaderText(&cfg, header.c_str());
    swkbdConfigSetSubText(&cfg, sub.c_str());
    swkbdConfigSetStringLenMin(&cfg, 4);
    swkbdConfigSetStringLenMax(&cfg, 8);
    swkbdConfigSetPasswordFlag(&cfg, 1); // esconde os dígitos com bolinha
    swkbdConfigSetBlurBackground(&cfg, true);
    // Sem tecla extra nos cantos do teclado numérico: assim só entra dígito, e
    // a senha nunca ganha um "-" ou "." que ele não conseguiria repetir.
    swkbdConfigSetLeftOptionalSymbolKey(&cfg, "");
    swkbdConfigSetRightOptionalSymbolKey(&cfg, "");

    char buffer[0x21] = { 0 };
    Result rc         = swkbdShow(&cfg, buffer, sizeof(buffer));
    swkbdClose(&cfg);

    if (R_FAILED(rc) || buffer[0] == '\0')
        return false; // cancelou

    out = buffer;
    return true;
}

bool Parental::unlockAtStartup()
{
    if (!Parental::isSet())
        return true;

    // Três tentativas, não infinitas: quem errou três vezes não sabe a senha,
    // e um teclado que reabre pra sempre vira um app que não fecha nunca.
    for (int tentativa = 1; tentativa <= 3; tentativa++)
    {
        std::string pin;
        char sub[128];
        if (tentativa == 1)
            snprintf(sub, sizeof(sub), "%s",
                TR("Digite a senha pra abrir o SwitchSaveSync",
                    "Type the password to open SwitchSaveSync"));
        else
            snprintf(sub, sizeof(sub),
                TR("Senha errada. Tentativa %d de 3", "Wrong password. Attempt %d of 3"),
                tentativa);

        if (!Parental::prompt(TR("SwitchSaveSync — senha", "SwitchSaveSync — password"), sub, pin))
            return false; // cancelou de propósito: o app fecha, sem drama

        if (Parental::matches(pin))
            return true;
    }

    return false;
}
