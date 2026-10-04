# .bash_profile

# Get the aliases and functions
[ -f "$HOME/.bashrc" ] && . "$HOME/.bashrc"

if [ "$(tty)" = /dev/tty1 ]; then
	export XDG_SESSION_TYPE=wayland
	# Everforest rice: consistent mouse cursor
	export XCURSOR_THEME=Adwaita
	export XCURSOR_SIZE=24
exec dbus-run-session -- sh -c '
    umask 077
    log_base=${XDG_RUNTIME_DIR:-"$HOME/.cache"}
    mkdir -p "$log_base" || exit 1
    log_dir=$(mktemp -d "$log_base/everforest-session.XXXXXX") || exit 1
    pipewire >"$log_dir/pipewire.log" 2>&1 &
    pw_pid=$!
    trap "kill \"$pw_pid\" 2>/dev/null; wait \"$pw_pid\" 2>/dev/null || :" 0
    trap "exit 130" INT
    trap "exit 143" TERM
    dwl -s "$HOME/.local/bin/everbar-start"
    result=$?
    exit "$result"
'
fi
