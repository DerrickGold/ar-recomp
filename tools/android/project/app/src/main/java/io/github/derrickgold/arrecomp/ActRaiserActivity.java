package io.github.derrickgold.arrecomp;

import org.libsdl.app.SDLActivity;

/* SDLActivity loads these in order, then calls SDL_main in libmain.so. The
 * game links SDL3_ttf unconditionally, so it must load before main. */
public class ActRaiserActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "SDL3_ttf", "main" };
    }
}
