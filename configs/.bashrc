# .bashrc

# If not running interactively, don't do anything
[[ $- != *i* ]] && return

alias ls='ls --color=auto'
alias opencode='docker start -ai opencode-sandbox'
PS1='[\u@\h \W]\$ '

export MANPAGER='nvim +Man!'
export PATH="$HOME/.local/bin:$PATH"
