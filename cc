#!/usr/bin/env bash
# cc — менеджер tmux-сессий с Claude Code. Единица = задача, не директория.
# Команды и переменные окружения: cc help (справка — в cmd_help ниже,
# второй копии списка команд в этом файле быть не должно).
set -euo pipefail

# бамп этой строки в main = релиз: CI создаёт тег vX.Y.Z и выкладывает cc
CC_VERSION="0.1.0"

LOGDIR="${HOME}/.cc-logs"
CC_REPO="${CC_REPO:-callmemars1/cc-tmux-manager}"
CLAUDE_BIN="${CLAUDE_BIN:-claude}"
CLAUDE_FLAGS="${CLAUDE_FLAGS:-}"

die() { printf '%s\n' "$*" >&2; exit 1; }
usage_die() { die "usage: $* (cc help — полная справка)"; }
norm() { local n="${1//[^a-zA-Z0-9_-]/-}"; printf '%s' "${n##-}"; }

start_log() {
  mkdir -p "$LOGDIR"
  tmux pipe-pane -o -t "=$1:" "cat >> '$LOGDIR/$1.log'"
}

cmd_help() {
  cat <<EOF
cc — менеджер tmux-сессий с Claude Code. Единица работы — задача.

Команды:
  cc "задача словами"      новая сессия t-HHMMSS; Claude сам её переименует
  cc <slug> [задача...]    явное имя (без пробелов → слаг, а не задача)
  cc -C <dir> <slug> [...] то же, но в другой рабочей директории
  cc ls                    сессии: подключена ли, чем занята, что вывела
  cc a <slug>              подключиться, отобрав у зависшего клиента
  cc peek <slug>           хвост сессии, не подключаясь
  cc mv <old> <new>        переименовать (лог переезжает вместе с ней)
  cc log <slug>            писать вывод в $LOGDIR/<slug>.log
  cc kill <slug>           убить сессию
  cc killall               убить весь tmux-сервер
  cc update                обновить сам cc до последнего релиза
  cc version               версия cc (он же --version)
  cc help                  эта справка (он же -h / --help)

Без аргументов — то же, что cc ls. Если сессия с таким слагом уже есть,
cc <slug> задача не создаёт новую, а досылает текст в существующую.

Переменные окружения:
  CC_DIR        рабочая директория новых сессий (иначе — текущая)
  CC_AUTOLOG=1  включать pipe-pane сразу при создании сессии
  CC_AUTONAME=0 не просить Claude переименовывать временную сессию
  CLAUDE_BIN    чем запускать Claude Code (по умолчанию claude)
  CLAUDE_FLAGS  доп. флаги к запуску
  CC_REPO       откуда обновляться (owner/repo)
EOF
}

# обновление делегируем install.sh из репозитория: он один знает, откуда брать
# cc и как его безопасно подменить (сюда логику не дублируем)
cmd_update() {
  local dir url tmp status
  dir="$(cd "$(dirname "$0")" && pwd)"
  # CC_INSTALLER_REF — только для проверки install.sh с ветки, в справке не нужен
  url="https://raw.githubusercontent.com/$CC_REPO/${CC_INSTALLER_REF:-main}/install.sh"
  tmp="$(mktemp)"
  if command -v curl >/dev/null 2>&1; then
    curl -fsL "$url" -o "$tmp" || { rm -f "$tmp"; die "не скачался install.sh: $url"; }
  elif command -v wget >/dev/null 2>&1; then
    wget -qO "$tmp" "$url" || { rm -f "$tmp"; die "не скачался install.sh: $url"; }
  else
    rm -f "$tmp"; die "нужен curl или wget"
  fi
  status=0
  CC_INSTALL_DIR="$dir" CC_SOURCE=remote bash "$tmp" || status=$?
  rm -f "$tmp"
  return "$status"
}

cmd_ls() {
  local sessions last att title
  sessions="$(tmux ls -F '#{session_name}' 2>/dev/null || true)"
  [ -n "$sessions" ] || { echo "нет запущенных сессий"; return 0; }
  while read -r s; do
    att="$(tmux display -p -t "=$s:" '#{?session_attached,●,○}' 2>/dev/null || true)"
    # pane_title: Claude Code кладёт туда текущую задачу, шелл — user@host:cwd
    title="$(tmux display -p -t "=$s:" '#{pane_title}' 2>/dev/null || true)"
    case "$title" in "$USER@"*:*) title="${title#*:}";; esac
    last="$(tmux capture-pane -p -t "=$s:" 2>/dev/null | grep -v '^[[:space:]]*$' | tail -n1 | cut -c1-46 || true)"
    printf '%s %-24s %-34s %s\n' "$att" "$s" "${title:0:34}" "$last"
  done <<< "$sessions"
}

case "${1:-}" in
  ls|"")   cmd_ls; exit 0 ;;
  help|-h|--help)
           cmd_help; exit 0 ;;
  version|--version)
           echo "cc $CC_VERSION"; exit 0 ;;
  update|upgrade)
           cmd_update; exit 0 ;;
  a|at|attach)
           [ $# -ge 2 ] || usage_die "cc a <slug>"
           exec tmux attach -d -t "=$(norm "$2")" ;;
  peek)    [ $# -ge 2 ] || usage_die "cc peek <slug>"
           tmux capture-pane -p -S -60 -t "=$(norm "$2"):"; exit 0 ;;
  mv)      [ $# -ge 3 ] || usage_die "cc mv <old> <new>"
           old="$(norm "$2")"; new="$(norm "$3")"
           if [ "$old" = "$new" ]; then exit 0; fi
           if tmux has-session -t "=$new" 2>/dev/null; then die "сессия '$new' уже есть"; fi
           tmux rename-session -t "=$old" "$new"
           # лог переезжает вместе с сессией, иначе pipe-pane пишет в старый файл
           if [ -f "$LOGDIR/$old.log" ]; then
             mv "$LOGDIR/$old.log" "$LOGDIR/$new.log"
             tmux pipe-pane -t "=$new:" 2>/dev/null || true
             start_log "$new"
           fi
           exit 0 ;;
  log)     [ $# -ge 2 ] || usage_die "cc log <slug>"
           start_log "$(norm "$2")"; echo "→ $LOGDIR/$(norm "$2").log"; exit 0 ;;
  kill)    [ $# -ge 2 ] || usage_die "cc kill <slug>"
           tmux kill-session -t "=$(norm "$2")"; exit 0 ;;
  killall) tmux kill-server 2>/dev/null || true; exit 0 ;;
esac

# ── создание / подключение / досылка задачи ──────────────────────────
dir="${CC_DIR:-$PWD}"
if [ "${1:-}" = "-C" ]; then
  [ $# -ge 3 ] || usage_die "cc -C <dir> <slug> [задача...]"
  dir="$2"; shift 2
fi
[ -d "$dir" ] || die "нет такой директории: $dir"
dir="$(cd "$dir" && pwd)"

# один аргумент с пробелами — это задача, а не слаг: имя даст сам Claude
autoname=0
if [ $# -eq 1 ] && [[ "$1" == *" "* ]]; then
  name="t-$(date +%H%M%S)"; task="$1"; autoname=1
else
  name="$(norm "$1")"; shift; task="$*"
fi
[ -n "$name" ] || die "пустое имя сессии"
[ "${CC_AUTONAME:-1}" = "1" ] || autoname=0

# инструкция на самопереименование: сессия стартует как t-HHMMSS,
# Claude выводит слаг из первого запроса и зовёт cc mv
rename_prompt="Your work runs inside a tmux session currently named '$name'. \
Before anything else, derive a short latin kebab-case slug (2-4 words) from the \
user's first request and run exactly: cc mv $name <slug>. Do it once, silently, \
without commenting. If the task later changes substantially, rename again with \
cc mv <current-name> <new-slug>."

if ! tmux has-session -t "=$name" 2>/dev/null; then
  tmux new-session -d -s "$name" -c "$dir"
  if [ "${CC_AUTOLOG:-}" = "1" ]; then start_log "$name"; fi
  # send-keys в интерактивный шелл → alias claude раскрывается,
  # printf %q экранирует и задачу, и системный промпт целиком
  cmdline="$CLAUDE_BIN $CLAUDE_FLAGS"
  if [ "$autoname" = "1" ]; then
    cmdline="$cmdline --append-system-prompt $(printf '%q' "$rename_prompt")"
  fi
  if [ -n "$task" ]; then
    cmdline="$cmdline $(printf '%q' "$task")"
  fi
  tmux send-keys -t "=$name:" "$cmdline" Enter
  echo "создана сессия '$name' в $dir"
elif [ -n "$task" ]; then
  tmux send-keys -t "=$name:" "$task" Enter
  echo "задача отправлена в '$name'"
fi

if [ -n "${TMUX:-}" ]; then
  tmux switch-client -t "=$name" 2>/dev/null || echo "переключись сам: tmux switch-client -t $name"
else
  exec tmux attach -d -t "=$name"
fi
