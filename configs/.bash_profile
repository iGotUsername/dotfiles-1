# .bash_profile

# Get the aliases and functions
[ -f "$HOME/.bashrc" ] && . "$HOME/.bashrc"

if [ "$(tty)" = /dev/tty1 ]; then
	export XDG_SESSION_TYPE=wayland
	# Everforest rice: consistent mouse cursor
	export XCURSOR_THEME=Adwaita
	export XCURSOR_SIZE=24

    # Keep desktop-session output off tty1.
    session_log_dir="$HOME/.local/state/dwl"
    if (umask 077; mkdir -p "$session_log_dir"); then
        if [ -f "$session_log_dir/session.log" ]; then
            mv -f "$session_log_dir/session.log" \
                "$session_log_dir/session.log.previous"
        fi
        if (umask 077; : > "$session_log_dir/session.log"); then
            exec >> "$session_log_dir/session.log" 2>&1
        fi
    fi

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
    eval "$(gnome-keyring-daemon --start --components=secrets)"
    dwl -s "$HOME/.local/bin/everbar-start"
    result=$?
    exit "$result"
'
fi
