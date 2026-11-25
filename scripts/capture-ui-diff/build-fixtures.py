#!/usr/bin/env python3
"""Build the native macOS RDP capture fixture from the existing Harbor sysroot."""
import argparse
from pathlib import Path
import shlex
import subprocess

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--sysroot',type=Path,default=Path.home()/'.local/harbor-macos/universal')
parser.add_argument('--link-line',type=Path,default=Path.home()/'harbor-build/link-line.txt')
args=parser.parse_args()
output=ROOT/'.cache/ui-rdp-fixture'
output.parent.mkdir(exist_ok=True)
flags=[t for t in shlex.split(args.link_line.read_text().replace('\\\n',' '))
       if t!='-pthread' and 'libharbor-core' not in t]
subprocess.run(['clang','-std=c11','-Wno-deprecated-declarations',
    '-I'+str(args.sysroot/'include/freerdp3'),'-I'+str(args.sysroot/'include/winpr3'),
    str(HERE/'fixtures/rdp.c'),'-o',str(output),*flags],check=True)
print(output)
