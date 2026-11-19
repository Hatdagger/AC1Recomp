#!/bin/sh
# Makes one README picture from its page, at 2x, cut to the page's height,
# then cuts it to 256 colours so the file stays small.
#   sh shot.sh banner.html ../banner.png
set -e
H=$(google-chrome --headless=new --no-sandbox --disable-gpu --window-size=1100,400 --virtual-time-budget=2000 \
  --dump-dom "file://$PWD/$1" 2>/dev/null | sed -n 's/.*data-h="\([0-9]*\)".*/\1/p' | head -1)
[ -n "$H" ] || H=600
google-chrome --headless=new --no-sandbox --disable-gpu --hide-scrollbars --force-device-scale-factor=2 \
  --window-size=1100,$H --screenshot="$2" "file://$PWD/$1" >/dev/null 2>&1
python3 -c "import sys; from PIL import Image; im=Image.open(sys.argv[1]).convert('RGB'); im.quantize(colors=256, method=Image.Quantize.FASTOCTREE).save(sys.argv[1], optimize=True)" "$2"
