# bash completion for powerprofile-x
# Install as: $BASH_COMPLETION_USER_DIR/completions/powerprofile-x
#         or: /usr/share/bash-completion/completions/powerprofile-x
# Requires the bash-completion package (for _init_completion).

_powerprofile_x()
{
	local cur prev words cword split
	_init_completion -s || return

	case "$prev" in
	--config|--state)
		_filedir
		return
		;;
	--output)
		local IFS=$'\n'
		COMPREPLY=( $(compgen -W \
			"$(xrandr --query 2>/dev/null | awk '/ connected/ { print $1 }')" \
			-- "$cur") )
		return
		;;
	esac

	# Handled a --opt=value split that matched no case above.
	$split && return

	if [[ $cur == -* ]]; then
		COMPREPLY=( $(compgen -W '--once --config --state --output
			--verbose --version --help' -- "$cur") )
	fi
}
complete -F _powerprofile_x powerprofile-x
