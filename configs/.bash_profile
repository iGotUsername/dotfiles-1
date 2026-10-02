# .bash_profile

# Get the aliases and functions
[ -f $HOME/.bashrc ] && . $HOME/.bashrc

if [ "$(tty)" = /dev/tty1 ]; then
	export XDG_SESSION_TYPE=wayland
	# Everforest rice: consistent mouse cursor
	export XCURSOR_THEME=Adwaita
	export XCURSOR_SIZE=24
	exec dbus-run-session -- sh -c 'pipewire >/tmp/pipewire.log 2>&1 & exec dwl -s "$HOME/.local/bin/everbar-start"'
fi
