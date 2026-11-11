package com.killersheaven.hailtotheking;

import org.libsdl.app.SDLActivity;

/* SDL's activity does the work (window, GLES context, audio, controllers); this only
 * names the native libraries: SDL itself, then the game (libmain.so, entry SDL_main). */
public class HTTKActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }
}
