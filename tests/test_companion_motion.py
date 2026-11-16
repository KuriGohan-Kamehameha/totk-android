"""What moves on the Purah Pad (Motion.java): view choice and fade, page slide, heart pulse, marker glide, and the
rule that frames are asked for only while something moves. Motion uses no Android class; the probe drives its clock."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / 'games/totk/android/java/com/aure/totk/companion'

PROBE = r'''package com.aure.totk.companion;

public final class MotionProbe {
    static void require(boolean ok, String what) { if (!ok) throw new AssertionError(what); }
    static void near(double a, double b, String what) { if (!(Math.abs(a - b) < 1e-3)) throw new AssertionError(what + ": " + a + " != " + b); }

    public static void main(String[] args) {
        // ---- which view ----
        int none = CompanionState.NONE, loadingScreen = CompanionState.LOADING;
        require(Motion.view("ready", false, none) == Motion.CONTENT, "a loaded save shows the pages");
        require(Motion.view("ready", true, none) == Motion.LOADING, "the last good snapshot kept over a load is the loading view");
        require(Motion.view("ready", false, loadingScreen) == Motion.LOADING, "a loading screen the game side names");
        require(Motion.view("title", false, none) == Motion.TITLE, "title");
        require(Motion.view("starting", false, none) == Motion.LOADING && Motion.view("noPlayer", false, none) == Motion.LOADING
                && Motion.view(null, false, none) == Motion.LOADING, "before the game can be read");
        require(Motion.view("unsupported", false, none) == Motion.UNSUPPORTED && Motion.view("unsupported", true, loadingScreen) == Motion.UNSUPPORTED,
                "unsupported whatever else is said");

        // ---- an idle screen asks for no frame ----
        Motion m = new Motion();
        long t = 1_000_000;
        require(m.view() == Motion.LOADING && m.fade(t) == 1f, "starts on the loading view, fully in");
        m.show(Motion.TITLE, t);
        require(m.fade(t) == 1f && m.nextFrame(t, false, false) == -1 && !m.animating(t, true, true), "the first view appears at once and the title is still");

        // ---- fade between views ----
        m.show(Motion.CONTENT, t += 5000);
        near(m.fade(t), 0, "fade starts"); require(m.nextFrame(t, true, true) == 0, "fading: next display frame");
        near(m.fade(t + Motion.FADE_MS / 2), .5, "half way");
        require(m.fade(t + Motion.FADE_MS) == 1f && m.nextFrame(t + Motion.FADE_MS, true, true) == -1, "fade ends and frames stop");
        m.show(Motion.CONTENT, t + 10); near(m.fade(t + Motion.FADE_MS / 2), .5, "the same view again does not restart the fade");
        t += 1000;

        // ---- page slide ----
        require(m.slide(t) == 1f && !m.sliding(t), "no slide: the page is in place");
        m.slide(1, 1, t); require(!m.sliding(t), "the same page does not slide");
        m.slide(0, 2, t);
        require(m.sliding(t) && m.slide(t) == 0f && m.slideFrom() == 0 && m.slideDirection() == 1, "to a higher page: in from the right");
        float last = 0;
        for (long dt = 0; dt <= Motion.SLIDE_MS; dt += 11) {
            float p = m.slide(t + dt);
            require(p >= last && p <= 1, "monotonic 0..1"); last = p;
            require(m.nextFrame(t + dt, false, false) == (dt < Motion.SLIDE_MS ? 0 : -1), "frames exactly while it slides (" + dt + ")");
        }
        require(m.slide(t + Motion.SLIDE_MS / 2) > .8f, "eased out: most of the way by half time");
        require(m.slide(t + Motion.SLIDE_MS) == 1f && !m.sliding(t + Motion.SLIDE_MS), "ends in place");
        m.slide(2, 1, t += 1000); require(m.slideDirection() == -1 && m.slideFrom() == 2, "to a lower page: in from the left");
        m.show(Motion.LOADING, t + 20); require(!m.sliding(t + 30), "a view change ends the slide");
        near(Motion.easeOut(0), 0, "ease 0"); near(Motion.easeOut(1), 1, "ease 1");

        // ---- loading mark: eight steps a second, a frame per step and no more ----
        long step = Motion.LOADING_FRAME_MS;
        t = 80 * step;
        require(Motion.loadingStep(t) == 0 && Motion.loadingStep(t + step - 1) == 0 && Motion.loadingStep(t + step) == 1
                && Motion.loadingStep(t + 8 * step) == 0, "steps");
        m = new Motion(); m.show(Motion.LOADING, t);
        require(m.nextFrame(t, true, true) == step && m.nextFrame(t + 100, true, true) == step - 100, "next frame at the next step");
        t = 1_000_000;

        // ---- heart pulse ----
        m = new Motion(); m.show(Motion.CONTENT, t);
        m.life(40, t);
        require(m.pulse(t) == 0 && m.heartScale(3, t) == 1f && m.nextFrame(t, true, true) == -1, "the first sample is the baseline");
        m.life(40, t += 125); require(m.pulse(t) == 0, "unchanged: nothing");
        m.life(34, t += 125);                                          // six quarters lost: hearts 8 and 9 (quarters 32..39) changed
        near(m.pulse(t), 1, "pulse at the change");
        require(m.pulsed(8) && m.pulsed(9) && !m.pulsed(7) && !m.pulsed(10), "the hearts that changed");
        require(m.heartScale(8, t) > 1.4f && m.heartScale(7, t) > 1f && m.heartScale(7, t) < 1.1f, "they swell, the rest barely");
        require(m.nextFrame(t, true, false) == 0 && m.nextFrame(t, false, false) == -1, "frames only where hearts are drawn");
        float before = 1;
        for (long dt = 0; dt <= Motion.PULSE_MS; dt += 50) { float p = m.pulse(t + dt); require(p <= before && p >= 0, "decays"); before = p; }
        require(m.pulse(t + Motion.PULSE_MS) == 0 && m.heartScale(8, t + Motion.PULSE_MS) == 1f
                && m.nextFrame(t + Motion.PULSE_MS, true, false) == -1, "over: hearts at rest, frames stop");
        m.life(35, t += 2000); require(m.pulsed(8) && !m.pulsed(9), "a quarter gained pulses its heart");
        m.forget(); m.life(12, t += 10);
        require(m.pulse(t) == 0, "after a load the hearts start afresh");
        m.life(11, t += 125); m.show(Motion.LOADING, t + 1); m.show(Motion.CONTENT, t + 2); m.life(3, t + 3);
        require(m.pulse(t + 300 + Motion.FADE_MS) == 0, "leaving the pages forgets the hearts too");

        // ---- marker glide ----
        m = new Motion(); m.show(Motion.CONTENT, t += 5000);
        require(!m.markerKnown(), "no position yet");
        m.target(100, 200, 90, t);
        require(m.markerKnown() && m.markerX(t) == 100 && m.markerZ(t) == 200 && m.heading(t) == 90 && m.nextFrame(t, true, true) == -1,
                "the first sample is where the marker is");
        m.target(108, 200, 90, t += 125);
        near(m.markerX(t), 100, "glide starts where it was"); near(m.markerX(t + Motion.FOLLOW_MS / 2), 104, "half way");
        near(m.markerX(t + Motion.FOLLOW_MS), 108, "arrives"); near(m.markerZ(t + 40), 200, "z still");
        require(m.nextFrame(t + 10, false, true) == Motion.FOLLOW_FRAME_MS, "gliding: frames at the slower rate");
        require(m.nextFrame(t + 10, true, false) == -1, "no frames on a page without the marker");
        require(m.nextFrame(t + Motion.FOLLOW_MS, false, true) == -1, "arrived: frames stop");
        // A new sample mid-glide continues from where the marker is drawn: no jump.
        m.target(116, 200, 90, t += 125);
        double at = m.markerX(t);
        near(at, 100 + 8 * 125.0 / Motion.FOLLOW_MS, "mid-glide position"); near(m.markerX(t + Motion.FOLLOW_MS), 116, "arrives at the newest");
        for (long dt = 0; dt < Motion.FOLLOW_MS; dt += 16) require(m.markerX(t + dt + 16) >= m.markerX(t + dt), "moves one way");
        // Standing still: the same sample again moves nothing and asks for nothing.
        t += 1000; m.target(116, 200, 90, t);
        require(m.nextFrame(t, true, true) == -1 && m.markerX(t) == 116, "an unchanged sample is not motion");
        // Heading turns the short way round.
        m.target(116, 200, 350, t += 1000); t += 1000;
        m.target(116, 200, 10, t);
        near(((m.heading(t + Motion.FOLLOW_MS / 2) % 360) + 360) % 360, 0, "through north, not round the south");
        near(((m.heading(t + Motion.FOLLOW_MS) % 360) + 360) % 360, 10, "ends on the sample");
        m.target(116, 200, Double.NaN, t += 1000); require(Double.isNaN(m.heading(t + 500)), "no heading from the game: none here");
        // A warp is not glided over.
        m.target(116 + Motion.SNAP_DISTANCE + 1, 200, 0, t += 1000);
        require(m.markerX(t) == 116 + Motion.SNAP_DISTANCE + 1 && m.nextFrame(t, true, true) == -1, "a jump snaps");
        m.target(Double.NaN, 200, 0, t += 125); require(!m.markerKnown(), "an unreadable position: no marker of ours");
        m.target(5, 5, 0, t += 125); require(m.markerX(t) == 5 && m.nextFrame(t, true, true) == -1, "and the next one snaps in");

        // ---- everything at once ends, and then nothing asks for a frame ----
        m = new Motion(); m.show(Motion.CONTENT, t += 5000);
        m.life(20, t); m.target(0, 0, 0, t);
        t += 1000; m.life(19, t); m.target(3, 0, 0, t); m.slide(0, 1, t);
        require(m.animating(t, true, true), "moving");
        long longest = Math.max(Math.max(Motion.SLIDE_MS, Motion.PULSE_MS), Math.max(Motion.FOLLOW_MS, Motion.FADE_MS));
        require(!m.animating(t + longest, true, true) && m.nextFrame(t + longest + 60_000, true, true) == -1, "then at rest for good");
        System.out.println("pass: companion motion");
    }
}
'''


class CompanionMotionTests(unittest.TestCase):
    def test_motion_needs_no_android(self):
        self.assertNotIn('android', (JAVA / 'Motion.java').read_text().replace('Android', ''))

    def test_view_only_redraws_while_motion_asks(self):
        # The view has one scheduling point, driven by Motion.nextFrame, and one pending delayed frame at most.
        view = (JAVA / 'CompanionView.java').read_text()
        schedule = view.split('    private void schedule() {', 1)[1].split('\n    }\n', 1)[0]
        self.assertIn('motion.nextFrame(frameNow', schedule)
        self.assertIn('removeCallbacks(frame);', schedule)
        self.assertEqual(view.count('postDelayed('), 1)
        self.assertEqual(view.count('postInvalidateOnAnimation()'), 2)   # schedule, and the pending-action spinner as before
        poller = (JAVA / 'SnapshotPoller.java').read_text()
        self.assertIn('queue.postDelayed(this, 125)', poller)             # 8 Hz live
        self.assertIn('now - lastFull >= 1000', poller)                   # 1 Hz full

    def test_timing(self):
        javac, java = shutil.which('javac'), shutil.which('java')
        if not javac or not java:
            self.skipTest('a JDK is required')
        with tempfile.TemporaryDirectory(prefix='totk-motion-') as temp:
            temp = Path(temp)
            probe = temp / 'MotionProbe.java'
            probe.write_text(PROBE)
            subprocess.run([javac, '--release', '17', '-Xlint:-options', '-d', str(temp / 'classes'), str(JAVA / 'Motion.java'),
                            str(JAVA / 'CompanionState.java'), str(probe)], check=True, capture_output=True, text=True)
            done = subprocess.run([java, '-cp', str(temp / 'classes'), 'com.aure.totk.companion.MotionProbe'],
                                  capture_output=True, text=True, timeout=60)
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertIn('pass: companion motion', done.stdout)


if __name__ == '__main__':
    unittest.main()
