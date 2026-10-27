"""Resolve transformed CAR4 geometry and preserve each submitted draw instance."""

def geometry_key(draw):
    return tuple(draw[k] for k in ('shape','packet','material','texset','texture','wheel','spinning'))+(tuple(draw['matrix']),)

def draw_key(draw):
    return geometry_key(draw)+tuple(draw.get(k) for k in ('type','blend','blendEquation','alphaFunc','alphaRef','alphaFail','cull','depthWrite','textureAlpha','textureFunction'))+(tuple(draw.get('address',[])),)

def resolve_draws(states):
    geometry={};draws={}
    for state_index,state in enumerate(states):
        occurrences={}
        for source in state:
            draw=dict(source);g=geometry_key(draw)
            if draw['vertices']:geometry[g]=draw['vertices']
            elif g not in geometry:raise ValueError('Missing transformed geometry instance')
            else:draw['vertices']=geometry[g]
            identity=draw_key(draw);ordinal=occurrences.get(identity,0);occurrences[identity]=ordinal+1
            key=identity,ordinal
            if key not in draws:draws[key]=[draw,0]
            draws[key][1]|=1<<state_index
    return list(draws.values())
