#!/usr/bin/env python3
"""Board GOP archive, with static exact21H461 ABI qualification."""
import os,subprocess
from pathlib import Path
ROOT=Path(os.environ.get('SMUFS_BUILD_TREE',str(Path(__file__).resolve().parents[1])))
tools=ROOT/'scripts/display-tools'
env=os.environ.copy();env.update(SMGOP_BUILD_TREE=str(ROOT),SMGOP_OUTPUT=str(ROOT/'build/native-legacy-display'),
 SMGOP_REFERENCE_DIR=str(ROOT/'scripts/display-reference'),SMGOP_DRIVER_DIR=str(ROOT/'kits/instantnoodlep/GopFramebufferR20'),
 SMGOP_BOARD_SOURCE=str(ROOT/'kits/instantnoodlep/SmGopDisplay.cpp'))
for script in ['audit_display.py','build_display.py']:
 subprocess.run(['python3',str(tools/script)],env=env,check=True)
