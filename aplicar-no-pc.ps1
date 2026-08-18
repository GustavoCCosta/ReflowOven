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

$sujo = git status --porcelain
if ($sujo) {
    Write-Host $sujo
    Fail "arvore suja. commite ou descarte antes de aplicar o patch dos agentes."
}

# O cabecalho do MAIN.patch declara sobre qual commit ele foi gerado.
$baseDeclarada = (Select-String -Path $Patch -Pattern '^#\s*base:\s*origin/main\s*=\s*(\S+)' |
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

# ------------------------------------------------------------------ aplicar
Info "criando branch $Branch a partir de origin/main ($mainAtual)"
git checkout --quiet -B $Branch origin/main
if ($LASTEXITCODE -ne 0) { Fail "nao consegui criar a branch." }

Info "aplicando $Patch"
git apply --3way --whitespace=nowarn $Patch
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Fail @"
o patch nao aplicou. quase sempre significa que o MAIN.patch foi gerado sobre
uma main mais antiga. avise o Gerente com o commit atual ($mainAtual) e peca a
regeracao. a branch $Branch ficou criada e limpa; nada foi perdido.
"@
}

# ------------------------------------------------------------------- resumo
Write-Host ""
Info "aplicado. resumo:"
git diff --stat origin/main
Write-Host ""
Info "arquivos com conflito de 3-way (se houver):"
git diff --name-only --diff-filter=U
Write-Host ""

Write-Host "PAREI AQUI DE PROPOSITO. Nada foi commitado." -ForegroundColor Green
Write-Host ""
Write-Host "Revise:        git diff origin/main"
Write-Host "Se aprovar:    git add -A"
Write-Host "               git commit -m ""agentes: <resumo do que entrou>"""
Write-Host "               git checkout main"
Write-Host "               git merge --ff-only $Branch"
Write-Host "               git push origin main"
Write-Host ""
Write-Host "Se desistir:   git checkout main; git branch -D $Branch"
Write-Host ""
Write-Host "Depois do push, avise o Gerente: ele regenera o MAIN.patch sobre a main nova."