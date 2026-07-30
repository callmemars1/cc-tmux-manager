#!/usr/bin/env bash
# cc — менеджер tmux-сессий с Claude Code. Единица = задача, не директория.
# Команды и переменные окружения: cc help (справка — в cmd_help ниже,
# второй копии списка команд в этом файле быть не должно).
set -euo pipefail

# бамп этой строки в main = релиз: CI создаёт тег vX.Y.Z и выкладывает cc
CC_VERSION="0.2.0"

LOGDIR="${HOME}/.cc-logs"
CC_REPO="${CC_REPO:-callmemars1/cc-tmux-manager}"
CLAUDE_BIN="${CLAUDE_BIN:-claude}"
CLAUDE_FLAGS="${CLAUDE_FLAGS:-}"

die() { printf '%s\n' "$*" >&2; exit 1; }
usage_die() { die "usage: $* (cc help — полная справка)"; }
norm() { local n="${1//[^a-zA-Z0-9_-]/-}"; printf '%s' "${n##-}"; }
temp_name() { printf 't-%s' "$(date +%H%M%S)"; }
# директорию проверяем только там, где она нужна: cc ls с битым CC_DIR
# не должен падать
resolve_dir() { [ -d "$1" ] || die "нет такой директории: $1"; (cd "$1" && pwd); }

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
  cc s                     выбрать сессию стрелками (↑↓ или k/j, Enter, q)
  cc n [slug]              задача пишется в \$EDITOR; сессия стартует на выходе
  cc ls                    плоский список: подключена ли, чем занята, что вывела
  cc a <slug>              подключиться, отобрав у зависшего клиента
  cc peek <slug>           хвост сессии, не подключаясь
  cc mv <old> <new>        переименовать (лог переезжает вместе с ней)
  cc log <slug>            писать вывод в $LOGDIR/<slug>.log
  cc kill <slug>           убить сессию
  cc killall               убить весь tmux-сервер
  cc update                обновить сам cc до последнего релиза
  cc version               версия cc (он же --version)
  cc help                  эта справка (он же -h / --help)

Без аргументов — то же, что cc s; если вывод не в терминал (пайп, CI) —
то же, что cc ls. Если сессия с таким слагом уже есть, cc <slug> задача
не создаёт новую, а досылает текст в существующую.

Переменные окружения:
  CC_DIR        рабочая директория новых сессий (иначе — текущая)
  CC_AUTOLOG=1  включать pipe-pane сразу при создании сессии
  CC_AUTONAME=0 не просить Claude переименовывать временную сессию
  CLAUDE_BIN    чем запускать Claude Code (по умолчанию claude)
  CLAUDE_FLAGS  доп. флаги к запуску
  VISUAL/EDITOR чем открывать задачу в cc n (по умолчанию vim)
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

# снапшот сессий: имена в PICK_NAMES, готовые строки в PICK_ROWS. Снимается
# один раз — capture-pane на каждую сессию не бесплатен, и перерисовка пикера
# не должна его повторять.
PICK_NAMES=()
PICK_ROWS=()
snapshot_sessions() {
  local sessions s last att title
  PICK_NAMES=(); PICK_ROWS=()
  sessions="$(tmux ls -F '#{session_name}' 2>/dev/null || true)"
  [ -n "$sessions" ] || return 0
  while read -r s; do
    [ -n "$s" ] || continue
    att="$(tmux display -p -t "=$s:" '#{?session_attached,●,○}' 2>/dev/null || true)"
    # pane_title: Claude Code кладёт туда текущую задачу, шелл — user@host:cwd
    title="$(tmux display -p -t "=$s:" '#{pane_title}' 2>/dev/null || true)"
    case "$title" in "$USER@"*:*) title="${title#*:}";; esac
    last="$(tmux capture-pane -p -t "=$s:" 2>/dev/null | grep -v '^[[:space:]]*$' | tail -n1 | cut -c1-46 || true)"
    PICK_NAMES+=("$s")
    PICK_ROWS+=("$(printf '%s %-24s %-34s %s' "$att" "$s" "${title:0:34}" "$last")")
  done <<< "$sessions"
}

cmd_ls() {
  snapshot_sessions
  [ "${#PICK_ROWS[@]}" -gt 0 ] || { echo "нет запущенных сессий"; return 0; }
  printf '%s\n' "${PICK_ROWS[@]}"
}

PICK_STTY=""
PICK_DRAWN=0

# перерисовка: подняться на PICK_DRAWN строк и перепечатать окно. Строки
# собираем в один printf '%s' — %b распаковал бы бэкслеши из вывода сессий.
pick_draw() { # pick_draw <top> <win> <cols> <cur>
  local top="$1" win="$2" cols="$3" cur="$4" esc=$'\033' out="" i row hint
  [ "$PICK_DRAWN" -eq 0 ] || out="${esc}[${PICK_DRAWN}A"
  for (( i = top; i < top + win; i++ )); do
    # обрезка по ширине: перенос строки сбил бы счётчик и картинка поехала бы
    row="${PICK_ROWS[$i]:0:$(( cols - 1 ))}"
    if [ "$i" -eq "$cur" ]; then
      out="$out"$'\r'"${esc}[K${esc}[7m${row}${esc}[0m"$'\n'
    else
      out="$out"$'\r'"${esc}[K${row}"$'\n'
    fi
  done
  # подсказку тоже режем по ширине: её перенос сбивал бы PICK_DRAWN
  hint="↑↓ выбрать · Enter подключиться · q выход"
  [ "$cols" -ge 42 ] || hint="↑↓ · Enter · q"
  out="$out"$'\r'"${esc}[K${hint:0:$(( cols - 1 ))}"$'\n'
  printf '%s' "$out"
  PICK_DRAWN=$(( win + 1 ))
}

# вернуть терминал как было и стереть нарисованное
pick_restore() {
  local esc=$'\033'
  [ -z "$PICK_STTY" ] || stty "$PICK_STTY" 2>/dev/null || true
  PICK_STTY=""
  [ "$PICK_DRAWN" -eq 0 ] || printf '%s' "${esc}[${PICK_DRAWN}A${esc}[J"
  printf '%s' "${esc}[?25h"
  PICK_DRAWN=0
}

cmd_pick() {
  # без терминала пикера быть не может (cc | grep, CI) — отдаём плоский список
  if [ ! -t 0 ] || [ ! -t 1 ]; then cmd_ls; return 0; fi

  snapshot_sessions
  local n="${#PICK_NAMES[@]}"
  [ "$n" -gt 0 ] || { echo "нет запущенных сессий"; return 0; }

  local cols rows win cur=0 top=0 key rest sel
  cols="$(tput cols 2>/dev/null || echo 80)"
  rows="$(tput lines 2>/dev/null || echo 24)"
  win=$(( rows - 2 ))
  [ "$win" -ge 1 ] || win=1
  [ "$win" -le "$n" ] || win="$n"

  PICK_STTY="$(stty -g)"
  # -isig: Ctrl-C/Ctrl-Z приходят байтами и обрабатываются в цикле — сигнал
  # иначе выбросил бы скрипт, оставив терминал в raw-режиме.
  # min 1 time 0: без этого read может вернуть ноль байт и цикл сожрёт CPU.
  stty -echo -icanon -isig min 1 time 0
  printf '%s' $'\033[?25l'
  # трап только внутри пикера: глобальный EXIT-трап в cc держать не хотим
  trap 'pick_restore; exit 130' TERM HUP

  while :; do
    [ "$cur" -ge "$top" ] || top="$cur"
    [ "$cur" -lt $(( top + win )) ] || top=$(( cur - win + 1 ))
    pick_draw "$top" "$win" "$cols" "$cur"

    key=""
    IFS= read -rsn1 key || break
    case "$key" in
      $'\033')
        # Esc и стрелка начинаются одинаково: остаток читаем с таймаутом,
        # иначе одиночный Esc подвиснет в ожидании второго байта
        rest=""
        IFS= read -rsn2 -t 0.05 rest || true
        case "$rest" in
          '[A') [ "$cur" -eq 0 ] || cur=$(( cur - 1 )) ;;
          '[B') [ "$cur" -ge $(( n - 1 )) ] || cur=$(( cur + 1 )) ;;
          '') break ;;
        esac
        ;;
      k) [ "$cur" -eq 0 ] || cur=$(( cur - 1 )) ;;
      j) [ "$cur" -ge $(( n - 1 )) ] || cur=$(( cur + 1 )) ;;
      # read -n1 съедает перевод строки, так что Enter приходит пустой строкой
      '')
        sel="${PICK_NAMES[$cur]}"
        pick_restore; trap - TERM HUP
        attach_session "$sel"
        return 0 ;;
      q|Q|$'\003') break ;;
    esac
  done

  pick_restore; trap - TERM HUP
}

# задачу в существующую сессию: многострочный текст — только через
# bracketed paste, иначе каждый \n был бы Enter'ом и Claude отправил бы
# первую строку как отдельный запрос
send_task() { # send_task <имя> <задача>
  local name="$1" task="$2"
  case "$task" in
    *$'\n'*)
      printf '%s' "$task" | tmux load-buffer -b cc-task -
      tmux paste-buffer -b cc-task -p -d -t "=$name:"
      tmux send-keys -t "=$name:" Enter ;;
    *)
      tmux send-keys -t "=$name:" "$task" Enter ;;
  esac
}

start_session() { # start_session <dir> <имя> <задача> <autoname 0|1>
  local dir name="$2" task="$3" autoname="$4" cmdline rename_prompt
  dir="$(resolve_dir "$1")"
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
    send_task "$name" "$task"
    echo "задача отправлена в '$name'"
  fi
}

attach_session() { # attach_session <имя>
  local name="$1"
  # между снапшотом пикера и выбором сессия могла закрыться
  tmux has-session -t "=$name" 2>/dev/null || die "сессия '$name' уже закрылась"
  if [ -n "${TMUX:-}" ]; then
    tmux switch-client -t "=$name" 2>/dev/null || echo "переключись сам: tmux switch-client -t $name"
  else
    exec tmux attach -d -t "=$name"
  fi
}

# задача пишется в редакторе: сессия создаётся только после сохранения и выхода
cmd_new_from_editor() { # cmd_new_from_editor <dir> [слаг]
  local dir slug="${2:-}" editor tmp task status=0 name autoname
  if [ ! -t 0 ] || [ ! -t 1 ]; then die "cc n нужен терминал: задачу вводит редактор"; fi
  # проверяем директорию до редактора: обидно потерять уже набранную задачу
  dir="$(resolve_dir "$1")"
  editor="${VISUAL:-${EDITOR:-vim}}"
  command -v "${editor%% *}" >/dev/null 2>&1 \
    || die "не нашёлся редактор '${editor%% *}' — задай VISUAL или EDITOR"

  tmp="$(mktemp "${TMPDIR:-/tmp}/cc-task.XXXXXX")"
  # $EDITOR может содержать флаги ("code -w") — тут нужно разбиение на слова
  # shellcheck disable=SC2086
  $editor "$tmp" || status=$?
  if [ "$status" -ne 0 ]; then
    rm -f "$tmp"
    die "редактор вышел с кодом $status — сессия не создана"
  fi
  task="$(cat "$tmp")"
  rm -f "$tmp"
  [ -n "${task//[[:space:]]/}" ] || { echo "задача пустая — сессия не создана"; return 0; }

  if [ -n "$slug" ]; then
    name="$(norm "$slug")"; autoname=0
    [ -n "$name" ] || die "пустое имя сессии"
  else
    name="$(temp_name)"; autoname=1
  fi
  start_session "$dir" "$name" "$task" "$autoname"
  attach_session "$name"
}

# -C разбираем до команд: так он работает и с cc n, и с cc <slug>
dir="${CC_DIR:-$PWD}"
if [ "${1:-}" = "-C" ]; then
  [ $# -ge 2 ] || usage_die "cc -C <dir> <slug> [задача...]"
  dir="$2"; shift 2
fi

case "${1:-}" in
  s|sel|"") cmd_pick; exit 0 ;;
  ls)      cmd_ls; exit 0 ;;
  n|new)   [ $# -le 2 ] || usage_die "cc n [slug]"
           cmd_new_from_editor "$dir" "${2:-}"; exit 0 ;;
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
# один аргумент с пробелами — это задача, а не слаг: имя даст сам Claude
if [ $# -eq 1 ] && [[ "$1" == *" "* ]]; then
  name="$(temp_name)"; task="$1"; autoname=1
else
  name="$(norm "$1")"; shift; task="$*"; autoname=0
  [ -n "$name" ] || die "пустое имя сессии"
fi

start_session "$dir" "$name" "$task" "$autoname"
attach_session "$name"
