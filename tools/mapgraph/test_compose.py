#!/usr/bin/env python3
"""Run with PYTHONDONTWRITEBYTECODE=1; scratch stays under workspace/work."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
import compose as c

HERE = Path(__file__).resolve().parent
WORK = HERE.parents[2] / 'work/phase2/composer'


class ComposerTests(unittest.TestCase):
    def setUp(self):
        WORK.mkdir(parents=True, exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=WORK)
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


if __name__=='__main__':
    unittest.main()
