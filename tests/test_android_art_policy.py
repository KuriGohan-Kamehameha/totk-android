"""Production second-screen art decisions on the host JVM, without Android stubs."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT / 'games/totk'
SOURCE = TOTK / 'android/java/com/aure/totk/setup/ArtPolicy.java'
PROBE = TOTK / 'tests/art-policy/ArtPolicyProbe.java'


class ArtPolicyTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'javac/java not found')
    def test_all_display_stage_decline_combinations(self):
        with tempfile.TemporaryDirectory(prefix='totk-art-policy-') as work:
            subprocess.run(['javac', '--release', '17', '-encoding', 'UTF-8', '-d', work,
                            str(SOURCE), str(PROBE)], check=True, capture_output=True, text=True)
            result = subprocess.run(['java', '-cp', work, 'com.aure.totk.setup.ArtPolicyProbe'],
                                    check=True, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.stdout.strip(), '20 combinations; asset-state precedence passed')


    def test_the_eden_duo_page_is_not_offered_art(self):
        # games.toml companion = "duo" (both games today): the setup neither lists, offers, builds nor updates the art
        # stage; its code stays for CompanionView (ArtPolicy.used).
        setup = SOURCE.parent
        service = (setup / 'SetupService.java').read_text()
        self.assertIn('opened.request.put("skipCompanion", !ArtPolicy.used(GameBuild.COMPANION)', service)
        activity = (setup / 'SetupActivity.java').read_text()
        self.assertIn('return ArtPolicy.used(GameBuild.COMPANION) && SettingsStore.secondaryDisplay(this);', activity)
        for call in ('ArtPolicy.updateAutomatically(artScreen(),', 'ArtPolicy.offerBeforePlay(artScreen(),',
                     'if (!InstallState.OPTIONAL.equals(stage) || ArtPolicy.used(GameBuild.COMPANION)) all.add(stage);',
                     'Widgets.Row companionRow = !ArtPolicy.used(GameBuild.COMPANION) ? null'):
            self.assertIn(call, activity)
        self.assertNotIn('ArtPolicy.offerBeforePlay(SettingsStore.secondaryDisplay', activity)
        self.assertIn('ArtPolicy.used(GameBuild.COMPANION) && !done("companion")', (setup / 'InstallState.java').read_text())
        self.assertTrue((ROOT / 'games/totk/android/setup/companion/stage_companion.c').is_file())   # the stage stays


if __name__ == '__main__':
    unittest.main()
