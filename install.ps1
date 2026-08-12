# SwitchSaveSync — puts the app on the SD card for you. Windows.
#
#   irm https://raw.githubusercontent.com/NspxMiguel/SwitchSaveSync/main/install.ps1 | iex
#
# Or double-click install.bat, which is this same script without the typing.
#
# It finds the card, downloads the latest release and copies the files where
# they belong. It NEVER deletes anything and it never formats: the only files
# it writes are the four that belong to this app.
#
#   switch\SwitchSaveSync.nro                              the app
#   switch\.overlays\SwitchSaveSync.ovl                    Ultrahand overlay
#   atmosphere\contents\00FF0000535953FF\exefs.nsp         autosync sysmodule
#   atmosphere\contents\00FF0000535953FF\toolbox.json      goes beside it
#
# Options:  -Card E:\   -AppOnly   -Zip arquivo.zip   -Version v0.4.0   -Yes   -Eject
#           -NoHbl   (don't touch atmosphere\config\override_config.ini)
#
# Speaks Portuguese when Windows does. Written for PowerShell 5.1, which is
# what comes with Windows 10 and 11 — nothing to install.

[CmdletBinding()]
param(
    [string] $Card,
    [string] $Zip,
    [string] $Version,
    [switch] $AppOnly,
    [switch] $Yes,
    [switch] $Eject,
    [switch] $NoHbl
)

$ErrorActionPreference = 'Stop'
# Sem isto o Invoke-WebRequest desenha uma barra de progresso que deixa o
# download ate' 10x mais lento no PowerShell 5.1. Nao e' exagero.
$ProgressPreference = 'SilentlyContinue'

$repo    = 'NspxMiguel/SwitchSaveSync'
$api     = "https://api.github.com/repos/$repo/releases"
$pagina  = "https://github.com/$repo/releases"

$arquivosApp  = @('switch\SwitchSaveSync.nro')
$arquivosAuto = @(
    'switch\.overlays\SwitchSaveSync.ovl',
    'atmosphere\contents\00FF0000535953FF\exefs.nsp',
    'atmosphere\contents\00FF0000535953FF\toolbox.json'
)

# --------------------------------------------------------------- conversa

$pt = (Get-Culture).Name -like 'pt*'

function T([string] $emPortugues, [string] $emIngles) {
    if ($pt) { return $emPortugues } else { return $emIngles }
}

function Diz([string] $emPortugues, [string] $emIngles, [string] $cor = 'Gray') {
    Write-Host (T $emPortugues $emIngles) -ForegroundColor $cor
}

function Morre([string] $emPortugues, [string] $emIngles) {
    Write-Host ('  x ' + (T $emPortugues $emIngles)) -ForegroundColor Red
    exit 1
}

function Humano([long] $n) {
    if ($n -ge 1MB) { return ('{0:N1} MB' -f ($n / 1MB)) }
    if ($n -ge 1KB) { return ('{0:N0} KB' -f ($n / 1KB)) }
    return "$n B"
}

# ------------------------------------------------------- achar o cartao

# Quanto mais disto a unidade tem, mais ela parece o cartao de um Switch com
# CFW. Nenhum item sozinho decide: um cartao recem-formatado so tem Nintendo\.
function Nota([string] $raiz) {
    $nota = 0
    if (Test-Path (Join-Path $raiz 'atmosphere'))  { $nota += 3 }
    if (Test-Path (Join-Path $raiz 'bootloader')) { $nota += 2 }
    if (Test-Path (Join-Path $raiz 'switch'))     { $nota += 2 }
    if (Test-Path (Join-Path $raiz 'Nintendo'))   { $nota += 1 }
    if (Test-Path (Join-Path $raiz 'payload.bin')){ $nota += 1 }
    if (Test-Path (Join-Path $raiz 'boot.dat'))   { $nota += 1 }
    return $nota
}

function Candidatos {
    # DriveType 2 e' removivel e 3 e' fixo. O leitor de cartao embutido em
    # muito notebook se apresenta como FIXO, entao os dois entram — o que
    # protege de verdade e' exigir FAT32/exFAT e nunca aceitar o disco do
    # Windows.
    $sistema = $env:SystemDrive
    Get-CimInstance Win32_LogicalDisk -ErrorAction SilentlyContinue |
        Where-Object {
            ($_.DriveType -eq 2 -or $_.DriveType -eq 3) -and
            $_.DeviceID -ne $sistema -and
            $_.FileSystem -and
            ($_.FileSystem.ToUpper() -in @('FAT32', 'EXFAT', 'FAT')) -and
            (Test-Path ($_.DeviceID + '\'))
        } |
        ForEach-Object {
            $raiz = $_.DeviceID + '\'
            [pscustomobject]@{
                Raiz   = $raiz
                Nota   = (Nota $raiz)
                Rotulo = $_.VolumeName
                Fs     = $_.FileSystem
                Livre  = $_.FreeSpace
            }
        } |
        Sort-Object -Property Nota -Descending
}

function EscolheCartao {
    $lista = @(Candidatos)
    if ($lista.Count -eq 0) {
        Diz 'Nao achei nenhum cartao FAT32/exFAT.' 'No FAT32/exFAT card found.'
        Diz 'Enfia o cartao do Switch no computador e roda de novo — ou passa -Card E:\' `
            'Put the Switch card in the computer and run again — or pass -Card E:\'
        exit 1
    }

    if ($lista.Count -eq 1 -and ($Yes -or $lista[0].Nota -ge 3)) {
        return $lista[0].Raiz
    }

    Write-Host ''
    Diz 'Cartoes que eu achei:' 'Cards I found:'
    for ($i = 0; $i -lt $lista.Count; $i++) {
        $c = $lista[$i]
        if ($c.Nota -ge 3) {
            $marca = T 'parece um Switch com CFW' 'looks like a Switch on CFW'
            $cor = 'Green'
        } elseif ($c.Nota -ge 1) {
            $marca = T 'talvez' 'maybe'
            $cor = 'Yellow'
        } else {
            $marca = T 'vazio, nao da pra saber' 'empty, no way to tell'
            $cor = 'Gray'
        }
        Write-Host ("  {0}) {1}  {2}  {3}  — " -f ($i + 1), $c.Raiz, $c.Rotulo, $c.Fs) -NoNewline
        Write-Host $marca -ForegroundColor $cor
    }

    while ($true) {
        $r = Read-Host (T "`nQual? [1-$($lista.Count), ou Enter pra cancelar]" `
                          "`nWhich one? [1-$($lista.Count), or Enter to cancel]")
        if ([string]::IsNullOrWhiteSpace($r)) {
            Diz 'Cancelado, nada foi escrito.' 'Cancelled, nothing was written.'
            exit 0
        }
        $n = 0
        if ([int]::TryParse($r, [ref] $n) -and $n -ge 1 -and $n -le $lista.Count) {
            return $lista[$n - 1].Raiz
        }
    }
}

# ------------------------------------------- o R que faz o app abrir direito

# Segurar R em cima de um jogo so' entra no homebrew se o Atmosphere estiver
# com override_any_app ligado, e ele NAO liga sozinho: o pacote traz o arquivo
# em config_templates\ e nunca escreve por cima do que esta em config\. Sem
# isso, a pessoa segura R, o jogo abre normal, e conclui que o app nao presta.
#
# Regra: se o arquivo NAO existe, eu crio. Se existe, eu nao reescrevo a
# configuracao de ninguem — no maximo pergunto se posso acrescentar, guardando
# uma copia antes.
$blocoHbl = @'
[hbl_config]
program_id_1=010000000000100d
override_key_0=R

; Segurar R ao abrir um jogo instalado entra no homebrew em MODO APLICACAO,
; que e o que da memoria e rede de jogo de verdade. Abrir o jogo sem segurar
; nada continua abrindo o jogo.
override_any_app=true
override_any_app_key=R
override_any_app_address_space=39_bit
path=atmosphere/hbl.nsp
'@

function CuidaDoHbl([string] $raiz) {
    if ($NoHbl) { return }
    $ini = Join-Path $raiz 'atmosphere\config\override_config.ini'

    if (-not (Test-Path $ini)) {
        $pasta = Split-Path $ini -Parent
        if (-not (Test-Path $pasta)) { New-Item -ItemType Directory -Path $pasta -Force | Out-Null }
        Set-Content -LiteralPath $ini -Value $blocoHbl -Encoding ASCII
        Write-Host '  v ' -ForegroundColor Green -NoNewline
        Write-Host (T 'atmosphere\config\override_config.ini criado (e o que faz o R funcionar)' `
                      'atmosphere\config\override_config.ini created (this is what makes R work)')
        return
    }

    if ((Get-Content -LiteralPath $ini -Raw) -match '(?im)^\s*override_any_app\s*=\s*true') { return }

    Write-Host ''
    Write-Host (T 'O teu override_config.ini nao tem override_any_app=true — sem isso, segurar R nao faz nada.' `
                  'Your override_config.ini has no override_any_app=true — without it, holding R does nothing.') -ForegroundColor Yellow

    if ($Yes) {
        Diz "Nao mexo num arquivo de configuracao que ja existe. Acrescenta a mao, no fim de ${ini}:" `
            "I don't rewrite a config file that already exists. Add this yourself, at the end of ${ini}:"
        Write-Host "`n    override_any_app=true`n    override_any_app_key=R`n    override_any_app_address_space=39_bit`n    path=atmosphere/hbl.nsp`n"
        return
    }

    $r = Read-Host (T 'Posso acrescentar essas linhas no fim dele? Guardo uma copia antes. [s/N]' `
                      'May I append those lines to it? I keep a copy first. [y/N]')
    if ($r -notmatch '^(s|sim|y|yes)$') {
        Diz 'Ok, nao mexi nele.' 'Fine, I left it alone.'
        return
    }

    Copy-Item -LiteralPath $ini -Destination ($ini + '.antes-do-SwitchSaveSync') -Force
    Add-Content -LiteralPath $ini -Value "`n; --- SwitchSaveSync ---`noverride_any_app=true`noverride_any_app_key=R`noverride_any_app_address_space=39_bit`npath=atmosphere/hbl.nsp"
    Write-Host '  v ' -ForegroundColor Green -NoNewline
    Write-Host (T 'acrescentado (copia do antigo em override_config.ini.antes-do-SwitchSaveSync)' `
                  'appended (the old one is at override_config.ini.antes-do-SwitchSaveSync)')
}

# ------------------------------------------------------------- baixar

function UrlDoZip {
    if ($Version) { $alvo = "$api/tags/$Version" } else { $alvo = "$api/latest" }
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    } catch { }

    try {
        $release = Invoke-RestMethod -Uri $alvo -Headers @{ 'User-Agent' = 'SwitchSaveSync-installer' }
        $asset = $release.assets | Where-Object { $_.name -like '*-sd.zip' } | Select-Object -First 1
        if ($asset) { return $asset.browser_download_url }
    } catch { }

    # A API do GitHub da 60 chamadas por hora por IP. Quando ela fecha, a
    # pagina da release em HTML ainda responde e tem o mesmo link.
    try {
        if ($Version) { $pag = "$pagina/expanded_assets/$Version" } else { $pag = "$pagina/latest" }
        $html = (Invoke-WebRequest -Uri $pag -UseBasicParsing -Headers @{ 'User-Agent' = 'SwitchSaveSync-installer' }).Content
        $m = [regex]::Match($html, "/$repo/releases/download/[^""]*-sd\.zip")
        if ($m.Success) { return 'https://github.com' + $m.Value }
    } catch { }

    return $null
}

# ------------------------------------------------------------- instalar

Write-Host 'SwitchSaveSync' -ForegroundColor White
Diz 'Instalador de cartao — nao apaga nada do que ja esta la.' `
    'SD card installer — it deletes nothing that is already there.'

if (-not $Card) { $Card = EscolheCartao }
if (-not (Test-Path $Card)) { Morre "nao achei $Card" "no such path: $Card" }

if ((Nota $Card) -lt 3 -and -not $Yes) {
    Write-Host (T "Essa unidade nao tem atmosphere\ nem switch\ — pode nao ser o cartao do Switch." `
                  "That drive has no atmosphere\ or switch\ — it may not be the Switch card.") -ForegroundColor Yellow
    $r = Read-Host (T "Escrever em $Card mesmo assim? [s/N]" "Write to $Card anyway? [y/N]")
    if ($r -notmatch '^(s|sim|y|yes)$') {
        Diz 'Cancelado, nada foi escrito.' 'Cancelled, nothing was written.'
        exit 0
    }
}

$temporaria = Join-Path ([System.IO.Path]::GetTempPath()) ('sss-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporaria -Force | Out-Null

try {
    $zipLocal = Join-Path $temporaria 'sd.zip'

    if ($Zip) {
        if (-not (Test-Path $Zip)) { Morre "nao achei o arquivo $Zip" "no such file: $Zip" }
        Copy-Item -LiteralPath $Zip -Destination $zipLocal -Force
        Write-Host ('  ' + (T 'arquivo: ' 'file: ') + $Zip)
    } else {
        Write-Host ('  ' + (T 'procurando a versao mais nova... ' 'looking for the newest release... ')) -NoNewline
        $url = UrlDoZip
        if (-not $url) {
            Morre "nao achei o zip da release. Sem internet? Baixa a mao em $pagina/latest e roda com -Zip" `
                  "couldn't find the release zip. No internet? Download it from $pagina/latest and use -Zip"
        }
        Write-Host (Split-Path $url -Leaf)
        Write-Host ('  ' + (T 'baixando... ' 'downloading... ')) -NoNewline
        Invoke-WebRequest -Uri $url -OutFile $zipLocal -UseBasicParsing -Headers @{ 'User-Agent' = 'SwitchSaveSync-installer' }
        Write-Host (Humano (Get-Item $zipLocal).Length)
    }

    # Descompactar num canto isolado, conferir, e so entao encostar no cartao.
    $saco = Join-Path $temporaria 'aberto'
    New-Item -ItemType Directory -Path $saco -Force | Out-Null
    Expand-Archive -LiteralPath $zipLocal -DestinationPath $saco -Force

    if (-not (Test-Path (Join-Path $saco $arquivosApp[0]))) {
        Morre 'o zip nao tem o app dentro — versao errada?' 'that zip has no app inside — wrong release?'
    }

    if ($AppOnly) { $lista = $arquivosApp } else { $lista = $arquivosApp + $arquivosAuto }

    $precisa = 0
    foreach ($rel in $lista) {
        $o = Join-Path $saco $rel
        if (Test-Path $o) { $precisa += (Get-Item $o).Length }
    }
    $livre = (Get-CimInstance Win32_LogicalDisk -ErrorAction SilentlyContinue |
              Where-Object { $_.DeviceID -eq $Card.TrimEnd('\') } | Select-Object -First 1).FreeSpace
    if ($livre -and $livre -lt $precisa) {
        Morre "o cartao nao tem espaco: precisa de $(Humano $precisa)" `
              "the card is full: it needs $(Humano $precisa)"
    }

    Write-Host ''
    Diz "Copiando pra $Card" "Copying to $Card"
    foreach ($rel in $lista) {
        $origem = Join-Path $saco $rel
        if (-not (Test-Path $origem)) {
            Write-Host ("  - $rel " + (T '(nao veio no zip)' '(not in the zip)')) -ForegroundColor Yellow
            continue
        }
        $destino = Join-Path $Card $rel
        $pasta = Split-Path $destino -Parent
        if (-not (Test-Path $pasta)) { New-Item -ItemType Directory -Path $pasta -Force | Out-Null }
        $tinha = Test-Path $destino
        Copy-Item -LiteralPath $origem -Destination $destino -Force
        $sufixo = ''
        if ($tinha) { $sufixo = ' ' + (T '(estava la, atualizei)' '(was there, updated)') }
        Write-Host '  v ' -ForegroundColor Green -NoNewline
        Write-Host ("$rel  " + (Humano (Get-Item $destino).Length) + $sufixo)
    }

    # Conferir o tamanho: cartao cheio ou mal encaixado mente na hora da copia
    # e so aparece quando o console recusa o arquivo.
    foreach ($rel in $lista) {
        $o = Join-Path $saco $rel
        $d = Join-Path $Card $rel
        if ((Test-Path $o) -and (Get-Item $o).Length -ne (Get-Item $d).Length) {
            Morre "$rel saiu do tamanho errado — tira o cartao e poe de novo, depois roda outra vez" `
                  "$rel came out the wrong size — reseat the card and run this again"
        }
    }

    CuidaDoHbl $Card

    Write-Host ''
    Write-Host (T 'Pronto.' 'Done.') -ForegroundColor Green
    if ($AppOnly) { Diz 'So o app foi instalado (-AppOnly).' 'Only the app was installed (-AppOnly).' }
    Diz 'Agora, no console:' 'Now, on the console:'
    Diz '  1. Poe o cartao de volta e liga o console no CFW.' `
        '  1. Put the card back and boot the console into CFW.'
    Diz '  2. Abre o menu de homebrew SEGURANDO R em cima de um jogo instalado — nao pelo Album.' `
        '  2. Open the homebrew menu HOLDING R on an installed game — not from the Album.'
    Diz '  3. Abre o SwitchSaveSync e entra na tua conta do Google na primeira tela.' `
        '  3. Open SwitchSaveSync and sign in to your Google account on the first screen.'
    if (-not $AppOnly) {
        Diz '  4. O autosync nao liga sozinho, de proposito: liga ele no overlay do Ultrahand quando quiser.' `
            '  4. Autosync does not start on its own, on purpose: turn it on from the Ultrahand overlay.'
    }

    if ($Eject) {
        try {
            $shell = New-Object -ComObject Shell.Application
            $shell.Namespace(17).ParseName($Card.TrimEnd('\')).InvokeVerb('Eject')
            Diz 'Cartao ejetado, pode tirar.' 'Card ejected, you can pull it.'
        } catch {
            Diz 'Nao consegui ejetar — tira pelo icone da bandeja.' `
                "Couldn't eject — use the tray icon."
        }
    }
}
finally {
    Remove-Item -LiteralPath $temporaria -Recurse -Force -ErrorAction SilentlyContinue
}
