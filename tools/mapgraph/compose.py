#!/usr/bin/env python3
"""Offline, bounded segment-map composer. See README.md for the v1 contract."""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import re
import tempfile

MAX_BYTES = 8 * 1024 * 1024
MAX_INSTANCES = 256
MAX_WORLD_COORD = 131072  # idlib/Lib.h; dmap drops a brush whose bounds pass it (ubrush.cpp BoundBrush)
IDENT = re.compile(r'[A-Za-z][A-Za-z0-9_]{0,47}\Z')
# Unknown epairs fail closed: additional game classes need an audited extension.
SAFE_KEYS = set('classname name model origin light_origin angle angles rotation _color light_radius light_center light_target light_right light_up light_start light_end texture skin shaderParm0 shaderParm1 shaderParm2 shaderParm3 shaderParm4 shaderParm5 shaderParm6 shaderParm7 noshadows nospecular parallel ambient falloff s_shader s_looping s_global s_volume wait speed damage health solid inline hide noTouch start_on bindOrientated bindToJoint bindToBody'.split())
REF_KEYS = {'bind', 'target', 'killtarget'}
# Map-only classes are known by their entityDef ("dmapOnly" "1"); a dmapOnly map key does nothing and is rejected.
KIT_KEYS = {
    'info_landmark': {'landmark', 'kind', 'entry', 'exit', 'radius', 'required', 'sealed_by'},
    'trigger_transition': {'landmark', 'cap', 'carry'},
    'trigger_changelevel': {'landmark', 'wait_party'},
    'info_landing': {'landmark', 'index'},
}
WORLD_RESET = ('never', 'timed', 'cleared')  # WorldStore.cpp reads worldspawn world_reset / world_reset_time
TOKEN = re.compile(r'\s+|//[^\n]*|/\*.*?\*/|"(?:[^"\\]|\\.)*"|[{}()]|[^\s{}()"]+', re.S)


class Invalid(ValueError):
    pass


def require(ok, message):
    if not ok:
        raise Invalid(message)


def keys(obj, allowed, required=()):
    require(isinstance(obj, dict), 'expected object')
    require(not (obj.keys() - set(allowed)), 'unsupported fields: ' + str(obj.keys() - set(allowed)))
    require(set(required) <= obj.keys(), 'missing fields: ' + str(set(required) - obj.keys()))


def ident(value):
    require(isinstance(value, str) and IDENT.fullmatch(value), 'invalid identifier: ' + str(value))
    return value


def integer(value, low, high):
    require(type(value) is int and low <= value <= high, 'integer out of bounds')
    return value


def vec(value, n=3):
    require(isinstance(value, list) and len(value) == n, 'invalid vector')
    require(all(type(x) in (float, int) and math.isfinite(x) and abs(x) <= 1048576 for x in value), 'invalid coordinate')
    return value


def fmt(x):
    require(math.isfinite(x), 'non-finite output')
    return '0' if abs(x) < 1e-12 else format(x, '.12g')


def quote(s):
    require(isinstance(s, str) and not any(c in s for c in '\n\r\\"'), 'unsupported string escape')
    return '"' + s + '"'


def dot(a, b):
    return sum(x*y for x, y in zip(a, b))


def basis(n):
    # Same basis as idMapBrushSide::GetTextureVectors / ComputeAxisBase, in closed form: with
    # rz = atan2(y, x), sin(rz) = y/hypot(x, y) and cos(rz) = x/hypot(x, y), and likewise for ry. libm's
    # sin/cos/atan2 may differ in the last bit between platforms; IEEE division and CPython's own hypot
    # do not, so the output bytes do not depend on the platform. atan2(0, 0) is 0: sin 0, cos 1.
    n = [0.0 if abs(x) < 1e-6 else x for x in n]
    h = math.hypot(n[0], n[1])
    sin_z, cos_z = (n[1]/h, n[0]/h) if h else (0.0, 1.0)
    r = math.hypot(h, n[2])
    sin_y, cos_y = -n[2]/r, h/r  # ry = -atan2(n[2], h)
    return [-sin_z, cos_z, 0], [-sin_y*cos_z, -sin_y*sin_z, -cos_y]


def brush_points(sides):
    # The engine's way to a brush's corners: each side's plane as a large square (idWinding::BaseForPlane),
    # clipped to the back of every other side (ClipInPlace). Planes read n.x + d = 0, inside is n.x + d <= 0.
    for plane, _, _ in sides:
        n, d = plane[:3], plane[3]
        a = [0.0, 0.0, 1.0] if abs(n[2]) < 0.9 else [1.0, 0.0, 0.0]
        u = [a[j] - n[j]*dot(a, n) for j in range(3)]
        length = math.sqrt(dot(u, u))
        u = [x/length for x in u]
        v = [n[1]*u[2] - n[2]*u[1], n[2]*u[0] - n[0]*u[2], n[0]*u[1] - n[1]*u[0]]
        w = [[-d*n[j] + (su*u[j] + sv*v[j])*4*1048576 for j in range(3)] for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
        for other, _, _ in sides:
            if other is plane or not w:
                continue
            dist = [dot(other[:3], q) + other[3] for q in w]
            clipped = []
            for k in range(len(w)):
                p, q, dp, dq = w[k-1], w[k], dist[k-1], dist[k]
                if (dp <= 0) != (dq <= 0):
                    t = dp/(dp - dq)
                    clipped.append([p[j] + t*(q[j] - p[j]) for j in range(3)])
                if dq <= 0:
                    clipped.append(q)
            w = clipped
        yield from w


def extent(entities):
    # World bounds of everything a segment places: brush corners (entity brush planes are local to the
    # entity origin), patch control points (absolute on disk), entity origins and light origins.
    lo, hi = [math.inf]*3, [-math.inf]*3

    def add(p):
        for j in range(3):
            lo[j], hi[j] = min(lo[j], p[j]), max(hi[j], p[j])
    for i, (ep, primitives) in enumerate(entities):
        origin = [float(x) for x in ep.get('origin', '0 0 0').split()] if i else [0.0, 0.0, 0.0]
        if i:
            add(origin)
            if 'light_origin' in ep:
                add([float(x) for x in ep['light_origin'].split()])
        for p in primitives:
            if p[0] == 'brushDef3':
                for q in brush_points(p[1]):
                    add([q[j] + origin[j] for j in range(3)])
            else:
                for column in p[3]:
                    for point in column:
                        add(point[:3])
    return lo, hi


class Parser:
    def __init__(self, raw):
        require(len(raw) <= MAX_BYTES, 'segment too large')
        self.ts = []
        pos = 0
        for m in TOKEN.finditer(raw):
            require(m.start() == pos, 'invalid token')
            pos = m.end()
            t = m.group()
            if not t.isspace() and not t.startswith(('//', '/*')):
                self.ts.append(t)
        require(pos == len(raw), 'invalid trailing input')
        self.i = 0

    def pop(self, expected=None):
        require(self.i < len(self.ts), 'unexpected EOF')
        t = self.ts[self.i]
        self.i += 1
        require(expected is None or t == expected, 'expected ' + str(expected) + ', got ' + t)
        return t

    def string(self):
        t = self.pop()
        require(t.startswith('"') and t.endswith('"') and '\\' not in t, 'expected unescaped quoted string')
        return t[1:-1]

    def matrix(self, n):
        self.pop('(')
        try:
            v = [float(self.pop()) for _ in range(n)]
        except ValueError as e:
            raise Invalid('invalid number') from e
        vec(v, n)
        self.pop(')')
        return v

    def parse(self):
        self.pop('Version')
        self.pop('2')
        entities = []
        while self.i < len(self.ts):
            self.pop('{')
            ep, primitives = {}, []
            while self.ts[self.i] != '}':
                if self.ts[self.i] != '{':
                    require(not primitives, 'entity epairs must precede primitives')
                    k, v = self.string(), self.string()
                    require(k not in ep, 'duplicate epair ' + k)
                    ep[k] = v
                    continue
                self.pop('{')
                kind = self.pop()
                require(kind in ('brushDef3', 'patchDef2', 'patchDef3'), 'unsupported primitive ' + kind)
                self.pop('{')
                if kind == 'brushDef3':
                    sides = []
                    while self.ts[self.i] != '}':
                        plane = self.matrix(4)
                        require(abs(dot(plane[:3], plane[:3])-1) < 1e-5, 'non-unit brush plane')
                        self.pop('(')
                        uv = [self.matrix(3), self.matrix(3)]
                        self.pop(')')
                        material = self.string()
                        for _ in range(3):
                            self.pop('0')
                        sides.append([plane, uv, material])
                    require(4 <= len(sides) <= 128, 'brush side count')
                    primitives.append([kind, sides])
                else:
                    material = self.string()
                    info = self.matrix(7 if kind == 'patchDef3' else 5)
                    w, h = info[:2]
                    require(all(x == 0 for x in info[-3:]), 'unsupported patch reserved metadata')
                    if kind == 'patchDef3':
                        require(all(x == int(x) and 1 <= x <= 64 for x in info[2:4]), 'invalid patch subdivisions')
                    require(w == int(w) and h == int(h) and 3 <= w <= 31 and 3 <= h <= 31 and w % 2 and h % 2, 'patch dimensions')
                    self.pop('(')
                    columns = []
                    for _ in range(int(w)):
                        self.pop('(')
                        columns.append([self.matrix(5) for _ in range(int(h))])
                        self.pop(')')
                    self.pop(')')
                    primitives.append([kind, material, info, columns])
                self.pop('}')
                self.pop('}')
            self.pop('}')
            entities.append([ep, primitives])
            require(len(entities) <= 4096, 'too many entities')
        require(entities and entities[0][0] == {'classname': 'worldspawn'}, 'first entity must be plain worldspawn')
        require(all(e[0].get('classname') not in (None, 'worldspawn') for e in entities[1:]), 'missing class or multiple worlds')
        return entities


def parse(raw):
    try:
        return Parser(raw).parse()
    except IndexError as e:
        raise Invalid('truncated map') from e


def transform(entities, name, translation):
    entities = copy.deepcopy(entities)
    # one lower-case set per name space: the engine compares entity names and landmark ids ignoring case
    names, seen = {}, set()
    for i, (ep, _) in enumerate(entities[1:], 1):
        local = ep.get('name', 'entity_' + str(i))
        ident(local)
        require(local.lower() not in seen, 'duplicate entity name')
        seen.add(local.lower())
        names[local] = name + '__' + local
        require(len(names[local]) <= 63, 'namespaced entity name exceeds 63 characters')
    landmarks, seen = {}, set()
    for i, (ep, _) in enumerate(entities[1:], 1):
        if ep.get('classname') == 'info_landmark':
            local = ident(ep.get('landmark', ep.get('name', 'entity_' + str(i))))
            require(local.lower() not in seen, 'duplicate landmark identity')
            seen.add(local.lower())
            landmarks[local] = name + '__' + local
            require(len(landmarks[local]) <= 63, 'namespaced landmark exceeds 63 characters')
    for i, (ep, primitives) in enumerate(entities):
        if i:
            require(all(k in SAFE_KEYS or k in KIT_KEYS.get(ep.get('classname'), set()) or k in REF_KEYS or re.fullmatch(r'target[0-9]+', k) for k in ep), 'unsupported entity key in ' + str(ep))
            require(not ep['classname'].startswith(('target_call', 'target_set', 'func_mover')), 'unsupported entity semantics')
            oldname = ep.get('name', 'entity_' + str(i))
            ep['name'] = names[oldname]
            if ep.get('model') == oldname:
                require(primitives, 'self-named model requires primitives')
                ep['model'] = names[oldname]
            elif primitives and 'model' in ep:
                raise Invalid('primitive entity model must reference its own name')
            elif 'model' in ep:
                require('/' in ep['model'] and Path(ep['model']).suffix.lower() in ('.ase', '.lwo', '.ma', '.obj', '.gltf', '.glb', '.md5mesh'), 'unsupported asset model or nonlocal model reference')
            for k in list(ep):
                if k in REF_KEYS or k == 'sealed_by' or re.fullmatch(r'target[0-9]+', k):
                    require(ep[k] in names, 'unresolved local reference ' + ep[k])
                    ep[k] = names[ep[k]]
            if ep['classname'] == 'info_landmark':
                local = ep.get('landmark', oldname)
                ep['landmark'] = landmarks[local]
            elif ep['classname'] in KIT_KEYS:
                require(ep.get('landmark') in landmarks, 'unresolved local landmark reference')
                ep['landmark'] = landmarks[ep['landmark']]
            try:
                origin = vec([float(x) for x in ep.get('origin', '0 0 0').split()])
            except ValueError as e:
                raise Invalid('invalid entity origin') from e
            ep['origin'] = ' '.join(fmt(a+b) for a, b in zip(origin, translation))
            if 'light_origin' in ep:
                light_origin = vec([float(x) for x in ep['light_origin'].split()])
                ep['light_origin'] = ' '.join(fmt(a+b) for a, b in zip(light_origin, translation))
        for p in primitives:
            if p[0] == 'brushDef3':
                for plane, uv, _ in p[1]:
                    if not i:
                        plane[3] -= dot(plane[:3], translation)
                    s, t = basis(plane[:3])
                    for row in uv:
                        vector = [row[0]*s[j] + row[1]*t[j] for j in range(3)]
                        row[2] -= dot(vector, translation)
            else:
                for column in p[3]:
                    for point in column:
                        point[:3] = [a+b for a, b in zip(point[:3], translation)]
    return entities


def render(entities):
    def matrix(v):
        return '( ' + ' '.join(fmt(x) for x in v) + ' )'
    out = ['Version 2']
    for ep, primitives in entities:
        out.append('{')
        # Epairs keep source order, before the primitives: idDict keeps the order it reads, which is
        # the order target, target1, target2 fire in, and the engine parser captures origin as it reads.
        out += [quote(k) + ' ' + quote(v) for k, v in ep.items()]
        for p in primitives:
            out += ['{', p[0], '{']
            if p[0] == 'brushDef3':
                for plane, uv, material in p[1]:
                    out.append(matrix(plane) + ' ( ' + ' '.join(matrix(r) for r in uv) + ' ) ' + quote(material) + ' 0 0 0')
            else:
                out += [quote(p[1]), matrix(p[2]), '(']
                for column in p[3]:
                    out.append('( ' + ' '.join(matrix(v) for v in column) + ' )')
                out.append(')')
            out += ['}', '}']
        out.append('}')
    return ('\n'.join(out)+'\n').encode()


def unique_object(pairs):
    result = {}
    for k, v in pairs:
        require(k not in result, 'duplicate JSON key ' + k)
        result[k] = v
    return result


def canonical(value):
    return (json.dumps(value, sort_keys=True, indent=2)+'\n').encode()


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def compose(recipe_path, seed, flags=(), triggers=()):
    integer(seed, 0, 2**64-1)
    recipe_path = Path(recipe_path).resolve()
    require(recipe_path.stat().st_size <= MAX_BYTES, 'recipe too large')
    raw = recipe_path.read_bytes()
    require(len(raw) <= MAX_BYTES, 'recipe too large')
    recipe = json.loads(raw, object_pairs_hook=unique_object)
    keys(recipe, ('version', 'name', 'segments', 'nodes', 'links', 'flags', 'triggers', 'world'), ('version', 'name', 'segments', 'nodes', 'links'))
    require(type(recipe['version']) is int and recipe['version'] == 1, 'unsupported recipe version')
    require(isinstance(recipe['name'], str) and len(recipe['name']) <= 31 and re.fullmatch(r'[A-Za-z0-9_]+(?:/[A-Za-z0-9_]+)*', recipe['name']) and not recipe['name'].startswith('maps/'), 'invalid canonical map name (max 31)')
    require(isinstance(recipe['segments'], dict) and 1 <= len(recipe['segments']) <= 256, 'segment count')
    require(isinstance(recipe['nodes'], list) and 1 <= len(recipe['nodes']) <= MAX_INSTANCES, 'node count')
    # flag and trigger names are declared in the recipe, so a misspelt `when` entry or input fails
    declared = {}
    for field in ('flags', 'triggers'):
        names = recipe.get(field, [])
        require(isinstance(names, list) and len(names) <= 256 and all(isinstance(v, str) for v in names), 'invalid ' + field + ' declaration')
        declared[field] = {ident(v) for v in names}
        require(len(declared[field]) == len(names), 'duplicate name in ' + field)
    flags, triggers = sorted(set(flags)), sorted(set(triggers))
    for field, inputs in (('flags', flags), ('triggers', triggers)):
        for v in inputs:
            require(ident(v) in declared[field], 'undeclared ' + field[:-1] + ' ' + v)
    worldspawn = {'classname': 'worldspawn'}
    world = recipe.get('world', {})
    keys(world, ('world_reset', 'world_reset_time'))
    if 'world_reset' in world:
        require(world['world_reset'] in WORLD_RESET, 'world_reset must be one of ' + ', '.join(WORLD_RESET))
        worldspawn['world_reset'] = world['world_reset']
    if 'world_reset_time' in world:
        worldspawn['world_reset_time'] = str(integer(world['world_reset_time'], 0, 2**31-1))
    segments, hashes = {}, {}
    for sid, entry in sorted(recipe['segments'].items()):
        ident(sid)
        keys(entry, ('map', 'sockets'), ('map', 'sockets'))
        require(isinstance(entry['map'], str), 'map path must be string')
        path = (recipe_path.parent / entry['map']).resolve()
        require(path.is_relative_to(recipe_path.parent) and path.suffix == '.map', 'map must be inside recipe directory')
        require(not path.with_suffix('.script').exists(), 'segment scripts unsupported')
        require(path.stat().st_size <= MAX_BYTES, 'segment too large')
        data = path.read_bytes()
        ents = parse(data.decode('utf-8'))
        checked = transform(ents, 'validate', [0, 0, 0])  # Audit every segment, including unselected choices.
        sockets = entry['sockets']
        require(isinstance(sockets, dict) and len(sockets) <= 32, 'socket count')
        for socket, s in sockets.items():
            ident(socket)
            keys(s, ('center', 'normal', 'size', 'kind', 'cap'), ('center', 'normal', 'size', 'kind'))
            ident(s['kind'])
            vec(s['center']); vec(s['normal']); vec(s['size'])
            n, size = s['normal'], s['size']
            require(sum(abs(x) for x in n) == 1 and all(x in (-1, 0, 1) for x in n), 'socket normal must be axial')
            require(all((size[j] == 0 if n[j] else size[j] > 0) for j in range(3)), 'socket size must span plane')
        segments[sid] = (ents, sockets, extent(checked))
        hashes[sid] = {'map': entry['map'], 'sha256': digest(data)}
    instances, choices, sockets = {}, [], {}
    for node in recipe['nodes']:
        keys(node, ('id', 'translation', 'choices'), ('id', 'translation', 'choices'))
        name = ident(node['id'])
        require(name not in instances, 'duplicate node id')
        tr = vec(node['translation'])
        require(all(type(x) is int for x in tr), 'translation must be integer')
        require(isinstance(node['choices'], list) and 1 <= len(node['choices']) <= 256, 'choice count')
        eligible = []
        for c in node['choices']:
            keys(c, ('segment', 'weight', 'when'), ('segment', 'weight'))
            require(isinstance(c['segment'], str) and c['segment'] in segments, 'unknown segment')
            integer(c['weight'], 1, 2**31-1)
            when = c.get('when', {})
            keys(when, ('flags_all', 'flags_none', 'triggers_all', 'triggers_none'))
            for condition, vals in when.items():
                require(isinstance(vals, list) and len(vals) <= 256, 'invalid condition')
                for val in vals:
                    require(ident(val) in declared['flags' if condition.startswith('flags') else 'triggers'], 'undeclared name in ' + condition + ': ' + val)
            if all((set(vals) <= set(flags if k.startswith('flags') else triggers)) if k.endswith('all') else not (set(vals) & set(flags if k.startswith('flags') else triggers)) for k, vals in when.items()):
                eligible.append(c)
        require(eligible, 'no eligible choice for ' + name)
        total = sum(c['weight'] for c in eligible)
        # Rejection sampling avoids modulo bias; independent per-node streams.
        counter = 0
        limit = (2**256 // total)*total
        while True:
            draw = int.from_bytes(hashlib.sha256(canonical([1, seed, name, counter])).digest(), 'big')
            if draw < limit:
                break
            counter += 1
        draw %= total
        chosen = None
        for c in eligible:
            if draw < c['weight']:
                chosen = c['segment']; break
            draw -= c['weight']
        instances[name] = (chosen, tr)
        choices.append({'id': name, 'segment': chosen, 'translation': tr})
        for socket, s in segments[chosen][1].items():
            sockets[name + '.' + socket] = (name, s, [a+b for a, b in zip(s['center'], tr)])
    require(isinstance(recipe['links'], list) and len(recipe['links']) <= 4096, 'link count')
    used = set()
    for link in recipe['links']:
        require(isinstance(link, list) and len(link) == 2, 'link must have two endpoints')
        a, b = link
        require(isinstance(a, str) and isinstance(b, str) and a in sockets and b in sockets and a != b and a not in used and b not in used, 'invalid/reused socket')
        na, sa, ca = sockets[a]; nb, sb, cb = sockets[b]
        require(na != nb and sa['kind'] == sb['kind'] and ca == cb and sa['size'] == sb['size'] and sa['normal'] == [-x for x in sb['normal']], 'socket incongruence')
        used.update(link)
    caps = []
    for endpoint, (name, s, center) in sorted(sockets.items()):
        if endpoint in used:
            continue
        cap = s.get('cap')
        require(isinstance(cap, str) and cap in segments, 'unclosed socket ' + endpoint)
        capents, capsockets, _ = segments[cap]
        require(not capsockets and len(capents) == 1, 'cap must be world geometry without sockets/entities')
        # Require an axial solid spanning the full doorway, with positive depth.
        covered = False
        for p in capents[0][1]:
            if p[0] != 'brushDef3' or len(p[1]) != 6:
                continue
            lo, hi = [-math.inf]*3, [math.inf]*3
            valid = True
            for plane, _, _ in p[1]:
                axes = [j for j in range(3) if plane[j] != 0]
                if len(axes) != 1 or abs(plane[axes[0]]) != 1:
                    valid = False; break
                j = axes[0]
                if plane[j] > 0: hi[j] = min(hi[j], -plane[3])
                else: lo[j] = max(lo[j], plane[3])
            if valid and all(math.isfinite(lo[j]) and math.isfinite(hi[j]) and lo[j] < hi[j] and lo[j] <= s['center'][j]-s['size'][j]/2 and hi[j] >= s['center'][j]+s['size'][j]/2 for j in range(3)):
                covered = True
        require(covered, 'cap does not cover socket ' + endpoint)
        caps.append({'id': name + '__cap_' + endpoint.split('.')[1], 'segment': cap, 'translation': instances[name][1]})
    output = [[worldspawn, []]]
    for instance in choices + caps:
        lo, hi = segments[instance['segment']][2]
        tr = instance['translation']
        require(all(lo[j] + tr[j] >= -MAX_WORLD_COORD and hi[j] + tr[j] <= MAX_WORLD_COORD for j in range(3)), 'instance ' + instance['id'] + ' exceeds MAX_WORLD_COORD (' + str(MAX_WORLD_COORD) + ')')
        transformed = transform(segments[instance['segment']][0], instance['id'], tr)
        output[0][1].extend(transformed[0][1])
        output.extend(transformed[1:])
    require(len(output) <= 8192 and sum(len(e[1]) for e in output) <= 65536, 'composed map exceeds limits')
    final_names = [e[0]['name'].lower() for e in output[1:]]
    require(len(final_names) == len(set(final_names)), 'global entity name collision')
    landmark_ids = [e[0]['landmark'] for e in output[1:] if e[0]['classname'] == 'info_landmark']
    require(len(landmark_ids) == len({x.lower() for x in landmark_ids}), 'global landmark identity collision')
    mapbytes = render(output)
    manifest = {'version': 1, 'map_name': recipe['name'], 'algorithm': 'sha256-counter-rejection-v1', 'seed': seed, 'flags': flags, 'triggers': triggers, 'recipe_sha256': digest(raw), 'segments': hashes, 'instances': choices, 'caps': caps, 'map_sha256': digest(mapbytes), 'entity_names': [e[0]['name'] for e in output[1:]], 'landmark_ids': landmark_ids}
    return mapbytes, canonical(manifest)


def write_new(path, data):
    # Beside the target, then renamed over it: a reader or a failed run never leaves a partial file.
    fd, temp = tempfile.mkstemp(dir=path.parent, prefix='.' + path.name + '.', suffix='.tmp')
    try:
        with os.fdopen(fd, 'wb') as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        return temp
    except BaseException:
        os.unlink(temp)
        raise


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('recipe', type=Path)
    ap.add_argument('--seed', type=int, required=True)
    ap.add_argument('--flag', action='append', default=[])
    ap.add_argument('--trigger', action='append', default=[])
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    try:
        data, manifest = compose(args.recipe, args.seed, args.flag, args.trigger)
        output = args.output.resolve()
        require(output.suffix == '.map', 'output must be .map')
        manifest_path = output.with_suffix('.manifest.json')
        recipe = args.recipe.resolve()
        inputs = {recipe} | {(recipe.parent / s['map']).resolve() for s in json.loads(manifest)['segments'].values()}
        require(output not in inputs and manifest_path not in inputs, 'output would replace a recipe input')
        output.parent.mkdir(parents=True, exist_ok=True)
        temps = []
        try:
            temps.append(write_new(output, data))
            temps.append(write_new(manifest_path, manifest))
            # each file is replaced whole; a failure between the two renames leaves a manifest whose
            # map_sha256 no longer matches the map, which a consumer can see
            os.replace(temps[0], output)
            os.replace(temps[1], manifest_path)
        finally:
            for temp in temps:
                if os.path.exists(temp):
                    os.unlink(temp)
    except (Invalid, ValueError, OSError) as e:
        ap.exit(2, 'compose: ' + str(e) + '\n')


if __name__ == '__main__':
    main()
