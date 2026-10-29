#!/usr/bin/env python3
"""Run with PYTHONDONTWRITEBYTECODE=1; scratch goes to the system temporary directory."""
import copy
import json
import math
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest
import compose as c

HERE = Path(__file__).resolve().parent


class ComposerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)
        for p in (HERE/'fixtures').iterdir():
            (self.directory/p.name).write_bytes(p.read_bytes())
        self.recipe = json.loads((self.directory/'kit.json').read_text())

    def compose(self, seed=0, flags=(), triggers=()):
        p = self.directory/'kit.json'
        p.write_bytes(c.canonical(self.recipe))
        return c.compose(p, seed, flags, triggers)

    def test_1000_seeds_byte_repeatability(self):
        seen = set()
        for seed in range(1000):
            a = self.compose(seed, ['extra_start'])
            b = self.compose(seed, ['extra_start'])
            self.assertEqual(a, b)
            manifest = json.loads(a[1])
            self.assertEqual(manifest['map_sha256'], c.digest(a[0]))
            seen.add(manifest['instances'][1]['segment'])
        self.assertEqual(seen, {'room_a', 'room_b'})

    def test_conditions_and_caps(self):
        for seed in range(10):
            _, manifest = self.compose(seed, ['extra_start'], ['boss_dead'])
            m = json.loads(manifest)
            self.assertEqual(m['instances'][1]['segment'], 'room_b')
            self.assertEqual(len(m['caps']), 2)
        self.recipe['nodes'] = self.recipe['nodes'][:1]
        self.recipe['links'] = []
        self.assertEqual(len(json.loads(self.compose()[1])['caps']), 2)

    def test_geometry_and_texture_attachment(self):
        brush = c.parse((HERE/'fixtures/room_a.map').read_text())[0][1][0]
        patch = ['patchDef2', 'textures/base_wall/lfwall1', [3,3,0,0,0], [[[x,y,16,x/2,y/2] for y in range(3)] for x in range(3)]]
        entities = [[{'classname':'worldspawn'},[copy.deepcopy(brush),copy.deepcopy(patch)]],
                    [{'classname':'func_static','name':'solid','model':'solid','origin':'10 20 30'},[copy.deepcopy(brush),copy.deepcopy(patch)]],
                    [{'classname':'light','name':'lamp','origin':'1 2 3','light_origin':'4 5 6','light_target':'0 0 -16','target':'solid'},[]],
                    [{'classname':'func_static','name':'mesh','model':'models/test.ase','origin':'7 8 9','rotation':'1 0 0 0 1 0 0 0 1'},[]]]
        tr=[256,-512,64]
        moved=c.parse(c.render(c.transform(entities,'node',tr)).decode())
        for i in (0,1):
            oldorg=[0,0,0] if not i else [10,20,30]
            neworg=[0,0,0] if not i else [a+b for a,b in zip(oldorg,tr)]
            for old,new in zip(entities[i][1][0][1],moved[i][1][0][1]):
                plane, uv, _=old; np, nuv, _=new
                self.assertEqual(np[:3],plane[:3])
                self.assertEqual(np[3],plane[3]-(c.dot(plane[:3],tr) if not i else 0))
                # Pick a point on each plane, then simulate engine local xyz + origin UV term.
                point=[-plane[3]*x for x in plane[:3]]
                newpoint=[a+b for a,b in zip(point,tr)] if not i else point
                s,t=c.basis(plane[:3])
                for r,nr in zip(uv,nuv):
                    v=[r[0]*s[j]+r[1]*t[j] for j in range(3)]
                    before=c.dot(v,point)+r[2]+c.dot(v,oldorg)
                    after=c.dot(v,newpoint)+nr[2]+c.dot(v,neworg)
                    self.assertAlmostEqual(before,after,9)
            for oldcol,newcol in zip(entities[i][1][1][3],moved[i][1][1][3]):
                for old,new in zip(oldcol,newcol):
                    self.assertEqual(new[:3],[a+b for a,b in zip(old[:3],tr)])
                    self.assertEqual(new[3:],old[3:])
                    # Engine patch Parse subtracts entity origin from disk xyz.
                    self.assertEqual([new[j]-neworg[j] for j in range(3)], [old[j]-oldorg[j]+(tr[j] if not i else 0) for j in range(3)])
        self.assertEqual(moved[1][0]['model'],'node__solid')
        self.assertEqual(moved[2][0]['target'],'node__solid')
        self.assertEqual(moved[2][0]['light_origin'],'260 -507 70')
        self.assertEqual(moved[2][0]['light_target'],'0 0 -16')
        self.assertEqual(moved[3][0]['model'],'models/test.ase')
        self.assertEqual(moved[3][0]['origin'],'263 -504 73')
        patch[0]='patchDef3'; patch[2]=[3,3,2,2,0,0,0]
        self.assertEqual(c.parse(c.render([[{'classname':'worldspawn'},[patch]]]).decode())[0][1][0][0],'patchDef3')

    def test_negative_recipe_validation(self):
        original=copy.deepcopy(self.recipe)
        mutations=[lambda r:r['nodes'][0].update(rotation=90),
                   lambda r:r['nodes'][0].update(translation=[0.5,0,0]),
                   lambda r:r['nodes'][1].update(translation=[257,0,0]),
                   lambda r:r['links'].append(r['links'][0]),
                   lambda r:r['segments']['room_a']['sockets']['west'].pop('cap'),
                   lambda r:r['segments']['room_a']['sockets']['west'].update(cap='cap_east'),
                   lambda r:r['nodes'][0]['choices'][0].update(weight=0),
                   lambda r:r['nodes'][0]['choices'][0].update(when={'flags_all':['missing']}),
                   lambda r:r.update(name='../escape'),
                   lambda r:r.update(name='x'*32),
                   lambda r:r['segments']['room_a'].update(map='../escape.map'),
                   lambda r:r['nodes'][1].update(id='entry'),
                   lambda r:r['segments']['room_b']['sockets']['west'].update(kind='wrong'),
                   lambda r:r['links'][0].__setitem__(0, []),
                   lambda r:r['nodes'][0]['choices'][0].update(segment=[])]
        for mutation in mutations:
            self.recipe=copy.deepcopy(original); mutation(self.recipe)
            with self.assertRaises(c.Invalid):self.compose()

    def test_namespace_collision_and_json_duplicates(self):
        a=c.transform([[{'classname':'worldspawn'},[]],[{'classname':'light','name':'b__c'},[]]],'a',[0,0,0])
        b=c.transform([[{'classname':'worldspawn'},[]],[{'classname':'light','name':'c'},[]]],'a__b',[0,0,0])
        # Full compose rejects the ambiguity in concatenated names.
        for sid, ents in [('room_a',a),('room_b',b)]:
            ents[1][0]['name']='b__c' if sid=='room_a' else 'c'
            (self.directory/(sid+'.map')).write_bytes(c.render(ents))
        self.recipe['nodes'][0]['id']='a'
        self.recipe['nodes'][1]['id']='a__b'
        self.recipe['links']=[['a.east','a__b.west']]
        with self.assertRaises(c.Invalid):self.compose()
        with self.assertRaises(c.Invalid):json.loads('{"x":1,"x":2}',object_pairs_hook=c.unique_object)

    def test_authored_detail_kit(self):
        data,manifest=c.compose(HERE/'fixtures/compile_c1.json',1)
        entities=c.parse(data.decode()); m=json.loads(manifest)
        self.assertEqual(m['landmark_ids'],['leaf__outside'])
        byname={e[0].get('name'):e for e in entities}
        self.assertEqual(byname['leaf__leave'][0]['landmark'],'leaf__outside')
        self.assertEqual(byname['leaf__carry'][0]['landmark'],'leaf__outside')
        self.assertEqual(byname['leaf__switch'][0]['target'],'leaf__lamp')
        self.assertEqual(byname['leaf__plinth'][0]['model'],'leaf__plinth')
        self.assertEqual(byname['leaf__panel'][1][0][0],'patchDef3')
        self.assertEqual(byname['leaf__cannister'][0]['origin'],'-576 584 136')
        e=c.parse((HERE/'fixtures/room_detail.map').read_text())
        marker=next(ep for ep,_ in e if ep.get('classname')=='info_landmark')
        marker['sealed_by']='plinth'
        transformed=c.transform(e,'node',[0,0,0])
        self.assertEqual(next(ep for ep,_ in transformed if ep.get('classname')=='info_landmark')['sealed_by'],'node__plinth')
        marker['link']='external:outside'
        with self.assertRaises(c.Invalid):c.transform(e,'node',[0,0,0])
        marker.pop('link'); marker['sealed_by']='missing'
        with self.assertRaises(c.Invalid):c.transform(e,'node',[0,0,0])

    def test_negative_map_validation(self):
        raw=(HERE/'fixtures/room_a.map').read_text()
        for bad in [raw.replace('Version 2','Version 220'), raw.replace('brushDef3','brushDef'),raw[:-3]]:
            with self.assertRaises(c.Invalid):c.parse(bad)
        for ep in [{'classname':'light','name':'x','scriptobject':'Bad'},
                   {'classname':'light','name':'x','target':'outside'},
                   {'classname':'light','name':'x','velocity':'1 2 3'},
                   {'classname':'light','name':'x'*48}]:
            with self.assertRaises(c.Invalid):
                c.transform([[{'classname':'worldspawn'},[]],[ep,[]]],'n'*48,[0,0,0])
        patch=['patchDef3','textures/base_wall/lfwall1',[3,3,0,2,0,0,0],[[[x,y,0,0,0] for y in range(3)] for x in range(3)]]
        with self.assertRaises(c.Invalid):c.parse(c.render([[{'classname':'worldspawn'},[patch]]]).decode())
        patch[2]=[3,3,2,2,0,0,1]
        with self.assertRaises(c.Invalid):c.parse(c.render([[{'classname':'worldspawn'},[patch]]]).decode())
        (self.directory/'room_a.script').write_text('void main() {}')
        with self.assertRaises(c.Invalid):self.compose()

    def test_epairs_keep_source_order(self):
        # idDict keeps the order it reads, and targets fire in that order: no sorting
        relay = {'classname': 'trigger_relay', 'name': 'relay', 'target2': 'b', 'target': 'a', 'target10': 'c', 'target1': 'd'}
        entities = [[{'classname': 'worldspawn'}, []], [relay, []]] + [[{'classname': 'light', 'name': x}, []] for x in 'abcd']
        out = c.parse(c.render(c.transform(entities, 'n', [0, 0, 0])).decode())
        self.assertEqual(list(out[1][0].items()), [('classname', 'trigger_relay'), ('name', 'n__relay'), ('target2', 'n__b'), ('target', 'n__a'),
                                                   ('target10', 'n__c'), ('target1', 'n__d'), ('origin', '0 0 0')])

    def test_closed_form_basis(self):
        def reference(n):  # the atan2 form of ComputeAxisBase
            n = [0.0 if abs(x) < 1e-6 else x for x in n]
            ry = -math.atan2(n[2], math.hypot(n[0], n[1]))
            rz = math.atan2(n[1], n[0])
            return [-math.sin(rz), math.cos(rz), 0], [-math.sin(ry)*math.cos(rz), -math.sin(ry)*math.sin(rz), -math.cos(ry)]
        axial = [[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]]
        rng = random.Random(7)
        normals = axial + [[1e-7, 0, -1], [0.6, 0, 0.8], [0, -0.8, 0.6]]
        for _ in range(500):
            v = [rng.uniform(-1, 1) for _ in range(3)]
            length = math.sqrt(c.dot(v, v))
            normals.append([x/length for x in v])
        for n in normals:
            for a, b in zip(c.basis(n), reference(n)):
                for x, y in zip(a, b):
                    self.assertAlmostEqual(x, y, places=14)
        # exact where libm leaves cos(pi/2) = 6.1e-17
        for n in axial:
            self.assertTrue(all(x in (-1, 0, 1) for v in c.basis(n) for x in v))

    def test_declared_flags_and_triggers(self):
        self.compose(0, ['extra_start'], ['boss_dead'])
        with self.assertRaisesRegex(c.Invalid, 'undeclared flag'):self.compose(0, ['extra_strat'])
        with self.assertRaisesRegex(c.Invalid, 'undeclared trigger'):self.compose(0, [], ['boss_died'])
        with self.assertRaisesRegex(c.Invalid, 'undeclared flag'):self.compose(0, ['boss_dead'])  # a trigger is not a flag
        original = copy.deepcopy(self.recipe)
        mutations = [lambda r: r['nodes'][1]['choices'][1]['when'].update(flags_none=['extra_strat']),
                     lambda r: r['nodes'][1]['choices'][1]['when'].update(triggers_all=['extra_start']),
                     lambda r: r.update(flags=['extra_start', 'extra_start']),
                     lambda r: r.update(flags='extra_start'),
                     lambda r: r.update(triggers=[1]),
                     lambda r: r.pop('triggers')]
        for mutation in mutations:
            self.recipe = copy.deepcopy(original); mutation(self.recipe)
            with self.assertRaisesRegex(c.Invalid, 'undeclared|declaration|duplicate'):self.compose(0, ['extra_start'])

    def test_world_keys(self):
        self.recipe['world'] = {'world_reset': 'timed', 'world_reset_time': 600}
        self.assertTrue(self.compose()[0].startswith(b'Version 2\n{\n"classname" "worldspawn"\n"world_reset" "timed"\n"world_reset_time" "600"\n{\n'))
        for world, reason in [({'world_reset': 'sometimes'}, 'world_reset'), ({'world_reset_time': -1}, 'integer'), ({'world_reset_time': '600'}, 'integer'),
                              ({'gravity': '0'}, 'unsupported'), ([], 'expected object')]:
            self.recipe['world'] = world
            with self.assertRaisesRegex(c.Invalid, reason):self.compose()

    def test_output_bounds(self):
        self.recipe['nodes'] = self.recipe['nodes'][:1]
        self.recipe['links'] = []
        self.assertEqual(c.extent(c.parse((self.directory/'room_a.map').read_text())), ([-16, -144, -16], [272, 144, 144]))
        # room_a spans x -16..272, y -144..144, z -16..144; a brush may touch the limit, not pass it
        for tr, ok in [([c.MAX_WORLD_COORD-272, 0, 0], True), ([c.MAX_WORLD_COORD-271, 0, 0], False),
                       ([0, 144-c.MAX_WORLD_COORD, 16-c.MAX_WORLD_COORD], True), ([0, 0, 15-c.MAX_WORLD_COORD], False)]:
            self.recipe['nodes'][0]['translation'] = tr
            if ok:
                self.compose()
            else:
                with self.assertRaisesRegex(c.Invalid, 'MAX_WORLD_COORD'):self.compose()
        # a sloped side (x + z <= 64), an entity brush (local to its origin) and an absolute patch
        r = math.sqrt(0.5)
        wedge = ['brushDef3', [[n + [d], [[1, 0, 0], [0, 1, 0]], 'textures/common/nodraw'] for n, d in
                               [([-1, 0, 0], 0), ([0, -1, 0], 0), ([0, 0, -1], 0), ([0, 1, 0], -64), ([r, 0, r], -64*r)]]]
        lo, hi = c.extent([[{'classname': 'worldspawn'}, [wedge]]])
        for a, b in zip(lo + hi, [0, 0, 0, 64, 64, 64]):
            self.assertAlmostEqual(a, b, 9)
        patch = ['patchDef2', 'textures/base_wall/lfwall1', [3, 3, 0, 0, 0], [[[x, y, -200000, 0, 0] for y in range(3)] for x in range(3)]]
        lo, hi = c.extent([[{'classname': 'worldspawn'}, []], [{'classname': 'func_static', 'origin': '1000 0 0'}, [wedge, patch]]])
        self.assertEqual([round(x, 6) for x in lo + hi], [0, 0, -200000, 1064, 64, 64])

    def test_names_and_map_only_keys(self):
        world = [{'classname': 'worldspawn'}, []]
        for ents in ([[{'classname': 'light', 'name': 'Lamp'}, []], [{'classname': 'light', 'name': 'lamp'}, []]],
                     [[{'classname': 'info_landmark', 'name': 'a', 'landmark': 'Door'}, []], [{'classname': 'info_landmark', 'name': 'b', 'landmark': 'door'}, []]]):
            with self.assertRaises(c.Invalid):c.transform([world] + ents, 'n', [0, 0, 0])
        landing = {'classname': 'info_landing', 'name': 'land', 'landmark': 'door', 'index': '0'}
        ents = [world, [{'classname': 'info_landmark', 'name': 'door'}, []], [landing, []]]
        c.transform(ents, 'n', [0, 0, 0])
        landing['dmapOnly'] = '1'  # the entityDef decides; a map key does nothing
        with self.assertRaises(c.Invalid):c.transform(ents, 'n', [0, 0, 0])

    def test_cli_atomic_output_and_input_refusal(self):
        recipe = self.directory/'kit.json'
        recipe.write_bytes(c.canonical(self.recipe))
        def run(output):
            return subprocess.run([sys.executable, str(HERE/'compose.py'), str(recipe), '--seed', '3', '--flag', 'extra_start', '--output', str(output)],
                                  capture_output=True, env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1'))
        before = (self.directory/'room_a.map').read_bytes()
        os.symlink(self.directory/'room_b.map', self.directory/'link.map')
        for output in (self.directory/'room_a.map', self.directory/'link.map', self.directory/'kit.map'):
            if output.name == 'kit.map':
                recipe = recipe.rename(self.directory/'kit.manifest.json')  # the manifest would replace the recipe
            result = run(output)
            self.assertEqual(result.returncode, 2)
            self.assertIn(b'recipe input', result.stderr)
        self.assertEqual((self.directory/'room_a.map').read_bytes(), before)
        out = self.directory/'out'/'c0.map'
        for _ in range(2):  # the second run replaces the first whole
            self.assertEqual(run(out).returncode, 0)
        data, manifest = c.compose(recipe, 3, ['extra_start'])
        self.assertEqual(out.read_bytes(), data)
        self.assertEqual(out.with_suffix('.manifest.json').read_bytes(), manifest)
        self.assertEqual(sorted(x.name for x in out.parent.iterdir()), ['c0.manifest.json', 'c0.map'])


if __name__=='__main__':
    unittest.main()
