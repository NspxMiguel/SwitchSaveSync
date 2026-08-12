// parental.hpp — trava de senha na entrada do app.
//
// Controle parental com senha. O estrago que uma criança pode fazer aqui é
// bem específico: puxar o save da nuvem por cima do save do console (ou o
// contrário) e apagar progresso. Uma pergunta de senha na ENTRADA resolve os
// dois de uma vez — sem entrar, não tem botão pra clicar.
//
// O que isto NÃO é: segurança. Quem tiver o cartão SD na mão apaga o
// pin.txt e entra. É tranca de criança, e está escrito assim na tela pra
// ninguém confiar demais.
#pragma once

#include <string>

namespace Parental
{
// Tem senha gravada no cartão?
bool isSet();

// Grava a senha. Devolve false se não conseguiu escrever no cartão — nesse
// caso NADA foi gravado, e é importante avisar em vez de fingir que travou.
bool save(const std::string& pin);

// Apaga a senha (destrava o app).
void clear();

// A senha digitada bate com a gravada?
bool matches(const std::string& pin);

// Abre o teclado numérico do console e devolve o que foi digitado. Devolve
// false se cancelaram (aí a string sai vazia).
bool prompt(const std::string& header, const std::string& sub, std::string& out);

// A tranca da entrada: se não tem senha, passa direto. Se tem, pergunta —
// até 3 tentativas. Devolve false quando é pra o app NÃO abrir.
//
// Roda antes da interface gráfica existir, de propósito: o teclado do
// console é um applet do sistema, e chamá-lo antes de a borealis criar o
// contexto de vídeo é o caminho mais simples e sem efeito colateral.
bool unlockAtStartup();
}
