# Zveřejní aktuální stav větve main do veřejného repozitáře rozhodovac-ai jako jediný commit bez historie.
# Web běží ze složky docs veřejného repozitáře na https://rozhodovac.zkusmo.cz (doménu určuje docs/CNAME).
# Commit má pevné datum a čas, aby u kódu nebylo vidět, kdy se zveřejňovalo. Čas samotného pushe
# GitHub ukazuje dál (Activity, Deployments), ten skrýt nejde.
param(
    [switch]$Yes
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$Public = "https://stanislavkaska@github.com/stanislavkaska/rozhodovac-ai.git"

function Invoke-Git {
    & git @args
    if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' ') skončil chybou $LASTEXITCODE" }
}

$branch = (git rev-parse --abbrev-ref HEAD).Trim()
if ($branch -ne "main") { throw "Zveřejňuje se jen z větve main, teď je $branch." }
if (git status --porcelain) { throw "V pracovní kopii jsou neuložené změny, nejdřív commit a push." }

Invoke-Git fetch -q origin
$head = (git rev-parse HEAD).Trim()
$origin = (git rev-parse origin/main).Trim()
if ($head -ne $origin) { throw "main se liší od origin/main, nejdřív push nebo pull. Zveřejnit jde jen to, co je v soukromém repozitáři." }

if (-not (Test-Path "docs\CNAME")) { throw "Chybí docs\CNAME, web by přišel o doménu." }

$tree = (git rev-parse "HEAD^{tree}").Trim()
$files = @(git ls-tree -r --name-only HEAD).Count
$message = "Rozhodovač"
# 12:00 UTC, aby datum vyšlo 1. 11. 2026 v každém časovém pásmu
$fixedDate = "2026-11-01T12:00:00+0000"

Write-Host "Zdroj:   $($head.Substring(0, 7)) ($files souborů)"
Write-Host "Cíl:     $Public (větev main se přepíše, zůstane jediný commit)"
Write-Host "Zpráva:  $message"
if (-not $Yes) {
    $answer = Read-Host "Zveřejnit? [a/N]"
    if ($answer -notmatch "^[aAyY]") { Write-Host "Nic se nezveřejnilo."; exit 0 }
}

$env:GIT_AUTHOR_DATE = $fixedDate
$env:GIT_COMMITTER_DATE = $fixedDate
try {
    $commit = (git commit-tree $tree -m $message).Trim()
} finally {
    Remove-Item Env:GIT_AUTHOR_DATE, Env:GIT_COMMITTER_DATE -ErrorAction SilentlyContinue
}
if ($LASTEXITCODE -ne 0 -or -not $commit) { throw "git commit-tree selhal." }
Invoke-Git push --force $Public "${commit}:refs/heads/main"

Write-Host "Zveřejněno. Web se obnoví zhruba do minuty: https://rozhodovac.zkusmo.cz"
