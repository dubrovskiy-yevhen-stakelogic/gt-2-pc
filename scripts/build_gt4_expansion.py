"""Build an append-only GT2 catalogue from privately extracted GT4 SpecDB data.

Inputs and generated game data belong under work/. Original GT2 row numbers
are retained: saved CarConfig records refer to these numbers directly.
"""
import argparse
import hashlib
import json
import math
import re
from pathlib import Path
import struct
import unicodedata

SIZES = [12,16,24,20,12,28,76,12,12,12,12,12,20,16,12,16,12,36,76,12,12,32,16,12,4,64,8,16,16,8,72]
SPEC_TABLES = [0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,21,17,18,19,20,22,23,27,28,29]
PARTS = ['brakes','brakeController','steering','chassis','lightweight','racingModify','engine','portPolish',
         'engineBalance','displacement','computer','naTune','turboKit','drivetrain','flywheel','clutch',
         'propellerShaft','lsd','gearbox','suspension','intercooler','muffler','tyresFront','tyresRear',
         'activeStability','tractionControl','profile']

def u16(b,o): return struct.unpack_from('<H',b,o)[0]
def u32(b,o): return struct.unpack_from('<I',b,o)[0]
def put(b,o,v,kind='H'): struct.pack_into('<'+kind,b,o,v)
def pack(s):
    n=0
    for c in s: n=n*64+'-0123456789abcdefghijklmnopqrstuvwxyz'.index(c)
    return n
def read_json(p): return json.loads(Path(p).read_text(encoding='utf-8-sig'))
def write_json(p,v): Path(p).write_text(json.dumps(v,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
def write_catalogue_version(output):
    # The marker must have the same bytes on Windows, Android and Unix.
    (Path(output)/'catalogue.version').write_bytes(b'gt2-car-expansion-1\n')
def ascii_name(s): return unicodedata.normalize('NFKD',s.replace('`',"'")).encode('ascii','replace').decode()

def gtdt(b):
    assert b[:4]==b'GTDT'
    return [bytearray(b[o:o+n]) for o,n in (struct.unpack_from('<II',b,8+8*i) for i in range(u16(b,6)))]

def encode_gtdt(parts):
    b=bytearray(b'GTDT\x6c\0'+struct.pack('<H',len(parts))+bytes(8*len(parts)))
    for i,p in enumerate(parts):
        put(b,8+i*8,len(b),'I');put(b,12+i*8,len(p),'I');b.extend(p)
    return b

def wsdb(b,names):
    assert b[4:8]==b'WSDB' and u32(b,0)==len(b)
    out=bytearray(b);first=u16(b,8)
    for s in names:
        data=s.encode('utf-16le');out.extend(struct.pack('<H',len(data)//2)+data+b'\0\0')
    put(out,8,first+len(names));put(out,0,len(out),'I')
    return out,first

def carinfo(b,cars):
    count=u16(b,4);shift=8*len(cars)
    out=bytearray(b[:8])+bytearray(8*(count+len(cars)))+bytearray(b[8+count*8:])
    put(out,4,count+len(cars))
    for i in range(count):
        cid,word=struct.unpack_from('<II',b,8+i*8)
        struct.pack_into('<II',out,8+i*8,cid,(word&~0x3ffff)+((word&0x3ffff)+shift))
    assert max(u32(b,8+8*i) for i in range(count))<pack(cars[0]['id'])
    for i,c in enumerate(cars):
        pos=len(out)
        assert pos<0x40000
        struct.pack_into('<II',out,8+8*(count+i),pack(c['id']),pos) # encoded paint count is count - 1
        out.extend(struct.pack('<HBB',0x4210,ord('0'),0))
        if len(out)%2: out.append(0x7f)
        out.extend(c['name'].encode('ascii')+b'\0')
        if len(out)%2: out.append(0)
    return out

def resample(points,limit=16):
    """Keep extrema and greedily retain the points with greatest interpolation error."""
    if len(points)<=limit:return points
    keep={0,len(points)-1,max(range(len(points)),key=lambda i:points[i][1]),
          max(range(len(points)),key=lambda i:points[i][0]*points[i][1])}
    while len(keep)<limit:
        errors=[]
        ordered=sorted(keep)
        for lo,hi in zip(ordered,ordered[1:]):
            x0,y0=points[lo];x1,y1=points[hi]
            for i in range(lo+1,hi):
                x,y=points[i];errors.append((abs(y-(y0+(y1-y0)*(x-x0)/(x1-x0))),i))
        keep.add(max(errors)[1])
    return [points[i] for i in sorted(keep)]

def normalize(catalog,physics,donors):
    tables={k:{r['id']:dict(zip(t['columns'],r['values'])) for r in t['rows']} for k,t in physics.items()}
    names={c['label']:c for c in catalog};cars=[]
    for row in sorted(physics['GENERIC_CAR']['rows'],key=lambda r:r['id']):
        g=tables['GENERIC_CAR'][row['id']];parts=tables['DEFAULT_PARTS'][int(g['Df_Tbl_Index'])]
        def part(t,k):return tables[t][int(parts[k])]
        ch=part('CHASSIS','Chassis');en=part('ENGINE','Engine');ge=part('GEAR','Gear');dt=part('DRIVETRAIN','DrvTrain');rm=part('RACINGMODIFY','R_Modify')
        record=names[row['label']]; adaptations=[]
        points=[(int(en['rpm'+chr(65+i)]),int(en['torque'+chr(65+i)])) for i in range(int(en['torquepoint']))]
        assert len(points)>=2 and all(a[0]<b[0] for a,b in zip(points,points[1:])),row['label']
        reduced=resample(points)
        if len(reduced)<len(points):adaptations.append('torque curve reduced to 16 samples, retaining torque and power peaks')
        # GT2 first multiplies each raw sample by 0x27334 in signed 32 bits.
        # Large GT4 engines (Tank Car) need an equivalent smaller sample and
        # larger multiplier to avoid overflow before the final wide division.
        torque_divisor=max(1,math.ceil(max(y for x,y in reduced)/13300))
        torque_multiplier=int(en['torquevol'])*torque_divisor
        assert 0<torque_multiplier<=255,row['label']
        if torque_divisor>1:adaptations.append('torque samples divided by %d and multiplier raised equally to avoid GT2 intermediate overflow'%torque_divisor)
        gears=[int(ge['Gear'+n]) for n in ['1st','2nd','3rd','4th','5th','6th','7th','8th','9th','10th','11th']][:int(ge['Nshift'])]
        auto_gears=not any(gears)
        if auto_gears:
            assert int(ge['maxspeedDF'])>0,row['label']
            adaptations.append('GT4 automatic ratio table: GT2 ratio generator uses GT4 default top-speed setting and final drive')
        else:
            while gears and gears[-1]==0:
                gears.pop();adaptations.append('trailing empty source gear omitted')
            assert gears and all(g>0 for g in gears),row['label']
        if len(gears)>7:
            gears=[gears[i] for i in [0,1,2,3,4,6,7]];adaptations.append('8 ratios adapted to 7; first and top ratios retained')
        if int(ge['geartype'])!=0:adaptations.append('GT4 transmission type %s represented by fixed GT2 ratios'%ge['geartype'])
        if 'electric' in en['aspiration'].lower() or len(gears)==1:adaptations.append('single-speed/electric powertrain uses GT2 torque-curve simulation')
        drive=int(dt['drivetype']);mass=int(ch['mass']);length=int(ch['length']);width=int(rm['chassisWidth']);height=int(ch['height'])
        assert 0<mass<32768 and 0<length<12000 and 0<width<5000 and 0<height<5000,row['label']
        def score(d):
            p=d['params'];b=p['body'];wanted=['rear','front','four_wheel','rear_type3','rear_type4'][drive]
            return (0 if p['drivetrain']['driveType']==wanted else 20)+abs(math.log(b['weightKg']/mass))+abs(b['wheelbase']-int(ch['wheelbase']))/2000+abs(b['height']-height)/2000
        donor=min(donors,key=score)
        # GT2 and GT4 both store these samples in 0.01 kgf.m. GT4's curve
        # multiplier remains separate; no advertised-PS rescaling is performed.
        price=int(g['Price'])
        if price<=0:
            curve_ps=max(x*100*y/100/716.2 for x,y in points)
            price=max(10000,round(curve_ps*2000/1000)*1000)
            adaptations.append('unpriced GT4 prize/concept: expansion price = max(10000, curve PS * 2000), rounded to 1000')
        c={'id':'z%04d'%row['id'],'specId':row['id'],'label':row['label'],'name':ascii_name(record['name'][0]),
           'code':record['models'][0]['code'],'maker':int(g['Maker']),'category':int(g['Category']),'year':int(g['Year']),'price':price,'sourcePrice':int(g['Price']),
           'donor':donor['carId'],'length':length,'width':width,'height':height,'mass':mass,'drive':drive,
           'chassis':ch,'engine':en,'gear':ge,'drivetrain':dt,'racingModify':rm,'brake':part('BRAKE','Brake'),
           'suspension':part('SUSPENSION','Susp'),'points':reduced,'gears':gears,'autoGears':auto_gears,'engineSampleDivisor':torque_divisor,'engineMultiplier100':torque_multiplier,'adaptations':adaptations}
        assert len(c['id'])==5 and len(set(x for x,y in reduced))==len(reduced)
        cars.append(c)
    return cars

def skeleton(b,c,donor):
    """Scale the private GT2 collision/wheel scaffold to the GT4 dimensions.

    The imported GT4 mesh is mandatory and supplies the visible body. The
    scaffold also supplies shadows, fallback procedural interior and wheels.
    """
    b=bytearray(b);d=donor['params']['body'];scale=[c['width']/d['width'],c['height']/d['height'],c['length']/(d['frontLength']+d['rearLength'])]
    def scaled(o,s):put(b,o,round(struct.unpack_from('<h',b,o)[0]*s),'h')
    for i in range(4):
        o=0x20+i*8
        scaled(o,scale[0]);scaled(o+2,scale[1]);scaled(o+4,int(c['chassis']['wheelbase'])/d['wheelbase']);scaled(o+6,scale[0])
    o=0x884
    for _ in range(u32(b,0x868)):
        counts=struct.unpack_from('<10H',b,o)
        for j in range(8):scaled(o+0x3c+j*2,scale[j%4] if j%4<3 else 1)
        for i in range(counts[0]):
            for axis in range(3):scaled(o+0x50+i*8+axis*2,scale[axis])
        o+=0x50+counts[0]*8+counts[1]*4+(counts[2]+counts[3])*16+(counts[6]+counts[7])*28
    if o+28<=len(b):
        for i in range(u16(b,o)):
            scaled(o+28+i*4,scale[0]);scaled(o+30+i*4,scale[2])
    return b

def make_tables(base,cars,donors,name_start,empty_string):
    tables=gtdt(base);original=[bytes(t) for t in tables]
    lookup={u32(tables[30],i):i for i in range(0,len(tables[30]),72)}
    for ci,c in enumerate(cars):
        cid=pack(c['id']);donor=donors[c['donor']];pos=lookup[pack(c['donor'])];catalogue=bytearray(tables[30][pos:pos+72])
        indices={t:u16(catalogue,4+2*i) for i,t in enumerate(SPEC_TABLES)}
        def row(t):
            r=bytearray(tables[t][indices[t]*SIZES[t]:(indices[t]+1)*SIZES[t]]);put(r,0,cid,'I');return r
        def append(t,r):
            indices[t]=len(tables[t])//SIZES[t];assert indices[t]<65536;tables[t].extend(r)
        ch=c['chassis'];en=c['engine'];ge=c['gear'];rm=c['racingModify']
        r=row(3);r[4]=int(ch['percentageF']);put(r,10,c['height'],'h');put(r,12,int(ch['wheelbase']),'h');put(r,14,c['mass'],'h');append(3,r)
        r=row(5);put(r,4,0,'I');put(r,8,cid,'I');r[12]=r[13]=100;r[14]=0;r[15]=int(rm['cd'])
        r[18]=int(rm['clDFF']);r[21]=int(rm['clDFR']);put(r,22,int(rm['chassisTreadF']),'h');put(r,24,int(rm['chassisThreadR']),'h');put(r,26,c['width'],'h');append(5,r)
        r=row(6);r[12:44]=bytes(32);r[59:75]=bytes(16)
        for j in range(3):put(r,4+j*2,empty_string+1+ci*4+j)
        displacement=re.match(r'^(\d+)cc$',en['discplacement'])
        put(r,44,min(8191,int(displacement[1])) if displacement else 0)
        peak=max(c['points'],key=lambda p:p[0]*p[1]);torque=max(c['points'],key=lambda p:p[1])
        put(r,46,round(peak[0]*peak[1]/716.2*int(en['torquevol'])/100));put(r,48,peak[0]*10)
        put(r,50,round(torque[1]/10*int(en['torquevol'])/100));put(r,52,empty_string+1+ci*4+3)
        for j,(rpm,torque) in enumerate(c['points']):put(r,12+2*j,round(torque/c['engineSampleDivisor']));r[59+j]=rpm
        r[54]=c['engineMultiplier100'];r[55]=min(255,int(en['clutchmeetrpm']));r[56]=min(100,max(30,c['points'][0][0]*10));r[57]=int(en['revlimit']);r[58]=min(int(en['shiftlimit']),max(1,r[57]-5));r[75]=len(c['points']);append(6,r)
        r=row(13);r[8]=c['drive'];r[7]=50 if c['drive']==2 else 255;r[9]=2 if c['drive']==2 else 254;append(13,r)
        r=row(17);put(r,4,0,'I');r[8]=0;r[9]=len(c['gears']);ratios=[int(ge['GearReverse'])]+c['gears'];ratios+= [-1]*(8-len(ratios))
        for j,v in enumerate(ratios):put(r,10+2*j,v,'h')
        put(r,26,int(ge['finalgearDF']),'h');r[32]=int(c['autoGears']);r[33]=int(ge['maxspeedDF']) if c['autoGears'] else 255;append(17,r)
        r=row(18);put(r,4,0,'I');r[8]=0;s=c['suspension']
        for o,key in [(11,'camberDFF'),(14,'camberDFR'),(21,'rideheightDFF'),(24,'rideheightMDFR'),(31,'springrateDFF'),(34,'springrateDFR')]:
            r[o]=max(0,min(255,int(s[key])))
        append(18,r)
        # GT4 and GT2 brake and inertia codes use different scales. Keep the
        # donor's calibrated GT2 coefficients, explicitly recorded in provenance.
        for t in [0,15,21,22,23]:
            r=row(t)
            if t!=23:put(r,4,0,'I');r[8]=0
            else:r[4]=0
            append(t,r)
        # Stock engine curve already includes factory aspiration. Do not apply
        # a GT2 donor's turbo/power/weight upgrades a second time.
        for t in [4,7,8,9,10,11,12,14,16,19,20,27,28]:indices[t]=0
        for j,t in enumerate(SPEC_TABLES):put(catalogue,4+2*j,indices[t])
        put(catalogue,0,cid,'I');put(catalogue,60,name_start+ci);put(catalogue,62,empty_string);catalogue[58]=c.get('gt2Maker',255);catalogue[64]=int(c['category'] in (1,101));catalogue[65]=c['year']%100;put(catalogue,68,c['price'],'I')
        tables[30].extend(catalogue)
    for i,before in enumerate(original):assert tables[i][:len(before)]==before,('original rows changed',i)
    return encode_gtdt(tables)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--assets',type=Path,required=True);p.add_argument('--catalog',type=Path,required=True);p.add_argument('--physics',type=Path,required=True)
    p.add_argument('--gt2-catalog',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--maker-names',type=Path,required=True,help='Reviewed private mapping: GT4 maker ID to {name,gt2Maker}')
    a=p.parse_args()
    if a.output.exists():p.error('choose a new output directory')
    donors={v['carId']:v for v in (json.loads(f.read_text(encoding='utf-8-sig',errors='replace')) for f in a.gt2_catalog.glob('*.json'))}
    cars=normalize(read_json(a.catalog),read_json(a.physics),list(donors.values()))
    makers=read_json(a.maker_names)
    for c in cars:c.update(makerName=makers[str(c['maker'])]['name'],gt2Maker=makers[str(c['maker'])].get('gt2Maker',255))
    out=a.output/'assets';out.mkdir(parents=True);(out/'carparam').mkdir();(out/'carobj').mkdir()
    base=a.assets
    # Region-local string indices differ; append identical new names after each
    # locale's existing strings and build its matching tables independently.
    for locale in ['usa','eng']:
        source=base/'carparam'/f'{locale}_unistrdb.dat.gz';names=[c['name'] for c in cars]+['']
        if not source.exists():continue
        for c in cars:
            names.extend(ascii_name(c['engine'][key]) for key in ['enginetype','cam','aspiration'])
            names.append(str(max(c['points'],key=lambda p:p[1])[0]*100)+' rpm')
        strings,start=wsdb(source.read_bytes(),names);(out/'carparam'/source.name).write_bytes(strings)
        # Arcade table 30 is an event table, not a car catalogue. Arcade's
        # all-cars/garage route already resolves GT-mode records separately.
        for kind in ['gtmode']:
            source=base/'carparam'/f'{locale}_{kind}_data.dat.gz'
            if source.exists():(out/'carparam'/source.name).write_bytes(make_tables(source.read_bytes(),cars,donors,start,start+len(cars)))
    for source in base.glob('.carinfo*'):(out/source.name).write_bytes(carinfo(source.read_bytes(),cars))
    colors=base.joinpath('.carcolor').read_bytes();count=(u16(colors,8)-8)//2
    latin=bytearray(base.joinpath('.cclatain').read_bytes());color_index=u16(latin,0)//2
    newlatin=bytearray(2*(color_index+1))+latin[2*color_index:]
    for i in range(color_index):put(newlatin,2*i,u16(latin,2*i)+2)
    put(newlatin,2*color_index,len(newlatin));newlatin.extend(b'Imported GT4 colour\0')
    colorout=bytearray(colors[:8])+bytearray(2*(count+len(cars)))+colors[8+2*count:]
    for i in range(count):put(colorout,8+2*i,u16(colors,8+2*i)+2*len(cars))
    for i in range(len(cars)):put(colorout,8+2*(count+i),len(colorout));colorout.extend(struct.pack('<H',color_index))
    (out/'.carcolor').write_bytes(colorout);(out/'.cclatain').write_bytes(newlatin)
    for c in cars:
        for ext in ['cdo','cdp']:
            source=base/'carobj'/(c['donor']+'.'+ext+'.gz')
            if not source.exists():source=source.with_suffix('')
            b=bytearray(source.read_bytes())
            if ext=='cdo':b=skeleton(b,c,donors[c['donor']])
            else:put(b,0,1);b[2]=ord('0')
            (out/'carobj'/(c['id']+'.'+ext)).write_bytes(b)
    write_json(out/'carparam/gt4_expansion.json',{'format':'gt2-car-expansion','version':1,'cars':cars,
        'adaptation':'GT2 integer physics; calibrated GT2 donor brake/inertia/tyre/differential codes, wheels, shadow and procedural cockpit scaffold; GT4 dimensions, mass, distribution, torque curve, ratios, drag and downforce. One imported body colour per car.'})
    hashes={p.relative_to(a.output).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(a.output.rglob('*')) if p.is_file()}
    write_json(a.output/'build.json',{'cars':len(cars),'files':hashes,'sourceSpecSha256':hashlib.sha256(a.physics.read_bytes()).hexdigest()})
    write_catalogue_version(a.output)
    print(f'{len(cars)} new cars generated at {a.output}')

if __name__=='__main__':main()
