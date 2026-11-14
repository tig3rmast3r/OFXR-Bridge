"""Resource checks for the optional Chinese UI (pip install fonttools)."""
from pathlib import Path
from io import BytesIO
import json,re,unittest
from fontTools.ttLib import TTFont
ROOT=Path(__file__).resolve().parents[1]
class LocalizedUIAssets(unittest.TestCase):
 def test_printf_arguments_unchanged(self):
  pairs=json.loads((ROOT/'src/ui/locales/zh-CN.json').read_text(encoding='utf-8'))
  pattern=r'%(?:[-+0#]*\d*(?:\.\d+)?(?:ll|hh|[hljztL])?[diuoxXfFeEgGscpaA]|%)'
  for english,chinese in pairs.items():
   with self.subTest(english=english):self.assertEqual(re.findall(pattern,english),re.findall(pattern,chinese))
 def test_embedded_chinese_glyphs(self):
  source=(ROOT/'src/ui/settings_font_zh.inc').read_text(encoding='ascii')
  data=bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})',source));cmap=TTFont(BytesIO(data)).getBestCmap()
  text=(ROOT/'src/ui/locales/zh-CN.json').read_text(encoding='utf-8')
  text+=''.join(p.read_text(encoding='utf-8') for p in (ROOT/'src/ui').glob('*.cpp'))
  required={ord(ch) for ch in text if '\u4e00'<=ch<='\u9fff'}
  self.assertFalse(required-cmap.keys())
 def test_catalog_avoids_translatable_widget_ids(self):
  pairs=json.loads((ROOT/'src/ui/locales/zh-CN.json').read_text(encoding='utf-8'))
  self.assertGreater(len(pairs),450)
  self.assertTrue(all('###' not in key for key in pairs))
if __name__=='__main__':unittest.main()
