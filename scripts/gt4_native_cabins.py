"""Apply hash-bound authored cabin selections without changing the race shell."""
import copy
from gt4_native_instances import resolve_draws

def split_exterior_glazing(draws, selection):
    """Keep the full exterior, excluding only reviewed canopy triangles inside."""
    rules={(r['shape'],r['packet']):set(r['triangles']) for r in selection}
    seen=set();result=[]
    for draw,mask in draws:
        key=draw['shape'],draw['packet']
        if key not in rules:result.append((draw,mask));continue
        seen.add(key);chosen=rules[key];vertices=draw['vertices'];count=len(vertices)//3
        if len(vertices)%3 or not chosen or min(chosen)<0 or max(chosen)>=count:
            raise ValueError('Glazing triangle selection no longer matches source')
        for exterior in (False,True):
            part=copy.deepcopy(draw);part['vertices']=[]
            for triangle in range(count):
                if (triangle in chosen)==exterior:part['vertices'].extend(vertices[triangle*3:triangle*3+3])
            if part['vertices']:
                if exterior:part['exteriorOnly']=True
                result.append((part,mask))
    if seen!=set(rules):raise ValueError('Glazing packet selection no longer matches source')
    return result

def cabin_draws(meta, selection):
    selected=[]
    rules={(r['shape'],r['packet']):r for r in selection['packets']}
    seen=set()
    for draw,mask in resolve_draws(meta['states']):
        key=draw['shape'],draw['packet']
        if key not in rules:continue
        draw=copy.deepcopy(draw);rule=rules[key];seen.add(key)
        if 'triangles' in rule:
            vertices=draw['vertices'];draw['vertices']=[]
            for index in rule['triangles']:
                if index<0 or index*3+3>len(vertices):raise ValueError('Cabin triangle selection out of range')
                draw['vertices'].extend(vertices[index*3:index*3+3])
        selected.append((draw,mask))
    if seen!=set(rules):raise ValueError('Cabin packet selection no longer matches source')
    return selected

def merge_cabin(body,donor,selection,copy_texture):
    """Disjoint texture/material namespaces retain base paint and donor furniture."""
    result=copy.deepcopy(body)
    offset=len(body['materials'][0]);texture_offset=1+max((d['texset'] for s in body['states'] for d in s),default=0)
    for state in result['states']:
        for draw in state:
            draw['exteriorOnly']=[draw['shape'],draw['packet']] in selection.get('exteriorOnlyPackets',[])
    for index,materials in enumerate(result['materials']):materials.extend(copy.deepcopy(donor['materials'][min(index,len(donor['materials'])-1)]))
    for materials in result['paintMaterials']:materials.extend(copy.deepcopy(donor['materials'][0]))
    copied=set()
    for draw,mask in cabin_draws(donor,selection):
        if draw['material']>=0:draw['material']+=offset
        if draw['texture']>=0:
            old=draw['texset'],draw['texture'];draw['texset']+=texture_offset
            if old not in copied:copy_texture(old,(draw['texset'],draw['texture']));copied.add(old)
        draw['interiorOnly']=True
        for state in range(len(result['states'])):
            if mask&(1<<state):result['states'][state].append(copy.deepcopy(draw))
    return result
