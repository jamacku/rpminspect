#!/bin/sh
# Copyright © 2019 Red Hat, Inc.
# Author(s): David Shea <dshea@redhat.com>
#            David Cantrell <dcantrell@redhat.com>
#
# This program is free software: you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public License
# as published by the Free Software Foundation, either version 3 of
# the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this program.  If not, see
# <https://www.gnu.org/licenses/>.
#
# SPDX-License-Identifier: LGPL-3.0-or-later

# shellcheck disable=SC2129

# WHAT IS THIS:
# This script parses /usr/include/elf.h to generate a C function that,
# given a ELF relocation entry, determines whether that relocation indicates
# that the ELF object is compiled to use position independent code.

# THAT SOUNDS REALLY AWFUL:
# Yeah it kind of does. The problem is that the only way we know to determine
# whether a .o file was compiled with -fPIC or not is to look for relocations
# that either use a procedure linkage table (PLT) or global offset table (GOT),
# and the values for these relocation types are all architecture specific.
# Some alternatives are:
#  * do it the old way: run `eu-readelf --reloc` and grep for
#    (PLT|GOT) in the reloc type: The disadvantage is that you need
#    run eu-readelf and parse its output a whole bunch of times.
#  * write a function that hard-codes all of the relevant
#    architectures and relocation types: That's basically what this
#    script is doing. Writing it by hand(ish) would have the advantage
#    that no bad symbols slip through, but that assumes that the
#    writer actually knows what any of this stuff means. This would
#    also means the function has to be manually updated every time a
#    new arch comes along.
#  * find someone who knows what any of this stuff means to explain a
#    better way: Maybe?
#
# Anyway here goes. The machine types are in the EM_<arch> #define's, and the
# reloc types are in the R_<arch>_<type> #define's. The only case we (sort of)
# care about where the arch's don't match is ia64: it's EM_IA_64 but R_IA64_*.

if ! cpp_output="$(cpp -dM /usr/include/elf.h)"; then
    echo "Unable to read elf.h" >&2
    echo "Ensure glibc-headers is installed" >&2
    exit 1
fi

if [ -z "$1" ]; then
    echo "Usage: $(basename "$0") [output file]" >&2
    exit 1
fi

echo "/*" > "$1"
echo " * This file is autogenerated. DO NOT EDIT." >> "$1"
echo " * See pic_bits.sh to modify." >> "$1"
echo " */" >> "$1"
echo "#include <stdbool.h>" >> "$1"
echo "#include <stdio.h>" >> "$1"
echo "#include <err.h>" >> "$1"
echo "#include <elf.h>" >> "$1"
echo "#include \"rpminspect.h\"" >> "$1"
echo "bool is_pic_reloc(Elf64_Half machine, Elf64_Xword rel_type)" >> "$1"
echo "{" >> "$1"
echo "    switch (machine) {" >> "$1"

# get a list of the EM_* arch lines:
echo "$cpp_output" | sed -n -E 's/^#define[[:space:]]+(EM_[^[:space:]]+).*/\1/p' | \
    while read -r arch; do
        # for each arch, look for corresponding reloc types that have
        # either GOT or PLT in the macro name.
        archpart="$(echo "$arch" | sed 's/^EM_//')"

        # some machine names do not map directly to the names for arch types
        [ "$archpart" = "IA_64" ] && archpart="IA64"
        [ "$archpart" = "S390" ] && archpart="390"

        relocs="$(echo "$cpp_output" | sed -n -E 's/^#define[[:space:]]+(R_'"${archpart}"'_[^[:space:]]*(PLT|GOT)[^[:space:]]*).*/\1/p')"
        if [ -n "$relocs" ]; then
            echo "        case ${arch}:" >> "$1"
            echo "            switch (rel_type) {" >> "$1"
            echo "$relocs" | while read -r reloc_type ; do
                echo "                case ${reloc_type}:" >> "$1"
            done
            echo "                    return true;" >> "$1"
            echo "                default:" >> "$1"
            echo "                    return false;" >> "$1"
            echo "            }" >> "$1"
        fi
    done

echo "        default:" >> "$1"
# use printf to avoid a non-bash /bin/sh's echo messing up the \'s.
printf '            warnx(_("unknown machine type %%u\\n"), machine);\n' >> "$1"
printf '            warnx(_("Recompile librpminspect with a newer elf.h, or make necessary modifications to pic_bits.sh\\n"));\n' >> "$1"
echo "            return false;" >> "$1"
echo "    }" >> "$1"
echo "}" >> "$1"
