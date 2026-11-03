#!/usr/bin/env python3
"""Inspect V849 F8 NR raw witnesses without dependencies; never modify raw data."""
import argparse, csv, json, math, struct, zlib
from pathlib import Path

def png(path, width, height, rows):
    def chunk(kind, data):
        return struct.pack('>I', len(data))+kind+data+struct.pack('>I', zlib.crc32(kind+data)&0xffffffff)
    payload=b''.join(b'\0'+bytes(row) for row in rows)
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(payload))+chunk(b'IEND',b''))

def packed_float(bits, mantissa):
    exponent=bits>>mantissa; fraction=bits&((1<<mantissa)-1)
    return math.ldexp(fraction, -14-mantissa) if not exponent else (float('inf') if exponent==31 else math.ldexp(1+fraction/(1<<mantissa),exponent-15))

def decoder(fmt):
    floats={2:('4f',16),10:('4e',8),16:('2f',8),34:('2e',4),41:('f',4),40:('f',4),54:('e',2)}
    if fmt in floats:
        code,size=floats[fmt];return size,lambda b,o:struct.unpack_from('<'+code,b,o)
    if fmt in (28,29,87,91):
        return 4,lambda b,o: tuple(b[o+i]/255 for i in ((2,1,0,3) if fmt in (87,91) else (0,1,2,3)))
    if fmt==56:return 2,lambda b,o:(struct.unpack_from('<H',b,o)[0]/65535,)
    if fmt==24:
        def rgb10(b,o):
            v=struct.unpack_from('<I',b,o)[0];return ((v&1023)/1023,((v>>10)&1023)/1023,((v>>20)&1023)/1023)
        return 4,rgb10
    if fmt==26:
        def rgb11(b,o):
            v=struct.unpack_from('<I',b,o)[0];return (packed_float(v&2047,6),packed_float((v>>11)&2047,6),packed_float(v>>22,5))
        return 4,rgb11
    raise ValueError('Unsupported DXGI format '+str(fmt))

def inspect(directory):
    directory=Path(directory);out=directory/'nr-inspection';out.mkdir(exist_ok=True)
    report=[]
    with (directory/'manifest.tsv').open(encoding='utf-8-sig',newline='') as f: records=list(csv.DictReader(f,delimiter='\t'))
    for entry in records:
        if not entry['stage'].startswith('nr_'):continue
        item={'index':entry['index'],'stage':entry['stage'],'handle':entry['stream'],'serial':entry['serial'],'status':entry['status'],'raw':entry['file']}
        try:
            if int(entry['status'],16)!=0:raise ValueError('Capture failed: HRESULT 0x'+entry['status'])
            raw=(directory/entry['file']).read_bytes();fmt=int(entry['format']);size,read=decoder(fmt)
            pitch=int(entry['row_pitch']);rw=int(entry['resource_width']);rh=int(entry['resource_height'])
            x=int(entry['x']);y=int(entry['y']);w=int(entry['width']) or rw;h=int(entry['height']) or rh
            if x+w>rw or y+h>rh or len(raw)<pitch*rh:raise ValueError('Invalid extent/footprint')
            step=max(1,math.ceil(max(w,h)/1024));xs=range(x,x+w,step);ys=range(y,y+h,step)
            pixels=[[read(raw,yy*pitch+xx*size) for xx in xs] for yy in ys]
            values=[v for row in pixels for pixel in row for v in pixel[:3] if math.isfinite(v)]
            if not values:raise ValueError('No finite RGB samples')
            lo=min(values);hi=max(values)
            chroma=[max(p[:3])-min(p[:3]) for row in pixels for p in row if len(p)>=3 and all(math.isfinite(v) for v in p[:3])]
            item.update(format=fmt,width=w,height=h,sampled_min=lo,sampled_max=hi,sampled_mean=sum(values)/len(values),mean_rgb_chroma=sum(chroma)/len(chroma) if chroma else None)
            is_depth='depth' in entry['stage'];is_motion='motion' in entry['stage'];hdr=('source_color' in entry['stage'] or 'resolved_color' in entry['stage']) and hi>1
            def byte(v):return round(max(0,min(1,v if math.isfinite(v) else 0))*255)
            def rgb(p):
                if is_depth:return [byte((p[0]-lo)/max(hi-lo,1e-12))]*3
                if is_motion:return [byte(.5+p[0]/max(abs(lo),abs(hi),1e-12)/2),byte(.5+p[1]/max(abs(lo),abs(hi),1e-12)/2),128]
                c=list(p[:3]) if len(p)>=3 else [p[0]]*3
                if hdr:c=[max(0,v)/(1+max(0,v)) for v in c]
                return [byte(v) for v in c]
            name=f"{entry['index']}-{entry['stage']}-view{entry['stream']}-eval{entry['serial']}.png"
            png(out/name,len(xs),len(ys),([v for p in row for v in rgb(p)] for row in pixels))
            item['preview']=name;item['preview_transform']='depth min/max' if is_depth else 'signed vectors' if is_motion else 'x/(1+x)' if hdr else 'clamp 0..1'
        except (ValueError,OSError,KeyError,struct.error,OverflowError) as exc:item['error']=str(exc)
        report.append(item)
    (out/'statistics.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf-8')
    print(f'{len(report)} NR images: {out}')
    return report

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('capture_directory');args=parser.parse_args();inspect(args.capture_directory)
