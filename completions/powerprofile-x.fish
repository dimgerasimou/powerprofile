# fish completion for powerprofile-x
# Install as: ~/.config/fish/completions/powerprofile-x.fish
#         or: /usr/share/fish/vendor_completions.d/powerprofile-x.fish

function __powerprofile_x_outputs
    xrandr --query 2>/dev/null | string match -rg '^(\S+) connected'
end

# No positional arguments take files.
complete -c powerprofile-x -f

complete -c powerprofile-x -l once    -d 'Apply the current profile once and exit'
complete -c powerprofile-x -l config  -d 'Read another config file' -r -F
complete -c powerprofile-x -l state   -d 'Follow another state file' -r -F
complete -c powerprofile-x -l output  -d 'Use this output instead of the internal panel' -x -a '(__powerprofile_x_outputs)'
complete -c powerprofile-x -l verbose -d 'Log actions to stderr'
complete -c powerprofile-x -l version -d 'Print version and exit'
complete -c powerprofile-x -l help    -d 'Print help and exit'
