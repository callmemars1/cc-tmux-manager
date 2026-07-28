#!/usr/bin/env bash
# cc — менеджер tmux-сессий с Claude Code. Единица = задача, не директория.
#
#   cc "задача словами"        временное имя t-HHMMSS, Claude сам переименует
#   cc <slug> [задача...]      явное имя (пробелов нет → это слаг, а не задача)
#   cc -C <dir> <slug> [...]   то же, но в другой рабочей директории
#   cc ls                      сессии: чем занята, в какой папке, последняя строка
#   cc a <slug>                подключиться (отобрав у зависшего клиента)
#   cc peek <slug>             хвост сессии, не подключаясь
#   cc mv <old> <new>          переименовать (когда прояснился реальный слаг)
#   cc log <slug>              писать вывод в ~/.cc-logs/<slug>.log
#   cc kill <slug> | killall
#
#   CC_DIR        рабочая директория по умолчанию (иначе — текущая)
#   CC_AUTOLOG=1  включать pipe-pane сразу при создании сессии
#   CC_AUTONAME=0 не просить Claude переименовывать временную сессию
#   CLAUDE_FLAGS  доп. флаги; alias claude из ~/.bashrc раскрывается сам
set -euo pipefail

LOGDIR="${HOME}/.cc-logs"
CLAUDE_BIN="${CLAUDE_BIN:-claude}"
CLAUDE_FLAGS="${CLAUDE_FLAGS:-}"

die() { printf '%s\n' "$*" >&2; exit 1; }
norm() { local n="${1//[^a-zA-Z0-9_-]/-}"; printf '%s' "${n##-}"; }

start_log() {
  mkdir -p "$LOGDIR"
  tmux pipe-pane -o -t "=$1:" "cat >> '$LOGDIR/$1.log'"
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
  a|at|attach)
           [ $# -ge 2 ] || die "usage: cc a <slug>"
           exec tmux attach -d -t "=$(norm "$2")" ;;
  peek)    [ $# -ge 2 ] || die "usage: cc peek <slug>"
           tmux capture-pane -p -S -60 -t "=$(norm "$2"):"; exit 0 ;;
  mv)      [ $# -ge 3 ] || die "usage: cc mv <old> <new>"
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
  log)     [ $# -ge 2 ] || die "usage: cc log <slug>"
           start_log "$(norm "$2")"; echo "→ $LOGDIR/$(norm "$2").log"; exit 0 ;;
  kill)    [ $# -ge 2 ] || die "usage: cc kill <slug>"
           tmux kill-session -t "=$(norm "$2")"; exit 0 ;;
  killall) tmux kill-server 2>/dev/null || true; exit 0 ;;
esac

# ── создание / подключение / досылка задачи ──────────────────────────
dir="${CC_DIR:-$PWD}"
if [ "${1:-}" = "-C" ]; then
  [ $# -ge 3 ] || die "usage: cc -C <dir> <slug> [задача...]"
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
