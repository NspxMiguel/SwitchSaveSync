// config.h de mentira, só pros testes.
//
// O core/config.h de verdade é gitignored (tem a credencial real), então numa
// máquina limpa — CI, ou quem acabou de clonar — ele não existe e o teste não
// compilaria. Como `#include "config.h"` procura primeiro na pasta do arquivo
// que inclui, o de verdade sempre ganha deste aqui quando existe.
//
// Nenhum valor daqui vale nada: são só letras pro compilador ter o que ligar.
#pragma once

#define GOOGLE_CLIENT_ID     "id-de-mentira.apps.googleusercontent.com"
#define GOOGLE_CLIENT_SECRET "segredo-de-mentira"
#define GOOGLE_OAUTH_SCOPE   "https://www.googleapis.com/auth/drive.file"
#define DRIVE_APP_FOLDER_NAME "Nintendo Switch Saves"
#define SSS_AUTH_ENDPOINT    ""
