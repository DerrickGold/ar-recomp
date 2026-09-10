# AppImage components

The Linux Builder includes unmodified upstream appimagetool and a type-2
runtime, pinned in `installer/packaging/appimage.cmake`. The tool's AppImage
retains its embedded dependency notices. The runtime is copied into each
locally generated game AppImage; these notices travel with that game too.

Corresponding upstream source and dependency build recipes:

- https://github.com/AppImage/appimagetool/tree/8c8c91f762b412a19f4e8d2c4b35afb98f2d7c81
- https://github.com/AppImage/type2-runtime/tree/75849dce7cc37e4319b633df1f116ca895c71a12

`appimagetool-LICENSE.txt` and `runtime-LICENSE.txt` are copied verbatim from
those revisions. These licenses do not change the licenses of the game's
contents.
