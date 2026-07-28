#!/usr/bin/env bash
# install.sh — ставит и обновляет cc. Одна и та же команда для обоих случаев:
# повторный запуск просто заменяет установленный cc на новую версию.
#
#   curl -fsSL https://raw.githubusercontent.com/callmemars1/cc-tmux-manager/main/install.sh | bash
#   ./install.sh          # из клона — поставит лежащий рядом cc
#
# Переменные окружения:
#   CC_INSTALL_DIR  куда ставить (по умолчанию $PREFIX/bin или ~/.local/bin)
#   PREFIX          альтернатива CC_INSTALL_DIR: ставит в $PREFIX/bin
#   CC_REF          ветка или тег вместо последнего релиза
#   CC_REPO         откуда брать (owner/repo)
#   CC_SOURCE       remote — не брать локальную копию рядом со скриптом
#   CC_FORCE        1 — перезаписать cc, даже если он установлен симлинком
set -euo pipefail

REPO="${CC_REPO:-callmemars1/cc-tmux-manager}"
REF="${CC_REF:-}"
BINDIR="${CC_INSTALL_DIR:-${PREFIX:+$PREFIX/bin}}"
BINDIR="${BINDIR:-$HOME/.local/bin}"

die() { printf '%s\n' "$*" >&2; exit 1; }
version_of() { sed -n 's/^CC_VERSION="\(.*\)"$/\1/p' "$1" | head -n1; }

fetch() { # fetch <url> <dest>
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$1" -o "$2"
  elif command -v wget >/dev/null 2>&1; then
    wget -qO "$2" "$1"
  else
    die "нужен curl или wget"
  fi
}

case "${1:-}" in
  -h|--help) sed -n '2,17p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'; exit 0 ;;
  "") ;;
  *) die "неизвестный аргумент: $1 (install.sh --help)" ;;
esac

tmp="$(mktemp)"
staged="$BINDIR/.cc.new.$$"
trap 'rm -f "$tmp" "$staged"' EXIT

# Локальная копия рядом со скриптом — это запуск из клона. При установке через
# curl | bash BASH_SOURCE — не файл, так что ветка не сработает случайно.
self="${BASH_SOURCE[0]:-}"
local_cc=""
if [ -z "$REF" ] && [ "${CC_SOURCE:-}" != "remote" ] && [ -f "$self" ]; then
  candidate="$(cd "$(dirname "$self")" && pwd)/cc"
  [ -f "$candidate" ] && local_cc="$candidate"
fi

if [ -n "$local_cc" ]; then
  origin="локальная копия $local_cc"
  cp "$local_cc" "$tmp"
elif [ -n "$REF" ]; then
  origin="$REPO@$REF"
  fetch "https://raw.githubusercontent.com/$REPO/$REF/cc" "$tmp" \
    || die "не скачался cc из $origin"
else
  origin="последний релиз $REPO"
  if ! fetch "https://github.com/$REPO/releases/latest/download/cc" "$tmp"; then
    # релизов может ещё не быть — тогда берём main
    printf '%s\n' "у $REPO нет релизов, ставлю с main" >&2
    origin="$REPO@main"
    fetch "https://raw.githubusercontent.com/$REPO/main/cc" "$tmp" \
      || die "не скачался cc из $origin"
  fi
fi

# скачанное могло оказаться HTML-страницей ошибки — проверяем, что это наш скрипт
head -n1 "$tmp" | grep -q '^#!' || die "полученный файл не похож на скрипт ($origin)"
new="$(version_of "$tmp")"
[ -n "$new" ] || die "в полученном cc нет CC_VERSION ($origin)"
bash -n "$tmp" || die "полученный cc не проходит проверку синтаксиса ($origin)"

target="$BINDIR/cc"
old=""
if [ -L "$target" ] && [ "${CC_FORCE:-}" != "1" ]; then
  die "$target — симлинк на $(readlink "$target"): обнови клон через git pull
(или переустанови поверх симлинка: CC_FORCE=1 ...)"
fi
[ -f "$target" ] && old="$(version_of "$target" || true)"

mkdir -p "$BINDIR"
# ставим через staged + mv: rename атомарен и не портит уже запущенные cc
install -m 755 "$tmp" "$staged"
mv -f "$staged" "$target"

if [ -z "$old" ]; then
  echo "cc $new установлен: $target ($origin)"
elif [ "$old" = "$new" ]; then
  echo "cc $new уже актуален: $target ($origin)"
else
  echo "cc обновлён: $old → $new ($target)"
fi

case ":$PATH:" in
  *":$BINDIR:"*) ;;
  *) printf '%s\n' "$BINDIR не в PATH — добавь в профиль: export PATH=\"$BINDIR:\$PATH\"" >&2 ;;
esac
