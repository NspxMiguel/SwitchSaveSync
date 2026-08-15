// ftpd.h — servidor de FTP dentro do app, pra desenvolvimento.
//
// Existe por um motivo prático: pra mexer nos arquivos do console era preciso
// abrir o Sphaira e o DBI toda vez, só pra trocar um .nro ou olhar um log. Com
// isto, o próprio app abre a porta enquanto está na tela, e do computador dá
// pra usar Finder, FileZilla, `ftp`, `curl` ou um script.
//
// É função de DESENVOLVIMENTO, e ela se comporta como tal:
//
//   - só existe enquanto a tela estiver aberta. Sair da tela derruba o
//     servidor. Nada fica escutando pelas costas de ninguém;
//   - sem senha. É rede local e é o console de quem está com ele na mão —
//     pedir senha aqui daria uma sensação de segurança que não existe, porque
//     o FTP manda tudo em texto puro de qualquer jeito. A tela avisa;
//   - modo passivo só. O ativo pede que o servidor abra conexão de volta pro
//     cliente, o que quase nunca passa pelo roteador e não serve pra nada
//     numa rede doméstica;
//   - raiz em `sdmc:`, o cartão inteiro. O propósito é justamente trocar
//     arquivo em qualquer canto (`/switch`, `/atmosphere`, `/Nintendo`).
//
// Uma thread só, com poll(): sem trabalho, ela dorme; com transferência, ela
// bombeia pedaços de 64 KB. Não existe thread por conexão — o app está com a
// interface gráfica de pé e não sobra memória pra isso.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FTPD_PORTA_PADRAO 5001

// Sobe o servidor. Devolve false se a porta já estiver ocupada ou a rede não
// estiver de pé — o motivo fica em ftpd_ultimo_erro().
// Quantas conexoes atender ao mesmo tempo (1 a 4). Serve pra apertar o cinto
// quando a rede do app subiu na config enxuta de modo applet: la sobram poucos
// sockets pro processo, e prometer quatro sessoes vira transferencia falhando
// no meio em vez de uma recusa limpa.
void ftpd_limite_sessoes(int n);

bool ftpd_start(u16 porta);

// Derruba o servidor e espera a thread morrer. Pode chamar mesmo parado.
void ftpd_stop(void);

bool ftpd_running(void);

// Endereço do console nesta rede, em texto ("10.0.0.80"). "" se não deu.
const char *ftpd_endereco(void);

u16 ftpd_porta(void);

// Pra tela: quantas conexões abertas agora, e quanto já passou nos dois
// sentidos desde que o servidor subiu.
void ftpd_estado(int *conexoes, u64 *enviados, u64 *recebidos);

// A última coisa que aconteceu, pra tela mostrar sinal de vida
// ("RETR /switch/SwitchSaveSync/autosync.log"). Nunca contém senha porque não
// existe senha.
const char *ftpd_ultima_linha(void);

const char *ftpd_ultimo_erro(void);

#ifdef __cplusplus
}
#endif
