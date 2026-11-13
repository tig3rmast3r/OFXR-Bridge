#!/usr/bin/env python3
"""Generate the UTF-8 English/Chinese lookup table from the translation catalog."""
from pathlib import Path
import argparse,json
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--check',action='store_true');args=p.parse_args()
source=root/'src/ui/locales/zh-CN.json';output=root/'src/common/ui_translations.inc'
pairs=json.loads(source.read_text(encoding='utf-8'))
lines=['// Generated from src/ui/locales/zh-CN.json. Do not edit by hand.','constexpr Translation translations[] = {']
lines+=['    {'+json.dumps(a,ensure_ascii=False)+','+json.dumps(b,ensure_ascii=False)+'},' for a,b in sorted(pairs.items())]
lines+=['};',''];text='\n'.join(lines)
if args.check:
 if output.read_text(encoding='utf-8')!=text:raise SystemExit('Regenerate translations with tools/generate_ui_translations.py')
else:output.write_text(text,encoding='utf-8')
