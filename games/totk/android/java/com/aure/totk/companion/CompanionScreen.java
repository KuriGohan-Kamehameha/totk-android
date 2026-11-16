package com.aure.totk.companion;

import android.view.View;

/** What the second screen's shell (PresentationController) knows of the page it shows, whatever draws it: Tears of the
 *  Kingdom's CompanionView (Canvas) or Breath of the Wild's DuoSurfaceView (the native Eden Duo page).
 *  CompanionContent.create, one per game build, returns one. All calls on the UI thread. */
public interface CompanionScreen {
    /** The view to show. */
    View view();
    /** Asks for a page by number; a screen that keeps no pages of its own ignores it. */
    void setPage(int page);
    /** The page shown now, for the common state (CompanionState.page); -1 when the screen keeps none. */
    int page();
    /** The common game state changed (never null; status NONE when there is none to read). */
    void onGameState(CompanionState state);
}
