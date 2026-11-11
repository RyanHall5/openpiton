# OpenPiton + Ariane environment for RIT RHEL 9 lab workstations (VCS 2017.12).
#
# Source this in every new terminal before using vcs / sims / noctrace:
#     source ~/openpiton/rit/setup/openpiton_env.sh
# (adjust the path if you cloned somewhere else; PITON_ROOT is worked out from
# where this file lives, so the clone can be anywhere).
#
# Run rit/setup/install_deps.sh once first; it builds everything referenced here.
#
# Line order matters:
#   * VCS_HOME must be set BEFORE piton_settings.bash is sourced (it reads it).
#   * The ldshim PATH line must come AFTER piton_settings.bash, which puts
#     /usr/bin near the front of PATH; otherwise the system ld wins and Fix 1
#     silently stops working.

module load synopsys

_rit_setup_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export PITON_ROOT=$(cd "$_rit_setup_dir/../.." && pwd)
unset _rit_setup_dir
export ARIANE_ROOT=$PITON_ROOT/piton/design/chip/tile/ariane
export RISCV=$HOME/tools/riscv64-unknown-elf-gcc-8.3.0-2019.08.0-x86_64-linux-centos6
export PATH=$HOME/.local/bin:$RISCV/bin:$PATH
export VCS_HOME=/tools/rhel6/synopsys/vcs_mx/current
# Only needed if install_deps.sh had to install Bit::Vector into ~/perl5
[ -d "$HOME/perl5/lib/perl5" ] && export PERL5LIB=$HOME/perl5/lib/perl5${PERL5LIB:+:$PERL5LIB}

source $PITON_ROOT/piton/piton_settings.bash

# --- RHEL 9 workarounds for VCS 2017.12 (must stay at the end) ---
export PATH=$HOME/ldshim:$PATH          # Fix 1: use GNU ld 2.30
export VCS_UVM_HOME=$HOME/opt/uvm-1.1   # Fix 3: patched private UVM copy

# Fixes 1+2 also need these on every VCS build; use as:
#   sims ... -vcs_build $PITON_VCS_FIX_ARGS
export PITON_VCS_FIX_ARGS="-vcs_build_args=-LDFLAGS -vcs_build_args=-B$HOME/opt/binutils-2.30/bin -vcs_build_args=$HOME/opt/vcs_compat/pthread_yield.o"

# NoC trace analysis: python3 -m noctrace ...
export PYTHONPATH=$PITON_ROOT/rit${PYTHONPATH:+:$PYTHONPATH}
