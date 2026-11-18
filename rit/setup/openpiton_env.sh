# OpenPiton + Ariane environment for RIT RHEL 9 lab workstations.
#
# Source this in every new terminal before using vcs / sims / noctrace:
#     source ~/openpiton/rit/setup/openpiton_env.sh
#
# Which VCS: by default VCS 2017.12 (/tools/rhel6, built for RHEL 6; needs the
# three RHEL 9 workarounds below). For the newer VCS built for EL9, which needs
# none of them, set RIT_VCS first, in a NEW terminal:
#     RIT_VCS=el9 source ~/openpiton/rit/setup/openpiton_env.sh
# (RIT_VCS_VERSION picks the release, default W-2024.09; others are in
# /tools/synopsys-el9/vcs/.) A model must be run with the VCS that built it;
# pbuild records which one in build/manycore/<build_id>/rit_vcs_home.
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
if [ "${RIT_VCS:-}" = "el9" ]; then
    export VCS_HOME=/tools/synopsys-el9/vcs/${RIT_VCS_VERSION:-W-2024.09}
else
    export VCS_HOME=/tools/rhel6/synopsys/vcs_mx/current
fi
# Only needed if install_deps.sh had to install Bit::Vector into ~/perl5
[ -d "$HOME/perl5/lib/perl5" ] && export PERL5LIB=$HOME/perl5/lib/perl5${PERL5LIB:+:$PERL5LIB}

source $PITON_ROOT/piton/piton_settings.bash

if [ "${RIT_VCS:-}" = "el9" ]; then
    # VCS built for EL9: no workarounds. Use its own UVM, the system linker.
    unset VCS_UVM_HOME
    unset PITON_VCS_FIX_ARGS
    _rit_vcs_fix_args=()
    echo "openpiton_env: VCS $VCS_HOME (EL9 build, no RHEL 9 workarounds)"
else
    # --- RHEL 9 workarounds for VCS 2017.12 (must stay at the end) ---
    export PATH=$HOME/ldshim:$PATH          # Fix 1: use GNU ld 2.30
    export VCS_UVM_HOME=$HOME/opt/uvm-1.1   # Fix 3: patched private UVM copy
    # Fixes 1+2 also need three extra VCS arguments on every build (-vcs_build),
    # not on runs. Leaving them off fails the link with
    # "undefined reference to 'pthread_yield'" or the ld sh_info error.
    _rit_vcs_fix_args=(-vcs_build_args=-LDFLAGS
                       -vcs_build_args=-B"$HOME"/opt/binutils-2.30/bin
                       -vcs_build_args="$HOME"/opt/vcs_compat/pthread_yield.o)
    # Same fix arguments as one variable, for scripts that call sims directly:
    #   sims ... -vcs_build $PITON_VCS_FIX_ARGS
    export PITON_VCS_FIX_ARGS="${_rit_vcs_fix_args[*]}"
fi

# pbuild: sims with -vcs_build (plus the fix arguments for VCS 2017). Takes the
# same arguments as sims otherwise:
#   pbuild -sys=manycore -x_tiles=2 -y_tiles=2 -build_id=ariane_2x2 -ariane
# Records the VCS that built the model in build/manycore/<build_id>/rit_vcs_home.
pbuild() {
    local bid="" a
    for a in "$@"; do case $a in -build_id=*) bid=${a#-build_id=};; esac; done
    sims "$@" -vcs_build "${_rit_vcs_fix_args[@]}" || return
    [ -n "$bid" ] && [ -d "$MODEL_DIR/manycore/$bid" ] && echo "$VCS_HOME" > "$MODEL_DIR/manycore/$bid/rit_vcs_home"
    return 0
}

# NoC trace analysis: python3 -m noctrace ...
export PYTHONPATH=$PITON_ROOT/rit${PYTHONPATH:+:$PYTHONPATH}

# nocrun: run simulations into their own folders (rit/docs/runs.md)
export PATH=$PITON_ROOT/rit/bin:$PATH
export NOC_RUNS=${NOC_RUNS:-$HOME/noc_runs}
