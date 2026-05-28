#!/usr/bin/env bash

# This script is intentionally sourced so it can update the caller's shell.
# Executing it in a child process would discard aliases, PS1, and exported paths.
if ! (return 0 2>/dev/null); then
    echo "ERROR: setup.sh must be sourced to update your current shell."
    echo "Run: source setup.sh"
    echo "  or: . setup.sh"
    exit 1
fi

if [ -z "${BASH_VERSION:-}" ]; then
    echo "ERROR: setup.sh uses Bash-specific prompt and alias behavior."
    echo "Please source it from bash: source setup.sh"
    return 1
fi

__vkfwd_setup_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export VKFWD_ROOT="$__vkfwd_setup_dir"

# Keep repository command helpers discoverable after setup without requiring
# users to modify their global shell startup files.
case ":${PATH}:" in
    *":${VKFWD_ROOT}/dev/bin:"*) ;;
    *) export PATH="${VKFWD_ROOT}/dev/bin:${PATH}" ;;
esac

# The local virtual environment isolates Python command-line tooling for this
# checkout while keeping activation in the caller's shell, matching the sourced
# setup contract above.
__vkfwd_venv="${VKFWD_ROOT}/dev/env/.pyvenv"
if [ ! -d "$__vkfwd_venv" ]; then
    python3 -m venv "$__vkfwd_venv"
fi
if [ -f "$__vkfwd_venv/bin/activate" ]; then
    # shellcheck source=/dev/null
    . "$__vkfwd_venv/bin/activate"
else
    echo "WARNING: Python virtual environment activation script not found: $__vkfwd_venv/bin/activate"
fi

# Reference a shared RandomGraphics git config when the workspace parent owns
# one. The include is repository-local so sourcing this file does not mutate the
# user's global git behavior.
__vkfwd_gitconfig="${VKFWD_ROOT}/../.gitconfig"
if [ -f "$__vkfwd_gitconfig" ]; then
    __vkfwd_gitconfig="$(realpath "$__vkfwd_gitconfig")"
    if __vkfwd_git_error="$(git -C "$VKFWD_ROOT" config --local include.path "$__vkfwd_gitconfig" 2>&1)"; then
        echo "Configured local git include: $__vkfwd_gitconfig"
    else
        echo "WARNING: failed to configure local git include: $__vkfwd_gitconfig"
        [ -z "$__vkfwd_git_error" ] || echo "         $__vkfwd_git_error"
    fi
fi

# Alias definitions live under dev/env to match the other RandomGraphics
# projects and to keep shell conveniences separate from the root entrypoint.
if [ -f "${VKFWD_ROOT}/dev/env/alias.bash.txt" ]; then
    # shellcheck source=dev/env/alias.bash.txt
    . "${VKFWD_ROOT}/dev/env/alias.bash.txt"
fi

__vkfwd_git_branch() {
    local branch
    branch="$(git -C "$VKFWD_ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null)" || branch="n/a"
    printf '%s' "$branch"
}

# Preserve the user's original prompt once. Re-sourcing should refresh vkfwd
# settings without nesting another banner in front of the existing prompt.
if [ -z "${__VKFWD_ORIGINAL_PS1+x}" ]; then
    __VKFWD_ORIGINAL_PS1="${PS1:-}"
fi
__vkfwd_repo_name="$(basename "$VKFWD_ROOT")"
PS1="\n\e[00;92m==== [${__vkfwd_repo_name}] - \e[01;96m${VKFWD_ROOT}\e[00;92m - \e[01;93m\$(__vkfwd_git_branch)\e[00;92m ====\e[m\n${__VKFWD_ORIGINAL_PS1}"

echo
echo "VKFWD_ROOT    = ${VKFWD_ROOT}"
echo "VIRTUAL_ENV   = ${VIRTUAL_ENV:-}"
echo

unset __vkfwd_gitconfig
unset __vkfwd_git_error
unset __vkfwd_repo_name
unset __vkfwd_setup_dir
unset __vkfwd_venv
