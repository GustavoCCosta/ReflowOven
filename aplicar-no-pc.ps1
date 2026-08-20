<#
.SYNOPSIS
    Aplica o flow/MAIN.patch dos agentes numa branch de trabalho, e para.

.DESCRIPTION
    Nunca aplica na main e nunca commita. Cria (ou recria) a branch
    agentes/<data> a partir de origin/main, aplica o patch com --3way, mostra o
    resumo, e devolve o controle para voce revisar.

    O commit e o push sao seus, depois de olhar o diff.

.PARAMETER Patch
    Caminho do MAIN.patch. Padrao: MAIN.patch ao lado deste script.
    Caminho relativo passado a mao e resolvido a partir de onde voce chamou.

.PARAMETER Repo
    Raiz do repositorio. Padrao: a pasta deste script.

.PARAMETER Branch
    Nome da branch de trabalho. Padrao: agentes/<yyyy-MM-dd>

.EXAMPLE
    # MAIN.patch ao lado do script, que e o caso normal:
    .\aplicar-no-pc.ps1

.EXAMPLE
    # patch em outro lugar:
    .\aplicar-no-pc.ps1 -Patch $HOME\Downloads\MAIN.patch
#>
[CmdletBinding()]
param(
    [string]$Patch  = (Join-Path $PSScriptRoot "MAIN.patch"),
    [string]$Repo   = $PSScriptRoot,
    [string]$Branch = ("agentes/" + (Get-Date -Format "yyyy-MM-dd"))
)

$ErrorActionPreference = "Stop"

function Fail($msg) { Write-Host "erro: $msg" -ForegroundColor Red; exit 1 }
function Info($msg) { Write-Host $msg -ForegroundColor Cyan }

# Resolve o patch ANTES do Set-Location, senao um caminho relativo passado a
# mao passaria a valer a partir do repositorio, e nao de onde voce chamou.
if (-not [System.IO.Path]::IsPathRooted($Patch)) {
    $Patch = Join-Path (Get-Location).Path $Patch
}

# ---------------------------------------------------------------- checagens
Set-Location $Repo
if (-not (Test-Path ".git")) { Fail "'$Repo' nao e um repositorio git." }
if (-not (Test-Path $Patch)) {
    Fail @"
patch nao encontrado: $Patch
baixe o flow/MAIN.patch do Project e salve ao lado deste script, ou passe o
caminho com -Patch.
"@
}
$Patch = (Resolve-Path $Patch).Path

# Arquivo NAO rastreado nao atrapalha o `git apply` nem o `checkout -B`. O que
# atrapalha e alteracao pendente em arquivo versionado, que o patch pode tentar
# tocar. Entao: falha so no segundo caso, e avisa no primeiro.
$modificado = git status --porcelain --untracked-files=no
if ($modificado) {
    Write-Host $modificado
    Fail @"
ha alteracoes pendentes em arquivos versionados. commite ou descarte antes de
aplicar o patch dos agentes:
    git stash push -u        (guarda e devolve depois com 'git stash pop')
    git checkout -- .        (descarta, sem volta)
"@
}

$naoRastreado = git status --porcelain --untracked-files=normal |
                Where-Object { $_ -like '??*' }
if ($naoRastreado) {
    Write-Host "arquivos nao rastreados na arvore (nao impedem nada):" -ForegroundColor Yellow
    $naoRastreado | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
    Write-Host ""
}

# Copia para fora da arvore ANTES de trocar de branch. Se o MAIN.patch estiver
# versionado numa branch e nao na outra, o `checkout` apaga o arquivo do disco
# e o `git apply` seguinte nao acha mais nada para aplicar.
#
# A copia tambem NORMALIZA o patch. Baixar um arquivo de texto no Windows
# costuma converter LF em CRLF e as vezes come a quebra de linha final; o
# `git apply` recusa os dois com "corrupt patch at line N", apontando a ultima
# linha. Um patch e um formato binario-ish: LF e nada de BOM.
$patchTmp = Join-Path ([System.IO.Path]::GetTempPath()) ("MAIN-" + [guid]::NewGuid().ToString("N") + ".patch")

$txt = [System.IO.File]::ReadAllText($Patch, [System.Text.Encoding]::UTF8)
$consertos = @()
if ($txt.Length -gt 0 -and $txt[0] -eq [char]0xFEFF) {
    $txt = $txt.Substring(1); $consertos += "BOM removido"
}
if ($txt.Contains("`r`n")) {
    $txt = $txt -replace "`r`n", "`n"; $consertos += "CRLF -> LF"
}
if (-not $txt.EndsWith("`n")) {
    $txt += "`n"; $consertos += "quebra de linha final recolocada"
}
[System.IO.File]::WriteAllText($patchTmp, $txt, (New-Object System.Text.UTF8Encoding($false)))

if ($consertos.Count -gt 0) {
    Write-Host ("patch normalizado antes de aplicar: " + ($consertos -join ", ")) -ForegroundColor Yellow
    Write-Host "(o arquivo original nao foi tocado)" -ForegroundColor Yellow
    Write-Host ""
}

# O cabecalho do MAIN.patch declara sobre qual commit ele foi gerado.
$baseDeclarada = (Select-String -Path $patchTmp -Pattern '^#\s*base:\s*origin/main\s*=\s*(\S+)' |
                  Select-Object -First 1).Matches.Groups[1].Value

Info "buscando origin..."
git fetch origin --quiet
$mainAtual = (git rev-parse --short origin/main).Trim()

if ($baseDeclarada) {
    Info "patch gerado sobre : $baseDeclarada"
    Info "origin/main agora  : $mainAtual"
    if (-not $mainAtual.StartsWith($baseDeclarada) -and
        -not $baseDeclarada.StartsWith($mainAtual)) {
        Write-Host ""
        Write-Host "AVISO: a main andou desde que o patch foi gerado." -ForegroundColor Yellow
        Write-Host "Se o apply falhar, peca ao Gerente para regerar o MAIN.patch" -ForegroundColor Yellow
        Write-Host "sobre $mainAtual." -ForegroundColor Yellow
        Write-Host ""
    }
}

# --------------------------------------------------- de onde sai a branch
# Se a main local tiver commits seus que ainda nao foram empurrados, a branch
# tem que sair dela — senao o `merge --ff-only` do fim nao funciona e o seu
# trabalho local fica de fora do push.
$temMainLocal = (git rev-parse --verify --quiet refs/heads/main)
if ($temMainLocal) {
    git merge-base --is-ancestor origin/main main
    $mainDescende = ($LASTEXITCODE -eq 0)
    $aFrente = (git rev-list --count origin/main..main).Trim()
    git merge-base --is-ancestor main origin/main
    $mainAtrasada = ($LASTEXITCODE -eq 0)

    if (-not $mainDescende -and -not $mainAtrasada) {
        Fail @"
sua main local divergiu de origin/main (nem uma contem a outra). resolva isso
antes — provavelmente com 'git pull --rebase' — e rode de novo.
"@
    }
    if ($mainDescende -and [int]$aFrente -gt 0) {
        $baseRef = "main"
        Info "sua main local esta $aFrente commit(s) a frente de origin/main;"
        Info "a branch vai sair da main local, para nao deixar esse trabalho de fora."
    } else {
        $baseRef = "origin/main"
    }
} else {
    $baseRef = "origin/main"
}

# ------------------------------------------------------------------ aplicar
Info "criando branch $Branch a partir de $baseRef"
git checkout --quiet -B $Branch $baseRef
if ($LASTEXITCODE -ne 0) { Fail "nao consegui criar a branch." }

if (-not (Test-Path $patchTmp)) { Fail "a copia temporaria do patch sumiu: $patchTmp" }

Info "aplicando $Patch"
git apply --3way --whitespace=nowarn $patchTmp
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Fail @"
o patch nao aplicou sobre $baseRef. quase sempre significa que o MAIN.patch foi
gerado sobre uma main mais antiga: avise o Gerente com o commit atual
($mainAtual) e peca a regeracao.
a branch $Branch ficou criada; volte com 'git checkout main' se quiser desfazer.
"@
}
Remove-Item -LiteralPath $patchTmp -Force -ErrorAction SilentlyContinue

# ------------------------------------------------------------------- resumo
Write-Host ""
Info "aplicado. resumo (contra $baseRef):"
git diff --stat $baseRef
Write-Host ""
Info "arquivos com conflito de 3-way (se houver):"
git diff --name-only --diff-filter=U
Write-Host ""

Write-Host "PAREI AQUI DE PROPOSITO. Nada foi commitado." -ForegroundColor Green
Write-Host ""
Write-Host "Revise:        git diff $baseRef"
Write-Host "Se aprovar:    git add -A          (confira o 'git status' antes:"
Write-Host "                                    -A pega arquivo nao rastreado tambem)"
Write-Host "               git commit -m ""agentes: <resumo do que entrou>"""
Write-Host "               git checkout main"
Write-Host "               git merge --ff-only $Branch"
Write-Host "               git push origin main"
Write-Host ""
Write-Host "Se desistir:   git checkout main; git branch -D $Branch"
Write-Host ""
Write-Host "Depois do push, avise o Gerente: ele regenera o MAIN.patch sobre a main nova."