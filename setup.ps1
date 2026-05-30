# This script is intentionally dot-sourced so it can update the caller's
# PowerShell session. Running it as a child script would lose functions, aliases,
# prompt changes, and virtual-environment activation state at script-scope.
if ($MyInvocation.InvocationName -ne '.') {
    Write-Error "setup.ps1 must be dot-sourced to update your current shell. Run: . .\setup.ps1"
    exit 1
}

$__vkfwd_setup_dir = Split-Path -Parent $PSCommandPath
$env:VKFWD_ROOT = $__vkfwd_setup_dir

# Keep repository command helpers discoverable after setup without requiring
# users to modify their profile or machine-wide PATH.
$__vkfwd_bin = Join-Path $env:VKFWD_ROOT 'dev\bin'
$__vkfwd_path_entries = $env:PATH -split [System.IO.Path]::PathSeparator
if ($__vkfwd_path_entries -notcontains $__vkfwd_bin) {
    $env:PATH = $__vkfwd_bin + [System.IO.Path]::PathSeparator + $env:PATH
}

function __vkfwd_find_python {
    # Prefer the launcher on Windows because it resolves the user's configured
    # Python 3 install without assuming python.exe is ahead of the app alias.
    if (Get-Command py -ErrorAction SilentlyContinue) {
        return @('py', '-3')
    }
    if (Get-Command python -ErrorAction SilentlyContinue) {
        return @('python')
    }
    if (Get-Command python3 -ErrorAction SilentlyContinue) {
        return @('python3')
    }

    return $null
}

# The local virtual environment isolates Python command-line tooling for this
# checkout while keeping activation in the caller's session, matching the
# dot-sourced setup contract above.
$__vkfwd_venv = Join-Path $env:VKFWD_ROOT 'dev\env\.pyvenv'
if (-not (Test-Path -LiteralPath $__vkfwd_venv -PathType Container)) {
    $__vkfwd_python = @(__vkfwd_find_python)
    if ($null -eq $__vkfwd_python) {
        Write-Error "Python 3 was not found on PATH; cannot create virtual environment: $__vkfwd_venv"
        return
    }

    $__vkfwd_python_args = @()
    if ($__vkfwd_python.Count -gt 1) {
        $__vkfwd_python_args = $__vkfwd_python[1..($__vkfwd_python.Count - 1)]
    }

    $__vkfwd_python_command = $__vkfwd_python[0]
    & $__vkfwd_python_command @__vkfwd_python_args -m venv $__vkfwd_venv
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to create Python virtual environment: $__vkfwd_venv"
        return
    }
}

$__vkfwd_activate = Join-Path $__vkfwd_venv 'Scripts\Activate.ps1'
if (-not (Test-Path -LiteralPath $__vkfwd_activate -PathType Leaf)) {
    $__vkfwd_activate = Join-Path $__vkfwd_venv 'bin\Activate.ps1'
}
if (Test-Path -LiteralPath $__vkfwd_activate -PathType Leaf) {
    . $__vkfwd_activate
} else {
    Write-Warning "Python virtual environment activation script not found: $__vkfwd_activate"
}

$__vkfwd_requirements = Join-Path $env:VKFWD_ROOT 'dev\env\requirements.txt'
if (Test-Path -LiteralPath $__vkfwd_requirements -PathType Leaf) {
    # Keep repo-local Python tools reproducible. The venv is owned by this
    # checkout, so installing pinned requirements here does not affect the
    # user's global Python environment.
    python -m pip install --no-cache-dir -r $__vkfwd_requirements
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to install Python tooling requirements: $__vkfwd_requirements"
        return
    }
}

# Reference a shared RandomGraphics git config when the workspace parent owns
# one. The include is repository-local so sourcing this file does not mutate the
# user's global git behavior.
$__vkfwd_gitconfig = Join-Path $env:VKFWD_ROOT '..\.gitconfig'
if (Test-Path -LiteralPath $__vkfwd_gitconfig -PathType Leaf) {
    $__vkfwd_gitconfig = (Resolve-Path -LiteralPath $__vkfwd_gitconfig).Path
    $__vkfwd_git_output = & git -C $env:VKFWD_ROOT config --local include.path $__vkfwd_gitconfig 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host "Configured local git include: $__vkfwd_gitconfig"
    } else {
        Write-Warning "failed to configure local git include: $__vkfwd_gitconfig"
        if ($__vkfwd_git_output) {
            Write-Warning $__vkfwd_git_output
        }
    }
}

# Alias definitions live under dev/env to match the Bash setup file while using
# a tiny data format instead of dot-sourcing arbitrary profile code.
$__vkfwd_alias_file = Join-Path $env:VKFWD_ROOT 'dev\env\alias.powershell.txt'
if (Test-Path -LiteralPath $__vkfwd_alias_file -PathType Leaf) {
    $__vkfwd_alias_line_number = 0
    foreach ($__vkfwd_alias_line in Get-Content -LiteralPath $__vkfwd_alias_file) {
        $__vkfwd_alias_line_number += 1
        $__vkfwd_alias_trimmed = $__vkfwd_alias_line.Trim()
        if (-not $__vkfwd_alias_trimmed -or $__vkfwd_alias_trimmed.StartsWith('#')) {
            continue
        }

        $__vkfwd_alias_parts = $__vkfwd_alias_trimmed -split '\s+', 3
        if ($__vkfwd_alias_parts.Count -ne 3) {
            Write-Warning "Ignoring malformed alias entry at ${__vkfwd_alias_file}:${__vkfwd_alias_line_number}"
            continue
        }

        switch ($__vkfwd_alias_parts[0]) {
            'alias' {
                Set-Alias -Name $__vkfwd_alias_parts[1] -Value $__vkfwd_alias_parts[2] -Scope Global
            }
            'jump' {
                $__vkfwd_jump_name = $__vkfwd_alias_parts[1]
                $__vkfwd_jump_path = $__vkfwd_alias_parts[2].Replace("'", "''")
                if ($__vkfwd_jump_path -eq '.') {
                    $__vkfwd_jump_script = 'Set-Location -LiteralPath $env:VKFWD_ROOT'
                } else {
                    $__vkfwd_jump_script = "Set-Location -LiteralPath (Join-Path `$env:VKFWD_ROOT '$__vkfwd_jump_path')"
                }

                Set-Item -Path "function:global:$__vkfwd_jump_name" -Value ([scriptblock]::Create($__vkfwd_jump_script))
            }
            default {
                Write-Warning "Ignoring unknown alias directive '$($__vkfwd_alias_parts[0])' at ${__vkfwd_alias_file}:${__vkfwd_alias_line_number}"
            }
        }
    }
}

function global:__vkfwd_git_branch {
    $branch = & git -C $env:VKFWD_ROOT rev-parse --abbrev-ref HEAD 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $branch) {
        return 'n/a'
    }

    return $branch
}

# Preserve the user's original prompt once. Re-sourcing should refresh vkfwd
# settings without nesting another banner in front of the existing prompt.
if (-not (Test-Path -LiteralPath 'variable:global:__VKFWD_ORIGINAL_PROMPT')) {
    $global:__VKFWD_ORIGINAL_PROMPT = (Get-Command prompt).ScriptBlock
}
$global:__VKFWD_REPO_NAME = Split-Path -Leaf $env:VKFWD_ROOT
function global:prompt {
    $esc = [char]27
    $branch = __vkfwd_git_branch
    "`n$esc[00;92m==== [$global:__VKFWD_REPO_NAME] - $esc[01;96m$env:VKFWD_ROOT$esc[00;92m - $esc[01;93m$branch$esc[00;92m ====$esc[m`n$(& $global:__VKFWD_ORIGINAL_PROMPT)"
}

Write-Host ''
Write-Host "VKFWD_ROOT    = $env:VKFWD_ROOT"
Write-Host "VIRTUAL_ENV   = $env:VIRTUAL_ENV"
Write-Host ''

Remove-Item variable:__vkfwd_activate -ErrorAction SilentlyContinue
Remove-Item variable:__vkfwd_alias_file -ErrorAction SilentlyContinue
Remove-Item variable:__vkfwd_alias_line -ErrorAction SilentlyContinue
Remove-Item variable:__vkfwd_alias_line_number -ErrorAction SilentlyContinue
Remove-Item variable:__vkfwd_alias_parts -ErrorAction SilentlyContinue
Remove-Item variable:__vkfwd_alias_trimmed -ErrorAction SilentlyContinue
Remove-Item variable:__vkfwd_bin -ErrorAction SilentlyContinue
Remove-Item variable:__vkfwd_git_output -ErrorAction SilentlyContinue
Remove-Item variable:__vkfwd_gitconfig -ErrorAction SilentlyContinue
Remove-Item variable:__vkfwd_jump_name -ErrorAction SilentlyContinue
Remove-Item variable:__vkfwd_jump_path -ErrorAction SilentlyContinue
Remove-Item variable:__vkfwd_jump_script -ErrorAction SilentlyContinue
Remove-Item variable:__vkfwd_path_entries -ErrorAction SilentlyContinue
Remove-Item variable:__vkfwd_python -ErrorAction SilentlyContinue
Remove-Item variable:__vkfwd_python_args -ErrorAction SilentlyContinue
Remove-Item variable:__vkfwd_python_command -ErrorAction SilentlyContinue
Remove-Item variable:__vkfwd_requirements -ErrorAction SilentlyContinue
Remove-Item variable:__vkfwd_setup_dir -ErrorAction SilentlyContinue
Remove-Item variable:__vkfwd_venv -ErrorAction SilentlyContinue
Remove-Item function:__vkfwd_find_python -ErrorAction SilentlyContinue
