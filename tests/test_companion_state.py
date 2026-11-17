"""The second screen's common game state (games/totk/runtime/companion_state.h): one layout for both games.

The Eden Duo host's side (duo/duo_state.h: Breath of the Wild's table and Tears of the Kingdom's) is run here as C; Tears
of the Kingdom's own reader (companion_state in companion_snapshot.c, also what the Tears of the Kingdom module reads the
game with: its vendored copy must stay byte for byte the runtime's) runs against a fake game image in
games/totk/tests/companion/test_companion.c (make companiontest). The Java side (CompanionState) reads the words any of
them produces, and the header and the Java class must agree."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT / 'games/totk'
HEADER = TOTK / 'runtime/companion_state.h'
JAVA = TOTK / 'android/java/com/aure/totk/companion/CompanionState.java'

C_PROBE = r'''
#include <stdio.h>
#include <string.h>
#include "duo/duo_state.h"

static int failures;
/* v: valid, title, loading, menu_open, slate_obtained, heal.valid, heal.life, heal.max; want: the getter's words. */
static void expect(const char *what, const int64_t v[DUO_STATE_VALUES], const int64_t want[COMPANION_STATE_FIELDS]) {
    CompanionState s; int64_t words[COMPANION_STATE_FIELDS];
    duo_state_fill(v, &s);
    companion_state_words(&s, want[7], words);
    if (memcmp(words, want, sizeof words)) {
        failures++;
        fprintf(stderr, "FAIL %s:", what);
        for (int i = 0; i < COMPANION_STATE_FIELDS; i++) fprintf(stderr, " %lld", (long long)words[i]);
        fprintf(stderr, "\n");
    }
    for (int i = 0; i < COMPANION_STATE_FIELDS; i++) printf("%lld%c", (long long)words[i], i + 1 < COMPANION_STATE_FIELDS ? ' ' : '\n');
}
static void expect_totk(const char *what, const int64_t v[DUO_TOTK_VALUES], const int64_t want[COMPANION_STATE_FIELDS]) {
    CompanionState s; int64_t words[COMPANION_STATE_FIELDS];
    duo_state_fill_totk(v, &s);
    companion_state_words(&s, want[7], words);
    if (memcmp(words, want, sizeof words)) {
        failures++;
        fprintf(stderr, "FAIL %s:", what);
        for (int i = 0; i < COMPANION_STATE_FIELDS; i++) fprintf(stderr, " %lld", (long long)words[i]);
        fprintf(stderr, "\n");
    }
    for (int i = 0; i < COMPANION_STATE_FIELDS; i++) printf("%lld%c", (long long)words[i], i + 1 < COMPANION_STATE_FIELDS ? ' ' : '\n');
}
#define V(...) (const int64_t[DUO_STATE_VALUES]){__VA_ARGS__}
#define T(...) (const int64_t[DUO_TOTK_VALUES]){__VA_ARGS__}
#define W(...) (const int64_t[COMPANION_STATE_FIELDS]){__VA_ARGS__}
int main(void) {
    enum { S = COMPANION_STATE_STARTING, T = COMPANION_STATE_TITLE, L = COMPANION_STATE_LOADING, R = COMPANION_STATE_READY };
    expect("world", V(1, 0, 0, 0, 1, 1, 9, 12), W(R, 1, 0, 1, 9, 12, -1, 16));
    expect("menu before the slate", V(1, 0, 0, 1, 0, 0, -1, -1), W(R, 1, 1, 0, -1, -1, -1, 16));
    expect("slate not read counts as owned", V(1, 0, 0, 0, -1, 1, 9, 12), W(R, 1, 0, 1, 9, 12, -1, 16));
    expect("title", V(1, 1, 0, 0, 1, 1, 9, 12), W(T, 0, 0, 1, -1, -1, -1, 16));
    expect("loading", V(1, 0, 1, 0, 1, 1, 9, 12), W(L, 0, 0, 1, -1, -1, -1, 16));
    expect("title wins over loading", V(1, 1, 1, 1, 0, 1, 9, 12), W(T, 0, 0, 1, -1, -1, -1, 16));
    expect("a sample the module does not vouch for", V(0, -1, -1, 0, -1, 0, -1, -1), W(S, 0, 0, 1, -1, -1, -1, 16));
    expect("hearts the heal advisor does not vouch for", V(1, 0, 0, 0, 1, 0, 9, 12), W(R, 1, 0, 1, -1, -1, -1, 16));
    expect("implausible hearts", V(1, 0, 0, 0, 1, 1, 9, 3), W(R, 1, 0, 1, -1, -1, -1, 16));
    expect("implausible hearts", V(1, 0, 0, 0, 1, 1, 9, 404), W(R, 1, 0, 1, -1, -1, -1, 16));
    expect("negative hearts", V(1, 0, 0, 0, 1, 1, -2, 12), W(R, 1, 0, 1, -1, -1, -1, 16));
    expect("menu flag other than 1", V(1, 0, 0, 2, 1, 1, 0, 400), W(R, 1, 0, 1, 0, 400, -1, 16));
    /* Tears of the Kingdom: status, menu_open, life, max_life as the module publishes them (-1 outside a save). */
    enum { U = COMPANION_STATE_UNSUPPORTED };
    expect_totk("totk world", T(5, 0, 47, 80), W(R, 1, 0, 1, 47, 80, -1, 16));
    expect_totk("totk pause menu", T(5, 1, 47, 80), W(R, 1, 1, 1, 47, 80, -1, 16));
    expect_totk("totk title", T(3, -1, -1, -1), W(T, 0, 0, 1, -1, -1, -1, 16));
    expect_totk("totk another build", T(2, -1, -1, -1), W(U, 0, 0, 1, -1, -1, -1, 16));
    expect_totk("totk starting", T(1, -1, -1, -1), W(S, 0, 0, 1, -1, -1, -1, 16));
    expect_totk("totk no sample yet", T(0, 0, 0, 0), W(S, 0, 0, 1, -1, -1, -1, 16));
    expect_totk("totk a status it never publishes", T(4, 0, 47, 80), W(S, 0, 0, 1, -1, -1, -1, 16));
    expect_totk("totk implausible hearts", T(5, 0, 9, 3), W(R, 1, 0, 1, -1, -1, -1, 16));
    expect_totk("totk hearts not read", T(5, 0, -1, 80), W(R, 1, 0, 1, -1, -1, -1, 16));
    /* The host's table by the running title, and the names it keeps. */
    const DuoStateTable *botw = duo_state_table(0x01007EF00011E000ull), *totk = duo_state_table(0x0100F2C0115B6000ull);
    if (!botw || !totk || botw == totk || duo_state_table(0x0100000000010000ull)) { failures++; fprintf(stderr, "FAIL tables\n"); }
    else {
        if (botw->count != DUO_STATE_VALUES || totk->count != DUO_TOTK_VALUES || botw->count > DUO_STATE_MAX_VALUES ||
            totk->count > DUO_STATE_MAX_VALUES) { failures++; fprintf(stderr, "FAIL table sizes\n"); }
        if (duo_state_index(totk, "totk.status") != DUO_TOTK_STATUS || duo_state_index(totk, "totk.state.menu_open") != DUO_TOTK_MENU_OPEN ||
            duo_state_index(totk, "totk.state.life") != DUO_TOTK_LIFE || duo_state_index(totk, "totk.state.max_life") != DUO_TOTK_MAX_LIFE ||
            duo_state_index(totk, "totk.state.page") != -1 || duo_state_index(totk, "totk.status_text") != -1 ||
            duo_state_index(totk, "botw.state.valid") != -1 || duo_state_index(totk, NULL) != -1 ||
            duo_state_index(botw, "botw.ready.heal.max") != DUO_STATE_MAX_LIFE || duo_state_index(botw, "botw.state.valid") != DUO_STATE_VALID ||
            duo_state_index(botw, "totk.status") != -1 || duo_state_index(NULL, "totk.status") != -1) {
            failures++; fprintf(stderr, "FAIL names\n");
        }
        /* The table's fill is the title's own. */
        int64_t v[DUO_STATE_MAX_VALUES] = {5, 1, 10, 16};
        CompanionState a, b;
        totk->fill(v, &a); duo_state_fill_totk(v, &b);
        if (memcmp(&a, &b, sizeof a)) { failures++; fprintf(stderr, "FAIL totk fill\n"); }
        int64_t w[DUO_STATE_MAX_VALUES] = {1, 0, 0, 0, 1, 1, 9, 12};
        botw->fill(w, &a); duo_state_fill(w, &b);
        if (memcmp(&a, &b, sizeof a)) { failures++; fprintf(stderr, "FAIL botw fill\n"); }
    }
    CompanionState clear;
    companion_state_clear(&clear);
    if (clear.status != COMPANION_STATE_NONE || clear.in_game || clear.menu_open || clear.tablet_owned != 1 || clear.life != -1 ||
        clear.max_life != -1 || clear.page != -1) { failures++; fprintf(stderr, "FAIL clear\n"); }
    return failures != 0;
}
'''

JAVA_PROBE = r'''
import com.aure.totk.companion.CompanionState;

public final class StateProbe {
    static void require(boolean ok, String what) { if (!ok) throw new AssertionError(what); }
    public static void main(String[] args) {
        CompanionState s = new CompanionState();
        // Every line the C side printed is a state Java reads without loss.
        for (String line : args) {
            String[] parts = line.trim().split(" ");
            long[] v = new long[parts.length];
            for (int i = 0; i < v.length; i++) v[i] = Long.parseLong(parts[i]);
            require(v.length == CompanionState.FIELDS, "words per state");
            require(s.set(v), "readable: " + line);
            require(s.status == v[0] && s.inGame == (v[1] == 1) && s.menuOpen == (v[2] == 1) && s.tabletOwned == (v[3] != 0)
                    && s.life == v[4] && s.maxLife == v[5] && s.page == v[6], "round trip: " + line);
            require(s.inGame == (s.status == CompanionState.READY), "in game is exactly a loaded save: " + line);
        }
        // What says nothing: no words, too few, no sample, an old one, no status, a status this build does not know.
        long[] ready = {CompanionState.READY, 1, 1, 1, 10, 16, 2, 0};
        require(s.set(ready) && s.inGame && s.menuOpen && s.life == 10 && s.maxLife == 16 && s.page == 2, "ready");
        long key = s.key();
        require(!s.set(null) && s.status == CompanionState.NONE && !s.inGame && s.tabletOwned && s.life == -1 && s.page == -1, "null clears");
        require(!s.set(new long[CompanionState.FIELDS - 1]), "too few");
        for (long age : new long[]{-1, CompanionState.MAX_AGE_MS + 1}) { ready[CompanionState.AGE_MS] = age; require(!s.set(ready), "age " + age); }
        ready[CompanionState.AGE_MS] = CompanionState.MAX_AGE_MS; require(s.set(ready), "as old as allowed");
        require(s.key() == key, "the age is not part of what changed");
        for (long status : new long[]{CompanionState.NONE, -1, 6}) { ready[0] = status; require(!s.set(ready), "status " + status); }
        // Outside a save the in-game fields are not believed, whatever the words say.
        require(s.set(new long[]{CompanionState.TITLE, 1, 1, 0, 9, 12, 1, 0}) && !s.inGame && !s.menuOpen && s.tabletOwned
                && s.life == -1 && s.maxLife == -1 && s.page == 1, "title carries the page only");
        // The key tells every field apart.
        java.util.Set<Long> keys = new java.util.HashSet<>();
        long[][] distinct = {{5, 1, 0, 1, 10, 16, 0, 0}, {5, 1, 1, 1, 10, 16, 0, 0}, {5, 1, 0, 0, 10, 16, 0, 0}, {5, 1, 0, 1, 9, 16, 0, 0},
                {5, 1, 0, 1, 10, 20, 0, 0}, {5, 1, 0, 1, 10, 16, 1, 0}, {5, 1, 0, 1, -1, -1, 0, 0}, {5, 1, 0, 1, 10, 16, -1, 0},
                {3, 0, 0, 1, -1, -1, 0, 0}, {4, 0, 0, 1, -1, -1, 0, 0}, {1, 0, 0, 1, -1, -1, 0, 0}, {2, 0, 0, 1, -1, -1, 0, 0}};
        for (long[] v : distinct) { require(s.set(v), "set"); require(keys.add(s.key()), "distinct key " + java.util.Arrays.toString(v)); }
        s.clear();
        require(keys.add(s.key()), "no state has its own key");
        require(s.key() != -1, "-1 is free for 'never told'");
        System.out.println("pass: common state");
    }
}
'''


class CompanionStateTests(unittest.TestCase):
    def test_header_and_java_agree(self):
        header, java = HEADER.read_text(), JAVA.read_text()
        statuses = dict(re.findall(r'COMPANION_STATE_([A-Z]+) = (\d+)', header))
        self.assertEqual(statuses.pop('FIELDS'), '8')
        self.assertEqual(statuses, {'NONE': '0', 'STARTING': '1', 'UNSUPPORTED': '2', 'TITLE': '3', 'LOADING': '4', 'READY': '5'})
        constants = dict(re.findall(r'\b([A-Z_]+) = (\d+)', java))
        for name, value in statuses.items():
            self.assertEqual(constants[name], value, name)
        # companion_state_words: out[i] = s-><field> in the order of the Java index constants.
        words = re.findall(r'out\[(\d)\] = (?:s->)?(\w+);', header)
        self.assertEqual(words, [('0', 'status'), ('1', 'in_game'), ('2', 'menu_open'), ('3', 'tablet_owned'), ('4', 'life'),
                                 ('5', 'max_life'), ('6', 'page'), ('7', 'age_ms')])
        for index, name in words:
            self.assertEqual(constants[name.upper()], index, name)
        self.assertEqual(constants['FIELDS'], re.search(r'COMPANION_STATE_FIELDS = (\d+)', header).group(1))

    def test_mit_side_needs_nothing_of_the_gpl_host(self):
        # The contract, the reader and the Java side are the port's own (MIT) and stand without duo/: the runtime's
        # companion files include nothing under duo/ (duo_link.h is the runtime's own header, and only duo_port.c, which
        # opens the library with dlopen, includes duo/duo_port.h, the port's MIT interface).
        self.assertNotIn('#include "', HEADER.read_text().replace('#include <stdint.h>', ''))
        link = (TOTK / 'runtime/duo_link.h').read_text()
        self.assertEqual(re.findall(r'#include "([^"]+)"', link), ['runtime.h'])
        for name in ('companion.h', 'companion_snapshot.c', 'companion_actions.c', 'companion_jni.c', 'companion_state.h',
                     'game_patches_v143.c'):
            text = (TOTK / 'runtime' / name).read_text()
            self.assertNotIn('duo/', text.replace('duo/duo_state.h, handed over', ''), name)
            for include in re.findall(r'#include "([^"]+)"', text):
                self.assertFalse(include.startswith('../duo') or include.startswith('duo/'), (name, include))
        port = (TOTK / 'runtime/duo_port.c').read_text()
        self.assertEqual([i for i in re.findall(r'#include "([^"]+)"', port) if 'duo/' in i], ['../duo/duo_port.h'])
        self.assertIn('dlopen', port)
        # Tears of the Kingdom: the host's sample first, its own reader until there is one (or without the host).
        jni = (TOTK / 'runtime/companion_jni.c').read_text()
        self.assertIn('if (!runtime_duo_state(&s, &age)) {\n#ifdef GAME_BOTW\n        return JNI_FALSE;', jni)
        self.assertIn('companion_state((uintptr_t)runtime_module_base("main"), guest_read, &s);', jni)
        # The 1.4.3 game code starts the host only where duo_port.c can be linked (ELF), and never needs it.
        v143 = (TOTK / 'runtime/game_patches_v143.c').read_text()
        self.assertIn('#if DUO_LINK_WEAK\n    if(runtime_duo_frame)runtime_duo_frame();', v143)
        self.assertIn('#if DUO_LINK_WEAK\n    if (runtime_duo_main) runtime_duo_main(m);', v143)
        # The Java side is common to both games (games/games.toml): only DuoSurfaceView (the port's own view, MIT) and
        # CompanionContent, which picks it by the companion variable, name the Eden Duo page. DuoSurfaceView loads
        # libduo_host.so only when the game's companion is "duo"; a TotK APK never carries that library (audit_apk.py).
        java = TOTK / 'android/java/com/aure/totk'
        for path in [java / 'BifrostState.java', *sorted((java / 'companion').glob('*.java'))]:
            code = re.sub(r'/\*.*?\*/|//[^\n]*', '', path.read_text(), flags=re.S)   # comments may name the other screen
            if path.name == 'CompanionContent.java':
                self.assertIn('DUO = GameBuild.COMPANION.equals("duo")', code)
                self.assertIn('if (!DUO) return AssetStore.available(context);', code)
                continue
            if path.name == 'DuoSurfaceView.java':
                self.assertEqual(re.findall(r'loadLibrary\("(\w+)"\)', code), ['duo_host'])
                continue
            self.assertNotIn('Duo', code, path.name)

    def test_item_actions_reach_the_page_only_through_the_runtime(self):
        # DuoPort ABI 3: the runtime's own item actions (companion_actions.c) are the only route. The MIT side fills
        # five plain C functions for Tears of the Kingdom only; the GPL port forwards the module's port-only table to
        # them; the module itself never writes guest memory or calls game code.
        port_h = (TOTK / 'duo/duo_port.h').read_text()
        self.assertIn('#define DUO_PORT_ABI 3u', port_h)
        fields = re.findall(r'\(\*(\w+)\)\(', port_h.split('typedef struct DuoPort {', 1)[1].split('} DuoPort;', 1)[0])
        self.assertEqual(fields[-6:], ['state', 'item_actions', 'action_state', 'pouch_gen', 'request', 'result'])
        port_c = (TOTK / 'runtime/duo_port.c').read_text()
        self.assertIn('#define DUO_ITEM_ACTIONS 0', port_c.split('#else', 1)[0])            # Breath of the Wild: none
        self.assertIn('#define DUO_ITEM_ACTIONS 1', port_c.split('#ifdef GAME_BOTW', 1)[1].split('#endif', 1)[0])
        for name in ('item_actions', 'action_state', 'pouch_gen', 'item_request', 'item_result'):
            self.assertRegex(port_c, rf'runtime_companion_{name}\([^;]*\) __attribute__\(\(weak\)\);')
        jni = (TOTK / 'runtime/companion_jni.c').read_text()
        self.assertIn('return companion_request((uintptr_t)runtime_module_base("main"), action, category, slot, actor, '
                      'partition, pouch_gen);', jni)
        self.assertIn('static void actions_ready(void) { pthread_once(&actions_once, actions_init); }', jni)
        module_port = (TOTK / 'duo/port_module.cpp').read_text()
        self.assertIn('totk_native_port_bind_actions(instance, offered ? &g_native_actions : nullptr);', module_port)
        module = (TOTK / 'duo/vendor/companions/native/modules/0100F2C0115B6000.cpp').read_text()
        for forbidden in ('write_memory', 'queue_guest_call', 'write_batch'):
            self.assertNotIn(forbidden, module)
        self.assertIn('return refuse("notAvailableYet");   // Eden Duo: nothing is ever requested', module)

    def test_vendored_reader_is_the_runtime_reader(self):
        # The Tears of the Kingdom module reads the game with this runtime's reader, compiled unchanged: while both copies
        # exist they are the same bytes, so a fix lands in both (duo/vendor/README.md).
        vendored = TOTK / 'duo/vendor/companions/native/modules/totk_reader'
        for name in ('companion_snapshot.c', 'companion.h', 'companion_state.h'):
            with self.subTest(name=name):
                self.assertEqual((vendored / name).read_bytes(), (TOTK / 'runtime' / name).read_bytes(),
                                 f'{name}: the vendored reader differs from games/totk/runtime/{name}')
        self.assertIn('MIT', (vendored / 'LICENSE.MIT').read_text())

    def test_both_sides_map_onto_the_contract(self):
        compiler = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
        javac, java = shutil.which('javac'), shutil.which('java')
        if not compiler or not javac or not java:
            self.skipTest('a C compiler and a JDK are required')
        with tempfile.TemporaryDirectory(prefix='totk-state-') as temp:
            temp = Path(temp)
            (temp / 'probe.c').write_text(C_PROBE)
            subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(TOTK), str(temp / 'probe.c'),
                            '-o', str(temp / 'probe')], check=True, capture_output=True, text=True)
            done = subprocess.run([str(temp / 'probe')], capture_output=True, text=True, timeout=30)
            self.assertEqual(done.returncode, 0, done.stderr)
            lines = done.stdout.strip().splitlines()
            self.assertEqual(len(lines), 21)
            (temp / 'StateProbe.java').write_text(JAVA_PROBE)
            subprocess.run([javac, '--release', '17', '-Xlint:-options', '-d', str(temp / 'classes'), str(JAVA),
                            str(temp / 'StateProbe.java')], check=True, capture_output=True, text=True)
            done = subprocess.run([java, '-cp', str(temp / 'classes'), 'StateProbe', *lines], capture_output=True, text=True, timeout=60)
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertIn('pass: common state', done.stdout)


if __name__ == '__main__':
    unittest.main()
