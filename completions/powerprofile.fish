# fish completion for powerprofile
# Install as: ~/.config/fish/completions/powerprofile.fish
#         or: /usr/share/fish/vendor_completions.d/powerprofile.fish

function __powerprofile_profiles
    powerprofile --list 2>/dev/null
end

# No positional arguments take files.
complete -c powerprofile -f

complete -c powerprofile -l status  -d 'Print the AC state and live values'
complete -c powerprofile -l list    -d 'Print the profile names'
complete -c powerprofile -l auto    -d 'Apply bat or ac from the AC adapter'
complete -c powerprofile -l profile -d 'Apply one profile now' -x -a '(__powerprofile_profiles)'
complete -c powerprofile -l dry-run -d 'Print what would be done'
complete -c powerprofile -l config  -d 'Read another config file' -r -F
complete -c powerprofile -l verbose -d 'Log actions to stderr'
complete -c powerprofile -l version -d 'Print version and exit'
complete -c powerprofile -l help    -d 'Print help and exit'
