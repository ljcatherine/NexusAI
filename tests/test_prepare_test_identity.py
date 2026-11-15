"""prepare-test-identity.py: the PPSA99743 "NexusAI" identity and Orbit N icon."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('identity', REPO / 'prepare-test-identity.py')
identity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(identity)

UPSTREAM = {
    'conceptId': '99581',
    'contentId': 'UP9000-PPSA99581_00-PS5LMPLUS0000001',
    'contentVersion': '01.008.003',
    'localizedParameters': {'defaultLanguage': 'en-US', 'en-US': {'titleName': 'PS5LM Plus'}},
    'titleId': 'PPSA99581',
}
UPSTREAM_ICON = b'upstream icon'


def png(width, height):
    return (identity.PNG_MAGIC + b'\x00\x00\x00\x0dIHDR'
            + width.to_bytes(4, 'big') + height.to_bytes(4, 'big') + b'\x08\x06\x00\x00\x00')


class PrepareTestIdentityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.tree = self.make_tree('build', 'nexusai')

    def make_tree(self, parent, name):
        tree = self.root / parent / name
        (tree / 'scripts').mkdir(parents=True)
        (tree / 'scripts/build-app.sh').write_text('#!/bin/sh\n')
        sce_sys = tree / 'ps5/app/sce_sys'
        sce_sys.mkdir(parents=True)
        (sce_sys / 'param.json').write_text(json.dumps(UPSTREAM, indent=4))
        (sce_sys / 'icon0.png').write_bytes(UPSTREAM_ICON)
        return tree

    def param(self, tree=None):
        return json.loads(((tree or self.tree) / 'ps5/app/sce_sys/param.json').read_text())

    def icon(self, tree=None):
        return ((tree or self.tree) / 'ps5/app/sce_sys/icon0.png').read_bytes()

    def assert_untouched(self, tree=None):
        self.assertEqual(self.param(tree), UPSTREAM)
        self.assertEqual(self.icon(tree), UPSTREAM_ICON)

    def test_sets_nexusai_identity_and_icon(self):
        result = identity.prepare(self.tree, 'PPSA99743')
        self.assertEqual(result, {'titleId': 'PPSA99743', 'conceptId': '99743',
                                  'contentId': 'UP9000-PPSA99743_00-NEXUSPS5LM000001',
                                  'titleName': 'NexusAI'})
        param = self.param()
        self.assertEqual(param['titleId'], 'PPSA99743')
        self.assertEqual(param['conceptId'], '99743')
        self.assertEqual(param['contentId'], 'UP9000-PPSA99743_00-NEXUSPS5LM000001')
        self.assertEqual(param['localizedParameters']['en-US']['titleName'], 'NexusAI')
        self.assertEqual(param['contentVersion'], '01.008.003')
        self.assertEqual(self.icon(), identity.ICON.read_bytes())

    def test_running_twice_gives_the_same_result(self):
        identity.prepare(self.tree, 'PPSA99743')
        first = (self.param(), self.icon())
        identity.prepare(self.tree, 'PPSA99743')
        self.assertEqual((self.param(), self.icon()), first)

    def test_repo_icon_is_a_512_png(self):
        data = identity.ICON.read_bytes()
        self.assertEqual(data[:8], identity.PNG_MAGIC)
        self.assertEqual(data[16:24], png(512, 512)[16:24])

    def test_accepts_docker_and_isolated_clones(self):
        for parent, name in (('work', 'nexusai'), ('build', 'ps5lm-nexus-test')):
            tree = self.make_tree(parent, name)
            with self.subTest(tree=f'{parent}/{name}'):
                identity.prepare(tree, 'PPSA99743')
                self.assertEqual(self.param(tree)['titleId'], 'PPSA99743')

    def test_refuses_original_or_malformed_ids(self):
        for bad in ('PPSA99581', 'PPSA9974', 'CUSA99743', 'PPSA9974x', ''):
            with self.subTest(id=bad), self.assertRaisesRegex(ValueError, 'distinct, well-formed'):
                identity.prepare(self.tree, bad)
        self.assert_untouched()

    def test_refuses_other_folders(self):
        for parent, name in (('build', 'other'), ('src', 'nexusai'), ('work', 'ps5lm-nexus-test')):
            tree = self.make_tree(parent, name)
            with self.subTest(tree=f'{parent}/{name}'), self.assertRaisesRegex(ValueError, 'generated'):
                identity.prepare(tree, 'PPSA99743')
            self.assert_untouched(tree)

    def test_refuses_another_title_id_in_tree(self):
        other = dict(UPSTREAM, titleId='PPSA99742')
        (self.tree / 'ps5/app/sce_sys/param.json').write_text(json.dumps(other))
        with self.assertRaisesRegex(ValueError, 'unexpected input title ID'):
            identity.prepare(self.tree, 'PPSA99743')
        self.assertEqual(self.icon(), UPSTREAM_ICON)

    def test_refuses_symlinked_param_or_icon(self):
        for name in ('param.json', 'icon0.png'):
            tree = self.make_tree('build', f'ps5lm-nexus-{name}')
            target = tree / 'ps5/app/sce_sys' / name
            outside = self.root / f'outside-{name}'
            outside.write_bytes(target.read_bytes())
            target.unlink()
            target.symlink_to(outside)
            before = outside.read_bytes()
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'not a NexusAI native title'):
                identity.prepare(tree, 'PPSA99743')
            self.assertEqual(outside.read_bytes(), before)

    def test_refuses_icon_that_is_not_a_512_png(self):
        icon = self.root / 'icon.png'
        for data in (png(256, 256), png(512, 256), b'GIF89a' + b'\x00' * 32):
            icon.write_bytes(data)
            with self.subTest(data=data[:6]), self.assertRaisesRegex(ValueError, '512x512 PNG'):
                identity.prepare(self.tree, 'PPSA99743', icon)
        self.assert_untouched()


if __name__ == '__main__':
    unittest.main()
