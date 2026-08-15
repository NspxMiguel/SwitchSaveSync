#include "gfx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

// A camada é 1280x720 (tela cheia), mas o framebuffer é metade disso e o
// compositor estica. Motivo: memória. Em RGBA4444, 640x360 dá 460 KB por
// buffer; em 1280x720 RGBA8888 daria 3,6 MB por buffer, e essa heap também
// precisa caber o curl e o mbedTLS baixando o save ao mesmo tempo.
#define FB_W 640
#define FB_H 360

#define LAYER_W 1280
#define LAYER_H 720

extern u64 __nx_vi_layer_id;

static bool g_ready = false;
static ViDisplay g_display;
static ViLayer g_layer;
static NWindow g_window;
static Framebuffer g_fb;
static PadState g_pad;
static bool g_pad_ready = false;

static stbtt_fontinfo g_font;
static bool g_font_ready = false;

// ---------------------------------------------------------------------------
// Cor e pixel (RGBA4444: r nos 4 bits de baixo, a nos 4 de cima)
// ---------------------------------------------------------------------------

typedef u16 Color;

static inline Color rgba(u8 r, u8 g, u8 b, u8 a)
{
    return (Color)((r & 0xF) | ((g & 0xF) << 4) | ((b & 0xF) << 8) | ((a & 0xF) << 12));
}

static u16 *g_buf = NULL;
static u32 g_stride_px = FB_W;

static inline void blend_px(int x, int y, Color c, u8 alpha)
{
    if (!g_buf || x < 0 || y < 0 || x >= FB_W || y >= FB_H || alpha == 0)
        return;

    u16 *p = &g_buf[y * g_stride_px + x];

    if (alpha >= 0xF)
    {
        *p = c;
        return;
    }

    u16 d = *p;
    u8 inv = 0xF - alpha;
    u8 r = (((c >> 0) & 0xF) * alpha + ((d >> 0) & 0xF) * inv) / 0xF;
    u8 g = (((c >> 4) & 0xF) * alpha + ((d >> 4) & 0xF) * inv) / 0xF;
    u8 b = (((c >> 8) & 0xF) * alpha + ((d >> 8) & 0xF) * inv) / 0xF;
    *p = rgba(r, g, b, 0xF);
}

static void fill_rect(int x, int y, int w, int h, Color c)
{
    u8 a = (c >> 12) & 0xF;
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            blend_px(i, j, c, a);
}

static void stroke_rect(int x, int y, int w, int h, Color c)
{
    fill_rect(x, y, w, 2, c);
    fill_rect(x, y + h - 2, w, 2, c);
    fill_rect(x, y, 2, h, c);
    fill_rect(x + w - 2, y, 2, h, c);
}

// ---------------------------------------------------------------------------
// Texto
// ---------------------------------------------------------------------------

// UTF-8 -> codepoint. O texto tem acento ("Não puxar"), então não dá pra tratar
// byte a byte.
static const char *next_cp(const char *s, int *cp)
{
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) { *cp = c; return s + 1; }
    if ((c & 0xE0) == 0xC0 && s[1]) { *cp = ((c & 0x1F) << 6) | (s[1] & 0x3F); return s + 2; }
    if ((c & 0xF0) == 0xE0 && s[1] && s[2]) { *cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); return s + 3; }
    if ((c & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) { *cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); return s + 4; }
    *cp = '?';
    return s + 1;
}

static int text_width(const char *s, int size)
{
    if (!g_font_ready)
        return 0;

    float scale = stbtt_ScaleForPixelHeight(&g_font, (float)size);
    int total = 0, cp = 0;
    const char *p = s;
    while (*p)
    {
        const char *nxt = next_cp(p, &cp);
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
        total += (int)(adv * scale);
        p = nxt;
    }
    return total;
}

// Desenha a partir da linha de base em (x, y).
static void draw_text(int x, int y, int size, Color c, const char *s)
{
    if (!g_font_ready)
        return;

    float scale = stbtt_ScaleForPixelHeight(&g_font, (float)size);
    int cp = 0;
    const char *p = s;

    while (*p)
    {
        const char *nxt = next_cp(p, &cp);

        int gw = 0, gh = 0, gx = 0, gy = 0;
        unsigned char *bmp = stbtt_GetCodepointBitmap(&g_font, 0, scale, cp, &gw, &gh, &gx, &gy);
        if (bmp)
        {
            for (int j = 0; j < gh; j++)
                for (int i = 0; i < gw; i++)
                {
                    u8 v = bmp[j * gw + i];
                    if (v)
                        blend_px(x + gx + i, y + gy + j, c, (u8)(v >> 4));
                }
            stbtt_FreeBitmap(bmp, NULL);
        }

        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
        x += (int)(adv * scale);
        p = nxt;
    }
}

static void draw_text_centered(int cx, int y, int size, Color c, const char *s)
{
    draw_text(cx - text_width(s, size) / 2, y, size, c, s);
}

// ---------------------------------------------------------------------------
// Camada
// ---------------------------------------------------------------------------

static Result add_to_layer_stack(ViLayer *layer, ViLayerStack stack)
{
    // Não tem wrapper na libnx; é o mesmo comando que o Tesla manda.
    const struct
    {
        u32 stack;
        u64 layer_id;
    } in = { stack, layer->layer_id };

    return serviceDispatchIn(viGetSession_IManagerDisplayService(), 6000, in);
}

// O ActivateNpad do hid, na mão.
//
// A libnx tem hidInitializeNpad() pra isso, e ele NÃO serve aqui: a desmontagem
// mostra que ele termina em `cbnz w0, +0x80` / `bl diagAbortWithResult` — ou
// seja, o comando falhar mata o processo. Num aplicativo isso fecha o
// aplicativo; num processo de sistema quem paga é o console inteiro. Foi por
// isso que o padConfigureInput saiu daqui, e ele começava justamente chamando
// esta função — tirar um e deixar o outro não tirava nada.
//
// O comando é o mesmo que a libnx manda: ActivateNpad (103) até o firmware
// 4.x, ActivateNpadWithRevision (109) daí em diante, com a revisão que cada
// faixa de firmware espera. A diferença é só o fim: aqui o erro volta como
// erro. Sem o npad ativo o controle não responde, e pra isso a tela já tem
// saída por tempo.
static Result ativa_npad(void)
{
    u64 aruid = appletGetAppletResourceUserId(); // 0 no sysmodule: não é applet

    if (hosversionBefore(5, 0, 0))
    {
        const struct { u64 aruid; } in = { aruid };
        return serviceDispatchIn(hidGetServiceSession(), 103, in,
            .in_send_pid = true);
    }

    // As faixas saíram da desmontagem do hidInitializeNpad, não de chute:
    // cmp 0x4ffff -> cmd 103; 0x5ffff/0x7ffff/0x11ffff decidem a revisão
    // entre 1, 2, 3 e 5.
    u32 revisao = 1;
    if (hosversionAtLeast(18, 0, 0))
        revisao = 5;
    else if (hosversionAtLeast(8, 0, 0))
        revisao = 3;
    else if (hosversionAtLeast(6, 0, 0))
        revisao = 2;

    const struct
    {
        u32 revisao;
        u32 pad;
        u64 aruid;
    } in = { revisao, 0, aruid };

    return serviceDispatchIn(hidGetServiceSession(), 109, in,
        .in_send_pid = true);
}

static bool init_font(void)
{
    if (g_font_ready)
        return true;

    PlFontData fd;
    if (R_FAILED(plGetSharedFontByType(&fd, PlSharedFontType_Standard)))
        return false;

    if (!stbtt_InitFont(&g_font, (unsigned char *)fd.address,
            stbtt_GetFontOffsetForIndex((unsigned char *)fd.address, 0)))
        return false;

    g_font_ready = true;
    return true;
}

bool gfx_init(void)
{
    if (g_ready)
        return true;

    if (R_FAILED(viOpenDefaultDisplay(&g_display)))
        return false;

    if (R_FAILED(viCreateManagedLayer(&g_display, (ViLayerFlags)0, 0, &__nx_vi_layer_id)))
        goto fail_display;

    if (R_FAILED(viCreateLayer(&g_display, &g_layer)))
        goto fail_managed;

    viSetLayerScalingMode(&g_layer, ViScalingMode_FitToLayer);

    // Z máximo: essa tela tem que ficar na frente do jogo e do resto.
    s32 z = 0;
    if (R_SUCCEEDED(viGetZOrderCountMax(&g_display, &z)) && z > 0)
        viSetLayerZ(&g_layer, z);

    add_to_layer_stack(&g_layer, ViLayerStack_Default);
    add_to_layer_stack(&g_layer, ViLayerStack_Screenshot);
    add_to_layer_stack(&g_layer, ViLayerStack_Recording);
    add_to_layer_stack(&g_layer, ViLayerStack_Arbitrary);
    add_to_layer_stack(&g_layer, ViLayerStack_LastFrame);
    add_to_layer_stack(&g_layer, ViLayerStack_Null);
    add_to_layer_stack(&g_layer, ViLayerStack_ApplicationForDebug);
    add_to_layer_stack(&g_layer, ViLayerStack_Lcd);

    viSetLayerSize(&g_layer, LAYER_W, LAYER_H);
    viSetLayerPosition(&g_layer, 0, 0);

    if (R_FAILED(nwindowCreateFromLayer(&g_window, &g_layer)))
        goto fail_layer;

    if (R_FAILED(framebufferCreate(&g_fb, &g_window, FB_W, FB_H, PIXEL_FORMAT_RGBA_4444, 2)))
        goto fail_window;

    // Sem isso o framebuffer é block-linear (swizzled) e cada pixel exigiria
    // calcular o endereço no bloco. Linear troca um pouco de banda por código
    // simples — e essa tela desenha ~4 quadros por segundo.
    if (R_FAILED(framebufferMakeLinear(&g_fb)))
        goto fail_fb;

    // Limpa os DOIS buffers antes de qualquer coisa.
    //
    // A camada fica visível no instante em que é criada, e o que ela mostra
    // até alguém escrever é memória de vídeo que era de outro processo — no
    // console apareceu como um retângulo com pedaços do menu do Switch dentro.
    // Um framebufferBegin/End só limpa o buffer da vez; com duplo buffer, o
    // outro continua com lixo e ele volta a aparecer no quadro seguinte.
    // O End só existe se o Begin tiver entregado buffer: o header da libnx diz
    // que "each call to framebufferBegin must be paired with a framebufferEnd
    // call" — o par é do Begin que DEU CERTO. Chamar End sem buffer devolve à
    // fila do compositor uma coisa que nunca saiu dela, e o pull_redraw deste
    // mesmo arquivo já fazia certo (sai fora sem End quando não vem buffer).
    for (int i = 0; i < 2; i++)
    {
        u32 stride = 0;
        u16 *b = (u16 *)framebufferBegin(&g_fb, &stride);
        if (!b)
            break;
        memset(b, 0, (size_t)stride * FB_H);
        framebufferEnd(&g_fb);
    }

    init_font(); // sem fonte ainda dá pra mostrar a barra; não é motivo pra desistir

    if (!g_pad_ready)
    {
        // Aqui NÃO dá pra usar padConfigureInput: a desmontagem da libnx mostra
        // que ele chama diagAbortWithResult quando o hid recusa. Num aplicativo
        // isso fecha o aplicativo; num sysmodule mata um processo de sistema, e
        // quem paga é o console inteiro. E o hid tem motivo pra recusar: essas
        // duas chamadas mandam o AppletResourceUserId, que aqui é 0 porque o
        // sysmodule não é applet nenhum (__nx_applet_type = AppletType_None).
        //
        // Então chama as mesmas duas funções na mão e ignora o resultado. O pior
        // caso vira "a tela aparece mas não responde ao controle", e pra isso já
        // existe a saída por tempo — muito melhor que derrubar o sistema.
        static const HidNpadIdType ids[] = {
            HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4,
            HidNpadIdType_No5, HidNpadIdType_No6, HidNpadIdType_No7, HidNpadIdType_No8,
            HidNpadIdType_Handheld,
        };
        ativa_npad(); // o que o hidInitializeNpad faz, sem o abort dele dentro
        hidSetSupportedNpadIdType(ids, sizeof(ids) / sizeof(ids[0]));
        hidSetSupportedNpadStyleSet(HidNpadStyleSet_NpadStandard);
        padInitializeAny(&g_pad);

        // Uma leitura jogada fora, de propósito.
        //
        // O padGetButtonsDown é `~antes & agora`, e o padInitializeAny zera os
        // dois. Então a PRIMEIRA leitura reporta como "acabou de ser apertado"
        // tudo que já estava segurado — e essa leitura acontece antes de o
        // primeiro quadro aparecer. Quem estivesse com o Y segurado por acaso
        // marcava o jogo como "não puxar" sem ver tela nenhuma. Descartando uma
        // leitura aqui, a próxima já compara com o que a mão está segurando.
        padUpdate(&g_pad);
        g_pad_ready = true;
    }

    g_ready = true;
    return true;

    // Desfazer na ordem inversa, e sem repetir o erro do gfx_exit: quem destrói
    // a camada gerenciada é o viDestroyManagedLayer sozinho.
fail_fb:
    framebufferClose(&g_fb);
fail_window:
    nwindowClose(&g_window);
fail_layer:
    viDestroyManagedLayer(&g_layer); // já fecha a camada por dentro
    goto fail_display;
fail_managed:
    // Aqui o viCreateLayer falhou, então g_layer não vale nada — mas a camada
    // GERENCIADA existe, e vazar ela é o que trava o console. O id dela está
    // no global que o viCreateManagedLayer acabou de preencher, e o destroy só
    // precisa disso: a desmontagem mostra que ele lê o primeiro campo do
    // ViLayer e manda pro vi.
    g_layer = (ViLayer){ .layer_id = __nx_vi_layer_id };
    viDestroyManagedLayer(&g_layer);
fail_display:
    __nx_vi_layer_id = 0;
    viCloseDisplay(&g_display);
    return false;
}

void gfx_exit(void)
{
    if (!g_ready)
        return;

    // viCloseLayer NÃO pode vir antes do viDestroyManagedLayer.
    //
    // Isto congelava o console. Desmontando o libnx.a dá pra ver os dois:
    //
    //   viCloseLayer          ... stp xzr, xzr, [x2]   <- ZERA os 16 primeiros
    //                                                     bytes do ViLayer, e o
    //                                                     primeiro campo é o
    //                                                     layer_id
    //   viDestroyManagedLayer ldr x0, [x20]            <- lê o layer_id
    //                         str x0, [x19, #32]          e manda pro vi
    //
    // Ou seja: fechar antes zera o id, e o destroy manda "destrua a camada 0".
    // A camada gerenciada NUNCA era destruída — ficava viva no compositor, no
    // Z máximo, por cima do menu. Uma por sincronização, acumulando, até o
    // console travar. É por isso que ele congelou logo depois de sair da tela.
    //
    // A libtesla faz exatamente o que está aqui embaixo: framebuffer, janela,
    // destroy (que já fecha a camada por dentro), display. Sem o viCloseLayer.
    framebufferClose(&g_fb);
    nwindowClose(&g_window);
    viDestroyManagedLayer(&g_layer);
    viCloseDisplay(&g_display);

    // O viCreateLayer da libnx lê este global pra saber a qual camada
    // gerenciada se acoplar. Deixar o id de uma camada já destruída aqui é
    // marcar encontro com um fantasma na próxima abertura.
    __nx_vi_layer_id = 0;

    g_buf = NULL;
    g_ready = false;
}

u64 gfx_keys_down(void)
{
    if (!g_pad_ready)
        return 0;
    padUpdate(&g_pad);
    return padGetButtonsDown(&g_pad);
}

// ---------------------------------------------------------------------------
// A tela
// ---------------------------------------------------------------------------

static void draw_button(int x, int y, int w, int h, const char *label, bool selected, bool enabled)
{
    Color bg     = selected ? rgba(0x0, 0x9, 0xC, 0xF) : rgba(0x3, 0x3, 0x3, 0xF);
    Color border = selected ? rgba(0x6, 0xE, 0xF, 0xF) : rgba(0x5, 0x5, 0x5, 0xF);
    Color fg     = enabled ? rgba(0xF, 0xF, 0xF, 0xF) : rgba(0x7, 0x7, 0x7, 0xF);

    if (!enabled)
    {
        bg     = rgba(0x2, 0x2, 0x2, 0xF);
        border = selected ? rgba(0x6, 0x6, 0x6, 0xF) : rgba(0x3, 0x3, 0x3, 0xF);
    }

    fill_rect(x, y, w, h, bg);
    stroke_rect(x, y, w, h, border);
    draw_text_centered(x + w / 2, y + h / 2 + 5, 15, fg, label);
}

void gfx_draw_pull(const char *game, int pct, const char *line, int selected, bool done)
{
    if (!g_ready)
        return;

    u32 stride = 0;
    g_buf = (u16 *)framebufferBegin(&g_fb, &stride);
    if (!g_buf)
        return;
    g_stride_px = stride / sizeof(u16);

    // Fundo: o jogo continua atrás, só escurecido — igual às telas do console.
    for (u32 j = 0; j < FB_H; j++)
        for (u32 i = 0; i < FB_W; i++)
            g_buf[j * g_stride_px + i] = rgba(0x1, 0x1, 0x2, 0xE);

    const int px = 60, py = 60, pw = FB_W - 120, ph = 240;

    fill_rect(px, py, pw, ph, rgba(0x2, 0x2, 0x3, 0xF));
    stroke_rect(px, py, pw, ph, rgba(0x5, 0x5, 0x6, 0xF));

    draw_text_centered(FB_W / 2, py + 42, 24, rgba(0xF, 0xF, 0xF, 0xF), "Puxando save da nuvem");

    if (game && game[0])
        draw_text_centered(FB_W / 2, py + 68, 15, rgba(0xB, 0xB, 0xB, 0xF), game);

    // Barra
    const int bx = px + 40, by = py + 92, bw = pw - 80, bh = 14;
    fill_rect(bx, by, bw, bh, rgba(0x1, 0x1, 0x1, 0xF));

    int fillw;
    if (pct < 0)
        fillw = bw / 6; // total desconhecido: só mostra que está andando
    else
        fillw = (bw * (pct > 100 ? 100 : pct)) / 100;

    if (fillw > 0)
        fill_rect(bx, by, fillw, bh, rgba(0x0, 0xC, 0xE, 0xF));
    stroke_rect(bx, by, bw, bh, rgba(0x5, 0x5, 0x6, 0xF));

    char pctbuf[16];
    if (pct < 0)
        snprintf(pctbuf, sizeof(pctbuf), "...");
    else
        snprintf(pctbuf, sizeof(pctbuf), "%d%%", pct > 100 ? 100 : pct);
    draw_text_centered(FB_W / 2, by + bh + 22, 16, rgba(0xF, 0xF, 0xF, 0xF), pctbuf);

    if (line && line[0])
        draw_text_centered(FB_W / 2, by + bh + 42, 13, rgba(0x9, 0x9, 0x9, 0xF), line);

    // Botões
    const int byy = py + ph - 52, bh2 = 34;
    //
    // O nome da tecla vai escrito no botao, e a tecla NAO e o A de proposito:
    // esta tela aparece por cima do menu do console, e o menu continua lendo o
    // controle junto com a gente (o hid entrega o mesmo aperto pros dois). Um A
    // aqui e um A la embaixo — ou seja, abre o jogo em destaque. B e Y o menu
    // ignora ou so volta.
    draw_button(px + 24, byy, 300, bh2, "Y: Nao puxar save deste jogo",
        selected == GFX_BTN_NEVER, true);
    draw_button(px + pw - 24 - 110, byy, 110, bh2, done ? "B: Fechar" : "Aguarde",
        selected == GFX_BTN_OK, done);

    framebufferEnd(&g_fb);
    g_buf = NULL;
}
