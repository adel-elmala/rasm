# RASM logo

The logo is a carved star-and-cross tile, the classic tessellation of Islamic geometric art, lit as a real surface: the gold star is sixteen flat facets, and the blue crosses are a glazed ground. Beside it, the word RASM in Cormorant Garamond.

`carved_tile.py` renders every logo file in `docs/assets/logo/`:

| File | Used for |
|---|---|
| `logo-dark.png`, `logo-light.png` | The README header, chosen by GitHub's theme |
| `icon-512.png` | The master icon (GitHub avatar, social preview) |
| `icon-180.png` | The docs sidebar and the touch icon |
| `icon-32.png` | The docs favicon |

To render them again, with Python 3, numpy and Pillow:

```sh
python tools/logo/carved_tile.py
```

The tile is computed from a height field, per pixel: change the colors (`gold`, `lapis`), the light direction or the facet heights in `carved_tile()`.

## Font

`fonts/CormorantGaramond-SemiBold-RASM.ttf` is [Cormorant Garamond](https://github.com/CatharsisFonts/Cormorant) at weight 600, cut down to the letters R, A, S and M. It is licensed under the SIL Open Font License 1.1 (`fonts/OFL.txt`).
