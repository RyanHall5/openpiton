#!/usr/bin/env bash
# One-time setup of everything OpenPiton + Ariane needs on an RIT RHEL 9 lab
# workstation with the lab's VCS N-2017.12. No root needed; everything goes in
# your home directory. Safe to re-run: finished steps are skipped.
#
#   bash ~/openpiton/rit/setup/install_deps.sh
#
# Steps (see rit/docs/rhel9_vcs_setup.md for why each exists):
#   1. Ariane submodule
#   2. RISC-V toolchain (SiFive GCC 8.3.0)     -> ~/tools/
#   3. device tree compiler (dtc)              -> ~/.local/bin/dtc
#   4. Perl Bit::Vector (only if missing)      -> ~/perl5/
#   5. Fix 1: GNU ld 2.30 + ldshim             -> ~/opt/binutils-2.30/, ~/ldshim/ld
#   6. Fix 2: pthread_yield stub               -> ~/opt/vcs_compat/pthread_yield.o
#   7. Fix 3: patched private UVM copy         -> ~/opt/uvm-1.1/
#   8. Python packages for noctrace            -> pip --user (pandas, numpy, matplotlib)
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DL=$HOME/.cache/openpiton_deps
mkdir -p "$DL" "$HOME/tools" "$HOME/opt" "$HOME/.local/bin"
JOBS=$(nproc 2>/dev/null || echo 4)

say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
skip() { printf '   already done: %s\n' "$*"; }

for t in git gcc make wget tar perl; do
  command -v $t >/dev/null || { echo "missing required tool: $t"; exit 1; }
done

# ---------------------------------------------------------------- 1
say "1/8 Ariane submodule"
if [ -n "$(ls -A "$REPO/piton/design/chip/tile/ariane" 2>/dev/null)" ]; then
  skip "piton/design/chip/tile/ariane is populated"
else
  # Only Ariane (and its own submodules). Other OpenPiton submodules aren't
  # needed for simulation, and u-boot's uses an SSH URL that needs a GitHub key.
  git -C "$REPO" submodule update --init --recursive piton/design/chip/tile/ariane
fi

# ---------------------------------------------------------------- 2
say "2/8 RISC-V toolchain"
TC=riscv64-unknown-elf-gcc-8.3.0-2019.08.0-x86_64-linux-centos6
if [ -x "$HOME/tools/$TC/bin/riscv64-unknown-elf-gcc" ]; then
  skip "~/tools/$TC"
else
  wget -q --show-progress -O "$DL/$TC.tar.gz" "https://static.dev.sifive.com/dev-tools/$TC.tar.gz"
  tar xf "$DL/$TC.tar.gz" -C "$HOME/tools"
fi

# ---------------------------------------------------------------- 3
say "3/8 dtc"
if [ -x "$HOME/.local/bin/dtc" ]; then
  skip "$("$HOME/.local/bin/dtc" --version)"
else
  rm -rf "$DL/dtc"
  git clone -q https://git.kernel.org/pub/scm/utils/dtc/dtc.git "$DL/dtc"
  make -C "$DL/dtc" -j"$JOBS" NO_PYTHON=1 >/dev/null
  make -C "$DL/dtc" install NO_PYTHON=1 PREFIX="$HOME/.local" >/dev/null
fi

# ---------------------------------------------------------------- 4
say "4/8 Perl Bit::Vector"
if perl -MBit::Vector -e1 2>/dev/null || PERL5LIB=$HOME/perl5/lib/perl5 perl -MBit::Vector -e1 2>/dev/null; then
  skip "Bit::Vector available"
else
  [ -x "$HOME/perl5/bin/cpanm" ] || curl -sL https://cpanmin.us | perl - -l "$HOME/perl5" App::cpanminus
  "$HOME/perl5/bin/cpanm" -l "$HOME/perl5" Bit::Vector
fi

# ---------------------------------------------------------------- 5
say "5/8 Fix 1: GNU ld 2.30 (takes ~15 minutes the first time)"
if [ -x "$HOME/opt/binutils-2.30/bin/ld" ]; then
  skip "$("$HOME/opt/binutils-2.30/bin/ld" -v)"
else
  [ -f "$DL/binutils-2.30.tar.gz" ] || wget -q --show-progress -O "$DL/binutils-2.30.tar.gz" https://ftp.gnu.org/gnu/binutils/binutils-2.30.tar.gz
  rm -rf "$DL/binutils-2.30" && tar xf "$DL/binutils-2.30.tar.gz" -C "$DL"
  ( cd "$DL/binutils-2.30" &&
    ./configure --prefix="$HOME/opt/binutils-2.30" --disable-werror --disable-gold --disable-gdb >/dev/null &&
    make -j"$JOBS" MAKEINFO=true >/dev/null &&
    make install MAKEINFO=true >/dev/null )
fi
mkdir -p "$HOME/ldshim"
ln -sf "$HOME/opt/binutils-2.30/bin/ld" "$HOME/ldshim/ld"

# ---------------------------------------------------------------- 6
say "6/8 Fix 2: pthread_yield stub"
mkdir -p "$HOME/opt/vcs_compat"
cp "$HERE/pthread_yield.c" "$HOME/opt/vcs_compat/pthread_yield.c"
gcc -c -fPIC "$HOME/opt/vcs_compat/pthread_yield.c" -o "$HOME/opt/vcs_compat/pthread_yield.o"
echo "   built ~/opt/vcs_compat/pthread_yield.o"

# ---------------------------------------------------------------- 7
say "7/8 Fix 3: patched UVM copy"
VCS_HOME=${VCS_HOME:-/tools/rhel6/synopsys/vcs_mx/current}
if [ -d "$HOME/opt/uvm-1.1/dpi" ]; then
  skip "~/opt/uvm-1.1"
elif [ -d "$VCS_HOME/etc/uvm-1.1" ]; then
  cp -r "$VCS_HOME/etc/uvm-1.1" "$HOME/opt/uvm-1.1"
else
  echo "   can't find $VCS_HOME/etc/uvm-1.1 (run 'module load synopsys' or set VCS_HOME)"; exit 1
fi
# Replace curly quotes (UTF-8 bytes) that GCC 11 rejects. Byte-exact, so it
# doesn't depend on the terminal's locale.
LC_ALL=C sed -i -e 's/\xe2\x80\x9c/"/g' -e 's/\xe2\x80\x9d/"/g' \
                -e "s/\xe2\x80\x98/'/g" -e "s/\xe2\x80\x99/'/g" "$HOME/opt/uvm-1.1/dpi/uvm_hdl.c"
if LC_ALL=C grep -rlq $'\xe2\x80' "$HOME/opt/uvm-1.1/dpi"; then
  echo "   WARNING: non-ASCII quotes still present in ~/opt/uvm-1.1/dpi"
else
  echo "   ~/opt/uvm-1.1/dpi is clean"
fi

# ---------------------------------------------------------------- 8
say "8/8 Python packages for noctrace"
if python3 -c "import pandas, numpy, matplotlib" 2>/dev/null; then
  skip "pandas, numpy, matplotlib importable"
else
  # RHEL's system python3 often has no pip. Bootstrap a user-level pip
  # (~/.local), never touching the system install: try the stdlib ensurepip
  # first, then PyPA's get-pip.py pinned to this Python version.
  if ! python3 -m pip --version >/dev/null 2>&1; then
    echo "   no pip for $(python3 --version); installing a user-level pip"
    if ! python3 -m ensurepip --user >/dev/null 2>&1; then
      PYV=$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')
      if ! wget -q -O "$DL/get-pip.py" "https://bootstrap.pypa.io/pip/$PYV/get-pip.py"; then
        wget -q -O "$DL/get-pip.py" "https://bootstrap.pypa.io/get-pip.py"
      fi
      python3 "$DL/get-pip.py" --user
    fi
  fi
  python3 -m pip install --user pandas numpy matplotlib
  python3 -c "import pandas, numpy, matplotlib; print('   pandas', pandas.__version__, '| matplotlib', matplotlib.__version__)"
fi

say "Done. Next:"
cat <<EOF
   source $REPO/rit/setup/openpiton_env.sh
   cd \$PITON_ROOT/build
   sims -sys=manycore -x_tiles=2 -y_tiles=2 -build_id=ariane_2x2 -vcs_build -ariane \$PITON_VCS_FIX_ARGS
EOF
