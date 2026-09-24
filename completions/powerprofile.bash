# bash completion for powerprofile
# Install as: $BASH_COMPLETION_USER_DIR/completions/powerprofile
#         or: /usr/share/bash-completion/completions/powerprofile
# Requires the bash-completion package (for _init_completion).

_powerprofile()
{
	local cur prev words cword split
	_init_completion -s || return

	case "$prev" in
	--profile)
		local IFS=$'\n'
		COMPREPLY=( $(compgen -W \
			"$(powerprofile --list 2>/dev/null)" -- "$cur") )
		return
		;;
	--config)
		_filedir
		return
		;;
	esac

	# Handled a --opt=value split that matched no case above.
	$split && return

	if [[ $cur == -* ]]; then
		COMPREPLY=( $(compgen -W '--status --list --auto --profile
			--dry-run --config --verbose --version --help' -- "$cur") )
	fi
}
complete -F _powerprofile powerprofile
